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

#include "Desktop/Graphics3DTest.hpp"
#include <new>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/CubeMesh.hpp"
#include "Graphics/Logo/Logo.hpp"
#include "Kernel/Delay.hpp"
#include "console.hpp"
#include "Utility/AllocatedView.hpp"

namespace desktop {

using namespace graphics;

namespace {
constexpr float kTwoPi = 6.2831853f;
constexpr uint32_t kBackground = 0xFF10101A;
constexpr uint32_t kLogoSize = 256;   // the texture is the logo at this size (its own)
constexpr uint32_t kGreen = 0xFF40E060, kAmber = 0xFFFFB030, kRed = 0xFFFF5050, kWhite = 0xFFFFFFFF, kShadow = 0xFF000000;

// A cube with a color per face: four vertices a face, so the colors stay flat.
struct Face {
  float normal[3];     // which way the face points, and so which axes it spans
  float color[4];
};
constexpr Face kFaces[6] = {
    {{0, 0, 1}, {0.95f, 0.30f, 0.30f, 1}},  {{0, 0, -1}, {0.30f, 0.85f, 0.35f, 1}}, {{1, 0, 0}, {0.30f, 0.50f, 0.95f, 1}},
    {{-1, 0, 0}, {0.95f, 0.85f, 0.25f, 1}}, {{0, 1, 0}, {0.85f, 0.35f, 0.90f, 1}},  {{0, -1, 0}, {0.30f, 0.85f, 0.90f, 1}},
};
}  // namespace

struct CubeView::Resources {
  IGraphicsDevice* device = nullptr;
  allocators::IAllocator* allocator = nullptr;
  IShaderModule* vs = nullptr;
  IShaderModule* fs = nullptr;
  IPipeline* pipeline = nullptr;
  IBuffer* vertices = nullptr;
  IBuffer* indices = nullptr;
  // The logo on the faces; all null on a device that cannot do it (the cube is then drawn in flat colors).
  IShaderModule* textured_vs = nullptr;
  IShaderModule* textured_fs = nullptr;
  IPipeline* textured_pipeline = nullptr;
  IBuffer* textured_vertices = nullptr;
  IImage* logo = nullptr;
  IImage* color = nullptr;
  IImage* depth = nullptr;
  ICommandBuffer* commands = nullptr;
  Surface picture;
  uint32_t width = 0, height = 0;

