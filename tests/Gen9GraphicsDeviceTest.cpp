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

// Host-side tests for the Gen9 graphics device: it runs scenes on the render engine of the simulated GPU (FakeIntelGpu.hpp, with the model of the 3D pipeline in
// FakeIntelRender.hpp standing in for the hardware) and compares the pictures with the software device's.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/IntelGen9/Gen9GraphicsDevice.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"

using namespace hardware;
using namespace hardware::intel_gt;
using namespace hardware::intel_gt::gen9_3d;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"

static float as_float(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }

#include "FakeIntelRender.hpp"

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); memset(p, 0, size); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

struct CapturingDisplay final : IGraphicsProcessingUnit {
  std::vector<uint32_t> pixels;
  uint32_t bw = 0, bh = 0;
  int presents = 0;
  const char* name() const override { return "capture"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 0; }
  bool mode_info(uint32_t, DisplayMode*) const override { return false; }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t) override { return false; }
  uint32_t width() const override { return bw; }
  uint32_t height() const override { return bh; }
  void clear(uint32_t) override {}
  void put_pixel(uint32_t, uint32_t, uint32_t) override {}
  void fill_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void blit(uint32_t, uint32_t, uint32_t w, uint32_t h, const uint32_t* p, uint32_t pitch) override {
    bw = w; bh = h; pixels.assign(w * h, 0);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) pixels[y * w + x] = p[y * pitch + x];
  }
  void present() override { ++presents; }
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
};

using namespace graphics;

constexpr uint32_t kSize = 64;

// A scene: the same commands run on a device, the picture read back.
struct Scene {
  IGraphicsDevice& device;
  IShaderModule* vs;
  IShaderModule* fs;
  explicit Scene(IGraphicsDevice& d) : device(d), vs(d.create_shader_module(colored_vertex_shader())), fs(d.create_shader_module(colored_fragment_shader())) {}
  ~Scene() { device.destroy_shader_module(vs); device.destroy_shader_module(fs); }
  IPipeline* pipeline(CullMode cull = CullMode::None, FrontFace front = FrontFace::CounterClockwise, bool blend = false, bool depth_test = false, bool depth_write = false) {
    GraphicsPipelineDesc pd;
    describe_colored_pipeline(&pd);
    pd.vertex_shader = vs;
    pd.fragment_shader = fs;
    pd.cull_mode = cull;
    pd.front_face = front;
    pd.alpha_blend = blend;
    pd.depth_test = depth_test;
    pd.depth_write = depth_write;
    return device.create_graphics_pipeline(pd);
  }
};

struct Draw {
  Draw(const ColoredVertex* v, uint32_t n) : vertices(v), count(n) {}
  const ColoredVertex* vertices;
  uint32_t count;
  Mat4 mvp = Mat4::identity();
  IPipeline* pipeline = nullptr;
  bool has_viewport = false;
  Viewport viewport;
  bool has_scissor = false;
  Rect2D scissor;
  const uint16_t* indices = nullptr;
  uint32_t index_count = 0;
};

static std::vector<IBuffer*> g_pending_vb;

static bool render(IGraphicsDevice& device, Scene& scene, IImage* image, Vec4 clear, std::vector<Draw> draws, bool do_clear = true, IImage* depth = nullptr) {
  (void)scene;
  ICommandBuffer* cb = device.create_command_buffer();
  if (!cb) return false;
  cb->begin();
  RenderPassBegin pass;
  pass.color = image;
  pass.clear_color = do_clear;
  pass.clear_color_value = clear;
  pass.depth = depth;
  pass.clear_depth = depth != nullptr;
  pass.clear_depth_value = 1.0f;
  cb->begin_render_pass(pass);
  for (const Draw& d : draws) {
    IBuffer* vb = device.create_buffer(d.count * sizeof(ColoredVertex) + 16, kBufferVertex);
    IBuffer* ib = d.indices ? device.create_buffer(d.index_count * 2 + 16, kBufferIndex) : nullptr;
    if (!vb || (d.indices && !ib)) { device.destroy_command_buffer(cb); return false; }
    ByteView m = vb->map();
    memcpy(m.data(), d.vertices, d.count * sizeof(ColoredVertex));
    vb->unmap();
    if (ib) {
      ByteView im = ib->map();
      memcpy(im.data(), d.indices, d.index_count * 2);
      ib->unmap();
    }
    cb->bind_pipeline(d.pipeline);
    cb->bind_vertex_buffer(vb);
    if (ib) cb->bind_index_buffer(ib, IndexType::Uint16);
    cb->push_constants(&d.mvp, sizeof d.mvp);
    if (d.has_viewport) cb->set_viewport(d.viewport);
    if (d.has_scissor) cb->set_scissor(d.scissor);
    if (ib) cb->draw_indexed(d.index_count);
    else cb->draw(d.count);
    // (The buffers are freed after the submit; the command buffer only names them.)
    g_pending_vb.push_back(vb);
    if (ib) g_pending_vb.push_back(ib);
  }
  cb->end_render_pass();
  const bool ok = cb->end() && device.submit(cb);
  device.destroy_command_buffer(cb);
  for (IBuffer* b : g_pending_vb) device.destroy_buffer(b);
  g_pending_vb.clear();
  return ok;
}

static std::vector<uint32_t> read(IImage* image) {
  std::vector<uint32_t> out(image->extent().width * image->extent().height);
  CHECK(image->read_pixels(ArrayView<uint32_t>(out.data(), out.size())));
  return out;
}

static uint32_t channel_distance(uint32_t a, uint32_t b) {
  uint32_t worst = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int da = static_cast<int>((a >> shift) & 0xFF), db = static_cast<int>((b >> shift) & 0xFF);
    const uint32_t d = static_cast<uint32_t>(da > db ? da - db : db - da);
    if (d > worst) worst = d;
  }
  return worst;
}

// How many pixels of two pictures differ by more than `tolerance` in some channel.
static uint32_t differing(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, uint32_t tolerance) {
  uint32_t n = 0;
  if (a.size() != b.size()) return 0xFFFFFFFFu;
  for (size_t i = 0; i < a.size(); ++i) n += channel_distance(a[i], b[i]) > tolerance;
  return n;
}

static ColoredVertex cv(float x, float y, float z, float r, float g, float b, float a = 1) { return {{x, y, z}, {r, g, b, a}}; }

// The scene on the Gen9 device and on the software device; returns both pictures.
struct Pair {
  TestAllocator alloc;
  Rig rig{Generation::Gen9, VcsDriver::Engine::Render};
  CapturingDisplay display;
  SoftwareDevice software{alloc, nullptr};
  VcsDriver driver{rig.mem};
  Gen9GraphicsDevice* gpu = nullptr;
  Pair() {
    rig.gpu.on_batch = execute;
    if (driver.initialize(rig.config())) gpu = new Gen9GraphicsDevice(alloc, driver, rig.mem, &display);
    if (gpu && !gpu->initialize()) { delete gpu; gpu = nullptr; }
  }
  ~Pair() { delete gpu; }
};

// Draws the scene `build` describes with `format` targets on both devices.
template <class Build>
static void compare_scene(Pair& p, Format format, Vec4 clear, Build build, uint32_t tolerance, uint32_t allowed_bad, const char* what, std::vector<uint32_t>* gpu_out = nullptr, bool with_depth = false) {
  Scene gs(*p.gpu), ss(p.software);
  IImage* gi = p.gpu->create_image({{kSize, kSize}, format, kImageColorAttachment | kImagePresent});
  IImage* si = p.software.create_image({{kSize, kSize}, format, kImageColorAttachment});
  IImage* gdepth = with_depth ? p.gpu->create_image({{kSize, kSize}, Format::D32Float, kImageDepthAttachment}) : nullptr;
  IImage* sdepth = with_depth ? p.software.create_image({{kSize, kSize}, Format::D32Float, kImageDepthAttachment}) : nullptr;
  CHECK(gi && si && (!with_depth || (gdepth && sdepth)));
  if (!gi || !si) return;
  std::vector<Draw> gd, sd;
  build(gs, gd);
  build(ss, sd);
  const int jobs_before = p.rig.gpu.jobs;
  CHECK(render(*p.gpu, gs, gi, clear, gd, true, gdepth));
  CHECK(render(p.software, ss, si, clear, sd, true, sdepth));
  CHECK(p.rig.gpu.jobs > jobs_before && p.rig.gpu.violations.empty());
  const std::vector<uint32_t> a = read(gi), b = read(si);
  const uint32_t bad = differing(a, b, tolerance);
  if (bad > allowed_bad) printf("  scene \"%s\": %u pixels differ\n", what, bad);
  CHECK(bad <= allowed_bad);
  if (gpu_out) *gpu_out = a;
  for (const Draw& d : gd) if (d.pipeline) p.gpu->destroy_pipeline(d.pipeline);
  for (const Draw& d : sd) if (d.pipeline) p.software.destroy_pipeline(d.pipeline);
  p.gpu->destroy_image(gi);
  p.software.destroy_image(si);
  if (gdepth) p.gpu->destroy_image(gdepth);
  if (sdepth) p.software.destroy_image(sdepth);
}

