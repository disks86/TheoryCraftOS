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
#include "Graphics/Graphics2D/Raster2D.hpp"
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


// ---- Pixel-engine primitives, run on both renderers --------------------------------------------------------

namespace {
constexpr uint32_t kW = 64, kH = 64;
constexpr uint32_t kBlack = 0xFF000000u, kWhite = 0xFFFFFFFFu, kRed = 0xFFFF0000u, kGreen = 0xFF00FF00u, kBlue = 0xFF0000FFu;

// One renderer plus a way to read the finished frame back as 0xAARRGGBB.
struct Rig {
  IRenderer2D* r = nullptr;
  Surface* soft = nullptr;
  DeviceRenderer2D* dev = nullptr;
  ArrayView<uint32_t> frame;  // last read
  bool exact_clips = true;    // rounded / mask clips and layers are exact (software renderer)

  void read() {
    if (dev) {
      CHECK(dev->read_pixels(frame));
    } else {
      for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) frame[y * kW + x] = soft->decode(soft->row(y)[x]);
    }
  }
  uint32_t at(uint32_t x, uint32_t y) { return frame[y * kW + x]; }
  void reset() {
    r->reset_clip();
    r->set_blend(BlendMode::Alpha);
    r->clear(kBlack);
  }
};

bool chan_near(uint32_t a, uint32_t b, uint32_t tol) {
  for (int shift = 0; shift <= 24; shift += 8) {
    const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
    if ((d < 0 ? -d : d) > static_cast<int32_t>(tol)) return false;
  }
  return true;
}
uint32_t red_of(uint32_t c) { return (c >> 16) & 0xFF; }

uint32_t reference_blend_over(uint32_t s, uint32_t d) {  // the original, divide-per-channel formulation
  const uint32_t sa = s >> 24;
  if (sa == 255) return s;
  if (sa == 0) return d;
  const uint32_t da = d >> 24;
  const uint32_t inv = 255 - sa;
  const uint32_t oa = sa + (da * inv + 127) / 255;
  if (oa == 0) return 0;
  uint32_t out = oa << 24;
  for (int shift = 16; shift >= 0; shift -= 8) {
    const uint32_t sc = (s >> shift) & 0xFF, dc = (d >> shift) & 0xFF;
    const uint32_t num = sc * sa * 255 + dc * da * inv;
    uint32_t c = (num + oa * 127) / (oa * 255);
    if (c > 255) c = 255;
    out |= c << shift;
  }
  return out;
}
}  // namespace

static void test_blend_math() {
  for (uint32_t v = 0; v <= 65025; ++v) {
    if (raster::div255(v) != (v + 127) / 255) { CHECK(false); break; }
    if (v <= 65534 && raster::div255_floor(v) != v / 255) { CHECK(false); break; }
  }
  // blend_over keeps its results while dropping the divides.
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  bool same = true;
  for (uint32_t sa = 0; sa < 256 && same; ++sa)
    for (uint32_t da : {0u, 1u, 7u, 127u, 128u, 254u, 255u}) {
      for (int k = 0; k < 40; ++k) {
        const uint32_t s = (sa << 24) | (rnd() & 0xFFFFFF), d = (da << 24) | (rnd() & 0xFFFFFF);
        if (blend_over(s, d) != reference_blend_over(s, d)) { same = false; break; }
      }
      if (!same) break;
    }
  CHECK(same);
  CHECK(blend_over(0x80FF0000, 0xFF000000) == reference_blend_over(0x80FF0000, 0xFF000000));
  CHECK(blend_over(0x80FF0000, 0) == 0x80FF0000);
}

static void test_fast_paths() {
  TestAllocator a;
  for (Format tf : {Format::B8G8R8A8Unorm, Format::R8G8B8A8Unorm}) {
    for (Format sf : {Format::B8G8R8A8Unorm, Format::R8G8B8A8Unorm}) {
      Surface dst = make_surface(a, 16, 4, tf);
      Surface src = make_surface(a, 16, 4, sf);
      SoftwareRenderer2D r(dst);
      uint32_t seed = 7;
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 16; ++x) {
          seed = seed * 1664525u + 1013904223u;
          // Rows 0-1 opaque, row 2 mixed, row 3 translucent.
          uint32_t alpha = y < 2 ? 255 : (y == 2 ? ((x % 3 == 0) ? 255 : (x % 3 == 1 ? 0 : 99)) : 77);
          src.row(y)[x] = src.encode((alpha << 24) | ((seed >> 8) & 0xFFFFFF));
        }
      for (BlendMode m : {BlendMode::Alpha, BlendMode::Replace}) {
        r.set_blend(BlendMode::Replace);
        r.clear(0xFF336699);
        r.set_blend(m);
        r.draw_image(src, {0, 0, 16, 4}, 0, 0);
        bool ok = true;
        for (uint32_t y = 0; y < 4; ++y)
          for (uint32_t x = 0; x < 16; ++x) {
            const uint32_t s = src.decode(src.row(y)[x]);
            const uint32_t expect = m == BlendMode::Replace ? s : blend_over(s, 0xFF336699);
            ok = ok && dst.decode(dst.row(y)[x]) == expect;
          }
        CHECK(ok);
      }
      // Opaque and translucent spans.
      r.set_blend(BlendMode::Alpha);
      r.clear(0xFF336699);
      r.fill_rect({2, 0, 5, 2}, 0xFF010203);
      r.fill_rect({8, 1, 5, 2}, 0x40ABCDEF);
      CHECK(px(dst, 2, 0) == 0xFF010203 && px(dst, 6, 1) == 0xFF010203 && px(dst, 7, 0) == 0xFF336699);
      CHECK(px(dst, 8, 1) == blend_over(0x40ABCDEF, 0xFF336699) && px(dst, 12, 2) == blend_over(0x40ABCDEF, 0xFF336699));
      release_view(a, src.pixels);
      release_view(a, dst.pixels);
    }
  }
  CHECK(a.live == 0);
}

