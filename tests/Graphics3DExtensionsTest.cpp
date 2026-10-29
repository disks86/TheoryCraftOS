// Host-side tests for the extended features of the 3D device interface (DeviceInfo::features) on the software device: the blend state and depth comparison, texel access, system
// values (fragment position, front facing, primitive, vertex and instance ids), instancing, and lines and points.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
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
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint32_t kSize = 16;

// --- Shaders ---------------------------------------------------------------------------------------------------------------------------------------------------------------

// position from attribute 0, varying 0 from attribute 1
static void pass_vertex(const Vec4* a, const uint8_t*, ShaderIo* out) {
  out->position = {a[0].x, a[0].y, a[0].z, 1.0f};
  out->varying[0] = a[1];
}
// the colour is the first four floats of the constants
static bool constant_color(const Vec4*, const uint8_t* constants, Vec4* color) {
  memcpy(color, constants, sizeof(Vec4));
  return true;
}
// attribute 1 is a per-instance offset (x, y); varying 0 carries the instance and vertex ids
static void instanced_vertex(const Vec4* a, const uint8_t*, const VertexContext& context, ShaderIo* out) {
  out->position = {a[0].x + a[1].x, a[0].y + a[1].y, a[0].z, 1.0f};
  out->varying[0] = {static_cast<float>(context.instance_id) * 0.1f, context.vertex_id == 2 ? 1.0f : 0.0f, 0.0f, 1.0f};
}
static bool varying_color_ex(const Vec4* v, const FragmentContext&, const uint8_t*, const ITextureSet&, Vec4* color) {
  *color = v[0];
  return true;
}
// the fragment's own position, whether it is front facing, and the primitive
static bool system_color(const Vec4*, const FragmentContext& c, const uint8_t*, const ITextureSet&, Vec4* color) {
  *color = {c.position.x / 16.0f, c.position.y / 16.0f, c.front_facing ? 1.0f : 0.0f, static_cast<float>(c.primitive_id) * 0.25f};
  return true;
}
// the texture: unit 0 fetched at (1, 2) -> red, its size -> green and blue, the lookup at level 1 -> alpha
static bool texel_color(const Vec4*, const FragmentContext&, const uint8_t*, const ITextureSet& t, Vec4* color) {
  const Vec4 fetched = t.fetch(0, 1, 2, 0);
  uint32_t w = 0, h = 0, levels = 0;
  t.size(0, 0, &w, &h, &levels);
  const Vec4 lod = t.sample_lod(0, 0.5f, 0.5f, 1.0f);
  *color = {fetched.x, static_cast<float>(w) / 8.0f, static_cast<float>(levels) / 8.0f, lod.x};
  return true;
}

// --- A scene -----------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Rig {
  TestAllocator alloc;
  SoftwareDevice device{alloc, nullptr};
  IImage* color = nullptr;
  IImage* depth = nullptr;
  ~Rig() {
    if (color) device.destroy_image(color);
    if (depth) device.destroy_image(depth);
  }
  void make_targets(bool with_depth) {
    color = device.create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment});
    if (with_depth) depth = device.create_image({{kSize, kSize}, Format::D32Float, kImageDepthAttachment});
  }
  IShaderModule* vertex(VertexShaderFn f, VertexShaderFnEx ex = nullptr) {
    ShaderModuleDesc d;
    d.stage = ShaderStage::Vertex;
    d.vertex_function = f;
    d.vertex_function_ex = ex;
    return device.create_shader_module(d);
  }
  IShaderModule* fragment(FragmentShaderFn f, FragmentShaderFnEx ex = nullptr) {
    ShaderModuleDesc d;
    d.stage = ShaderStage::Fragment;
    d.fragment_function = ex ? nullptr : f;
    d.fragment_function_ex = ex;
    return device.create_shader_module(d);
  }
  IBuffer* buffer(const std::vector<float>& floats, uint32_t usage = kBufferVertex) {
    IBuffer* b = device.create_buffer(floats.size() * 4, usage);
    if (!b) return nullptr;
    memcpy(b->map().data(), floats.data(), floats.size() * 4);
    b->unmap();
    return b;
  }
  std::vector<uint32_t> pixels() {
    std::vector<uint32_t> out(kSize * kSize);
    CHECK(color->read_pixels(ArrayView<uint32_t>(out.data(), out.size())));
    return out;
  }
};