static void test_resources() {
  Pair p;
  CHECK(p.gpu && p.gpu->ready() && p.gpu->info().accelerated && p.gpu->info().kind == DeviceKind::Gpu);
  if (!p.gpu) return;
  // The engine has one lease.
  Gen9GraphicsDevice second(p.alloc, p.driver, p.rig.mem, nullptr);
  CHECK(!second.initialize() && !second.ready());
  // Images: colour only, sizes within reach.
  IImage* a = p.gpu->create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IImage* b = p.gpu->create_image({{33, 17}, Format::R8G8B8A8Unorm, kImageColorAttachment});
  CHECK(a && b && a->extent().width == kSize && b->format() == Format::R8G8B8A8Unorm);
  // Depth images are D32Float with the depth usage and nothing else; they cannot be read back.
  IImage* depth_image = p.gpu->create_image({{kSize, kSize}, Format::D32Float, kImageDepthAttachment});
  CHECK(depth_image && depth_image->format() == Format::D32Float);
  std::vector<uint32_t> nothing(kSize * kSize);
  CHECK(depth_image && !depth_image->read_pixels(ArrayView<uint32_t>(nothing.data(), nothing.size())));
  if (depth_image) p.gpu->destroy_image(depth_image);
  CHECK(!p.gpu->create_image({{kSize, kSize}, Format::D32Float, kImageColorAttachment}));
  CHECK(!p.gpu->create_image({{kSize, kSize}, Format::D32Float, kImageDepthAttachment | kImagePresent}));
  CHECK(!p.gpu->create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImageDepthAttachment}));
  CHECK(!p.gpu->create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageDepthAttachment}));
  CHECK(!p.gpu->create_image({{0, 8}, Format::B8G8R8A8Unorm, kImageColorAttachment}));
  CHECK(!p.gpu->create_image({{4097, 8}, Format::B8G8R8A8Unorm, kImageColorAttachment}));
  CHECK(!p.gpu->create_image({{8, 8}, Format::Undefined, kImageColorAttachment}));
  std::vector<uint32_t> too_small(10);
  CHECK(!a->read_pixels(ArrayView<uint32_t>(too_small.data(), too_small.size())));
  std::vector<uint32_t> pixels = read(a);
  CHECK(pixels[0] == 0 && pixels[kSize * kSize - 1] == 0);   // a new image is transparent black
  p.gpu->destroy_image(a);
  p.gpu->destroy_image(b);
  IBuffer* buf = p.gpu->create_buffer(100, kBufferVertex);
  CHECK(buf && buf->size() == 100 && buf->map().size() == 100);
  p.gpu->destroy_buffer(buf);
  CHECK(!p.gpu->create_buffer(0, kBufferVertex));
  // Shader modules and pipelines: the built-in ones, nothing else.
  IShaderModule* vs = p.gpu->create_shader_module(colored_vertex_shader());
  IShaderModule* fs = p.gpu->create_shader_module(colored_fragment_shader());
  ShaderModuleDesc plain = colored_vertex_shader();
  plain.eu_program = EuProgram::None;
  CHECK(vs && fs && !p.gpu->create_shader_module(plain));
  GraphicsPipelineDesc pd;
  describe_colored_pipeline(&pd);
  pd.vertex_shader = vs;
  pd.fragment_shader = fs;
  IPipeline* ok = p.gpu->create_graphics_pipeline(pd);
  CHECK(ok);
  GraphicsPipelineDesc depth = pd;
  depth.depth_test = true;
  IPipeline* tested = p.gpu->create_graphics_pipeline(depth);
  depth = pd; depth.depth_write = true;
  IPipeline* written = p.gpu->create_graphics_pipeline(depth);
  CHECK(tested && written);
  p.gpu->destroy_pipeline(tested);
  p.gpu->destroy_pipeline(written);
  GraphicsPipelineDesc wrong = pd;
  wrong.attributes[1].format = VertexFormat::Float3;
  CHECK(!p.gpu->create_graphics_pipeline(wrong));
  wrong = pd; wrong.vertex_shader = fs;
  CHECK(!p.gpu->create_graphics_pipeline(wrong));
  wrong = pd; wrong.vertex_stride = 0;
  CHECK(!p.gpu->create_graphics_pipeline(wrong));
  // Textured pipelines: the textured pair with a float2 coordinate and one texture is taken; a texture count on the coloured pair, no count on the textured one, a
  // coordinate of another size, or a mix of the two pairs, are not.
  {
    IShaderModule* tvs = p.gpu->create_shader_module(textured_vertex_shader());
    IShaderModule* tfs = p.gpu->create_shader_module(textured_fragment_shader());
    GraphicsPipelineDesc textured;
    describe_textured_pipeline(&textured);
    textured.vertex_shader = tvs;
    textured.fragment_shader = tfs;
    IPipeline* tp = p.gpu->create_graphics_pipeline(textured);
    CHECK(tp);
    p.gpu->destroy_pipeline(tp);
    GraphicsPipelineDesc bad = textured;
    bad.texture_count = 0;
    CHECK(!p.gpu->create_graphics_pipeline(bad));
    bad = textured; bad.texture_count = 2;
    CHECK(!p.gpu->create_graphics_pipeline(bad));
    bad = textured; bad.attributes[1].format = VertexFormat::Float4;
    CHECK(!p.gpu->create_graphics_pipeline(bad));
    bad = textured; bad.fragment_shader = fs;
    CHECK(!p.gpu->create_graphics_pipeline(bad));
    bad = textured; bad.vertex_shader = vs;
    CHECK(!p.gpu->create_graphics_pipeline(bad));
    wrong = pd; wrong.texture_count = 1;
    CHECK(!p.gpu->create_graphics_pipeline(wrong));
    p.gpu->destroy_shader_module(tvs);
    p.gpu->destroy_shader_module(tfs);
  }
  p.gpu->destroy_pipeline(ok);
  p.gpu->destroy_shader_module(vs);
  p.gpu->destroy_shader_module(fs);
  // Address space and GPU memory are reused: far more images than the address space or the memory could hold at once. (Page tables made on the way stay for good.)
  auto churn = [&](int rounds) {
    for (int i = 0; i < rounds; ++i) {
      IImage* big = p.gpu->create_image({{1024, 1024}, Format::B8G8R8A8Unorm, kImageColorAttachment});
      CHECK(big);
      if (!big) return;
      p.gpu->destroy_image(big);
    }
  };
  const int before = p.rig.mem.live();
  churn(50);
  const int warmed = p.rig.mem.live();
  churn(600);
  CHECK(p.rig.mem.live() == warmed && warmed - before <= 4 && p.rig.gpu.violations.empty());
}