static void test_rounded(Rig& g) {
  IRenderer2D& r = *g.r;
  g.reset();
  r.fill_rounded_rect({8, 8, 40, 30}, CornerRadii::uniform(10), kWhite);
  g.read();
  CHECK(g.at(8, 8) == kBlack && g.at(9, 9) == kBlack && g.at(10, 10) == kBlack);  // corner cut away
  CHECK(g.at(11, 11) == kWhite && g.at(28, 23) == kWhite);
  CHECK(g.at(28, 8) == kWhite && g.at(8, 23) == kWhite && g.at(47, 23) == kWhite && g.at(28, 37) == kWhite);  // straight edges: exact
  CHECK(g.at(48, 23) == kBlack && g.at(7, 23) == kBlack && g.at(28, 7) == kBlack && g.at(28, 38) == kBlack);
  CHECK(g.at(47, 8) == kBlack && g.at(8, 37) == kBlack && g.at(47, 37) == kBlack);  // the other three corners
  const uint32_t edge = red_of(g.at(10, 11));  // straddles the curve: partial coverage
  CHECK(edge > 100 && edge < 200);

  // Elliptical corners: wide and flat.
  g.reset();
  CornerRadii e{};
  e.tl_x = 16, e.tl_y = 4;
  r.fill_rounded_rect({8, 8, 40, 30}, e, kWhite);
  g.read();
  CHECK(g.at(8, 8) == kBlack && g.at(8, 12) == kWhite && g.at(23, 8) == kWhite && g.at(47, 8) == kWhite);
  CHECK(g.at(9, 8) == kBlack);  // far along the flat corner

  // Radii larger than the box are scaled down: a pill.
  g.reset();
  r.fill_rounded_rect({4, 4, 40, 10}, CornerRadii::uniform(100), kWhite);
  g.read();
  CHECK(g.at(24, 9) == kWhite && g.at(4, 4) == kBlack && g.at(43, 13) == kBlack && g.at(24, 4) == kWhite && g.at(24, 13) == kWhite);

  // Translucent fill and the radius-0 case.
  g.reset();
  r.fill_rounded_rect({4, 4, 20, 20}, CornerRadii{}, 0x80FFFFFF);
  r.fill_rounded_rect({30, 4, 20, 20}, CornerRadii::uniform(6), 0x80FFFFFF);
  g.read();
  CHECK(chan_near(g.at(4, 4), 0xFF808080, 1) && chan_near(g.at(40, 14), 0xFF808080, 1) && g.at(30, 4) == kBlack);

  // Replace mode: no anti-aliasing, every edge pixel is the color or untouched.
  g.reset();
  r.set_blend(BlendMode::Replace);
  r.fill_rounded_rect({8, 8, 40, 30}, CornerRadii::uniform(10), kWhite);
  g.read();
  bool binary = true;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) binary = binary && (g.at(x, y) == kBlack || g.at(x, y) == kWhite);
  CHECK(binary && g.at(28, 23) == kWhite && g.at(8, 8) == kBlack);
  r.set_blend(BlendMode::Alpha);

  // Borders with a different width per side.
  g.reset();
  r.stroke_rounded_rect({4, 4, 40, 30}, CornerRadii::uniform(8), {3, 1, 3, 5}, kWhite);
  g.read();
  CHECK(g.at(6, 19) == kWhite && g.at(8, 19) == kWhite && g.at(9, 19) == kBlack);       // left border: x 4..8
  CHECK(g.at(24, 4) == kWhite && g.at(24, 6) == kWhite && g.at(24, 7) == kBlack);       // top border: y 4..6
  CHECK(g.at(24, 33) == kWhite && g.at(24, 31) == kWhite && g.at(24, 30) == kBlack);    // bottom border: y 31..33
  CHECK(g.at(43, 19) == kWhite && g.at(42, 19) == kBlack);                               // right border: x 43
  CHECK(g.at(24, 19) == kBlack && g.at(4, 4) == kBlack && g.at(3, 19) == kBlack && g.at(44, 19) == kBlack);
  CHECK(g.at(24, 34) == kBlack);

  // A border wider than the box leaves no hole.
  g.reset();
  r.stroke_rounded_rect({4, 4, 10, 10}, CornerRadii{}, EdgeWidths::uniform(9), kGreen);
  g.read();
  CHECK(g.at(9, 9) == kGreen && g.at(4, 4) == kGreen && g.at(13, 13) == kGreen);
  // One-sided border (an underline).
  g.reset();
  r.stroke_rounded_rect({4, 4, 30, 20}, CornerRadii{}, {0, 0, 2, 0}, kRed);
  g.read();
  CHECK(g.at(10, 22) == kRed && g.at(10, 23) == kRed && g.at(10, 21) == kBlack && g.at(4, 10) == kBlack);
  // Clipped.
  g.reset();
  r.set_clip({20, 0, 10, 64});
  r.fill_rounded_rect({8, 8, 40, 30}, CornerRadii::uniform(10), kWhite);
  g.read();
  CHECK(g.at(19, 23) == kBlack && g.at(20, 23) == kWhite && g.at(29, 23) == kWhite && g.at(30, 23) == kBlack);
  r.reset_clip();
}