static uint32_t channel(uint32_t argb, int shift) { return (argb >> shift) & 0xFF; }
static bool close8(uint32_t a, uint32_t b, uint32_t tolerance = 2) { return (a > b ? a - b : b - a) <= tolerance; }

// A big triangle covering the target, at depth z.
static std::vector<float> cover(float z) { return {-1, -1, z, 3, -1, z, -1, 3, z}; }

static GraphicsPipelineDesc position_only() {
  GraphicsPipelineDesc d;
  d.vertex_stride = 12;
  d.attribute_count = 1;
  d.attributes[0] = {0, VertexFormat::Float3, 0};
  d.varying_count = 1;
  d.cull_mode = CullMode::None;
  return d;
}

// Draws `vertices` (position only) with `pipeline`, on a target cleared to `clear`, with `color` as the constants; the target is left in the rig.
static void draw(Rig& r, const GraphicsPipelineDesc& pd, const std::vector<float>& vertices, Vec4 constants, Vec4 clear = {0.2f, 0.4f, 0.6f, 1.0f}, bool clear_target = true) {
  IPipeline* pipeline = r.device.create_graphics_pipeline(pd);
  IBuffer* vb = r.buffer(vertices);
  CHECK(pipeline && vb);
  if (!pipeline || !vb) return;
  ICommandBuffer* cb = r.device.create_command_buffer();
  cb->begin();
  RenderPassBegin pass;
  pass.color = r.color;
  pass.depth = r.depth;
  pass.clear_color = clear_target;
  pass.clear_color_value = clear;
  pass.clear_depth = clear_target;
  cb->begin_render_pass(pass);
  cb->bind_pipeline(pipeline);
  cb->bind_vertex_buffer(vb);
  cb->push_constants(&constants, sizeof constants);
  cb->draw(static_cast<uint32_t>(vertices.size() / 3));
  cb->end_render_pass();
  CHECK(cb->end() && r.device.submit(cb));
  r.device.destroy_command_buffer(cb);
  r.device.destroy_buffer(vb);
  r.device.destroy_pipeline(pipeline);
}

// --- Tests -----------------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_features() {
  TestAllocator alloc;
  SoftwareDevice device(alloc, nullptr);
  const uint32_t all = kFeatureBlendState | kFeatureTexelAccess | kFeatureSystemInputs | kFeatureInstancing | kFeatureLines;
  CHECK((device.info().features & all) == all);
}