static void test_scenes() {
  Pair p;
  if (!p.gpu) { CHECK(false); return; }
  const Vec4 black = {0, 0, 0, 1}, gray = {0.25f, 0.25f, 0.25f, 1};

  // A clear.
  compare_scene(p, Format::B8G8R8A8Unorm, {0.2f, 0.4f, 0.6f, 1.0f}, [](Scene&, std::vector<Draw>&) {}, 1, 0, "clear");

  // One triangle with a colour at each corner, in both byte orders.
  static const ColoredVertex tri[3] = {cv(-0.8f, -0.8f, 0.5f, 1, 0, 0), cv(0.8f, -0.8f, 0.5f, 0, 1, 0), cv(0, 0.8f, 0.5f, 0, 0, 1)};
  for (Format f : {Format::B8G8R8A8Unorm, Format::R8G8B8A8Unorm}) {
    std::vector<uint32_t> g;
    compare_scene(p, f, black, [](Scene& s, std::vector<Draw>& d) { Draw x{tri, 3}; x.pipeline = s.pipeline(); d.push_back(x); }, 6, 260, "gouraud triangle", &g);
    // (the corner of the picture is clear, the middle of the triangle is lit)
    CHECK(g.size() == kSize * kSize && g[0] == 0xFF000000 && g[32 * kSize + 32] != 0xFF000000);
  }

  // Back-face culling with each winding, and the front face the other way: the same decisions as the software device.
  static const ColoredVertex ccw[3] = {cv(-0.8f, -0.8f, 0.5f, 1, 1, 0), cv(0.8f, -0.8f, 0.5f, 1, 1, 0), cv(0, 0.8f, 0.5f, 1, 1, 0)};
  static const ColoredVertex cw[3] = {cv(-0.8f, -0.8f, 0.5f, 0, 1, 1), cv(0, 0.8f, 0.5f, 0, 1, 1), cv(0.8f, -0.8f, 0.5f, 0, 1, 1)};
  const CullMode cullings[3] = {CullMode::None, CullMode::Back, CullMode::Front};
  const FrontFace faces[2] = {FrontFace::CounterClockwise, FrontFace::Clockwise};
  for (CullMode cull : cullings)
    for (FrontFace face : faces)
      compare_scene(p, Format::B8G8R8A8Unorm, black, [&](Scene& s, std::vector<Draw>& d) {
        Draw a{ccw, 3}; a.pipeline = s.pipeline(cull, face);
        Draw b{cw, 3}; b.mvp = Mat4::translation(0.1f, 0.1f, 0); b.pipeline = s.pipeline(cull, face);
        d.push_back(a); d.push_back(b);
      }, 6, 300, "culling");

  // Alpha blending over a background.
  static const ColoredVertex quad_a[3] = {cv(-0.9f, -0.9f, 0.5f, 1, 0, 0, 0.5f), cv(0.9f, -0.9f, 0.5f, 1, 0, 0, 0.5f), cv(-0.9f, 0.9f, 0.5f, 1, 0, 0, 0.5f)};
  static const ColoredVertex quad_b[3] = {cv(-0.5f, -0.5f, 0.5f, 0, 0, 1, 0.5f), cv(0.9f, 0.5f, 0.5f, 0, 0, 1, 0.5f), cv(-0.5f, 0.9f, 0.5f, 0, 0, 1, 0.5f)};
  compare_scene(p, Format::B8G8R8A8Unorm, {0, 0.5f, 0, 1}, [](Scene& s, std::vector<Draw>& d) {
    Draw a{quad_a, 3}; a.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, true);
    Draw b{quad_b, 3}; b.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, true);
    d.push_back(a); d.push_back(b);
  }, 8, 300, "alpha blend");

  // Indexed drawing with a repeated vertex, a viewport that is a quarter of the picture, and a scissor inside it.
  static const ColoredVertex quad[4] = {cv(-1, -1, 0.5f, 1, 0, 0), cv(1, -1, 0.5f, 0, 1, 0), cv(1, 1, 0.5f, 0, 0, 1), cv(-1, 1, 0.5f, 1, 1, 1)};
  static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
  compare_scene(p, Format::B8G8R8A8Unorm, gray, [](Scene& s, std::vector<Draw>& d) {
    Draw a{quad, 4}; a.indices = indices; a.index_count = 6; a.pipeline = s.pipeline();
    a.has_viewport = true; a.viewport = {16, 16, 32, 32, 0, 1};
    a.has_scissor = true; a.scissor = {20, 20, 20, 20};
    d.push_back(a);
  }, 6, 300, "indexed, viewport, scissor");

  // A triangle that crosses the near plane under a perspective-like matrix (w = z + 1): clipped, with one colour so that the screen-space interpolation does not matter.
  static const ColoredVertex crossing[3] = {cv(-0.5f, -0.5f, -0.5f, 1, 0.5f, 0), cv(0.5f, -0.5f, 0.5f, 1, 0.5f, 0), cv(0, 0.5f, 0.5f, 1, 0.5f, 0)};
  compare_scene(p, Format::B8G8R8A8Unorm, black, [](Scene& s, std::vector<Draw>& d) {
    Draw a{crossing, 3}; a.mvp.m[11] = 1.0f; a.mvp.m[15] = 1.0f; a.mvp.m[0] = a.mvp.m[5] = 1.0f; a.pipeline = s.pipeline();
    d.push_back(a);
  }, 6, 450, "near plane clip");

  // Depth: a near triangle drawn first and a far one after it (hidden where they overlap), then the other order, and a test without writes. Same pictures as software.
  static const ColoredVertex near_tri[3] = {cv(-0.7f, -0.7f, 0.2f, 1, 0, 0), cv(0.5f, -0.7f, 0.2f, 1, 0, 0), cv(-0.1f, 0.6f, 0.2f, 1, 0, 0)};
  static const ColoredVertex far_tri[3] = {cv(-0.5f, -0.5f, 0.8f, 0, 0, 1), cv(0.8f, -0.5f, 0.8f, 0, 0, 1), cv(0.2f, 0.8f, 0.8f, 0, 0, 1)};
  for (int order = 0; order < 2; ++order)
    compare_scene(p, Format::B8G8R8A8Unorm, gray, [&](Scene& s, std::vector<Draw>& d) {
      Draw a{near_tri, 3}; a.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, true);
      Draw b{far_tri, 3}; b.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, true);
      if (order == 0) { d.push_back(a); d.push_back(b); } else { d.push_back(b); d.push_back(a); }
    }, 6, 300, "depth test and write", nullptr, true);
  compare_scene(p, Format::B8G8R8A8Unorm, gray, [&](Scene& s, std::vector<Draw>& d) {
    Draw a{near_tri, 3}; a.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, true);
    Draw b{far_tri, 3}; b.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, false);   // tests, does not write
    Draw c{far_tri, 3}; c.mvp = Mat4::translation(-0.2f, 0.1f, 0); c.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, false, false);   // ignores depth
    d.push_back(a); d.push_back(b); d.push_back(c);
  }, 6, 300, "depth test only and no test", nullptr, true);
  // A viewport depth range (a quarter to three quarters) and a pipeline that tests with no depth image in the pass: nothing is hidden.
  compare_scene(p, Format::B8G8R8A8Unorm, gray, [&](Scene& s, std::vector<Draw>& d) {
    Draw a{near_tri, 3}; a.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, true); a.has_viewport = true; a.viewport = {0, 0, kSize, kSize, 0.25f, 0.75f};
    Draw b{far_tri, 3}; b.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, true); b.has_viewport = true; b.viewport = {0, 0, kSize, kSize, 0.25f, 0.75f};
    d.push_back(b); d.push_back(a);
  }, 6, 300, "viewport depth range", nullptr, true);
  compare_scene(p, Format::B8G8R8A8Unorm, gray, [&](Scene& s, std::vector<Draw>& d) {
    Draw a{near_tri, 3}; a.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, true);
    Draw b{far_tri, 3}; b.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, true);
    d.push_back(a); d.push_back(b);
  }, 6, 300, "depth test without a depth image");

  // Perspective: a triangle whose vertices are at different depths under w = z + 1 (clip z = z), colours interpolated perspective-correct, as in the software device.
  static const ColoredVertex slanted[3] = {cv(-0.8f, -0.8f, 0.1f, 1, 0, 0), cv(0.9f, -0.5f, 3.0f, 0, 1, 0), cv(-0.2f, 0.9f, 0.1f, 0, 0, 1)};
  compare_scene(p, Format::B8G8R8A8Unorm, black, [&](Scene& s, std::vector<Draw>& d) {
    Draw a{slanted, 3};
    a.mvp.m[11] = 1.0f; a.mvp.m[15] = 1.0f; a.mvp.m[0] = a.mvp.m[5] = 1.0f; a.mvp.m[10] = 0.2f;   // clip z = 0.2 z, w = z + 1
    a.pipeline = s.pipeline(CullMode::None, FrontFace::CounterClockwise, false, true, true);
    d.push_back(a);
  }, 12, 330, "perspective colours", nullptr, true);

  // Triangles behind the eye or at infinity are dropped, not drawn.
  static const ColoredVertex behind[3] = {cv(-0.5f, -0.5f, -3, 1, 1, 1), cv(0.5f, -0.5f, -3, 1, 1, 1), cv(0, 0.5f, -3, 1, 1, 1)};
  std::vector<uint32_t> g;
  compare_scene(p, Format::B8G8R8A8Unorm, black, [](Scene& s, std::vector<Draw>& d) { Draw a{behind, 3}; a.mvp.m[11] = 1.0f; a.pipeline = s.pipeline(); d.push_back(a); }, 1, 0, "behind the eye", &g);
  CHECK(g[32 * kSize + 32] == 0xFF000000);

  // More triangles than one job holds: several jobs, the same picture.
  static std::vector<ColoredVertex> many;
  many.clear();
  for (int i = 0; i < 800; ++i) {
    const float x = -0.9f + 1.8f * static_cast<float>(i % 40) / 40.0f, y = -0.9f + 1.8f * static_cast<float>(i / 40) / 20.0f;
    many.push_back(cv(x, y, 0.5f, 1, 0, 0));
    many.push_back(cv(x + 0.04f, y, 0.5f, 0, 1, 0));
    many.push_back(cv(x, y + 0.08f, 0.5f, 0, 0, 1));
  }
  const int jobs_before = p.rig.gpu.jobs;
  compare_scene(p, Format::B8G8R8A8Unorm, black, [](Scene& s, std::vector<Draw>& d) { Draw a{many.data(), static_cast<uint32_t>(many.size())}; a.pipeline = s.pipeline(); d.push_back(a); }, 8, 700, "many triangles");
  CHECK(p.rig.gpu.jobs - jobs_before >= 3);   // the clear and at least two of triangles
}