static void test_mask(Rig& g) {
  IRenderer2D& r = *g.r;
  g.reset();
  const uint8_t cov[16] = {0, 64, 128, 255, 255, 255, 255, 255, 10, 0, 0, 255, 0, 0, 0, 0};
  MaskView m{ConstByteView(cov, 16), 4, 4, 4};
  r.draw_mask(m, {0, 0, 4, 4}, 10, 10, kRed);
  g.read();
  CHECK(g.at(10, 10) == kBlack);                            // coverage 0 leaves the pixel
  CHECK(chan_near(g.at(11, 10), 0xFF400000, 1));            // 64/255 of red
  CHECK(chan_near(g.at(12, 10), 0xFF800000, 1));
  CHECK(g.at(13, 10) == kRed && g.at(10, 11) == kRed && g.at(13, 12) == kRed && g.at(11, 13) == kBlack);
  // A translucent color scales the coverage.
  g.reset();
  r.draw_mask(m, {0, 0, 4, 4}, 20, 20, 0x80FFFFFF);
  g.read();
  CHECK(chan_near(g.at(23, 20), 0xFF808080, 1) && chan_near(g.at(22, 20), 0xFF404040, 2));
  // Sub-rectangle and clipping on every side (nothing drawn off the canvas).
  g.reset();
  r.draw_mask(m, {1, 0, 3, 2}, 30, 30, kGreen);
  r.draw_mask(m, {0, 0, 4, 4}, -2, -2, kGreen);
  r.draw_mask(m, {0, 0, 4, 4}, 62, 62, kGreen);
  r.draw_mask(m, {2, 2, 100, 100}, 40, 40, kGreen);
  g.read();
  CHECK(chan_near(g.at(30, 30), 0xFF004000, 1) && g.at(32, 30) == kGreen && g.at(30, 31) == kGreen && g.at(33, 30) == kBlack && g.at(30, 32) == kBlack);
  CHECK(g.at(1, 0) == kGreen && g.at(0, 0) == kBlack && g.at(1, 1) == kBlack && g.at(63, 63) == kGreen && g.at(62, 62) == kBlack);
  // Invalid masks draw nothing.
  g.reset();
  MaskView bad{ConstByteView(cov, 16), 4, 8, 4};  // claims 8 rows, has 4
  r.draw_mask(bad, {0, 0, 4, 8}, 5, 5, kWhite);
  MaskView bad2{ConstByteView(cov, 16), 4, 4, 2};  // pitch below width
  r.draw_mask(bad2, {0, 0, 4, 4}, 5, 5, kWhite);
  g.read();
  CHECK(g.at(5, 5) == kBlack && g.at(8, 8) == kBlack);
  // Replace mode thresholds.
  g.reset();
  r.set_blend(BlendMode::Replace);
  r.draw_mask(m, {0, 0, 4, 4}, 10, 10, kRed);
  g.read();
  CHECK(g.at(11, 10) == kBlack && g.at(12, 10) == kRed);
  r.set_blend(BlendMode::Alpha);
}

static void test_scaled(Rig& g, TestAllocator& a) {
  IRenderer2D& r = *g.r;
  Surface img = make_surface(a, 2, 2);
  img.row(0)[0] = kRed; img.row(0)[1] = kGreen; img.row(1)[0] = kBlue; img.row(1)[1] = kWhite;
  g.reset();
  r.draw_image_scaled(img, {0, 0, 2, 2}, {10, 10, 4, 4}, ImageFilter::Nearest);
  g.read();
  CHECK(g.at(10, 10) == kRed && g.at(11, 11) == kRed && g.at(12, 10) == kGreen && g.at(13, 11) == kGreen);
  CHECK(g.at(10, 12) == kBlue && g.at(11, 13) == kBlue && g.at(12, 12) == kWhite && g.at(13, 13) == kWhite);
  CHECK(g.at(9, 10) == kBlack && g.at(14, 10) == kBlack && g.at(10, 14) == kBlack);
  // Bilinear: corners keep their color, in between blends.
  g.reset();
  r.draw_image_scaled(img, {0, 0, 2, 2}, {10, 10, 4, 4}, ImageFilter::Bilinear);
  g.read();
  CHECK(g.at(10, 10) == kRed && g.at(13, 10) == kGreen && g.at(10, 13) == kBlue && g.at(13, 13) == kWhite);
  CHECK(chan_near(g.at(11, 10), 0xFFBF4000, 2) && chan_near(g.at(12, 10), 0xFF40BF00, 2));  // 3/4 red + 1/4 green and back
  CHECK(chan_near(g.at(10, 11), 0xFFBF0040, 2));
  // Same size is the identity, even when bilinear.
  g.reset();
  r.draw_image_scaled(img, {0, 0, 2, 2}, {20, 20, 2, 2}, ImageFilter::Bilinear);
  g.read();
  CHECK(g.at(20, 20) == kRed && g.at(21, 20) == kGreen && g.at(20, 21) == kBlue && g.at(21, 21) == kWhite);
  // A source rectangle selects one texel; the filter never reaches outside it.
  g.reset();
  r.draw_image_scaled(img, {1, 0, 1, 1}, {30, 30, 6, 6}, ImageFilter::Bilinear);
  g.read();
  CHECK(g.at(30, 30) == kGreen && g.at(35, 35) == kGreen && g.at(33, 32) == kGreen);
  // Downscale picks an in-range texel; a partly transparent texel blends.
  img.row(0)[0] = 0x80FF0000;
  g.reset();
  r.draw_image_scaled(img, {0, 0, 2, 2}, {40, 40, 2, 2}, ImageFilter::Nearest);
  g.read();
  CHECK(chan_near(g.at(40, 40), 0xFF800000, 1));
  // Degenerate rectangles and clipping are harmless.
  g.reset();
  r.draw_image_scaled(img, {0, 0, 0, 0}, {10, 10, 4, 4}, ImageFilter::Bilinear);
  r.draw_image_scaled(img, {-5, -5, 20, 20}, {-100, -100, 10, 10}, ImageFilter::Nearest);
  r.draw_image_scaled(img, {0, 0, 2, 2}, {10, 10, 0, 0}, ImageFilter::Bilinear);
  r.draw_image_scaled(img, {0, 0, 2, 2}, {60, 60, 100, 100}, ImageFilter::Bilinear);
  g.read();
  CHECK(g.at(10, 10) == kBlack);
  release_view(a, img.pixels);
}

