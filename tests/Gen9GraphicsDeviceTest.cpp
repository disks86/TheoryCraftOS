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

int main() {
  test_resources();
  test_scenes();
  test_present_and_failures();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("gen9 graphics device tests passed\n");
  return 0;
}