static void test_present_and_failures() {
  Pair p;
  if (!p.gpu) { CHECK(false); return; }
  Scene scene(*p.gpu);
  IImage* shown = p.gpu->create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImagePresent});
  IImage* hidden = p.gpu->create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IImage* rgba = p.gpu->create_image({{kSize, kSize}, Format::R8G8B8A8Unorm, kImageColorAttachment | kImagePresent});
  CHECK(shown && hidden && rgba);
  CHECK(render(*p.gpu, scene, shown, {1, 0, 0, 1}, {}));
  CHECK(p.gpu->present(shown) && p.display.presents == 1 && p.display.bw == kSize && p.display.pixels[5] == 0xFFFF0000);
  CHECK(!p.gpu->present(hidden) && !p.gpu->present(rgba) && !p.gpu->present(nullptr));   // not made for presenting / not the display's byte order

  // Passes the device cannot do: with a depth image, with no colour image, nesting, a pipeline of another device, a draw without bindings.
  ICommandBuffer* cb = p.gpu->create_command_buffer();
  cb->begin();
  RenderPassBegin pass;
  pass.color = shown;
  pass.depth = reinterpret_cast<IImage*>(shown);   // (any depth image makes the pass fail before it is looked at)
  cb->begin_render_pass(pass);
  cb->end_render_pass();
  CHECK(cb->end() && !p.gpu->submit(cb));
  cb->begin();
  cb->draw(3);
  CHECK(cb->end() && !p.gpu->submit(cb));
  cb->begin();
  pass.depth = nullptr;
  cb->begin_render_pass(pass);
  CHECK(cb->end() && !p.gpu->submit(cb));   // never ended
  CHECK(!p.gpu->submit(nullptr));
  p.gpu->destroy_command_buffer(cb);

  // A hang: the submit fails, the engine is wedged, and nothing the engine might still write to is freed.
  IPipeline* pipeline = scene.pipeline();
  static const ColoredVertex tri[3] = {cv(-0.8f, -0.8f, 0.5f, 1, 0, 0), cv(0.8f, -0.8f, 0.5f, 0, 1, 0), cv(0, 0.8f, 0.5f, 0, 0, 1)};
  p.rig.gpu.hang = true;
  Draw d{tri, 3};
  d.pipeline = pipeline;
  CHECK(!render(*p.gpu, scene, hidden, {0, 0, 0, 1}, {d}));
  CHECK(p.driver.state() == VcsDriver::State::Wedged);
  const int live = p.rig.mem.live();
  p.gpu->destroy_image(hidden);
  p.gpu->destroy_image(shown);
  CHECK(p.rig.mem.live() == live);   // nothing is given back: the engine may still write to it
  CHECK(!p.gpu->create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment}));   // no new memory for an engine that does not work
  p.gpu->destroy_pipeline(pipeline);
  p.gpu->destroy_image(rgba);
}

// --- Textures -------------------------------------------------------------------

struct TexturedScene {
  Pair& p;
  IGraphicsDevice* devices[2];
  IShaderModule* vs[2];
  IShaderModule* fs[2];
  IPipeline* pipeline[2] = {nullptr, nullptr};
  explicit TexturedScene(Pair& pair, bool blend = false) : p(pair) {
    devices[0] = pair.gpu;
    devices[1] = &pair.software;
    for (int i = 0; i < 2; ++i) {
      vs[i] = devices[i]->create_shader_module(textured_vertex_shader());
      fs[i] = devices[i]->create_shader_module(textured_fragment_shader());
      GraphicsPipelineDesc pd;
      describe_textured_pipeline(&pd);
      pd.vertex_shader = vs[i];
      pd.fragment_shader = fs[i];
      pd.cull_mode = CullMode::None;
      pd.alpha_blend = blend;
      pipeline[i] = devices[i]->create_graphics_pipeline(pd);
    }
  }
  ~TexturedScene() {
    for (int i = 0; i < 2; ++i) {
      devices[i]->destroy_pipeline(pipeline[i]);
      devices[i]->destroy_shader_module(vs[i]);
      devices[i]->destroy_shader_module(fs[i]);
    }
  }
  // Draws `vertices` (triangles) with the texture of `width` x `height` `texels` (0xAARRGGBB) on device `i`; the picture in `out`.
  bool draw(int i, Format texture_format, uint32_t width, uint32_t height, const uint32_t* texels, const SamplerDesc& sampler, const TexturedVertex* vertices, uint32_t count, const Mat4& mvp,
            Format target_format, std::vector<uint32_t>* out, Vec4 clear = {0, 0, 0, 1}) {
    IGraphicsDevice& device = *devices[i];
    IImage* texture = device.create_image({{width, height}, texture_format, kImageSampled});
    IImage* target = device.create_image({{kSize, kSize}, target_format, kImageColorAttachment});
    IBuffer* vb = device.create_buffer(count * sizeof(TexturedVertex) + 16, kBufferVertex);
    bool ok = texture && target && vb && pipeline[i] && texture->write_pixels(ArrayView<const uint32_t>(texels, uint64_t{width} * height));
    if (ok) {
      ByteView m = vb->map();
      memcpy(m.data(), vertices, count * sizeof(TexturedVertex));
      vb->unmap();
      ICommandBuffer* cb = device.create_command_buffer();
      cb->begin();
      RenderPassBegin pass;
      pass.color = target;
      pass.clear_color_value = clear;
      cb->begin_render_pass(pass);
      cb->bind_pipeline(pipeline[i]);
      cb->bind_vertex_buffer(vb);
      cb->push_constants(&mvp, sizeof mvp);
      cb->bind_texture(0, texture, sampler);
      cb->draw(count);
      cb->end_render_pass();
      ok = cb->end() && device.submit(cb);
      device.destroy_command_buffer(cb);
      if (ok) *out = read(target);
    }
    if (texture) device.destroy_image(texture);
    if (target) device.destroy_image(target);
    if (vb) device.destroy_buffer(vb);
    return ok;
  }
};

// A quad over the whole target (two triangles) with texture coordinates (u0, v0) at the top left and (u1, v1) at the bottom right.
static void quad(TexturedVertex* v, float u0, float v0, float u1, float v1) {
  const TexturedVertex q[6] = {{{-1, -1, 0.5f}, {u0, v0}}, {{-1, 1, 0.5f}, {u0, v1}}, {{1, 1, 0.5f}, {u1, v1}},
                               {{-1, -1, 0.5f}, {u0, v0}}, {{1, 1, 0.5f}, {u1, v1}}, {{1, -1, 0.5f}, {u1, v0}}};
  memcpy(v, q, sizeof q);
}