static void test_gradients(Rig& g) {
  IRenderer2D& r = *g.r;
  const GradientStop rb[2] = {{0, kRed}, {1, kBlue}};
  g.reset();
  r.fill_linear_gradient({0, 0, 64, 8}, {16, 0}, {48, 0}, ArrayView<const GradientStop>(rb));
  g.read();
  CHECK(g.at(0, 0) == kRed && g.at(15, 3) == kRed && g.at(63, 7) == kBlue && g.at(48, 0) == kBlue);  // flat beyond the line's ends
  CHECK(chan_near(g.at(32, 4), 0xFF800080, 4));
  CHECK(red_of(g.at(20, 0)) > red_of(g.at(30, 0)) && red_of(g.at(30, 0)) > red_of(g.at(40, 0)));
  CHECK(g.at(10, 8) == kBlack && g.at(10, 9) == kBlack);                                             // clipped to the rectangle
  // End colors with the line spanning the rectangle: the first and last pixel are within a step of the stops.
  g.reset();
  r.fill_linear_gradient({0, 0, 64, 4}, {0, 0}, {64, 0}, ArrayView<const GradientStop>(rb));
  g.read();
  CHECK(chan_near(g.at(0, 0), kRed, 5) && chan_near(g.at(63, 3), kBlue, 5) && chan_near(g.at(32, 2), 0xFF800080, 5));
  // Vertical, three stops, hard stop.
  const GradientStop three[3] = {{0, kRed}, {0.5f, kGreen}, {1, kBlue}};
  g.reset();
  r.fill_linear_gradient({0, 0, 8, 64}, {0, 0}, {0, 64}, ArrayView<const GradientStop>(three));
  g.read();
  CHECK(chan_near(g.at(3, 0), kRed, 8) && chan_near(g.at(3, 32), kGreen, 8) && chan_near(g.at(3, 63), kBlue, 8) && chan_near(g.at(3, 16), 0xFF808000, 8));
  const GradientStop hard[4] = {{0, kRed}, {0.5f, kRed}, {0.5f, kBlue}, {1, kBlue}};
  g.reset();
  r.fill_linear_gradient({0, 0, 64, 4}, {0, 0}, {64, 0}, ArrayView<const GradientStop>(hard));
  g.read();
  CHECK(g.at(0, 0) == kRed && g.at(31, 1) == kRed && g.at(32, 1) == kBlue && g.at(63, 3) == kBlue);
  // Diagonal: monotonic along the line, constant across it.
  g.reset();
  r.fill_linear_gradient({0, 0, 64, 64}, {0, 0}, {64, 64}, ArrayView<const GradientStop>(rb));
  g.read();
  CHECK(red_of(g.at(5, 5)) > red_of(g.at(30, 30)) && red_of(g.at(30, 30)) > red_of(g.at(58, 58)));
  CHECK(chan_near(g.at(10, 40), g.at(40, 10), 6) && chan_near(g.at(31, 31), 0xFF800080, 6));
  // Gradient with alpha (transparent to opaque white) over black, a single stop, no stops.
  const GradientStop fade[2] = {{0, 0x00FFFFFF}, {1, kWhite}};
  g.reset();
  r.fill_linear_gradient({0, 0, 64, 4}, {0, 0}, {64, 0}, ArrayView<const GradientStop>(fade));
  g.read();
  CHECK(red_of(g.at(0, 0)) < 8 && red_of(g.at(63, 0)) > 247 && chan_near(g.at(32, 0), 0xFF808080, 6));
  const GradientStop one[1] = {{0.3f, kGreen}};
  g.reset();
  r.fill_linear_gradient({0, 0, 8, 8}, {0, 0}, {8, 0}, ArrayView<const GradientStop>(one));
  r.fill_linear_gradient({20, 0, 8, 8}, {0, 0}, {8, 0}, ArrayView<const GradientStop>());
  r.fill_linear_gradient({30, 0, 8, 8}, {5, 5}, {5, 5}, ArrayView<const GradientStop>(rb));  // zero-length line: last color
  g.read();
  CHECK(g.at(3, 3) == kGreen && g.at(22, 3) == kBlack && g.at(33, 3) == kBlue);

  // Radial.
  const GradientStop wb[2] = {{0, kWhite}, {1, kBlack}};
  const GradientStop rg[2] = {{0, kRed}, {1, kGreen}};
  g.reset();
  r.fill_radial_gradient({0, 0, 64, 64}, {32, 32}, {16, 16}, ArrayView<const GradientStop>(rg));
  g.read();
  CHECK(chan_near(g.at(32, 32), kRed, 14) && g.at(63, 32) == kGreen && g.at(0, 0) == kGreen && g.at(32, 55) == kGreen);
  CHECK(chan_near(g.at(40, 32), 0xFF808000, 12) && chan_near(g.at(32, 40), 0xFF808000, 12) && chan_near(g.at(23, 32), g.at(40, 32), 4));
  // Elliptical: wide in x.
  g.reset();
  r.fill_radial_gradient({0, 0, 64, 64}, {32, 32}, {30, 10}, ArrayView<const GradientStop>(rg));
  g.read();
  CHECK(chan_near(g.at(47, 32), 0xFF808000, 14) && g.at(32, 50) == kGreen && g.at(32, 42) == kGreen && chan_near(g.at(32, 37), 0xFF808000, 14));
  // Clipped to a rectangle that does not contain the center; off-center gradient.
  g.reset();
  r.fill_radial_gradient({40, 40, 10, 10}, {10, 10}, {80, 80}, ArrayView<const GradientStop>(wb));
  g.read();
  CHECK(g.at(39, 45) == kBlack && g.at(50, 45) == kBlack && g.at(45, 39) == kBlack);
  CHECK(red_of(g.at(40, 40)) > red_of(g.at(49, 49)) && red_of(g.at(40, 40)) > 0 && red_of(g.at(49, 49)) > 0);
  // Bad radii: last color.
  g.reset();
  r.fill_radial_gradient({0, 0, 8, 8}, {4, 4}, {0, 5}, ArrayView<const GradientStop>(rg));
  g.read();
  CHECK(g.at(3, 3) == kGreen);
}