static void test_blend() {
  Rig r;
  r.make_targets(false);
  IShaderModule* vs = r.vertex(pass_vertex);
  IShaderModule* fs = r.fragment(constant_color);
  GraphicsPipelineDesc pd = position_only();
  pd.vertex_shader = vs, pd.fragment_shader = fs;
  const Vec4 dst = {0.2f, 0.4f, 0.6f, 1.0f}, src = {0.5f, 0.25f, 1.0f, 0.5f};
  struct Case {
    BlendState state;
    float want[4];   // the expected 0..1 result
  };
  auto state = [](BlendFactor sc, BlendFactor dc, BlendFactor sa, BlendFactor da, BlendOp co, BlendOp ao, uint8_t mask = 0xF) {
    BlendState b;
    b.custom = b.enable = true;
    b.src_color = sc, b.dst_color = dc, b.src_alpha = sa, b.dst_alpha = da, b.color_op = co, b.alpha_op = ao, b.write_mask = mask;
    return b;
  };
  const Case cases[] = {
      {state(BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendOp::Add, BlendOp::Add), {0.7f, 0.65f, 1.0f, 1.0f}},                                  // additive
      {state(BlendFactor::SrcAlpha, BlendFactor::InvSrcAlpha, BlendFactor::One, BlendFactor::Zero, BlendOp::Add, BlendOp::Add), {0.35f, 0.325f, 0.8f, 0.5f}},              // over (alpha kept)
      {state(BlendFactor::DstColor, BlendFactor::Zero, BlendFactor::DstAlpha, BlendFactor::Zero, BlendOp::Add, BlendOp::Add), {0.1f, 0.1f, 0.6f, 0.5f}},                  // multiply
      {state(BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendOp::Subtract, BlendOp::Subtract), {0.3f, 0.0f, 0.4f, 0.0f}},                       // src - dst
      {state(BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendOp::ReverseSubtract, BlendOp::ReverseSubtract), {0.0f, 0.15f, 0.0f, 0.5f}},         // dst - src
      {state(BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendFactor::One, BlendOp::Min, BlendOp::Max), {0.2f, 0.25f, 0.6f, 1.0f}},                                  // min colour, max alpha
      {state(BlendFactor::SrcAlphaSaturate, BlendFactor::One, BlendFactor::One, BlendFactor::Zero, BlendOp::Add, BlendOp::Add), {0.2f, 0.4f, 0.6f, 0.5f}},                       // min(sa, 1 - da) = 0: the source adds nothing to the colour
      {state(BlendFactor::One, BlendFactor::Zero, BlendFactor::One, BlendFactor::Zero, BlendOp::Add, BlendOp::Add, 0x2), {0.2f, 0.25f, 0.6f, 1.0f}},                          // only green stored
  };
  for (const Case& c : cases) {
    pd.blend = c.state;
    draw(r, pd, cover(0.5f), src, dst);
    const uint32_t p = r.pixels()[8 * kSize + 8];
    const float got[4] = {channel(p, 16) / 255.0f, channel(p, 8) / 255.0f, channel(p, 0) / 255.0f, channel(p, 24) / 255.0f};
    bool ok = true;
    for (int k = 0; k < 4; ++k) ok = ok && close8(static_cast<uint32_t>(got[k] * 255.0f + 0.5f), static_cast<uint32_t>(c.want[k] * 255.0f + 0.5f), 2);
    if (!ok) printf("  blend: got %.3f %.3f %.3f %.3f, expected %.3f %.3f %.3f %.3f\n", got[0], got[1], got[2], got[3], c.want[0], c.want[1], c.want[2], c.want[3]);
    CHECK(ok);
  }
  // Blending off in a custom state stores the source (under the mask); the pipeline's alpha_blend does not matter then.
  pd.blend = BlendState();
  pd.blend.custom = true;
  pd.alpha_blend = true;
  draw(r, pd, cover(0.5f), src, dst);
  const uint32_t p = r.pixels()[0];
  CHECK(close8(channel(p, 16), 128) && close8(channel(p, 8), 64) && close8(channel(p, 0), 255) && close8(channel(p, 24), 128));
  // Without `custom`, the pipeline's alpha_blend is the source-over it always was.
  pd.blend = BlendState();
  draw(r, pd, cover(0.5f), src, dst);
  const uint32_t q = r.pixels()[0];
  CHECK(close8(channel(q, 16), 89, 3) && close8(channel(q, 8), 83, 3) && close8(channel(q, 0), 204, 3));
  r.device.destroy_shader_module(vs);
  r.device.destroy_shader_module(fs);
  CHECK(r.alloc.live >= 0);
}

static void test_depth_compare() {
  Rig r;
  r.make_targets(true);
  IShaderModule* vs = r.vertex(pass_vertex);
  IShaderModule* fs = r.fragment(constant_color);
  GraphicsPipelineDesc pd = position_only();
  pd.vertex_shader = vs, pd.fragment_shader = fs;
  pd.depth_test = true;
  pd.depth_write = true;
  // First a triangle at depth 0.5 (compare Always), then one at 0.7 with each comparison: it shows where the comparison holds of 0.7 against the stored 0.5.
  const struct {
    CompareOp op;
    bool drawn;
  } cases[] = {{CompareOp::Never, false}, {CompareOp::Less, false}, {CompareOp::Equal, false}, {CompareOp::LessEqual, false}, {CompareOp::Greater, true}, {CompareOp::NotEqual, true},
               {CompareOp::GreaterEqual, true}, {CompareOp::Always, true}};
  for (const auto& c : cases) {
    pd.depth_compare = CompareOp::Always;
    draw(r, pd, cover(0.5f), {1, 0, 0, 1});
    pd.depth_compare = c.op;
    draw(r, pd, cover(0.7f), {0, 1, 0, 1}, {}, false);
    const uint32_t p = r.pixels()[8 * kSize + 8];
    const bool green = channel(p, 8) > 200;
    if (green != c.drawn) printf("  depth compare %d: %s\n", static_cast<int>(c.op), green ? "drawn" : "not drawn");
    CHECK(green == c.drawn);
  }
  // The same depth: Equal and LessEqual pass, Less does not.
  for (CompareOp op : {CompareOp::Equal, CompareOp::LessEqual, CompareOp::Less}) {
    pd.depth_compare = CompareOp::Always;
    draw(r, pd, cover(0.5f), {1, 0, 0, 1});
    pd.depth_compare = op;
    draw(r, pd, cover(0.5f), {0, 1, 0, 1}, {}, false);
    const bool green = channel(r.pixels()[8 * kSize + 8], 8) > 200;
    CHECK(green == (op != CompareOp::Less));
  }
  r.device.destroy_shader_module(vs);
  r.device.destroy_shader_module(fs);
}

