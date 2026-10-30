/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "GraphicsDemo.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"
#include "console.hpp"

namespace {
using namespace graphics;

constexpr float kPi = 3.14159265f;
constexpr uint32_t kFrames = 90;

const ColoredVertex kCubeVertices[8] = {
    {{-1, -1, -1}, {0, 0, 0, 1}}, {{1, -1, -1}, {1, 0, 0, 1}}, {{1, 1, -1}, {1, 1, 0, 1}}, {{-1, 1, -1}, {0, 1, 0, 1}},
    {{-1, -1, 1}, {0, 0, 1, 1}},  {{1, -1, 1}, {1, 0, 1, 1}},  {{1, 1, 1}, {1, 1, 1, 1}},  {{-1, 1, 1}, {0, 1, 1, 1}},
};
const uint16_t kCubeIndices[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                   3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};

// Draws known shapes with the device-backed 2D renderer on `device` and reads the image back
// from wherever the device keeps it (host GPU memory on the virgl device).
bool device_2d_check(IGraphicsDevice& device, allocators::IAllocator& allocator) {
  constexpr uint32_t W = 64, H = 48;
  DeviceRenderer2D r(device, allocator, {W, H});
  ArrayView<uint32_t> px = allocate_view<uint32_t>(allocator, W * H);
  bool ok = r.valid() && !px.empty();
  if (ok) {
    r.clear(rgb(16, 16, 16));
    r.fill_rect({4, 4, 20, 10}, rgb(255, 0, 0));
    r.fill_circle(40, 30, 8, rgb(255, 255, 255));
    r.set_blend(BlendMode::Alpha);
    r.fill_rect({8, 8, 4, 4}, 0x80FFFFFF);  // half-transparent white over the red
    r.draw_text(2, 30, "Hi", rgb(0, 255, 0));
    ok = r.read_pixels(px);
  }
  if (ok) {
    const uint32_t mixed = px[9 * W + 9];
    uint32_t green = 0;
    for (uint32_t y = 30; y < 46; ++y)
      for (uint32_t x = 2; x < 18; ++x) green += (px[y * W + x] & 0xFFFFFF) == 0x00FF00;
    ok = (px[5 * W + 5] & 0xFFFFFF) == 0xFF0000 && (px[30 * W + 40] & 0xFFFFFF) == 0xFFFFFF &&
         (px[2 * W + 2] & 0xFFFFFF) == 0x101010 && ((mixed >> 8) & 0xFF) >= 126 && ((mixed >> 8) & 0xFF) <= 130 &&
         green > 10;
  }
  release_view(allocator, px);
  return ok;
}

// Offscreen groups on the device (the premultiplied target, the scaling shader, the composite): 1 ok, 0 wrong, -1 the device does not do them.
int groups_check(IGraphicsDevice& device, allocators::IAllocator& allocator) {
  constexpr uint32_t W = 64, H = 48;
  DeviceRenderer2D r(device, allocator, {W, H});
  ArrayView<uint32_t> px = allocate_view<uint32_t>(allocator, W * H);
  if (!r.valid() || px.empty()) {
    release_view(allocator, px);
    return 0;
  }
  r.clear(rgb(0, 0, 255));
  r.set_blend(BlendMode::Alpha);
  Matrix2D move;
  move.e = 20;
  int result = -1;
  if (r.begin_group({0, 0, 24, 24})) {   // two overlapping rectangles, laid over the backdrop at half opacity and moved right
    r.fill_rect({2, 2, 10, 10}, rgb(255, 255, 255));
    r.fill_rect({6, 6, 10, 10}, rgb(255, 255, 255));
    r.end_group(move, 128, ImageFilter::Nearest);
    if (r.begin_group({0, 24, 24, 24})) {   // and one at full opacity
      r.fill_rect({2, 26, 6, 6}, rgb(255, 0, 0));
      r.end_group(move, 255, ImageFilter::Nearest);
      result = r.read_pixels(px) ? 1 : 0;
    } else {
      result = 0;
    }
  }
  if (result == 1) {
    auto near = [&](uint32_t x, uint32_t y, int cr, int cg, int cb) {
      const uint32_t c = px[y * W + x];
      auto d = [](uint32_t a, int b) { return static_cast<int>(a) - b; };
      return d((c >> 16) & 0xFF, cr) <= 3 && d((c >> 16) & 0xFF, cr) >= -3 && d((c >> 8) & 0xFF, cg) <= 3 && d((c >> 8) & 0xFF, cg) >= -3 && d(c & 0xFF, cb) <= 3 && d(c & 0xFF, cb) >= -3;
    };
    // (the overlap of the two rectangles is not brighter than the rest: it is one picture)
    result = near(24, 5, 128, 128, 255) && near(30, 10, 128, 128, 255) && near(5, 5, 0, 0, 255) && near(24, 30, 255, 0, 0) && near(40, 40, 0, 0, 255) ? 1 : 0;
  }
  // Group filters (blur, colour function): -1 when the device has none, 0 wrong.
  if (result == 1 && r.supports_group_filters()) {
    r.set_blend(BlendMode::Alpha);
    r.clear(rgb(0, 0, 0));
    ColorTransform invert;   // rgb' = 1 - rgb
    for (int i = 0; i < 9; ++i) invert.m[i] = (i % 4 == 0) ? -1.0f : 0.0f;
    invert.add[0] = invert.add[1] = invert.add[2] = 1.0f;
    bool drawn = r.begin_filter_group({0, 0, 20, 20});
    if (drawn) {
      r.fill_rect({2, 2, 8, 8}, rgb(255, 0, 0));
      drawn = r.group_color(invert) && r.end_group(Matrix2D(), 255, ImageFilter::Nearest);
    }
    bool blurred = drawn && r.begin_filter_group({0, 0, 40, 40});
    if (blurred) {
      r.fill_rect({12, 12, 16, 16}, rgb(255, 255, 255));
      blurred = r.group_blur(3.0f) && r.end_group(Matrix2D(), 255, ImageFilter::Nearest);
    }
    bool ok = blurred && r.read_pixels(px);
    if (ok) {
      auto chan = [&](uint32_t x, uint32_t y) { return static_cast<int>((px[y * W + x] >> 8) & 0xFF); };
      const int left = chan(10, 20), right = chan(29, 20), center = chan(20, 20), far = chan(2, 44) + chan(60, 2);
      ok = (px[5 * W + 5] & 0xFFFFFF) == 0x00FFFF && chan(18, 0) == 0 &&   // (inverted red is cyan)
           center >= 240 && left > 20 && left < 235 && right > 20 && right < 235 && left - right <= 12 && right - left <= 12 && far <= 8;
    }
    result = ok ? 2 : 0;
  }
  release_view(allocator, px);
  return result;
}

bool demo_2d(SystemGraphics& graphics) {
  IRenderer2D* renderer = graphics.renderer_2d();
  if (!renderer) return false;
  IRenderer2D& r = *renderer;
  r.set_blend(BlendMode::Alpha);
  r.clear(rgb(16, 16, 24));
  for (int i = 0; i < 12; ++i) r.fill_rect({40 + i * 56, 80, 48, 120}, rgb(static_cast<uint8_t>(20 * i), 120, static_cast<uint8_t>(255 - 20 * i)));
  r.fill_circle(200, 340, 70, rgb(230, 180, 40));
  r.fill_circle(260, 340, 70, 0x8040A0FF);  // translucent
  r.fill_triangle({420, 420}, {620, 280}, {700, 440}, rgb(60, 200, 120));
  r.draw_line(40, 500, 980, 520, rgb(255, 255, 255));
  r.draw_rect({40, 540, 400, 60}, rgb(255, 255, 255));
  r.draw_text(52, 560, r.accelerated() ? "2D renderer: GPU (colored triangles)" : "2D renderer: software", rgb(255, 255, 255));
  r.draw_text(40, 30, graphics.accelerated() ? "3D next: GPU accelerated" : "3D next: software rasterizer", rgb(255, 220, 120));
  r.present();
  hardware::delay_us(800000);
  return true;
}

// Draws known triangles to a small image and checks where they land, so a device that
// disagrees with the documented conventions (+Y down, 0..1 depth, winding as seen on
// screen, back-face culling, depth test) is caught. The software device passes the same
// checks on the host (tests/GraphicsTest.cpp).
bool conventions_check(IGraphicsDevice& device, allocators::IAllocator& allocator) {
  constexpr uint32_t kSize = 32;
  IShaderModule* vs = device.create_shader_module(colored_vertex_shader());
  IShaderModule* fs = device.create_shader_module(colored_fragment_shader());
  GraphicsPipelineDesc culled;
  describe_colored_pipeline(&culled);
  culled.vertex_shader = vs;
  culled.fragment_shader = fs;  // back-face culling, counter-clockwise front faces: the defaults
  GraphicsPipelineDesc depth_desc = culled;
  depth_desc.cull_mode = CullMode::None;
  depth_desc.depth_test = depth_desc.depth_write = true;
  IPipeline* culling = vs && fs ? device.create_graphics_pipeline(culled) : nullptr;
  IPipeline* depth_test = vs && fs ? device.create_graphics_pipeline(depth_desc) : nullptr;
  IBuffer* buffer = device.create_buffer(sizeof(ColoredVertex) * 6, kBufferVertex);
  IImage* color = device.create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IImage* depth = device.create_image({{kSize, kSize}, Format::D32Float, kImageDepthAttachment});
  ICommandBuffer* commands = device.create_command_buffer();
  ArrayView<uint32_t> pixels = allocate_view<uint32_t>(allocator, kSize * kSize);
  bool ok = culling && depth_test && buffer && color && depth && commands && !pixels.empty();

  auto render = [&](const IPipeline* pipeline, const ColoredVertex* v, uint32_t count) {
    __builtin_memcpy(buffer->map().data(), v, sizeof(ColoredVertex) * count);
    buffer->unmap();
    commands->begin();
    RenderPassBegin pass;
    pass.color = color;
    pass.depth = depth;
    pass.clear_color_value = {0, 0, 1, 1};
    commands->begin_render_pass(pass);
    commands->bind_pipeline(pipeline);
    commands->bind_vertex_buffer(buffer);
    ColoredConstants constants;  // identity: vertices are in clip space
    commands->push_constants(&constants, sizeof(constants));
    commands->draw(count);
    commands->end_render_pass();
    return commands->end() && device.submit(commands) && color->read_pixels(pixels);
  };
  auto at = [&](uint32_t x, uint32_t y) { return pixels[y * kSize + x] & 0xFFFFFF; };
  if (ok) {
    // Counter-clockwise on screen (top-left, bottom-left, bottom-right): covers the lower-left half.
    const ColoredVertex front[3] = {{{-1, -1, 0.5f}, {1, 0, 0, 1}}, {{-1, 1, 0.5f}, {1, 0, 0, 1}}, {{1, 1, 0.5f}, {1, 0, 0, 1}}};
    ok = render(culling, front, 3) && at(3, 28) == 0xFF0000 && at(28, 3) == 0x0000FF;
    // The same triangle wound the other way is a back face: culled.
    const ColoredVertex back[3] = {front[0], front[2], front[1]};
    ok = ok && render(culling, back, 3) && at(3, 28) == 0x0000FF;
    // Two overlapping triangles: the nearer (green, z 0.2) is drawn first and must stay on top of red (z 0.8).
    const ColoredVertex two[6] = {{{-1, -1, 0.2f}, {0, 1, 0, 1}}, {{-1, 1, 0.2f}, {0, 1, 0, 1}}, {{1, 1, 0.2f}, {0, 1, 0, 1}},
                                  {{-1, -1, 0.8f}, {1, 0, 0, 1}}, {{-1, 1, 0.8f}, {1, 0, 0, 1}}, {{1, 1, 0.8f}, {1, 0, 0, 1}}};
    ok = ok && render(depth_test, two, 6) && at(3, 28) == 0x00FF00;
  }
  release_view(allocator, pixels);
  if (commands) device.destroy_command_buffer(commands);
  if (depth) device.destroy_image(depth);
  if (color) device.destroy_image(color);
  if (buffer) device.destroy_buffer(buffer);
  if (depth_test) device.destroy_pipeline(depth_test);
  if (culling) device.destroy_pipeline(culling);
  if (fs) device.destroy_shader_module(fs);
  if (vs) device.destroy_shader_module(vs);
  return ok;
}

bool demo_3d(SystemGraphics& graphics, allocators::IAllocator& allocator, uint64_t* microseconds) {
  IGraphicsDevice& device = graphics.device();
  hardware::IGraphicsProcessingUnit* display = graphics.display();
  const Extent2D size{display->width(), display->height()};

  IShaderModule* vs = device.create_shader_module(colored_vertex_shader());
  IShaderModule* fs = device.create_shader_module(colored_fragment_shader());
  GraphicsPipelineDesc desc;
  describe_colored_pipeline(&desc);
  desc.vertex_shader = vs;
  desc.fragment_shader = fs;
  desc.cull_mode = CullMode::None;
  desc.depth_test = desc.depth_write = true;
  IPipeline* pipeline = vs && fs ? device.create_graphics_pipeline(desc) : nullptr;
  IBuffer* vertices = device.create_buffer(sizeof(kCubeVertices), kBufferVertex);
  IBuffer* indices = device.create_buffer(sizeof(kCubeIndices), kBufferIndex);
  IImage* color = device.create_image({size, Format::B8G8R8A8Unorm, kImageColorAttachment | kImagePresent});
  IImage* depth = device.create_image({size, Format::D32Float, kImageDepthAttachment});
  ICommandBuffer* commands = device.create_command_buffer();
  bool ok = pipeline && vertices && indices && color && depth && commands;
  bool first_frame_checked = false;

  if (ok) {
    __builtin_memcpy(vertices->map().data(), kCubeVertices, sizeof(kCubeVertices));
    vertices->unmap();
    __builtin_memcpy(indices->map().data(), kCubeIndices, sizeof(kCubeIndices));
    indices->unmap();
    const Mat4 projection = Mat4::perspective(0.9f, static_cast<float>(size.width) / size.height, 0.1f, 20.0f);
    const uint64_t start = hardware::uptime_us();
    for (uint32_t frame = 0; ok && frame < kFrames; ++frame) {
      const float t = frame * (2 * kPi / kFrames);
      ColoredConstants constants{projection * Mat4::translation(0, 0, -4.5f) * Mat4::rotation_y(t) * Mat4::rotation_x(0.6f * t)};
      commands->begin();
      RenderPassBegin pass;
      pass.color = color;
      pass.depth = depth;
      pass.clear_color_value = {0.06f, 0.06f, 0.1f, 1.0f};
      commands->begin_render_pass(pass);
      commands->bind_pipeline(pipeline);
      commands->bind_vertex_buffer(vertices);
      commands->bind_index_buffer(indices, IndexType::Uint16);
      commands->push_constants(&constants, sizeof(constants));
      commands->draw_indexed(36);
      commands->end_render_pass();
      ok = commands->end() && device.submit(commands) && device.present(color);
      if (ok && frame == 0) {  // the cube must be on the image: the center is not the clear color
        ArrayView<uint32_t> pixels = allocate_view<uint32_t>(allocator, uint64_t{size.width} * size.height);
        if (!pixels.empty() && color->read_pixels(pixels)) {
          const uint32_t center = pixels[(size.height / 2) * size.width + size.width / 2];
          const uint32_t corner = pixels[0];
          first_frame_checked = center != corner && (center & 0xFFFFFF) != 0x0F0F1A;
        }
        release_view(allocator, pixels);
      }
    }
    *microseconds = hardware::uptime_us() - start;
  }

  if (commands) device.destroy_command_buffer(commands);
  if (depth) device.destroy_image(depth);
  if (color) device.destroy_image(color);
  if (indices) device.destroy_buffer(indices);
  if (vertices) device.destroy_buffer(vertices);
  if (pipeline) device.destroy_pipeline(pipeline);
  if (fs) device.destroy_shader_module(fs);
  if (vs) device.destroy_shader_module(vs);
  return ok && first_frame_checked;
}
}  // namespace