static void test_clip_stack(Rig& g, TestAllocator& a) {
  IRenderer2D& r = *g.r;
  g.reset();
  CHECK(r.push_clip_rect({10, 10, 30, 30}));
  r.fill_rect({0, 0, 64, 64}, kWhite);
  CHECK(r.push_clip_rect({20, 20, 30, 30}));
  r.fill_rect({0, 0, 64, 64}, kRed);
  r.pop_clip();
  r.fill_rect({12, 12, 4, 4}, kGreen);  // back to the first clip
  r.pop_clip();
  r.fill_rect({60, 60, 4, 4}, kBlue);   // no clip again
  g.read();
  CHECK(g.at(10, 10) == kWhite && g.at(19, 19) == kWhite && g.at(9, 10) == kBlack && g.at(10, 9) == kBlack);
  CHECK(g.at(20, 20) == kRed && g.at(39, 39) == kRed && g.at(40, 39) == kBlack && g.at(39, 40) == kBlack);
  CHECK(g.at(12, 12) == kGreen && g.at(63, 63) == kBlue);
  // set_clip and the stack are independent: the effective clip is their intersection.
  g.reset();
  r.set_clip({0, 0, 32, 64});
  r.push_clip_rect({16, 0, 32, 64});
  r.fill_rect({0, 0, 64, 64}, kWhite);
  g.read();
  CHECK(g.at(15, 5) == kBlack && g.at(16, 5) == kWhite && g.at(31, 5) == kWhite && g.at(32, 5) == kBlack);
  r.pop_clip();
  r.reset_clip();
  // Draw calls other than rectangles obey the stack too.
  g.reset();
  r.push_clip_rect({0, 0, 32, 32});
  r.fill_circle(32, 32, 20, kWhite);
  r.draw_line(0, 63, 63, 0, kGreen);
  r.draw_text(28, 2, "A", kRed);
  g.read();
  CHECK(g.at(40, 40) == kBlack && g.at(20, 20) == kWhite && g.at(40, 20) == kBlack);
  r.pop_clip();
  // An empty clip draws nothing; popping restores drawing.
  g.reset();
  r.push_clip_rect({5, 5, 0, 0});
  r.fill_rect({0, 0, 64, 64}, kWhite);
  r.pop_clip();
  r.fill_rect({1, 1, 2, 2}, kGreen);
  g.read();
  CHECK(g.at(10, 10) == kBlack && g.at(1, 1) == kGreen);
  // Too many pushes still balance (the overflow clips everything while it lasts).
  g.reset();
  for (uint32_t i = 0; i < kMaxClipDepth + 4; ++i) r.push_clip_rect({0, 0, 64, 64});
  r.fill_rect({0, 0, 64, 64}, kWhite);
  g.read();
  CHECK(g.at(10, 10) == kBlack);
  for (uint32_t i = 0; i < kMaxClipDepth + 4; ++i) r.pop_clip();
  r.pop_clip();  // an extra pop is harmless
  r.fill_rect({0, 0, 4, 4}, kWhite);
  g.read();
  CHECK(g.at(1, 1) == kWhite);

  // Rounded clip. The device renderer can only scissor to the bounds.
  g.reset();
  const bool exact = r.push_clip_rounded_rect({8, 8, 40, 40}, CornerRadii::uniform(12));
  CHECK(exact == g.exact_clips);
  r.fill_rect({0, 0, 64, 64}, kWhite);
  r.pop_clip();
  g.read();
  CHECK(g.at(28, 28) == kWhite && g.at(28, 8) == kWhite && g.at(8, 28) == kWhite && g.at(47, 28) == kWhite && g.at(7, 28) == kBlack && g.at(48, 28) == kBlack);
  if (g.exact_clips) {
    CHECK(g.at(8, 8) == kBlack && g.at(47, 47) == kBlack && g.at(47, 8) == kBlack && g.at(8, 47) == kBlack);
    const uint32_t partial = red_of(g.at(11, 11));
    CHECK(partial > 20 && partial < 235);
  } else {
    CHECK(g.at(8, 8) == kWhite);  // approximated by the bounding rectangle
  }
  // Rounded clip nested in a rounded clip multiplies coverage; a square-cornered "rounded" clip is a rect clip.
  if (g.exact_clips) {
    g.reset();
    r.push_clip_rounded_rect({0, 0, 40, 40}, CornerRadii::uniform(20));
    r.push_clip_rounded_rect({20, 20, 40, 40}, CornerRadii::uniform(20));
    r.fill_rect({0, 0, 64, 64}, kWhite);
    r.pop_clip();
    r.pop_clip();
    g.read();
    CHECK(g.at(30, 30) == kWhite && g.at(10, 10) == kBlack && g.at(50, 50) == kBlack && g.at(20, 20) == kBlack);
    CHECK(g.at(39, 39) == kBlack || red_of(g.at(39, 39)) < 128);
  }
  g.reset();
  CHECK(r.push_clip_rounded_rect({8, 8, 20, 20}, CornerRadii{}));
  r.fill_rect({0, 0, 64, 64}, kWhite);
  r.pop_clip();
  g.read();
  CHECK(g.at(8, 8) == kWhite && g.at(7, 8) == kBlack && g.at(27, 27) == kWhite && g.at(28, 27) == kBlack);

  // Mask clip: the left half of an 8x8 mask is covered, the right quarter half covered.
  uint8_t m8[64];
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 8; ++x) m8[y * 8 + x] = x < 4 ? 255 : (x < 6 ? 128 : 0);
  MaskView mv{ConstByteView(m8, 64), 8, 8, 8};
  g.reset();
  const bool mexact = r.push_clip_mask(mv, 10, 10);
  CHECK(mexact == g.exact_clips);
  r.fill_rect({0, 0, 64, 64}, kWhite);
  r.pop_clip();
  g.read();
  CHECK(g.at(9, 10) == kBlack && g.at(18, 10) == kBlack && g.at(10, 9) == kBlack && g.at(10, 18) == kBlack);
  CHECK(g.at(10, 10) == kWhite && g.at(13, 17) == kWhite);
  if (g.exact_clips) {
    CHECK(chan_near(g.at(14, 12), 0xFF808080, 1) && g.at(16, 12) == kBlack);
  }
  // A mask clip works on top of a rounded clip and with drawing that is itself translucent.
  if (g.exact_clips) {
    g.reset();
    r.push_clip_mask(mv, 10, 10);
    r.fill_rect({0, 0, 64, 64}, 0x80FFFFFF);
    r.pop_clip();
    g.read();
    CHECK(chan_near(g.at(10, 10), 0xFF808080, 1) && chan_near(g.at(14, 10), 0xFF404040, 1));
  }
  // An invalid mask clips everything.
  g.reset();
  MaskView bad{ConstByteView(m8, 8), 8, 8, 8};
  CHECK(!r.push_clip_mask(bad, 0, 0));
  r.fill_rect({0, 0, 64, 64}, kWhite);
  r.pop_clip();
  g.read();
  CHECK(g.at(3, 3) == kBlack);
  (void)a;
}