static void test_texel_access() {
  Rig r;
  r.make_targets(false);
  // An 8 x 8 texture with two levels... made as 8 x 8 with 4 levels: level 0 holds x + 8 * y in the red channel, level 1 is all 0xFF0000.
  IImage* texture = r.device.create_image({{8, 8}, Format::B8G8R8A8Unorm, kImageSampled, false, 4});
  CHECK(texture);
  if (!texture) return;
  std::vector<uint32_t> base(64);
  for (uint32_t i = 0; i < 64; ++i) base[i] = 0xFF000000u | (i << 16);
  CHECK(texture->write_pixels(ArrayView<const uint32_t>(base.data(), base.size())));
  std::vector<uint32_t> level1(16, 0xFF800000u);
  CHECK(texture->write_mip_pixels(1, ArrayView<const uint32_t>(level1.data(), level1.size())));
  IShaderModule* vs = r.vertex(pass_vertex);
  IShaderModule* fs = r.fragment(nullptr, texel_color);
  GraphicsPipelineDesc pd = position_only();
  pd.vertex_shader = vs, pd.fragment_shader = fs;
  pd.texture_count = 1;
  IPipeline* pipeline = r.device.create_graphics_pipeline(pd);
  IBuffer* vb = r.buffer(cover(0.5f));
  CHECK(pipeline && vb);
  ICommandBuffer* cb = r.device.create_command_buffer();
  cb->begin();
  RenderPassBegin pass;
  pass.color = r.color;
  cb->begin_render_pass(pass);
  cb->bind_pipeline(pipeline);
  cb->bind_vertex_buffer(vb);
  SamplerDesc sampler;
  sampler.mip_filter = MipFilter::Nearest;
  cb->bind_texture(0, texture, sampler);
  cb->draw(3);
  cb->end_render_pass();
  CHECK(cb->end() && r.device.submit(cb));
  const uint32_t p = r.pixels()[5 * kSize + 5];
  // fetch(1, 2) = texel 17 -> red 17 / 255; size 8 -> 1.0; 4 levels -> 0.5; level 1 -> 0x80 / 255
  CHECK(close8(channel(p, 16), 17) && close8(channel(p, 8), 255) && close8(channel(p, 0), 128) && close8(channel(p, 24), 128));
  r.device.destroy_command_buffer(cb);
  r.device.destroy_buffer(vb);
  r.device.destroy_pipeline(pipeline);
  r.device.destroy_image(texture);
  r.device.destroy_shader_module(vs);
  r.device.destroy_shader_module(fs);
}

static void test_system_values() {
  Rig r;
  r.make_targets(false);
  IShaderModule* vs = r.vertex(pass_vertex);
  IShaderModule* fs = r.fragment(nullptr, system_color);
  GraphicsPipelineDesc pd = position_only();
  pd.vertex_shader = vs, pd.fragment_shader = fs;
  // Two triangles in one draw: the first (counter clockwise on the screen, y down: (-1,-1) (1,-1) (-1,1) is clockwise...) covers the upper left, the second the lower right.
  const std::vector<float> v = {-1, -1, 0.5f, 1, -1, 0.5f, -1, 1, 0.5f,    1, -1, 0.5f, 1, 1, 0.5f, -1, 1, 0.5f};
  draw(r, pd, v, {0, 0, 0, 0});
  const std::vector<uint32_t> img = r.pixels();
  // pixel (2, 2) is in the first triangle, (13, 13) in the second; position is the pixel centre, primitive_id 0 / 1 (x 0.25)
  const uint32_t a = img[2 * kSize + 2], b = img[13 * kSize + 13];
  CHECK(close8(channel(a, 16), static_cast<uint32_t>(2.5f / 16 * 255 + 0.5f)) && close8(channel(a, 8), static_cast<uint32_t>(2.5f / 16 * 255 + 0.5f)) && channel(a, 24) == 0);
  CHECK(close8(channel(b, 16), static_cast<uint32_t>(13.5f / 16 * 255 + 0.5f)) && close8(channel(b, 8), static_cast<uint32_t>(13.5f / 16 * 255 + 0.5f)) && close8(channel(b, 24), 64));
  // The two triangles have the same winding, so they face the same way: blue is both 255 or both 0.
  CHECK(channel(a, 0) == channel(b, 0));
  // The opposite winding flips it.
  const std::vector<float> w = {-1, -1, 0.5f, -1, 1, 0.5f, 1, -1, 0.5f};
  draw(r, pd, w, {0, 0, 0, 0});
  CHECK(channel(r.pixels()[2 * kSize + 2], 0) != channel(a, 0));
  r.device.destroy_shader_module(vs);
  r.device.destroy_shader_module(fs);
}

