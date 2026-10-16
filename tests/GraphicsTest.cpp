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

// Host-side tests for the math, the software 2D renderer and the software 3D device.
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>
#include <string.h>
#include "Utility/AllocatedView.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Graphics/Graphics2D/Device/DeviceRenderer2D.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"

using namespace graphics;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

static bool near(float a, float b, float eps = 1e-3f) { return (a > b ? a - b : b - a) < eps; }

static void test_math() {
  CHECK(near(static_cast<float>(fmath::sin(1.0)), 0.841471f, 1e-5f));
  CHECK(near(static_cast<float>(fmath::cos(1.0)), 0.540302f, 1e-5f));
  CHECK(near(static_cast<float>(fmath::sin(100.0)), -0.506366f, 1e-4f));
  CHECK(near(static_cast<float>(fmath::sqrt(2.0)), 1.414214f, 1e-5f));
  CHECK(near(static_cast<float>(fmath::tan(0.5)), 0.546302f, 1e-5f));
  const Mat4 p = Mat4::perspective(1.5707963f, 1.0f, 0.1f, 10.0f);
  const Vec4 near_pt = p * Vec4{0, 0, -0.1f, 1};
  const Vec4 far_pt = p * Vec4{0, 0, -10.0f, 1};
  CHECK(near(near_pt.z / near_pt.w, 0.0f));
  CHECK(near(far_pt.z / far_pt.w, 1.0f));
  const Mat4 m = Mat4::translation(1, 2, 3) * Mat4::scaling(2, 2, 2);
  const Vec4 v = m * Vec4{1, 1, 1, 1};
  CHECK(near(v.x, 3) && near(v.y, 4) && near(v.z, 5));
  CHECK(pack_color({1, 0.5f, 0, 1}) == 0xFFFF8000u);
}

static Surface make_surface(TestAllocator& a, uint32_t w, uint32_t h, Format f = Format::B8G8R8A8Unorm) {
  Surface s;
  s.pixels = allocate_view<uint32_t>(a, w * h);
  s.width = w; s.height = h; s.pitch = w; s.format = f;
  for (uint32_t& p : s.pixels) p = 0;
  return s;
}

static uint32_t px(const Surface& s, uint32_t x, uint32_t y) { return s.decode(s.row(y)[x]); }