static void test_layers(Rig& g) {
  IRenderer2D& r = *g.r;
  g.reset();
  CHECK(r.push_layer({10, 10, 30, 30}, 128) == g.exact_clips);
  r.fill_rect({0, 0, 64, 64}, kRed);  // clipped to the layer bounds
  r.pop_layer();
  g.read();
  CHECK(chan_near(g.at(10, 10), 0xFF800000, 1) && chan_near(g.at(39, 39), 0xFF800000, 1));
  if (g.exact_clips) CHECK(g.at(9, 10) == kBlack && g.at(40, 40) == kBlack);
  if (!g.exact_clips) {
    // (the device renderer cannot clip to the bounds; it only applies the opacity)
  }
  // Group opacity: overlapping shapes in a layer composite as one.
  g.reset();
  r.push_layer({0, 0, 64, 64}, 128);
  r.fill_rect({10, 10, 20, 20}, kWhite);
  r.fill_rect({20, 20, 20, 20}, kWhite);
  r.pop_layer();
  g.read();
  CHECK(chan_near(g.at(12, 12), 0xFF808080, 1));
  if (g.exact_clips) CHECK(chan_near(g.at(25, 25), 0xFF808080, 1));  // not 192
  else CHECK(red_of(g.at(25, 25)) >= 128);
  // Opacity 255 is exact, opacity 0 shows nothing.
  g.reset();
  CHECK(r.push_layer({0, 0, 64, 64}, 255));
  r.fill_rect({4, 4, 8, 8}, kGreen);
  r.pop_layer();
  r.push_layer({0, 0, 64, 64}, 0);
  r.fill_rect({20, 4, 8, 8}, kGreen);
  r.pop_layer();
  g.read();
  CHECK(g.at(5, 5) == kGreen && g.at(21, 5) == kBlack);
  // Layers nest.
  g.reset();
  r.push_layer({0, 0, 64, 64}, 128);
  r.push_layer({0, 0, 64, 64}, 128);
  r.fill_rect({4, 4, 8, 8}, kWhite);
  r.pop_layer();
  r.fill_rect({20, 4, 8, 8}, kWhite);
  r.pop_layer();
  g.read();
  CHECK(chan_near(g.at(5, 5), 0xFF404040, 2) && chan_near(g.at(21, 5), 0xFF808080, 1));
  // A translucent layer over a drawn background keeps the background; shapes with partial alpha inside work.
  g.reset();
  r.fill_rect({0, 0, 64, 64}, kBlue);
  r.push_layer({8, 8, 16, 16}, 255);
  r.fill_rect({8, 8, 16, 16}, 0x80FF0000);
  r.pop_layer();
  g.read();
  CHECK(chan_near(g.at(10, 10), blend_over(0x80FF0000, kBlue), 1) && g.at(30, 30) == kBlue);
  // The clip stack is private to a layer; clips open inside it are dropped by pop_layer.
  g.reset();
  r.push_clip_rect({0, 0, 32, 64});
  r.push_layer({0, 0, 64, 64}, 255);
  r.push_clip_rect({0, 0, 8, 8});
  r.fill_rect({0, 0, 64, 64}, kWhite);
  r.pop_layer();  // leaves the layer's pushed clip open on purpose
  r.fill_rect({0, 0, 64, 4}, kGreen);  // the outer clip (x < 32) is back in force
  r.pop_clip();
  g.read();
  CHECK(g.at(40, 2) == kBlack && g.at(20, 2) == kGreen);
  if (g.exact_clips) CHECK(g.at(4, 6) == kWhite && g.at(10, 6) == kBlack);
  // Layer bounds outside the canvas, and an unbalanced pop, are harmless.
  g.reset();
  r.push_layer({-100, -100, 50, 50}, 128);
  r.fill_rect({0, 0, 64, 64}, kWhite);
  r.pop_layer();
  r.pop_layer();
  r.fill_rect({0, 0, 4, 4}, kWhite);
  g.read();
  CHECK(g.at(1, 1) == kWhite);
  if (g.exact_clips) CHECK(g.at(30, 30) == kBlack);
  // More layers than the stack holds still balance.
  g.reset();
  for (uint32_t i = 0; i < kMaxLayerDepth + 3; ++i) r.push_layer({0, 0, 64, 64}, 255);
  for (uint32_t i = 0; i < kMaxLayerDepth + 3; ++i) r.pop_layer();
  r.fill_rect({0, 0, 4, 4}, kWhite);
  g.read();
  CHECK(g.at(1, 1) == kWhite);
}