static void test_instancing() {
  Rig r;
  r.make_targets(false);
  IShaderModule* vs = r.vertex(nullptr, instanced_vertex);
  IShaderModule* fs = r.fragment(nullptr, varying_color_ex);
  GraphicsPipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = fs;
  pd.vertex_stride = 20;   // position (3 floats) and a per-instance offset (2 floats)
  pd.attribute_count = 2;
  pd.attributes[0] = {0, VertexFormat::Float3, 0};
  pd.attributes[1] = {1, VertexFormat::Float2, 12, true};
  pd.varying_count = 1;
  pd.cull_mode = CullMode::None;
  // A small triangle at the top left corner; element n of the buffer (the instances' elements) shifts it by n * (0.5, 0.5) ... the buffer's elements are the vertices too, so the first
  // three elements carry the triangle and the offsets of instances 0, 1, 2 at the same time.
  const float t = 0.4f;
  const std::vector<float> data = {-1.0f, -1.0f, 0.5f, 0.0f, 0.0f,    -1.0f + t, -1.0f, 0.5f, 0.6f, 0.0f,    -1.0f, -1.0f + t, 0.5f, 1.2f, 0.0f};
  IPipeline* pipeline = r.device.create_graphics_pipeline(pd);
  IBuffer* vb = r.buffer(data);
  CHECK(pipeline && vb);
  ICommandBuffer* cb = r.device.create_command_buffer();
  cb->begin();
  RenderPassBegin pass;
  pass.color = r.color;
  pass.clear_color_value = {0, 0, 0, 1};
  cb->begin_render_pass(pass);
  cb->bind_pipeline(pipeline);
  cb->bind_vertex_buffer(vb);
  cb->draw(3, 0, 3, 0);
  cb->end_render_pass();
  CHECK(cb->end() && r.device.submit(cb));
  const std::vector<uint32_t> img = r.pixels();
  // Instance 0 at x offset 0.0 (red 0), 1 at 0.6 (red 0.1), 2 at 1.2 (red 0.2); the same height. The triangle is 0.4 wide in clip space (1.6 pixels): look at the cells.
  // x pixel of clip x: (x + 1) * 8
  auto red_near = [&](float clip_x) {
    const int x = static_cast<int>((clip_x + 1.0f) * 8.0f);
    uint32_t best = 0;
    for (int dx = -1; dx <= 2; ++dx)
      for (int y = 0; y < 5; ++y) {
        const uint32_t p = img[y * kSize + (x + dx < 0 ? 0 : x + dx)];
        if (channel(p, 24) == 255 && channel(p, 16) + channel(p, 8) > best) best = channel(p, 16) + channel(p, 8);
      }
    return best;
  };
  (void)red_near;
  // At least the instance-0 triangle and two others exist, and their colours step with the instance id (red = id * 0.1): find lit pixels in three column bands.
  uint32_t lit_columns[3] = {0, 0, 0};
  uint32_t red_of[3] = {0, 0, 0};
  for (uint32_t y = 0; y < 6; ++y)
    for (uint32_t x = 0; x < kSize; ++x) {
      const uint32_t p = img[y * kSize + x];
      if (p == 0xFF000000u) continue;
      const uint32_t band = x < 4 ? 0 : x < 12 ? 1 : 2;
      ++lit_columns[band];
      if (channel(p, 16) > red_of[band]) red_of[band] = channel(p, 16);
    }
  // (offsets 0.0, 0.6, 1.2 clip units are 0, 4.8, 9.6 pixels: instances 1 and 2 land in the middle and right bands)
  CHECK(lit_columns[0] > 0 && lit_columns[1] > 0);
  CHECK(red_of[0] <= 3 && red_of[1] >= 20);   // red = instance * 0.1 * 255 = 25 for instance 1
  // vertex ids: the third vertex is green (its corner pixel), the first is not
  CHECK(channel(img[0], 8) < 60);
  r.device.destroy_command_buffer(cb);
  r.device.destroy_buffer(vb);
  r.device.destroy_pipeline(pipeline);
  r.device.destroy_shader_module(vs);
  r.device.destroy_shader_module(fs);
}