static void test_2d() {
  TestAllocator a;
  for (Format f : {Format::B8G8R8A8Unorm, Format::R8G8B8A8Unorm}) {
    Surface s = make_surface(a, 32, 32, f);
    SoftwareRenderer2D r(s);
    r.clear(0xFF000000);
    r.fill_rect({4, 4, 8, 8}, 0xFFFF0000);
    CHECK(px(s, 4, 4) == 0xFFFF0000 && px(s, 11, 11) == 0xFFFF0000);
    CHECK(px(s, 12, 11) == 0xFF000000 && px(s, 3, 4) == 0xFF000000);
    if (f == Format::R8G8B8A8Unorm) CHECK(s.row(4)[4] == 0xFF0000FFu);  // red in the low byte
    r.set_blend(BlendMode::Alpha);
    r.fill_rect({4, 4, 2, 2}, 0x80FFFFFF);  // half white over red
    const uint32_t mixed = px(s, 4, 4);
    CHECK(((mixed >> 16) & 0xFF) == 255 && ((mixed >> 8) & 0xFF) >= 127 && ((mixed >> 8) & 0xFF) <= 129);
    r.set_blend(BlendMode::Replace);
    r.draw_line(0, 31, 31, 0, 0xFF00FF00);
    CHECK(px(s, 0, 31) == 0xFF00FF00 && px(s, 31, 0) == 0xFF00FF00 && px(s, 15, 16) == 0xFF00FF00);
    // Clip.
    r.set_clip({20, 20, 4, 4});
    r.fill_rect({0, 0, 32, 32}, 0xFF0000FF);
    CHECK(px(s, 19, 20) != 0xFF0000FF && px(s, 20, 20) == 0xFF0000FF && px(s, 23, 23) == 0xFF0000FF && px(s, 24, 23) != 0xFF0000FF);
    r.reset_clip();
    // Triangle coverage ~ half the square.
    r.clear(0xFF000000);
    r.fill_triangle({0, 0}, {32, 0}, {0, 32}, 0xFFFFFFFF);
    uint32_t lit = 0;
    for (uint32_t y = 0; y < 32; ++y) for (uint32_t x = 0; x < 32; ++x) lit += px(s, x, y) == 0xFFFFFFFF;
    CHECK(lit >= 500 && lit <= 530);
    r.clear(0xFF000000);
    r.fill_circle(16, 16, 8, 0xFFFFFFFF);
    CHECK(px(s, 16, 16) == 0xFFFFFFFF && px(s, 16, 24) == 0xFFFFFFFF && px(s, 16, 25) == 0xFF000000 && px(s, 4, 4) == 0xFF000000);
    r.clear(0xFF000000);
    r.draw_text(0, 0, "A", 0xFFFFFFFF);
    uint32_t glyph = 0;
    for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 8; ++x) glyph += px(s, x, y) == 0xFFFFFFFF;
    CHECK(glyph > 10);
    // Image blit.
    Surface img = make_surface(a, 4, 4);
    for (uint32_t& p : img.pixels) p = 0xFF123456;
    r.draw_image(img, {0, 0, 4, 4}, 30, 30);  // partly off screen
    CHECK(px(s, 30, 30) == 0xFF123456 && px(s, 31, 31) == 0xFF123456);
    // Everything off-screen is harmless.
    r.fill_rect({-100, -100, 50, 50}, 1); r.draw_line(-1000, -1000, 1000, 1000, 1); r.put_pixel(-1, 99, 1);
    release_view(a, img.pixels);
    release_view(a, s.pixels);
  }
  CHECK(a.live == 0);
}

struct Scene {
  TestAllocator a;
  SoftwareDevice device{a, nullptr};
  IShaderModule *vs, *fs;
  GraphicsPipelineDesc desc;
  IBuffer* vb;
  Scene() {
    vs = device.create_shader_module(colored_vertex_shader());
    fs = device.create_shader_module(colored_fragment_shader());
    describe_colored_pipeline(&desc);
    desc.vertex_shader = vs; desc.fragment_shader = fs;
    vb = device.create_buffer(sizeof(ColoredVertex) * 12, kBufferVertex);
  }
  ~Scene() { device.destroy_buffer(vb); device.destroy_shader_module(vs); device.destroy_shader_module(fs); }
  void set_vertices(const ColoredVertex* v, uint32_t n) {
    memcpy(vb->map().data(), v, sizeof(ColoredVertex) * n);
    vb->unmap();
  }
};

static uint32_t at(const ArrayView<uint32_t>& p, uint32_t w, uint32_t x, uint32_t y) { return p[y * w + x]; }