static void test_copy_rect(Rig& g) {
  IRenderer2D& r = *g.r;
  auto pattern = [&]() {
    g.reset();
    for (uint32_t i = 0; i < 8; ++i) r.fill_rect({static_cast<int32_t>(i * 8), 0, 8, 64}, rgb(static_cast<uint8_t>(i * 30 + 10), 80, 200));
    for (uint32_t j = 0; j < 8; ++j) r.fill_rect({0, static_cast<int32_t>(j * 8), 64, 2}, rgb(250, static_cast<uint8_t>(j * 30), 0));
  };
  struct Case { Rect2D src; int32_t dx, dy; };
  const Case cases[] = {{{0, 0, 32, 32}, 16, 8}, {{16, 16, 40, 40}, 0, 0}, {{10, 20, 30, 30}, 12, -5}, {{0, 8, 64, 56}, 0, 0},
                        {{0, 0, 64, 56}, 0, 8}, {{0, 8, 64, 56}, 0, 0}, {{0, 0, 40, 40}, 30, 30}, {{-10, -10, 30, 30}, 5, 5}, {{50, 50, 100, 100}, -40, -40}};
  for (const Case& c : cases) {
    pattern();
    g.read();
    uint32_t model[kW * kH];
    for (uint32_t i = 0; i < kW * kH; ++i) model[i] = g.frame[i];
    CHECK(r.copy_rect(c.src, c.dx, c.dy));
    // Model: dest pixel (x, y) = old source pixel (x - dx + src.x, ...) where both lie in the canvas.
    uint32_t expect[kW * kH];
    for (uint32_t i = 0; i < kW * kH; ++i) expect[i] = model[i];
    for (int32_t y = 0; y < static_cast<int32_t>(kH); ++y)
      for (int32_t x = 0; x < static_cast<int32_t>(kW); ++x) {
        const int32_t sx = x - c.dx + c.src.x, sy = y - c.dy + c.src.y;
        if (sx < c.src.x || sy < c.src.y || sx >= c.src.x + static_cast<int32_t>(c.src.width) || sy >= c.src.y + static_cast<int32_t>(c.src.height)) continue;
        if (sx < 0 || sy < 0 || sx >= static_cast<int32_t>(kW) || sy >= static_cast<int32_t>(kH)) continue;
        expect[y * kW + x] = model[sy * kW + sx];
      }
    g.read();
    bool ok = true;
    for (uint32_t i = 0; i < kW * kH; ++i) ok = ok && g.frame[i] == expect[i];
    if (!ok) printf("copy_rect case %d (%d,%d)\n", int(&c - cases), c.dx, c.dy);
    CHECK(ok);
  }
  // Limited by the clip rectangle.
  pattern();
  g.read();
  const uint32_t before_px = g.at(40, 40), src_px = g.at(1, 1);
  r.set_clip({0, 0, 32, 64});
  CHECK(r.copy_rect({0, 0, 16, 16}, 36, 36));  // lands outside the clip
  CHECK(r.copy_rect({0, 0, 16, 16}, 20, 20));  // lands inside
  r.reset_clip();
  g.read();
  CHECK(g.at(40, 40) == before_px && g.at(21, 21) == src_px);
}

// Hostile arguments: nothing may crash, read out of bounds or overflow (the sanitizers watch).
static void test_extreme_arguments(Rig& g, TestAllocator& a) {
  IRenderer2D& r = *g.r;
  g.reset();
  const int32_t big = 0x7FFFFFF0, neg = -0x7FFFFFF0;
  const uint32_t huge = 0xFFFFFFF0u;
  const CornerRadii odd = {-5, 1e30f, 3, 0, 1e-9f, 7, 0, 0.0001f};
  const GradientStop stops[3] = {{-1, kRed}, {2, kBlue}, {0.5f, kGreen}};
  uint8_t cov[16] = {};
  const MaskView mv{ConstByteView(cov, 16), 4, 4, 4};
  Surface img = make_surface(a, 3, 3);
  const Rect2D rects[] = {{0, 0, 0, 0}, {big, big, huge, huge}, {neg, neg, huge, huge}, {neg, 5, 100, 3}, {5, big, 3, 100}, {-3, -3, 9, 9}, {60, 60, 100, 100}};
  for (const Rect2D& rc : rects) {
    r.fill_rounded_rect(rc, odd, 0x80FFFFFF);
    r.stroke_rounded_rect(rc, odd, {huge, 0, huge, 3}, kWhite);
    r.stroke_rounded_rect(rc, CornerRadii::uniform(1e9f), EdgeWidths::uniform(2), kWhite);
    r.draw_mask(mv, rc, rc.x, rc.y, kWhite);
    r.draw_mask(mv, {0, 0, 4, 4}, rc.x, rc.y, kWhite);
    r.draw_image_scaled(img, {0, 0, 3, 3}, rc, ImageFilter::Bilinear);
    r.draw_image_scaled(img, {-1e30f, 1e30f, 1e30f, 5}, rc, ImageFilter::Nearest);
    r.draw_image_scaled(img, {0.5f, 0.5f, 1.25f, 1.25f}, rc, ImageFilter::Bilinear);
    r.fill_linear_gradient(rc, {-1e30f, 0}, {1e30f, 1e-30f}, ArrayView<const GradientStop>(stops));
    r.fill_radial_gradient(rc, {1e30f, -1e30f}, {1e-20f, 1e20f}, ArrayView<const GradientStop>(stops));
    r.fill_radial_gradient(rc, {5, 5}, {1e9f, 1e9f}, ArrayView<const GradientStop>(stops));
    r.push_clip_rect(rc);
    r.push_clip_rounded_rect(rc, odd);
    r.push_clip_mask(mv, rc.x, rc.y);
    r.fill_rect({0, 0, 64, 64}, kWhite);
    r.pop_clip();
    r.pop_clip();
    r.pop_clip();
    r.push_layer(rc, 77);
    r.fill_rect({0, 0, 64, 64}, kWhite);
    r.pop_layer();
    r.copy_rect(rc, rc.x, rc.y);
    r.copy_rect({0, 0, 20, 20}, big, neg);
    r.copy_rect({0, 0, 20, 20}, neg, big);
  }
  GradientStop many[kMaxGradientStops + 10];
  for (uint32_t i = 0; i < kMaxGradientStops + 10; ++i) many[i] = {static_cast<float>(i) / 41.0f, i & 1 ? kRed : kBlue};
  r.fill_linear_gradient({0, 0, 64, 8}, {0, 0}, {64, 0}, ArrayView<const GradientStop>(many));
  r.fill_radial_gradient({0, 8, 64, 8}, {32, 12}, {32, 4}, ArrayView<const GradientStop>(many));
  g.read();  // still alive
  release_view(a, img.pixels);
  CHECK(true);
}