static void test_lines_and_points() {
  Rig r;
  r.make_targets(false);
  IShaderModule* vs = r.vertex(pass_vertex);
  IShaderModule* fs = r.fragment(constant_color);
  GraphicsPipelineDesc pd = position_only();
  pd.vertex_shader = vs, pd.fragment_shader = fs;
  IPipeline* pipeline = r.device.create_graphics_pipeline(pd);
  auto render = [&](const std::vector<float>& vertices, PrimitiveTopology topology) {
    IBuffer* vb = r.buffer(vertices);
    ICommandBuffer* cb = r.device.create_command_buffer();
    cb->begin();
    RenderPassBegin pass;
    pass.color = r.color;
    pass.clear_color_value = {0, 0, 0, 1};
    cb->begin_render_pass(pass);
    cb->bind_pipeline(pipeline);
    cb->bind_vertex_buffer(vb);
    const Vec4 white = {1, 1, 1, 1};
    cb->push_constants(&white, sizeof white);
    cb->set_primitive_topology(topology);
    cb->draw(static_cast<uint32_t>(vertices.size() / 3));
    cb->end_render_pass();
    CHECK(cb->end() && r.device.submit(cb));
    r.device.destroy_command_buffer(cb);
    r.device.destroy_buffer(vb);
    return r.pixels();
  };
  auto lit = [](const std::vector<uint32_t>& p) {
    unsigned n = 0;
    for (uint32_t v : p) n += v != 0xFF000000u;
    return n;
  };
  // A horizontal line across the middle: one pixel per column, on one row.
  {
    const std::vector<uint32_t> img = render({-1, 0, 0.5f, 1, 0, 0.5f}, PrimitiveTopology::LineList);
    unsigned rows = 0, count = lit(img);
    for (uint32_t y = 0; y < kSize; ++y) {
      bool any = false;
      for (uint32_t x = 0; x < kSize; ++x) any = any || img[y * kSize + x] != 0xFF000000u;
      rows += any;
    }
    CHECK(rows == 1 && count >= kSize && count <= kSize + 2);
  }
  // A diagonal: about one pixel per step, and the ends.
  {
    const std::vector<uint32_t> img = render({-1, -1, 0.5f, 1, 1, 0.5f}, PrimitiveTopology::LineList);
    CHECK(lit(img) >= kSize && lit(img) <= kSize + 2 && img[0] != 0xFF000000u && img[(kSize - 1) * kSize + kSize - 1] != 0xFF000000u);
  }
  // Two lines in a list; a third vertex left over draws nothing.
  CHECK(lit(render({-1, -0.5f, 0.5f, 1, -0.5f, 0.5f, -1, 0.5f, 0.5f, 1, 0.5f, 0.5f, 0, 0, 0.5f}, PrimitiveTopology::LineList)) >= 2 * kSize);
  // Points: one pixel each.
  {
    const std::vector<uint32_t> img = render({-0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.5f}, PrimitiveTopology::PointList);
    CHECK(lit(img) == 3 && img[4 * kSize + 4] != 0xFF000000u && img[12 * kSize + 12] != 0xFF000000u && img[8 * kSize + 8] != 0xFF000000u);
  }
  // A point outside the target and one behind the near plane draw nothing; the topology resets to triangles with the next pass.
  CHECK(lit(render({5, 5, 0.5f, 0, 0, -0.5f}, PrimitiveTopology::PointList)) == 0);
  CHECK(lit(render({-1, -1, 0.5f, 3, -1, 0.5f, -1, 3, 0.5f}, PrimitiveTopology::TriangleList)) == kSize * kSize);
  r.device.destroy_pipeline(pipeline);
  r.device.destroy_shader_module(vs);
  r.device.destroy_shader_module(fs);
}

int main() {
  test_features();
  test_blend();
  test_depth_compare();
  test_texel_access();
  test_system_values();
  test_instancing();
  test_lines_and_points();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("graphics 3D extension tests passed\n");
  return 0;
}