  ~Resources() {
    if (!device) return;
    if (commands) device->destroy_command_buffer(commands);
    if (depth) device->destroy_image(depth);
    if (logo) device->destroy_image(logo);
    if (textured_vertices) device->destroy_buffer(textured_vertices);
    if (textured_pipeline) device->destroy_pipeline(textured_pipeline);
    if (textured_fs) device->destroy_shader_module(textured_fs);
    if (textured_vs) device->destroy_shader_module(textured_vs);
    if (color) device->destroy_image(color);
    if (indices) device->destroy_buffer(indices);
    if (vertices) device->destroy_buffer(vertices);
    if (pipeline) device->destroy_pipeline(pipeline);
    if (fs) device->destroy_shader_module(fs);
    if (vs) device->destroy_shader_module(vs);
    release_view(*allocator, picture.pixels);
  }
};

CubeView::~CubeView() { release_resources(); }

void CubeView::set_graphics(SystemGraphics* graphics) {
  gui::UiGuard lock(this);
  graphics_ = graphics;
  invalidate();
}

void CubeView::set_use_gpu(bool use_gpu) {
  gui::UiGuard lock(this);
  use_gpu_ = use_gpu;
  second_start_us_ = 0;   // the frame rate starts over
  invalidate();
}

void CubeView::set_use_texture(bool use_texture) {
  gui::UiGuard lock(this);
  use_texture_ = use_texture;
  invalidate();
}

void CubeView::release_resources() {
  if (!resources_) return;
  Resources* r = resources_;
  resources_ = nullptr;
  resources_device_ = nullptr;
  r->~Resources();
  allocator().deallocate(r);
}

// Builds the device objects for this size: shaders, pipeline, the cube's buffers, the color and depth images, a command buffer, and the
// pixels the picture is read back into.
bool CubeView::ensure_resources(uint32_t width, uint32_t height) {
  IGraphicsDevice* device = graphics_ ? (use_gpu_ && graphics_->accelerated() ? &graphics_->device() : &graphics_->software_device()) : nullptr;
  if (!device) return false;
  if (resources_ && resources_device_ == device && resources_->width == width && resources_->height == height) return true;
  release_resources();

  void* memory = allocator().allocate(sizeof(Resources));
  if (!memory) return false;
  Resources* r = new (memory) Resources();
  r->device = device;
  r->allocator = &allocator();
  r->width = width;
  r->height = height;
  resources_ = r;
  resources_device_ = device;

  r->vs = device->create_shader_module(colored_vertex_shader());
  r->fs = device->create_shader_module(colored_fragment_shader());
  GraphicsPipelineDesc desc;
  describe_colored_pipeline(&desc);
  desc.vertex_shader = r->vs;
  desc.fragment_shader = r->fs;
  desc.cull_mode = CullMode::None;
  desc.depth_test = desc.depth_write = true;
  r->pipeline = r->vs && r->fs ? device->create_graphics_pipeline(desc) : nullptr;
  r->vertices = device->create_buffer(sizeof(ColoredVertex) * 24, kBufferVertex);
  r->indices = device->create_buffer(sizeof(uint16_t) * 36, kBufferIndex);
  r->color = device->create_image({{width, height}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  r->depth = device->create_image({{width, height}, Format::D32Float, kImageDepthAttachment});
  r->commands = device->create_command_buffer();
  r->picture.pixels = allocate_view<uint32_t>(allocator(), uint64_t{width} * height);
  r->picture.width = width;
  r->picture.height = height;
  r->picture.pitch = width;
  r->picture.format = Format::B8G8R8A8Unorm;
  if (!r->pipeline || !r->vertices || !r->indices || !r->color || !r->depth || !r->commands || !r->picture.valid()) {
    release_resources();
    return false;
  }

  // The geometry: each face is a square in the plane its normal points out of, its corners one unit either way along the other two axes.
  ColoredVertex* v = reinterpret_cast<ColoredVertex*>(r->vertices->map().data());
  uint16_t* index = reinterpret_cast<uint16_t*>(r->indices->map().data());
  for (uint32_t f = 0; f < 6; ++f) {
    const Face& face = kFaces[f];
    // The two axes the face spans: the ones its normal does not use.
    uint32_t axis_a = 0, axis_b = 1, axis_n = 2;
    if (face.normal[0] != 0) { axis_n = 0; axis_a = 1; axis_b = 2; }
    else if (face.normal[1] != 0) { axis_n = 1; axis_a = 0; axis_b = 2; }
    const float sign = face.normal[axis_n];
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (uint32_t c = 0; c < 4; ++c) {
      ColoredVertex& out = v[f * 4 + c];
      out.position[axis_n] = sign;
      out.position[axis_a] = corners[c][0];
      out.position[axis_b] = corners[c][1];
      for (int k = 0; k < 4; ++k) out.color[k] = face.color[k];
    }
    static const uint16_t order[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t k = 0; k < 6; ++k) index[f * 6 + k] = static_cast<uint16_t>(f * 4 + order[k]);
  }
  r->vertices->unmap();
  r->indices->unmap();

  // The logo on the faces: best effort, a device without texture sampling just keeps the colors.
  texture_available_ = false;
  graphics::Logo logo(allocator());
  ArrayView<uint32_t> texels = allocate_view<uint32_t>(allocator(), uint64_t{kLogoSize} * kLogoSize);
  if (!texels.empty() && logo.load(embedded_logo_png()) && logo.render(kLogoSize, kBackground & 0xFFFFFF, texels)) {
    r->textured_vs = device->create_shader_module(textured_vertex_shader());
    r->textured_fs = device->create_shader_module(textured_fragment_shader());
    GraphicsPipelineDesc textured;
    describe_textured_pipeline(&textured);
    textured.vertex_shader = r->textured_vs;
    textured.fragment_shader = r->textured_fs;
    textured.cull_mode = CullMode::None;
    textured.depth_test = textured.depth_write = true;
    if (r->textured_vs && r->textured_fs) r->textured_pipeline = device->create_graphics_pipeline(textured);
    r->textured_vertices = device->create_buffer(sizeof(TexturedVertex) * kCubeVertices, kBufferVertex);
    r->logo = device->create_image({{kLogoSize, kLogoSize}, Format::B8G8R8A8Unorm, kImageSampled});
    if (r->textured_pipeline && r->textured_vertices && r->logo && r->logo->write_pixels(ArrayView<const uint32_t>(texels.data(), texels.size()))) {
      ByteView bytes = r->textured_vertices->map();
      ArrayView<TexturedVertex> tv(reinterpret_cast<TexturedVertex*>(bytes.data()), kCubeVertices);   // (the mapped memory is a vertex buffer of this size, suitably aligned)
      uint16_t unused[kCubeIndices];
      build_textured_cube(tv, ArrayView<uint16_t>(unused, kCubeIndices));   // (the indices are the colored cube's, built above)
      r->textured_vertices->unmap();
      texture_available_ = true;
    } else {
      if (r->logo) r->device->destroy_image(r->logo);
      if (r->textured_vertices) r->device->destroy_buffer(r->textured_vertices);
      if (r->textured_pipeline) r->device->destroy_pipeline(r->textured_pipeline);
      if (r->textured_fs) r->device->destroy_shader_module(r->textured_fs);
      if (r->textured_vs) r->device->destroy_shader_module(r->textured_vs);
      r->logo = nullptr;
      r->textured_vertices = nullptr;
      r->textured_pipeline = nullptr;
      r->textured_fs = r->textured_vs = nullptr;
    }
  }
  release_view(allocator(), texels);
  return true;
}

// Draws one frame at `angle` and reads it back into the picture.
bool CubeView::draw_frame(float angle) {
  Resources& r = *resources_;
  const float aspect = static_cast<float>(r.width) / static_cast<float>(r.height);
  const ColoredConstants constants{Mat4::perspective(0.9f, aspect, 0.1f, 20.0f) * Mat4::translation(0, 0, -4.5f) * Mat4::rotation_y(angle) *
                                   Mat4::rotation_x(0.6f * angle)};
  RenderPassBegin pass;
  pass.color = r.color;
  pass.depth = r.depth;
  pass.clear_color_value = {0.063f, 0.063f, 0.102f, 1.0f};
  r.commands->begin();
  r.commands->begin_render_pass(pass);
  const bool textured = use_texture_ && texture_available_ && r.logo;
  active_textured_ = textured;
  if (textured) {
    SamplerDesc sampler;
    sampler.filter = Filter::Linear;
    sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
    r.commands->bind_pipeline(r.textured_pipeline);
    r.commands->bind_vertex_buffer(r.textured_vertices);
    r.commands->bind_texture(0, r.logo, sampler);
  } else {
    r.commands->bind_pipeline(r.pipeline);
    r.commands->bind_vertex_buffer(r.vertices);
  }
  r.commands->bind_index_buffer(r.indices, IndexType::Uint16);
  r.commands->push_constants(&constants, sizeof constants);
  r.commands->draw_indexed(36);
  r.commands->end_render_pass();
  return r.commands->end() && r.device->submit(r.commands) && r.color->read_pixels(r.picture.pixels);
}

void CubeView::paint(gui::Painter& painter) {
  const gui::Rect2D area{0, 0, bounds().width, bounds().height};
  painter.fill(area, kBackground);
  if (!graphics_) {
    painter.text_centered(area, "No graphics device.", kRed);
    return;
  }

  const uint64_t frame_start = hardware::uptime_us();
  if (!start_us_) start_us_ = frame_start;
  uint32_t width = static_cast<uint32_t>(area.width > 0 ? area.width : 0), height = static_cast<uint32_t>(area.height > 0 ? area.height : 0);
  if (width > kMaxWidth) width = kMaxWidth;
  if (height > kMaxHeight) height = kMaxHeight;
  const bool drawn = width >= 32 && height >= 32 && ensure_resources(width, height);
  failed_ = !drawn;
  if (drawn) {
    const float angle = static_cast<float>((frame_start - start_us_) % 6000000) * (kTwoPi / 6000000.0f);   // one turn in 6 seconds
    if (draw_frame(angle)) {
      painter.image(resources_->picture, {0, 0, width, height}, (area.width - static_cast<int32_t>(width)) / 2, (area.height - static_cast<int32_t>(height)) / 2);
      active_accelerated_ = resources_device_->info().accelerated;
      device_name_.assign(resources_device_->info().name);
    } else {
      failed_ = true;
    }
  }
  const uint64_t now = hardware::uptime_us();
  frame_us_ = now - frame_start;
  ++total_frames_;
  ++frames_in_second_;
  if (!second_start_us_) second_start_us_ = now;
  if (now - second_start_us_ >= 1000000) {
    fps_ = static_cast<uint32_t>(uint64_t{frames_in_second_} * 1000000 / (now - second_start_us_));
    frames_in_second_ = 0;
    second_start_us_ = now;
    if (++seconds_ % 5 == 1) {   // now and then, on the debug console too
      FixedString<128> report;
      report.assign("3D Test: ");
      report.append_uint(fps_);
      report.append(" FPS, ");
      report.append(active_accelerated_ && !failed_ ? "GPU accelerated" : "software");
      report.append(" (");
      report.append(device_name_.view());
      report.append(")\n");
      console::debug_puts(report.view());
    }
  }

  // The status, over the picture's corner.
  FixedString<96> line;
  line.assign("FPS: ");
  line.append_uint(fps_);
  line.append("   render: ");
  line.append_uint(frame_us_ / 1000);
  line.push_back('.');
  line.append_uint((frame_us_ % 1000) / 100);
  line.append(" ms");
  painter.text(9, 9, line.view(), kShadow);
  painter.text(8, 8, line.view(), kWhite);

  FixedString<64> faces;
  faces.assign(failed_ ? "" : active_textured_ ? "Faces: logo texture" : texture_available_ ? "Faces: flat colors" : "Faces: flat colors (no texture support)");
  painter.text(9, 63, faces.view(), kShadow);
  painter.text(8, 62, faces.view(), kWhite);

  const bool gpu = active_accelerated_ && !failed_;
  const uint32_t indicator = failed_ ? kRed : gpu ? kGreen : kAmber;
  painter.fill({8, 28, 12, 12}, indicator);
  painter.outline({8, 28, 12, 12}, kWhite);
  FixedString<96> mode;
  mode.assign(failed_ ? "3D drawing failed" : gpu ? "GPU accelerated" : "Software (CPU)");
  if (!failed_ && device_name_.view().size()) {
    mode.append(" - ");
    mode.append(device_name_.view());
  }
  painter.text(27, 27, mode.view(), kShadow);
  painter.text(26, 26, mode.view(), indicator);
  if (!gpu_available() && !failed_) {   // say why, so the machine's setup can be fixed
    FixedString<128> why;
    why.assign("No GPU: ");
    why.append(software_reason());
    painter.text(9, 45, why.view(), kShadow);
    painter.text(8, 44, why.view(), kAmber);
  }
  invalidate();   // the next frame, as soon as the render loop is ready
}

// --- The window ------------------------------------------------------------------------------------

Graphics3DTest::Graphics3DTest(allocators::IAllocator& allocator) : Frame(allocator), view_(allocator), bar_(allocator), gpu_box_(allocator), texture_box_(allocator) {
  set_title("3D Test");
  set_bounds({100, 60, 440, 380});
  set_padding(gui::Insets::all(4));
  set_minimize_enabled(true);

  bar_.set_style(gui::PanelStyle::Sunken);
  bar_.set_dock(gui::DockStyle::Bottom);
  bar_.set_size(0, 26);
  gpu_box_.set_text("Use GPU acceleration");
  gpu_box_.set_dock(gui::DockStyle::Fill);
  gpu_box_.set_checked(true);
  gpu_box_.set_enabled(false);   // until a GPU device is known
  gpu_box_.on_changed({&Graphics3DTest::on_gpu_changed, this});
  texture_box_.set_text("Logo texture");
  texture_box_.set_dock(gui::DockStyle::Left);
  texture_box_.set_size(130, 0);
  texture_box_.set_checked(true);
  texture_box_.on_changed({&Graphics3DTest::on_texture_changed, this});
  bar_.add(&texture_box_);
  bar_.add(&gpu_box_);
  view_.set_dock(gui::DockStyle::Fill);
  add(&bar_);
  add(&view_);
}

void Graphics3DTest::set_graphics(SystemGraphics* graphics) {
  view_.set_graphics(graphics);
  gpu_box_.set_enabled(view_.gpu_available());
  gpu_box_.set_text(view_.gpu_available() ? "Use GPU acceleration" : "Use GPU acceleration (none here)");
}

void Graphics3DTest::on_texture_changed(void* self, gui::Control&) {
  auto* t = static_cast<Graphics3DTest*>(self);
  t->view_.set_use_texture(t->texture_box_.checked());
}

void Graphics3DTest::on_gpu_changed(void* self, gui::Control&) {
  auto* t = static_cast<Graphics3DTest*>(self);
  t->view_.set_use_gpu(t->gpu_box_.checked());
}

}  // namespace desktop