// The Gen9 device and the software device draw the same textured scene: pictures agree except along the diagonal between the two triangles and at texel edges.
static void compare_textured(Pair& p, TexturedScene& ts, Format texture_format, uint32_t w, uint32_t h, const uint32_t* texels, const SamplerDesc& sampler, const TexturedVertex* vertices,
                             uint32_t count, const Mat4& mvp, Format target_format, uint32_t tolerance, uint32_t allowed_bad, const char* what, Vec4 clear = {0, 0, 0, 1}) {
  std::vector<uint32_t> gpu_picture, soft_picture;
  const int jobs = p.rig.gpu.jobs;
  CHECK(ts.draw(0, texture_format, w, h, texels, sampler, vertices, count, mvp, target_format, &gpu_picture, clear));
  CHECK(ts.draw(1, texture_format, w, h, texels, sampler, vertices, count, mvp, target_format, &soft_picture, clear));
  CHECK(p.rig.gpu.jobs > jobs && p.rig.gpu.violations.empty());
  const uint32_t bad = differing(gpu_picture, soft_picture, tolerance);
  if (bad > allowed_bad) printf("  textured scene \"%s\": %u pixels differ\n", what, bad);
  CHECK(bad <= allowed_bad);
}

static void test_textures() {
  Pair p;
  CHECK(p.gpu);
  if (!p.gpu) return;
  TexturedScene ts(p);
  CHECK(ts.pipeline[0] && ts.pipeline[1]);
  const Mat4 id = Mat4::identity();
  TexturedVertex v[6];

  // A 4 x 4 texture of different texels, nearest: each texel a block.
  uint32_t texels[16];
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) texels[y * 4 + x] = 0xFF000000u | ((x * 80) << 16) | ((y * 80) << 8) | 0x40;
  quad(v, 0, 0, 1, 1);
  compare_textured(p, ts, Format::B8G8R8A8Unorm, 4, 4, texels, SamplerDesc(), v, 6, id, Format::B8G8R8A8Unorm, 0, 200, "nearest 4x4");
  // The model really sampled: a block's colour on the Gen9 picture is the texel.
  {
    std::vector<uint32_t> picture;
    CHECK(ts.draw(0, Format::B8G8R8A8Unorm, 4, 4, texels, SamplerDesc(), v, 6, id, Format::B8G8R8A8Unorm, &picture));
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x) CHECK(picture[(y * 16 + 8) * kSize + x * 16 + 8] == texels[y * 4 + x]);
  }
  // Repeat and clamp addressing (nearest), coordinates outside 0..1.
  SamplerDesc repeat;
  quad(v, 0, 0, 2, 2);
  compare_textured(p, ts, Format::B8G8R8A8Unorm, 4, 4, texels, repeat, v, 6, id, Format::B8G8R8A8Unorm, 0, 400, "repeat");
  SamplerDesc clamp;
  clamp.address_u = clamp.address_v = AddressMode::ClampToEdge;
  quad(v, -1, -1, 2, 2);
  compare_textured(p, ts, Format::B8G8R8A8Unorm, 4, 4, texels, clamp, v, 6, id, Format::B8G8R8A8Unorm, 0, 300, "clamp");
  // Linear filtering of a black and white pair of texels, clamped and repeating.
  const uint32_t bw[2] = {0xFF000000u, 0xFFFFFFFFu};
  SamplerDesc linear;
  linear.filter = Filter::Linear;
  linear.address_u = linear.address_v = AddressMode::ClampToEdge;
  quad(v, 0, 0, 1, 1);
  compare_textured(p, ts, Format::B8G8R8A8Unorm, 2, 1, bw, linear, v, 6, id, Format::B8G8R8A8Unorm, 6, 200, "linear clamp");
  linear.address_u = AddressMode::Repeat;
  compare_textured(p, ts, Format::B8G8R8A8Unorm, 2, 1, bw, linear, v, 6, id, Format::B8G8R8A8Unorm, 6, 200, "linear repeat");
  // R8G8B8A8 texture and target.
  const uint32_t rgba[4] = {0xFF2060A0u, 0xFFA06020u, 0xFF60A020u, 0xFF4080C0u};
  quad(v, 0, 0, 1, 1);
  compare_textured(p, ts, Format::R8G8B8A8Unorm, 2, 2, rgba, SamplerDesc(), v, 6, id, Format::R8G8B8A8Unorm, 0, 200, "rgba texture and target");
  compare_textured(p, ts, Format::R8G8B8A8Unorm, 2, 2, rgba, SamplerDesc(), v, 6, id, Format::B8G8R8A8Unorm, 0, 200, "rgba texture, bgra target");
  // Perspective: a receding quad, the coordinates interpolated with the vertices' 1/w.
  const uint32_t checker[4] = {0xFFFFFFFFu, 0xFF000000u, 0xFF000000u, 0xFFFFFFFFu};
  const Mat4 mvp = Mat4::perspective(1.2f, 1.0f, 0.1f, 100.0f) * Mat4::translation(0, -0.5f, -2.5f) * Mat4::rotation_x(-1.1f);
  const TexturedVertex tilted[6] = {{{-1, -1, 0}, {0, 0}}, {{-1, 1, 0}, {0, 8}}, {{1, 1, 0}, {8, 8}}, {{-1, -1, 0}, {0, 0}}, {{1, 1, 0}, {8, 8}}, {{1, -1, 0}, {8, 0}}};
  compare_textured(p, ts, Format::B8G8R8A8Unorm, 2, 2, checker, SamplerDesc(), tilted, 6, mvp, Format::B8G8R8A8Unorm, 0, 450, "perspective", {0, 0, 1, 1});
  // Alpha and blending: a half-transparent texel over the clear colour.
  {
    TexturedScene blended(p, true);
    const uint32_t half[1] = {0x80FF0000u};
    quad(v, 0, 0, 1, 1);
    compare_textured(p, blended, Format::B8G8R8A8Unorm, 1, 1, half, SamplerDesc(), v, 6, id, Format::B8G8R8A8Unorm, 3, 200, "blended", {0, 0, 1, 1});
  }

  // What is refused: no texture bound, the target as the texture, an image not made for sampling, a depth image, a unit that does not exist, a wrong-size upload.
  auto run = [&](IImage* target, IImage* bound, bool bind) {
    IBuffer* vb = p.gpu->create_buffer(sizeof v + 16, kBufferVertex);
    ByteView m = vb->map();
    memcpy(m.data(), v, sizeof v);
    ICommandBuffer* cb = p.gpu->create_command_buffer();
    cb->begin();
    RenderPassBegin pass;
    pass.color = target;
    cb->begin_render_pass(pass);
    cb->bind_pipeline(ts.pipeline[0]);
    cb->bind_vertex_buffer(vb);
    cb->push_constants(&id, sizeof id);
    if (bind) cb->bind_texture(0, bound, SamplerDesc());
    cb->draw(6);
    cb->end_render_pass();
    const bool ok = cb->end() && p.gpu->submit(cb);
    p.gpu->destroy_command_buffer(cb);
    p.gpu->destroy_buffer(vb);
    return ok;
  };
  IImage* target = p.gpu->create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImageSampled});
  IImage* plain = p.gpu->create_image({{4, 4}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IImage* sampled = p.gpu->create_image({{4, 4}, Format::B8G8R8A8Unorm, kImageSampled});
  IImage* depth = p.gpu->create_image({{4, 4}, Format::D32Float, kImageDepthAttachment | kImageSampled});
  CHECK(target && plain && sampled && depth);
  CHECK(run(target, sampled, true));       // (the control: this one works)
  CHECK(!run(target, nullptr, false));     // nothing bound
  CHECK(!run(target, target, true));       // the target as the texture
  CHECK(!run(target, plain, true));        // not made for sampling
  CHECK(!run(target, depth, true));        // a depth image
  CHECK(!depth->write_pixels(ArrayView<const uint32_t>(texels, 16)));
  CHECK(!sampled->write_pixels(ArrayView<const uint32_t>(texels, 15)));
  CHECK(sampled->write_pixels(ArrayView<const uint32_t>(texels, 16)));
  {
    ICommandBuffer* cb = p.gpu->create_command_buffer();
    cb->begin();
    cb->bind_texture(kMaxTextureUnits, sampled, SamplerDesc());
    CHECK(!cb->end());
    p.gpu->destroy_command_buffer(cb);
  }
  p.gpu->destroy_image(target);
  p.gpu->destroy_image(plain);
  p.gpu->destroy_image(sampled);
  p.gpu->destroy_image(depth);
}

// --- Tiling and mipmaps ----------------------------------------------------------

// A texture with mip levels, drawn on both devices: `levels[k]` are the pixels of level k (all of them written with write_mip_pixels), or with `generate` only level 0 is
// written and generate_mipmaps() makes the rest.
static bool draw_mipped(TexturedScene& ts, int i, uint32_t w, uint32_t h, const std::vector<std::vector<uint32_t>>& levels, bool generate, const SamplerDesc& sampler,
                        const TexturedVertex* vertices, uint32_t count, const Mat4& mvp, std::vector<uint32_t>* out) {
  IGraphicsDevice& device = *ts.devices[i];
  IImage* texture = device.create_image({{w, h}, Format::B8G8R8A8Unorm, kImageSampled, false, static_cast<uint32_t>(levels.size())});
  IImage* target = device.create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IBuffer* vb = device.create_buffer(count * sizeof(TexturedVertex) + 16, kBufferVertex);
  bool ok = texture && target && vb && ts.pipeline[i];
  if (ok && generate) {
    ok = texture->write_pixels(ArrayView<const uint32_t>(levels[0].data(), levels[0].size())) && texture->generate_mipmaps();
  } else if (ok) {
    for (uint32_t k = 0; k < levels.size() && ok; ++k) ok = texture->write_mip_pixels(k, ArrayView<const uint32_t>(levels[k].data(), levels[k].size()));
  }
  if (ok) {
    ByteView m = vb->map();
    memcpy(m.data(), vertices, count * sizeof(TexturedVertex));
    vb->unmap();
    ICommandBuffer* cb = device.create_command_buffer();
    cb->begin();
    RenderPassBegin pass;
    pass.color = target;
    pass.clear_color_value = {0, 0, 0, 1};
    cb->begin_render_pass(pass);
    cb->bind_pipeline(ts.pipeline[i]);
    cb->bind_vertex_buffer(vb);
    cb->push_constants(&mvp, sizeof mvp);
    cb->bind_texture(0, texture, sampler);
    cb->draw(count);
    cb->end_render_pass();
    ok = cb->end() && device.submit(cb);
    device.destroy_command_buffer(cb);
    if (ok) *out = read(target);
  }
  if (texture) device.destroy_image(texture);
  if (target) device.destroy_image(target);
  if (vb) device.destroy_buffer(vb);
  return ok;
}

static void compare_mipped(Pair& p, TexturedScene& ts, uint32_t w, uint32_t h, const std::vector<std::vector<uint32_t>>& levels, bool generate, const SamplerDesc& sampler,
                           const TexturedVertex* vertices, uint32_t count, const Mat4& mvp, uint32_t tolerance, uint32_t allowed_bad, const char* what) {
  std::vector<uint32_t> gpu_picture, soft_picture;
  const int jobs = p.rig.gpu.jobs;
  CHECK(draw_mipped(ts, 0, w, h, levels, generate, sampler, vertices, count, mvp, &gpu_picture));
  CHECK(draw_mipped(ts, 1, w, h, levels, generate, sampler, vertices, count, mvp, &soft_picture));
  CHECK(p.rig.gpu.jobs > jobs && p.rig.gpu.violations.empty());
  const uint32_t bad = differing(gpu_picture, soft_picture, tolerance);
  if (bad > allowed_bad) printf("  mipmapped scene \"%s\": %u pixels differ\n", what, bad);
  CHECK(bad <= allowed_bad);
}

static uint32_t mip_color(uint32_t level) { return 0xFF000000u | ((20 + level * 30) << 16) | ((200 - level * 20) << 8) | 0x40; }

static void test_tiling_and_mipmaps() {
  Pair p;
  CHECK(p.gpu);
  if (!p.gpu) return;
  TexturedScene ts(p);
  const Mat4 id = Mat4::identity();
  TexturedVertex v[6];

  // Solid levels: which level a lookup read shows in the colour. 64 x 64, 7 levels. A quad whose coordinates run 0..n over the 64 pixels minifies n times.
  std::vector<std::vector<uint32_t>> solid;
  for (uint32_t k = 0; k < 7; ++k) solid.push_back(std::vector<uint32_t>(uint64_t{mip_extent(64, k)} * mip_extent(64, k), mip_color(k)));
  SamplerDesc nearest_mips, linear_mips, none;
  nearest_mips.mip_filter = MipFilter::Nearest;
  linear_mips.mip_filter = MipFilter::Linear;
  for (float n : {1.0f, 2.0f, 3.0f, 4.0f, 8.0f, 100.0f}) {
    quad(v, 0, 0, n, n);
    compare_mipped(p, ts, 64, 64, solid, false, nearest_mips, v, 6, id, 2, 100, "nearest mip");
    compare_mipped(p, ts, 64, 64, solid, false, linear_mips, v, 6, id, 3, 100, "linear mip");
  }
  quad(v, 0, 0, 4, 4);
  compare_mipped(p, ts, 64, 64, solid, false, none, v, 6, id, 0, 100, "no mip filter: level 0");
  // Bias and the bounds of the level of detail.
  SamplerDesc biased = linear_mips, capped = nearest_mips, floored = nearest_mips;
  biased.lod_bias = 1.0f;
  capped.max_lod = 1.0f;
  floored.min_lod = 2.0f;
  quad(v, 0, 0, 2, 2);
  compare_mipped(p, ts, 64, 64, solid, false, biased, v, 6, id, 3, 100, "bias");
  quad(v, 0, 0, 8, 8);
  compare_mipped(p, ts, 64, 64, solid, false, capped, v, 6, id, 2, 100, "max lod");
  quad(v, 0, 0, 1, 1);
  compare_mipped(p, ts, 64, 64, solid, false, floored, v, 6, id, 2, 100, "min lod");
  // Magnified: level 0, filtered as asked.
  quad(v, 0, 0, 0.25f, 0.25f);
  compare_mipped(p, ts, 64, 64, solid, false, linear_mips, v, 6, id, 2, 100, "magnified");

  // A generated chain from a picture with structure: a gradient and a pattern, 32 x 32 down to 1 x 1.
  std::vector<std::vector<uint32_t>> base(1, std::vector<uint32_t>(32 * 32));
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 0; x < 32; ++x) base[0][y * 32 + x] = 0xFF000000u | ((x * 8) << 16) | ((y * 8) << 8) | (((x ^ y) & 4) ? 0xC0u : 0x30u);
  base.resize(6);   // (the other levels are made by generate_mipmaps; the vectors only count them)
  for (float n : {1.0f, 2.0f, 3.5f, 6.0f}) {
    quad(v, 0, 0, n, n);
    SamplerDesc smooth = linear_mips;
    smooth.filter = Filter::Linear;
    smooth.address_u = smooth.address_v = AddressMode::Repeat;
    compare_mipped(p, ts, 32, 32, base, true, smooth, v, 6, id, 6, 160, "generated chain, trilinear");
    SamplerDesc hard = nearest_mips;
    hard.address_u = hard.address_v = AddressMode::Repeat;
    compare_mipped(p, ts, 32, 32, base, true, hard, v, 6, id, 3, 160, "generated chain, nearest");
  }
  // Sizes that are not powers of two (20 x 12: 20 x 12, 10 x 6, 5 x 3, 2 x 1, 1 x 1), every level written by hand.
  {
    std::vector<std::vector<uint32_t>> odd;
    for (uint32_t k = 0; k < 5; ++k) odd.push_back(std::vector<uint32_t>(uint64_t{mip_extent(20, k)} * mip_extent(12, k), mip_color(k)));
    for (float n : {1.0f, 2.0f, 4.0f}) {
      quad(v, 0, 0, n, n);
      SamplerDesc clamp = nearest_mips;
      clamp.address_u = clamp.address_v = AddressMode::ClampToEdge;
      compare_mipped(p, ts, 20, 12, odd, false, clamp, v, 6, id, 2, 200, "odd sizes");
    }
  }
  // Perspective: a floor receding from the viewer, the level of detail growing with the distance.
  {
    const TexturedVertex tilted[6] = {{{-1, -1, 0}, {0, 0}}, {{-1, 1, 0}, {0, 16}}, {{1, 1, 0}, {16, 16}}, {{-1, -1, 0}, {0, 0}}, {{1, 1, 0}, {16, 16}}, {{1, -1, 0}, {16, 0}}};
    Mat4 persp = Mat4::identity();
    persp.m[7] = 0.9f;    // w depends on y: rows farther up are farther away
    persp.m[15] = 1.2f;
    compare_mipped(p, ts, 64, 64, solid, false, linear_mips, tilted, 6, persp, 10, 700, "perspective floor, trilinear");
  }

  // What is refused: a mip chain on a render target, a depth image or a present image; more levels than the size has; none.
  CHECK(!p.gpu->create_image({{64, 64}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImageSampled, false, 2}));
  CHECK(!p.gpu->create_image({{64, 64}, Format::B8G8R8A8Unorm, kImagePresent | kImageSampled, false, 2}));
  CHECK(!p.gpu->create_image({{64, 64}, Format::D32Float, kImageDepthAttachment, false, 2}));
  CHECK(!p.gpu->create_image({{64, 64}, Format::B8G8R8A8Unorm, kImageSampled, false, 8}) && !p.gpu->create_image({{64, 64}, Format::B8G8R8A8Unorm, kImageSampled, false, 0}));
  CHECK(!p.gpu->create_image({{4096, 8}, Format::B8G8R8A8Unorm, kImageSampled, false, 14}));
  // Writing: the level has to exist and the pixels to be the level's count; level 0 is write_pixels; an image with one level cannot generate.
  IImage* image = p.gpu->create_image({{20, 12}, Format::B8G8R8A8Unorm, kImageSampled, false, 5});
  CHECK(image && image->mip_levels() == 5);
  if (image) {
    std::vector<uint32_t> level(20 * 12, 0xFF123456u);
    CHECK(image->write_mip_pixels(0, ArrayView<const uint32_t>(level.data(), 240)) && image->write_pixels(ArrayView<const uint32_t>(level.data(), 240)));
    CHECK(!image->write_mip_pixels(5, ArrayView<const uint32_t>(level.data(), 1)) && !image->write_mip_pixels(1, ArrayView<const uint32_t>(level.data(), 61)) && image->write_mip_pixels(1, ArrayView<const uint32_t>(level.data(), 60)));
    CHECK(!image->write_mip_pixels(4, ArrayView<const uint32_t>(level.data(), 2)) && image->write_mip_pixels(4, ArrayView<const uint32_t>(level.data(), 1)));
    std::vector<uint32_t> back(240);
    CHECK(image->read_pixels(ArrayView<uint32_t>(back.data(), 240)) && back[0] == 0xFF123456u && back[239] == 0xFF123456u);   // (level 0 reads back through the tiling)
    p.gpu->destroy_image(image);
  }
  IImage* single = p.gpu->create_image({{8, 8}, Format::B8G8R8A8Unorm, kImageSampled});
  CHECK(single && !single->generate_mipmaps());
  if (single) p.gpu->destroy_image(single);
  // Memory: the chain's memory comes back, and so does a plain image's.
  const int before = p.rig.mem.live();
  IImage* big = p.gpu->create_image({{256, 256}, Format::B8G8R8A8Unorm, kImageSampled, false, 9});
  IImage* plain = p.gpu->create_image({{33, 17}, Format::R8G8B8A8Unorm, kImageColorAttachment});
  CHECK(big && plain && p.rig.mem.live() > before);
  p.gpu->destroy_image(big);
  p.gpu->destroy_image(plain);
  CHECK(p.rig.mem.live() == before && p.rig.gpu.violations.empty());
  // Every texel of a tiled image is where the tiling says: a picture written is the picture read back, whatever the size (single tiles, partial tiles, odd widths).
  for (uint32_t w : {1u, 5u, 31u, 32u, 33u, 100u, 130u})
    for (uint32_t h : {1u, 3u, 31u, 32u, 33u, 70u}) {
      IImage* im = p.gpu->create_image({{w, h}, Format::B8G8R8A8Unorm, kImageColorAttachment});
      CHECK(im);
      if (!im) continue;
      std::vector<uint32_t> in(uint64_t{w} * h), out(uint64_t{w} * h);
      for (uint32_t i = 0; i < in.size(); ++i) in[i] = 0xFF000000u | (i * 2654435761u >> 8);
      CHECK(im->write_pixels(ArrayView<const uint32_t>(in.data(), in.size())) && im->read_pixels(ArrayView<uint32_t>(out.data(), out.size())) && in == out);
      p.gpu->destroy_image(im);
    }
}