int check_2d_groups(IGraphicsDevice& device, allocators::IAllocator& allocator) { return groups_check(device, allocator); }

bool run_graphics_demo(SystemGraphics& graphics, allocators::IAllocator& allocator) {
  if (!graphics.display()) return false;
  const bool ok_2d = demo_2d(graphics);
  uint64_t microseconds = 0;
  const bool device_2d = device_2d_check(graphics.device(), allocator);
  const int groups = groups_check(graphics.device(), allocator);
  const bool conventions = conventions_check(graphics.device(), allocator);
  const bool ok_3d = demo_3d(graphics, allocator, &microseconds);

  console::Guard guard;
  console::puts("Graphics: 3D device ");
  console::puts(graphics.device().info().name);
  console::puts(graphics.accelerated() ? " (GPU accelerated)\n" : " (software)\n");
  console::puts(ok_2d ? "  2D renderer: ok (" : "  2D renderer: FAILED (");
  console::puts(graphics.renderer_2d() ? graphics.renderer_2d()->name() : "none");
  console::puts(graphics.renderer_2d() && graphics.renderer_2d()->accelerated() ? ", GPU)\n" : ", CPU)\n");
  console::puts(device_2d ? "  2D on the 3D device (readback of shapes, alpha, text): ok\n"
                          : "  2D on the 3D device (readback of shapes, alpha, text): FAILED\n");
  console::puts(groups > 0 ? (groups > 1 ? "  2D offscreen groups (opacity, composite, blur and colour filters): ok\n" : "  2D offscreen groups (opacity, premultiplied composite): ok\n")
                           : groups == 0 ? "  2D offscreen groups (opacity, premultiplied composite): FAILED\n" : "  2D offscreen groups: not supported by this device\n");
  console::puts(conventions ? "  3D conventions (orientation, culling, depth): ok\n"
                            : "  3D conventions (orientation, culling, depth): FAILED\n");
  console::puts(ok_3d ? "  3D cube: ok, " : "  3D cube: FAILED, ");
  if (microseconds) {
    console::put_dec(kFrames * 1000000ull / microseconds);
    console::puts(" frames per second\n");
  } else {
    console::puts("no frames\n");
  }
  return ok_2d && ok_3d && conventions && device_2d && groups != 0;
}