static void test_3d() {
  Scene s;
  CHECK(s.vs && s.fs && s.vb);
  IImage* color = s.device.create_image({{64, 64}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IImage* depth = s.device.create_image({{64, 64}, Format::D32Float, kImageDepthAttachment});
  CHECK(color && depth);
  ArrayView<uint32_t> out = allocate_view<uint32_t>(s.a, 64 * 64);

  // A red triangle covering the lower-left half in NDC, listed counter-clockwise on screen
  // (top-left -> bottom-left -> bottom-right).
  const ColoredVertex tri[3] = {{{-1, -1, 0.5f}, {1, 0, 0, 1}}, {{-1, 1, 0.5f}, {1, 0, 0, 1}}, {{1, 1, 0.5f}, {1, 0, 0, 1}}};
  s.set_vertices(tri, 3);
  Mat4 id;
  IPipeline* p = s.device.create_graphics_pipeline(s.desc);
  CHECK(p);
  auto render = [&](IPipeline* pipe, const Mat4& mvp, uint32_t verts, bool use_depth) {
    ICommandBuffer* cb = s.device.create_command_buffer();
    cb->begin();
    RenderPassBegin pass;
    pass.color = color; pass.depth = use_depth ? depth : nullptr;
    pass.clear_color_value = {0, 0, 1, 1};
    cb->begin_render_pass(pass);
    cb->bind_pipeline(pipe);
    cb->bind_vertex_buffer(s.vb);
    ColoredConstants c{mvp};
    cb->push_constants(&c, sizeof(c));
    cb->draw(verts);
    cb->end_render_pass();
    bool ok = cb->end() && s.device.submit(cb);
    s.device.destroy_command_buffer(cb);
    return ok;
  };
  CHECK(render(p, id, 3, false));
  CHECK(color->read_pixels(out));
  CHECK(at(out, 64, 5, 58) == 0xFFFF0000);   // inside (lower left)
  CHECK(at(out, 64, 58, 5) == 0xFF0000FF);   // outside (upper right): clear color
  uint32_t red = 0;
  for (uint32_t v : out) red += v == 0xFFFF0000;
  CHECK(red >= 2016 && red <= 2080);          // half of 64*64, give or take the diagonal

  // Reverse winding is culled by default and drawn with cull none.
  const ColoredVertex rev[3] = {tri[0], tri[2], tri[1]};
  s.set_vertices(rev, 3);
  CHECK(render(p, id, 3, false));
  CHECK(color->read_pixels(out) && at(out, 64, 5, 58) == 0xFF0000FF);
  GraphicsPipelineDesc nocull = s.desc;
  nocull.cull_mode = CullMode::None;
  IPipeline* pn = s.device.create_graphics_pipeline(nocull);
  CHECK(render(pn, id, 3, false) && color->read_pixels(out) && at(out, 64, 5, 58) == 0xFFFF0000);

  // Interpolation: red/green/blue corners; the middle of the hypotenuse is a mix.
  const ColoredVertex rgb3[3] = {{{-1, -1, 0.5f}, {1, 0, 0, 1}}, {{-1, 1, 0.5f}, {0, 1, 0, 1}}, {{1, 1, 0.5f}, {0, 0, 1, 1}}};
  s.set_vertices(rgb3, 3);
  CHECK(render(p, id, 3, false) && color->read_pixels(out));
  const uint32_t mid = at(out, 64, 31, 33);  // just inside the hypotenuse
  CHECK(((mid >> 16) & 0xFF) > 100 && ((mid >> 16) & 0xFF) < 160 && (mid & 0xFF) > 100 && (mid & 0xFF) < 160);

  // Depth: near blue-ish triangle (z 0.2) drawn first, far red one (z 0.8) after must not cover it.
  GraphicsPipelineDesc dd = s.desc;
  dd.depth_test = dd.depth_write = true;
  dd.cull_mode = CullMode::None;
  IPipeline* pd = s.device.create_graphics_pipeline(dd);
  const ColoredVertex two[6] = {{{-1, -1, 0.2f}, {0, 1, 0, 1}}, {{-1, 1, 0.2f}, {0, 1, 0, 1}}, {{1, 1, 0.2f}, {0, 1, 0, 1}},
                                {{-1, -1, 0.8f}, {1, 0, 0, 1}}, {{-1, 1, 0.8f}, {1, 0, 0, 1}}, {{1, 1, 0.8f}, {1, 0, 0, 1}}};
  s.set_vertices(two, 6);
  CHECK(render(pd, id, 6, true) && color->read_pixels(out) && at(out, 64, 5, 58) == 0xFF00FF00);

  // Perspective + near-plane clipping: a big triangle crossing the camera plane must not crash or flood the screen.
  const Mat4 mvp = Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f) * Mat4::translation(0, 0, -2) * Mat4::rotation_x(1.2f);
  const ColoredVertex big[3] = {{{-30, 0, -30}, {1, 1, 1, 1}}, {{30, 0, -30}, {1, 1, 1, 1}}, {{0, 0, 30}, {1, 1, 1, 1}}};
  s.set_vertices(big, 3);
  CHECK(render(pn, mvp, 3, false));

  // Off-buffer vertex counts fail cleanly instead of reading out of range.
  CHECK(!render(p, id, 30, false));

  s.device.destroy_pipeline(pd); s.device.destroy_pipeline(pn); s.device.destroy_pipeline(p);
  s.device.destroy_image(color); s.device.destroy_image(depth);
  release_view(s.a, out);
}

// The device-backed 2D renderer (run on the software device here) draws what the CPU renderer draws:
// rectangles, text and images exactly, and shapes to within the pixels along their edges.
static void test_device_2d() {
  TestAllocator a;
  SoftwareDevice device(a, nullptr);
  const uint32_t W = 64, H = 48;
  DeviceRenderer2D d(device, a, {W, H});
  CHECK(d.valid());
  Surface cpu_surface = make_surface(a, W, H);
  SoftwareRenderer2D cpu(cpu_surface);
  ArrayView<uint32_t> pixels = allocate_view<uint32_t>(a, W * H);

  for (IRenderer2D* r : {static_cast<IRenderer2D*>(&d), static_cast<IRenderer2D*>(&cpu)}) {
    r->clear(0xFF101010);
    r->fill_rect({4, 4, 20, 10}, 0xFFFF0000);
    r->draw_rect({30, 4, 20, 10}, 0xFF00FF00);
    r->draw_text(2, 20, "Hi", 0xFFFFFFFF);
    r->set_clip({40, 30, 10, 10});
    r->fill_rect({0, 0, 64, 48}, 0xFF0000FF);
    r->reset_clip();
    r->set_blend(BlendMode::Alpha);
    r->fill_rect({8, 8, 4, 4}, 0x80FFFFFF);
  }
  CHECK(d.read_pixels(pixels));
  uint32_t differing = 0;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) differing += pixels[y * W + x] != px(cpu_surface, x, y);
  CHECK(differing == 0);
  CHECK(pixels[5 * W + 5] == 0xFFFF0000 && pixels[35 * W + 45] == 0xFF0000FF && pixels[35 * W + 39] == 0xFF101010);
  const uint32_t blended = pixels[9 * W + 9];
  CHECK(((blended >> 8) & 0xFF) >= 127 && ((blended >> 8) & 0xFF) <= 130);  // half white over red

  // Shapes the two rasterize slightly differently: the middle is solid, far outside is untouched.
  d.clear(0xFF000000);
  d.fill_circle(32, 24, 10, 0xFFFFFFFF);
  d.fill_triangle({0, 0}, {20, 0}, {0, 20}, 0xFFFFFFFF);
  d.draw_line(0, 47, 63, 47, 0xFF00FF00);
  CHECK(d.read_pixels(pixels));
  CHECK(pixels[24 * W + 32] == 0xFFFFFFFF && pixels[24 * W + 32 + 9] == 0xFFFFFFFF && pixels[24 * W + 32 + 12] == 0xFF000000);
  CHECK(pixels[3 * W + 3] == 0xFFFFFFFF && pixels[3 * W + 25] == 0xFF000000);
  CHECK(pixels[47 * W + 10] == 0xFF00FF00 && pixels[45 * W + 10] == 0xFF000000);

  // An image: runs of equal pixels become rectangles.
  Surface img = make_surface(a, 4, 2);
  for (uint32_t i = 0; i < 8; ++i) img.pixels[i] = i < 4 ? 0xFF112233 : 0xFF445566;
  d.clear(0xFF000000);
  d.draw_image(img, {0, 0, 4, 2}, 10, 10);
  CHECK(d.read_pixels(pixels) && pixels[10 * W + 10] == 0xFF112233 && pixels[11 * W + 13] == 0xFF445566 && pixels[12 * W + 10] == 0xFF000000);
  release_view(a, img.pixels);
  release_view(a, pixels);
  release_view(a, cpu_surface.pixels);
}

int main() {
  test_math();
  test_2d();
  test_3d();
  test_device_2d();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("graphics: all checks passed\n");
  return 0;
}