// --- Stencil --------------------------------------------------------------------

struct StencilItem {
  float x0, y0, x1, y1, z;
  float rgba[4];
  StencilState stencil;
  bool depth_test = false, depth_write = false;
};

// Runs the items in one render pass on `device` (colour cleared to black, depth to 1, stencil to `clear`) with a depth image that has a stencil plane (or `plain_depth`), and reads the
// picture back. False if the recording or the submit failed.
static bool stencil_run(IGraphicsDevice& device, const std::vector<StencilItem>& items, uint8_t clear, std::vector<uint32_t>* out, IImage* plain_depth = nullptr) {
  IShaderModule* vs = device.create_shader_module(colored_vertex_shader());
  IShaderModule* fs = device.create_shader_module(colored_fragment_shader());
  IImage* color = device.create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  ImageDesc dd{{kSize, kSize}, Format::D32Float, kImageDepthAttachment};
  dd.stencil = true;
  IImage* depth = plain_depth ? plain_depth : device.create_image(dd);
  bool ok = vs && fs && color && depth;
  std::vector<IPipeline*> pipelines;
  std::vector<IBuffer*> buffers;
  ICommandBuffer* cb = ok ? device.create_command_buffer() : nullptr;
  if (cb) {
    cb->begin();
    RenderPassBegin pass;
    pass.color = color;
    pass.depth = depth;
    pass.clear_color_value = {0, 0, 0, 1};
    pass.clear_stencil_value = clear;
    cb->begin_render_pass(pass);
    for (const StencilItem& it : items) {
      GraphicsPipelineDesc pd;
      describe_colored_pipeline(&pd);
      pd.vertex_shader = vs;
      pd.fragment_shader = fs;
      pd.cull_mode = CullMode::None;
      pd.stencil = it.stencil;
      pd.depth_test = it.depth_test;
      pd.depth_write = it.depth_write;
      IPipeline* pipeline = device.create_graphics_pipeline(pd);
      pipelines.push_back(pipeline);
      const float* c = it.rgba;
      const ColoredVertex v[6] = {cv(it.x0, it.y0, it.z, c[0], c[1], c[2], c[3]), cv(it.x0, it.y1, it.z, c[0], c[1], c[2], c[3]), cv(it.x1, it.y1, it.z, c[0], c[1], c[2], c[3]),
                                  cv(it.x0, it.y0, it.z, c[0], c[1], c[2], c[3]), cv(it.x1, it.y1, it.z, c[0], c[1], c[2], c[3]), cv(it.x1, it.y0, it.z, c[0], c[1], c[2], c[3])};
      IBuffer* vb = device.create_buffer(sizeof v + 16, kBufferVertex);
      buffers.push_back(vb);
      if (vb && pipeline) {
        memcpy(vb->map().data(), v, sizeof v);
        vb->unmap();
        cb->bind_pipeline(pipeline);
        cb->bind_vertex_buffer(vb);
        const Mat4 id = Mat4::identity();
        cb->push_constants(&id, sizeof id);
        cb->draw(6);
      } else {
        ok = false;
      }
    }
    cb->end_render_pass();
    ok = ok && cb->end() && device.submit(cb);
    device.destroy_command_buffer(cb);
  } else {
    ok = false;
  }
  if (ok && out) *out = read(color);
  for (IPipeline* p : pipelines) if (p) device.destroy_pipeline(p);
  for (IBuffer* b : buffers) if (b) device.destroy_buffer(b);
  if (color) device.destroy_image(color);
  if (depth && !plain_depth) device.destroy_image(depth);
  if (vs) device.destroy_shader_module(vs);
  if (fs) device.destroy_shader_module(fs);
  return ok;
}