static void test_primitives() {
  TestAllocator a;
  {
    Surface cpu_surface = make_surface(a, kW, kH);
    SoftwareRenderer2D cpu(cpu_surface, nullptr, &a);
    Rig g;
    g.r = &cpu;
    g.soft = &cpu_surface;
    g.frame = allocate_view<uint32_t>(a, kW * kH);
    g.exact_clips = true;
    test_rounded(g);
    test_mask(g);
    test_scaled(g, a);
    test_gradients(g);
    test_clip_stack(g, a);
    test_layers(g);
    test_copy_rect(g);
    test_extreme_arguments(g, a);
    // Without an allocator rounded clips and layers degrade instead of failing.
    Surface plain_surface = make_surface(a, kW, kH);
    SoftwareRenderer2D plain(plain_surface);
    plain.clear(kBlack);
    CHECK(!plain.push_clip_rounded_rect({8, 8, 40, 40}, CornerRadii::uniform(12)));
    plain.fill_rect({0, 0, 64, 64}, kWhite);
    plain.pop_clip();
    CHECK(!plain.push_layer({0, 0, 64, 64}, 128));
    plain.fill_rect({50, 50, 4, 4}, kRed);
    plain.pop_layer();
    CHECK(px(plain_surface, 8, 8) == kWhite && px(plain_surface, 7, 8) == kBlack && px(plain_surface, 51, 51) == kRed);
    release_view(a, plain_surface.pixels);

    // The same drawing in the other pixel order.
    Surface rgba = make_surface(a, kW, kH, Format::R8G8B8A8Unorm);
    SoftwareRenderer2D rgba_r(rgba, nullptr, &a);
    Rig g2;
    g2.r = &rgba_r;
    g2.soft = &rgba;
    g2.frame = g.frame;
    test_rounded(g2);
    test_gradients(g2);
    test_layers(g2);
    release_view(a, rgba.pixels);
    release_view(a, g.frame);
    release_view(a, cpu_surface.pixels);
  }
  {
    SoftwareDevice device(a, nullptr);
    DeviceRenderer2D d(device, a, {kW, kH});
    CHECK(d.valid());
    Rig g;
    g.r = &d;
    g.dev = &d;
    g.frame = allocate_view<uint32_t>(a, kW * kH);
    g.exact_clips = false;
    test_rounded(g);
    test_mask(g);
    test_scaled(g, a);
    test_gradients(g);
    test_clip_stack(g, a);
    test_layers(g);
    test_copy_rect(g);
    test_extreme_arguments(g, a);
    release_view(a, g.frame);
  }
  CHECK(a.live == 0);
}

// What the device renderer draws for rounded rectangles, masks, scaled images and clips matches the CPU
// renderer pixel for pixel; gradients to within the GPU's interpolation rounding.
static void test_renderers_agree() {
  TestAllocator a;
  SoftwareDevice device(a, nullptr);
  DeviceRenderer2D d(device, a, {kW, kH});
  Surface cs = make_surface(a, kW, kH);
  SoftwareRenderer2D cpu(cs, nullptr, &a);
  ArrayView<uint32_t> frame = allocate_view<uint32_t>(a, kW * kH);
  Surface img = make_surface(a, 3, 2);
  for (uint32_t i = 0; i < 6; ++i) img.pixels[i] = rgb(static_cast<uint8_t>(i * 40), static_cast<uint8_t>(255 - i * 30), static_cast<uint8_t>(i * 7));
  uint8_t cov[36];
  for (uint32_t i = 0; i < 36; ++i) cov[i] = static_cast<uint8_t>(i * 7);
  const MaskView mv{ConstByteView(cov, 36), 6, 6, 6};
  const GradientStop stops[3] = {{0, kRed}, {0.4f, 0xFF00FF80}, {1, 0xFF2000C0}};
  for (IRenderer2D* r : {static_cast<IRenderer2D*>(&d), static_cast<IRenderer2D*>(&cpu)}) {
    r->reset_clip();
    r->clear(0xFF101820);
    r->fill_rounded_rect({3, 3, 50, 40}, {12, 5, 20, 20, 3, 9, 0, 0}, 0xC0FFA000);
    r->stroke_rounded_rect({6, 6, 40, 30}, CornerRadii::uniform(9), {1, 4, 2, 6}, 0xFF3377FF);
    r->draw_mask(mv, {0, 0, 6, 6}, 50, 50, 0xB0FFFFFF);
    r->draw_image_scaled(img, {0, 0, 3, 2}, {44, 4, 15, 10}, ImageFilter::Bilinear);
    r->draw_image_scaled(img, {0.5f, 0, 2, 2}, {44, 20, 15, 10}, ImageFilter::Nearest);
    r->set_clip({10, 40, 30, 20});
    r->fill_rounded_rect({0, 30, 64, 34}, CornerRadii::uniform(30), 0x8000FF00);
    r->reset_clip();
    r->push_clip_rect({20, 44, 20, 12});
    r->fill_rounded_rect({0, 40, 64, 24}, CornerRadii::uniform(5), 0xFFFF00FF);
    r->pop_clip();
  }
  CHECK(d.read_pixels(frame));
  uint32_t differing = 0;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) differing += frame[y * kW + x] != px(cs, x, y);
  CHECK(differing == 0);
  for (IRenderer2D* r : {static_cast<IRenderer2D*>(&d), static_cast<IRenderer2D*>(&cpu)}) {
    r->clear(0xFF000000);
    r->fill_linear_gradient({2, 2, 60, 28}, {10, 5}, {50, 25}, ArrayView<const GradientStop>(stops));
    r->fill_radial_gradient({2, 32, 60, 30}, {30, 45}, {26, 12}, ArrayView<const GradientStop>(stops));
  }
  CHECK(d.read_pixels(frame));
  uint32_t far = 0;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) far += !chan_near(frame[y * kW + x], px(cs, x, y), 6);
  CHECK(far < kW * kH / 100);
  release_view(a, img.pixels);
  release_view(a, frame);
  release_view(a, cs.pixels);
}

int main() {
  test_math();
  test_2d();
  test_3d();
  test_device_2d();
  test_blend_math();
  test_fast_paths();
  test_primitives();
  test_renderers_agree();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("graphics: all checks passed\n");
  return 0;
}