static void test_stencil() {
  Pair p;
  CHECK(p.gpu);
  if (!p.gpu) return;
  const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
  auto item = [](float x0, float y0, float x1, float y1, float z, const float* rgba, StencilState st, bool dt = false, bool dw = false) {
    StencilItem it{x0, y0, x1, y1, z, {rgba[0], rgba[1], rgba[2], rgba[3]}, st, dt, dw};
    return it;
  };
  auto equals = [](uint8_t value) {
    StencilState st;
    st.enable = true;
    st.compare = CompareOp::Equal;
    st.reference = value;
    return st;
  };
  // Both devices run the scene; the pictures agree (up to the pixels on the diagonal between a rectangle's two triangles) and show what the scene is supposed to show.
  auto both = [&](const std::vector<StencilItem>& items, uint8_t clear, bool expect_white_at_probe, uint32_t probe_x, uint32_t probe_y, const char* what) {
    std::vector<uint32_t> gpu_picture, soft_picture;
    const int jobs = p.rig.gpu.jobs;
    CHECK(stencil_run(*p.gpu, items, clear, &gpu_picture));
    CHECK(stencil_run(p.software, items, clear, &soft_picture));
    CHECK(p.rig.gpu.jobs > jobs && p.rig.gpu.violations.empty());
    const uint32_t bad = differing(gpu_picture, soft_picture, 0);
    if (bad > 140) printf("  stencil scene \"%s\": %u pixels differ\n", what, bad);
    CHECK(bad <= 140);
    if (gpu_picture.size() == kSize * kSize) {
      const bool white_here = gpu_picture[probe_y * kSize + probe_x] == 0xFFFFFFFF;
      if (white_here != expect_white_at_probe) printf("  stencil scene \"%s\": the probe is %s\n", what, white_here ? "white" : "black");
      CHECK(white_here == expect_white_at_probe);
    }
  };
  // The clear value and a test against it.
  both({item(-1, -1, 1, 1, 0.5f, white, equals(0))}, 0, true, 30, 20, "cleared to 0, equals 0");
  both({item(-1, -1, 1, 1, 0.5f, white, equals(5))}, 0, false, 30, 20, "cleared to 0, equals 5");
  both({item(-1, -1, 1, 1, 0.5f, white, equals(5))}, 5, true, 30, 20, "cleared to 5, equals 5");
  // Every compare function: the reference 3, 5, 7 against a stored 5.
  struct Case { CompareOp op; bool less, equal, greater; };
  const Case cases[] = {{CompareOp::Never, false, false, false}, {CompareOp::Less, true, false, false}, {CompareOp::Equal, false, true, false}, {CompareOp::LessEqual, true, true, false},
                        {CompareOp::Greater, false, false, true}, {CompareOp::NotEqual, true, false, true}, {CompareOp::GreaterEqual, false, true, true}, {CompareOp::Always, true, true, true}};
  for (const Case& c : cases) {
    const uint8_t refs[3] = {3, 5, 7};
    const bool expect[3] = {c.less, c.equal, c.greater};
    for (int i = 0; i < 3; ++i) {
      StencilState st;
      st.enable = true;
      st.compare = c.op;
      st.reference = refs[i];
      both({item(-1, -1, 1, 1, 0.5f, white, st)}, 5, expect[i], 30, 20, "compare function");
    }
  }
  // Every operation, from a stored value, then probed for the expected result (the reference is 9).
  struct OpCase { StencilOp op; uint8_t start, expect; };
  const OpCase ops[] = {{StencilOp::Keep, 250, 250}, {StencilOp::Zero, 250, 0}, {StencilOp::Replace, 250, 9}, {StencilOp::IncrementClamp, 250, 251}, {StencilOp::IncrementClamp, 255, 255},
                        {StencilOp::DecrementClamp, 250, 249}, {StencilOp::DecrementClamp, 0, 0}, {StencilOp::Invert, 0x0F, 0xF0}, {StencilOp::IncrementWrap, 255, 0},
                        {StencilOp::DecrementWrap, 0, 255}, {StencilOp::IncrementWrap, 1, 2}};
  for (const OpCase& c : ops) {
    StencilState write;
    write.enable = true;
    write.pass_op = c.op;
    write.reference = 9;
    both({item(-1, -1, 1, 1, 0.5f, black, write), item(-1, -1, 1, 1, 0.5f, white, equals(c.expect))}, c.start, true, 30, 20, "operation");
  }
  // A mask: only the left half is written, and the write and compare masks.
  {
    StencilState left;
    left.enable = true;
    left.pass_op = StencilOp::Replace;
    left.reference = 1;
    both({item(-1, -1, 0, 1, 0.5f, black, left), item(-1, -1, 1, 1, 0.5f, white, equals(1))}, 0, true, 10, 40, "left half written");
    both({item(-1, -1, 0, 1, 0.5f, black, left), item(-1, -1, 1, 1, 0.5f, white, equals(1))}, 0, false, 50, 40, "right half not written");
    StencilState masked;
    masked.enable = true;
    masked.pass_op = StencilOp::Replace;
    masked.reference = 0xFF;
    masked.write_mask = 0x0F;
    both({item(-1, -1, 1, 1, 0.5f, black, masked), item(-1, -1, 1, 1, 0.5f, white, equals(0xAF))}, 0xA0, true, 30, 20, "write mask");
    StencilState compare_masked;
    compare_masked.enable = true;
    compare_masked.compare = CompareOp::Equal;
    compare_masked.compare_mask = 0xF0;
    compare_masked.reference = 0xAF;
    both({item(-1, -1, 1, 1, 0.5f, white, compare_masked)}, 0xA5, true, 30, 20, "compare mask");
  }
  // Fail, depth-fail and pass operations are separate.
  {
    StencilState fail;
    fail.enable = true;
    fail.compare = CompareOp::Never;
    fail.fail_op = StencilOp::Replace;
    fail.pass_op = StencilOp::Zero;
    fail.depth_fail_op = StencilOp::Zero;
    fail.reference = 4;
    both({item(-1, -1, 1, 1, 0.5f, white, fail), item(-1, -1, 1, 1, 0.5f, white, equals(4))}, 0, true, 30, 20, "fail op");
    StencilState none;
    StencilState counted;
    counted.enable = true;
    counted.depth_fail_op = StencilOp::IncrementClamp;
    counted.pass_op = StencilOp::Zero;
    const std::vector<StencilItem> depth_scene = {item(-1, -1, 0, 1, 0.2f, black, none, true, true), item(-1, -1, 1, 1, 0.8f, black, counted, true, false),
                                                  item(-1, -1, 0, 1, 0.5f, white, equals(1)), item(0, -1, 1, 1, 0.5f, white, equals(0))};
    both(depth_scene, 0, true, 10, 40, "depth fail op, behind");
    both(depth_scene, 0, true, 50, 40, "pass op, in front");
  }
  // Refusals: a stencil test needs a stencil plane; a stencil plane goes with a depth image; memory comes back.
  {
    IImage* plain = p.gpu->create_image({{kSize, kSize}, Format::D32Float, kImageDepthAttachment});
    CHECK(plain);
    CHECK(!stencil_run(*p.gpu, {item(-1, -1, 1, 1, 0.5f, white, equals(0))}, 0, nullptr, plain));
    CHECK(stencil_run(*p.gpu, {item(-1, -1, 1, 1, 0.5f, white, StencilState())}, 0, nullptr, plain));
    p.gpu->destroy_image(plain);
    ImageDesc bad{{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment};
    bad.stencil = true;
    CHECK(p.gpu->create_image(bad) == nullptr);
    const int before = p.rig.mem.live();
    for (int i = 0; i < 20; ++i) {
      ImageDesc d{{256, 256}, Format::D32Float, kImageDepthAttachment};
      d.stencil = true;
      IImage* image = p.gpu->create_image(d);
      CHECK(image);
      p.gpu->destroy_image(image);
    }
    CHECK(p.rig.mem.live() == before);
  }
}

int main() {
  test_resources();
  test_scenes();
  test_textures();
  test_tiling_and_mipmaps();
  test_stencil();
  test_present_and_failures();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("gen9 graphics device tests passed\n");
  return 0;
}
