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
#include "Kernel/Dxgkrnl/Conformance.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Kernel/Dxgkrnl/ConformanceBlobs.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk {

namespace {

constexpr uint32_t kSize = 32;
constexpr uint32_t kPixels = kSize * kSize;
constexpr ProcessId kProcess = 0x7FFFFFF1;
constexpr uint32_t kTolerance = 10;      // per channel (the rasterization rules differ a little at the edges)
constexpr uint32_t kAllowedBad = 24;     // pixels

const uint32_t kVertexTokens[] = {0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000, 0x0200001F, 0x8000000A, 0x900F0001, 0x03000014, 0xC00F0000, 0x90E40000, 0xA0E40000,
                                  0x02000001, 0xD00F0000, 0x90E40001, 0x0000FFFF};
const uint32_t kPixelTokens[] = {0xFFFF0200, 0x0200001F, 0x8000000A, 0x900F0000, 0x02000001, 0x800F0800, 0x90E40000, 0x0000FFFF};
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

ConstByteView bytes_of(const void* p, uint64_t n) { return ConstByteView(static_cast<const uint8_t*>(p), n); }

// One process with a device and a context on one adapter, and the steps a user-mode driver takes, each remembering the first one that failed.
class Rig {
 public:
  Rig(allocators::IAllocator& allocator, Kernel& kernel, uint32_t adapter_index) : allocator_(allocator), kernel_(kernel) {
    opened_ = step("open_process", kernel.open_process(kProcess));
    if (opened_)
      step("open_adapter", kernel.open_adapter_from_index(kProcess, adapter_index, &adapter_)) && step("query_adapter_caps", kernel.query_adapter_caps(kProcess, adapter_, &caps_)) &&
          step("create_device", kernel.create_device(kProcess, adapter_, &device_)) && step("create_context", kernel.create_context(kProcess, device_, ContextPriority::Normal, &context_));
  }
  ~Rig() {
    if (opened_) kernel_.close_process(kProcess);
  }

  allocators::IAllocator& allocator() { return allocator_; }
  const AdapterCaps& caps() const { return caps_; }
  bool ok() const { return status_ == status::kSuccess; }
  NtStatus status() const { return status_; }
  const char* where() const { return where_; }
  bool has(uint32_t feature) const { return (caps_.features & feature) == feature; }

  bool step(const char* name, NtStatus s) {
    if (s != status::kSuccess && status_ == status::kSuccess) status_ = s, where_ = name;
    return s == status::kSuccess;
  }

  Handle shader(const char* name, ShaderKind kind, graphics::ShaderStage stage, ConstByteView code) {
    Handle h = 0;
    ShaderDesc d;
    d.kind = kind, d.stage = stage, d.code = code;
    step(name, ok() ? kernel_.create_shader(kProcess, device_, d, &h) : status_);
    return h;
  }
  Handle vertex_shader(ConstByteView code, ShaderKind kind = ShaderKind::Dxbc) { return shader("create_shader (vertex)", kind, graphics::ShaderStage::Vertex, code); }
  Handle pixel_shader(ConstByteView code, ShaderKind kind = ShaderKind::Dxbc) { return shader("create_shader (pixel)", kind, graphics::ShaderStage::Fragment, code); }
  Handle pipeline(const PipelineDesc& d) {
    Handle h = 0;
    step("create_pipeline", ok() ? kernel_.create_pipeline(kProcess, device_, d, &h) : status_);
    return h;
  }
  Handle image(uint32_t flags, Format f, uint32_t w, uint32_t h) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h;
    Handle out = 0;
    step("create_allocation (image)", ok() ? kernel_.create_allocation(kProcess, device_, d, &out) : status_);
    return out;
  }
  Handle buffer(uint32_t flags, ConstByteView contents) {
    AllocationDesc d;
    d.flags = flags, d.size = contents.size();
    Handle out = 0;
    if (!step("create_allocation (buffer)", ok() ? kernel_.create_allocation(kProcess, device_, d, &out) : status_)) return 0;
    write(out, contents);
    return out;
  }
  void write(Handle a, ConstByteView contents) {
    LockResult lock;
    if (!step("lock", ok() ? kernel_.lock(kProcess, a, kLockDiscard, 0, &lock) : status_)) return;
    const bool copied = lock.data.copy_from(contents);
    kernel_.unlock(kProcess, a);
    step("write", copied ? status::kSuccess : status::kInvalidParameter);
  }
  // Reads a kSize x kSize image.
  void read(Handle a, ArrayView<uint32_t> out) {
    LockResult lock;
    if (!step("lock (read)", ok() ? kernel_.lock(kProcess, a, kLockReadOnly, 0, &lock) : status_)) return;
    bool good = true;
    for (uint32_t y = 0; y < kSize && good; ++y) {
      ByteView row;
      good = lock.data.try_subview(uint64_t{y} * lock.pitch, kSize * 4, &row);
      ByteReader in(good ? ConstByteView(row.data(), row.size()) : ConstByteView());
      for (uint32_t x = 0; good && x < kSize; ++x) out[uint64_t{y} * kSize + x] = in.le32();
      good = good && in.ok();
    }
    kernel_.unlock(kProcess, a);
    step("read", good ? status::kSuccess : status::kInvalidParameter);
  }
  // Runs a stream; the context is checked afterwards (a stream the device cannot run loses it).
  void render(const CommandWriter& commands, std::initializer_list<Handle> list) {
    if (!ok()) return;
    Handle handles[8];
    uint32_t n = 0;
    for (Handle h : list) handles[n++] = h;
    if (!step("render", commands.ok() ? kernel_.render(kProcess, context_, commands.view(), ArrayView<const Handle>(handles, n)) : status::kNoMemory)) return;
    DeviceState state;
    if (!step("device state", kernel_.get_device_state(kProcess, device_, &state))) return;
    step("render (device)", state.execution == DeviceExecState::Running ? status::kSuccess : (state.last_fault ? state.last_fault : status::kDeviceRemoved));
  }

 private:
  allocators::IAllocator& allocator_;
  Kernel& kernel_;
  Handle adapter_ = 0, device_ = 0, context_ = 0;
  AdapterCaps caps_;
  NtStatus status_ = status::kSuccess;
  const char* where_ = "";
  bool opened_ = false;
};

// Vertices are position (3 floats) and colour (4 floats): 28 bytes.
struct Vertex {
  float x, y, z, r, g, b, a;
};

PipelineDesc colour_pipeline_desc(Handle vs, Handle ps) {
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 1;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  return pd;
}

Handle d3d9_pipeline(Rig& rig, bool depth, bool blend) {
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.depth_test = pd.depth_write = depth;
  pd.alpha_blend = blend;
  return rig.pipeline(pd);
}

Handle dxbc_pipeline(Rig& rig, const uint8_t* vs, uint32_t vs_size, const uint8_t* ps, uint32_t ps_size, uint32_t textures) {
  Handle v = rig.vertex_shader(bytes_of(vs, vs_size));
  Handle p = rig.pixel_shader(bytes_of(ps, ps_size));
  PipelineDesc pd = colour_pipeline_desc(v, p);
  pd.varying_count = 3;
  pd.texture_count = textures;
  return rig.pipeline(pd);
}

void begin(CommandWriter& w, uint32_t color, uint32_t depth, graphics::Vec4 clear) {
  w.begin_pass(color, depth, kPassClearColor | (depth == kNoAllocation ? 0u : kPassClearDepth | kPassClearStencil), clear, 1.0f, 0);
}

using Scene = void (*)(Rig& rig, ArrayView<uint32_t> out);

// --- The scenes -------------------------------------------------------------------------------------------------------------------------------------------------------------

void scene_vertex_colours(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[3] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  Handle pipe = d3d9_pipeline(rig, false, false);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

// Shader Model 3: vs_3_0 (vs.hlsl) and ps_3_0 (the colour), real bytecode of the Direct3D 9 compiler.
void scene_sm3(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[3] = {{-0.9f, -0.9f, 0.5f, 1, 0, 0, 1}, {0.9f, -0.7f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.9f, 0.5f, 0, 0, 1, 1}};
  namespace b = conformance_blobs;
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs_3_0, sizeof b::k_vs_3_0), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color_3_0, sizeof b::k_ps_color_3_0), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  Handle pipe = rig.pipeline(pd);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

// Shader Model 1: vs_1_1 and ps_1_4 (a lerp of the two colours by the alpha of c0, times one half, plus their product).
void scene_sm1(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[3] = {{-0.9f, -0.9f, 0.5f, 1, 0, 0, 1}, {0.9f, -0.7f, 0.5f, 0, 1, 0, 0.5f}, {0.0f, 0.9f, 0.5f, 0, 0, 1, 0.25f}};
  namespace b = conformance_blobs;
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs_1_1, sizeof b::k_vs_1_1), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps1_lerp_1_4, sizeof b::k_ps1_lerp_1_4), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  Handle pipe = rig.pipeline(pd);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  float block[16];
  for (uint32_t i = 0; i < 16; ++i) block[i] = kIdentity[i];
  block[3] = 0.6f;   // c0.w, the lerp factor (the matrix uses the same registers)
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(block, sizeof block));
  w.draw(3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

void scene_depth(Rig& rig, ArrayView<uint32_t> out) {
  // Far red, then near green, then farther blue: where they overlap the green one is in front, whatever the order.
  const Vertex v[9] = {{-0.9f, -0.9f, 0.6f, 1, 0, 0, 1}, {0.5f, -0.9f, 0.6f, 1, 0, 0, 1}, {-0.9f, 0.5f, 0.6f, 1, 0, 0, 1},
                       {-0.5f, -0.5f, 0.3f, 0, 1, 0, 1}, {0.9f, -0.5f, 0.3f, 0, 1, 0, 1}, {-0.5f, 0.9f, 0.3f, 0, 1, 0, 1},
                       {-0.9f, -0.3f, 0.8f, 0, 0, 1, 1}, {0.9f, -0.3f, 0.8f, 0, 0, 1, 1}, {0.0f, 0.9f, 0.8f, 0, 0, 1, 1}};
  Handle pipe = d3d9_pipeline(rig, true, false);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle depth = rig.image(kAllocDepthStencil, Format::D32Float, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, 1, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(9);
  w.end_pass();
  rig.render(w, {target, depth, vb});
  rig.read(target, out);
}

void scene_blend(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[6] = {{-1, -1, 0.5f, 0.2f, 0.4f, 0.8f, 1}, {3, -1, 0.5f, 0.2f, 0.4f, 0.8f, 1}, {-1, 3, 0.5f, 0.2f, 0.4f, 0.8f, 1},
                       {-0.7f, -0.7f, 0.5f, 1, 0.5f, 0, 0.5f}, {0.8f, -0.7f, 0.5f, 1, 0.5f, 0, 0.5f}, {0.0f, 0.8f, 0.5f, 1, 0.5f, 0, 0.5f}};
  Handle opaque = d3d9_pipeline(rig, false, false);
  Handle blended = d3d9_pipeline(rig, false, true);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_pipeline(opaque);
  w.draw(3, 0);
  w.bind_pipeline(blended);
  w.draw(3, 3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

// vs.dxbc: position * the matrix in b0, colour through, texcoord = colour.xy * 2. ps_color.dxbc: the colour.
void scene_dxbc_constants(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[3] = {{-1, -1, 0.5f, 1, 0, 0, 1}, {1, -1, 0.5f, 0, 1, 0, 1}, {0, 1, 0.5f, 0, 0, 1, 1}};
  const float matrix[16] = {0.5f, 0, 0, 0, 0, 0.75f, 0, 0, 0, 0, 1, 0, 0.25f, -0.1f, 0, 1};
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_color, sizeof b::k_ps_color, 0);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(matrix, sizeof matrix));
  w.draw(3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

Handle checker(Rig& rig) {
  uint32_t texels[16];
  for (uint32_t i = 0; i < 16; ++i) texels[i] = ((i % 4 + i / 4) & 1) ? 0xFFFFA020u : 0xFF2060F0u;
  texels[5] = 0xFFFFFFFFu;
  Handle t = rig.image(kAllocTexture, Format::A8R8G8B8, 4, 4);
  rig.write(t, bytes_of(texels, sizeof texels));
  return t;
}

// A quad of two triangles whose colour is (u / 2, v / 2, 1, 1), which vs.dxbc turns into the texture coordinate (u, v).
void textured_quad(Rig& rig, Handle pipe, Handle texture, Handle target, graphics::Filter filter, ArrayView<uint32_t> out) {
  auto vert = [](float x, float y, float u, float v) { return Vertex{x, y, 0.5f, u / 2, v / 2, 1, 1}; };
  const Vertex v[6] = {vert(-0.9f, -0.9f, 0, 0), vert(0.9f, -0.9f, 1, 0), vert(-0.9f, 0.9f, 0, 1), vert(-0.9f, 0.9f, 0, 1), vert(0.9f, -0.9f, 1, 0), vert(0.9f, 0.9f, 1, 1)};
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  graphics::SamplerDesc sampler;
  sampler.filter = filter;
  sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_texture(0, 2, sampler);
  w.draw(6);
  w.end_pass();
  rig.render(w, {target, vb, texture});
  rig.read(target, out);
}

void scene_dxbc_texture(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_tex, sizeof b::k_ps_tex, 1);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle texture = checker(rig);
  textured_quad(rig, pipe, texture, target, graphics::Filter::Nearest, out);
}

void scene_dxbc_texture_linear(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_tex, sizeof b::k_ps_tex, 1);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle texture = checker(rig);
  textured_quad(rig, pipe, texture, target, graphics::Filter::Linear, out);
}

// Draw a picture into a texture, then draw the texture onto the screen: a Direct3D 9 render target that is also a texture, and every Direct3D 11 / 12 post-processing step.
void scene_render_to_texture(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle first = d3d9_pipeline(rig, false, false);
  Handle second = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_tex, sizeof b::k_ps_tex, 1);
  Handle texture = rig.image(kAllocRenderTarget | kAllocTexture, Format::A8R8G8B8, kSize, kSize);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  const Vertex tri[3] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  Handle tvb = rig.buffer(kAllocVertexBuffer, bytes_of(tri, sizeof tri));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0.1f, 0.1f, 0.1f, 1});
  w.bind_pipeline(first);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  rig.render(w, {texture, target, tvb});
  auto vert = [](float x, float y, float u, float v) { return Vertex{x, y, 0.5f, u / 2, v / 2, 1, 1}; };
  const Vertex quad[6] = {vert(-0.6f, -0.6f, 0, 0), vert(0.9f, -0.6f, 1, 0), vert(-0.6f, 0.9f, 0, 1), vert(-0.6f, 0.9f, 0, 1), vert(0.9f, -0.6f, 1, 0), vert(0.9f, 0.9f, 1, 1)};
  Handle qvb = rig.buffer(kAllocVertexBuffer, bytes_of(quad, sizeof quad));
  graphics::SamplerDesc sampler;
  sampler.filter = graphics::Filter::Linear;
  sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
  CommandWriter w2(rig.allocator());
  begin(w2, 0, kNoAllocation, {0, 0, 0, 1});
  w2.bind_pipeline(second);
  w2.bind_vertex_buffer(1);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.bind_texture(0, 2, sampler);
  w2.draw(6);
  w2.end_pass();
  rig.render(w2, {target, qvb, texture});
  rig.read(target, out);
}

void scene_strip(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[4] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {-0.8f, 0.8f, 0.5f, 0, 0, 1, 1}, {0.8f, 0.8f, 0.5f, 1, 1, 0, 1}};
  Handle pipe = d3d9_pipeline(rig, false, false);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.set_topology(Topology::TriangleStrip);
  w.draw(4);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

void scene_indexed(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[4] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {-0.8f, 0.8f, 0.5f, 0, 0, 1, 1}, {0.8f, 0.8f, 0.5f, 1, 1, 0, 1}};
  const uint16_t indices[8] = {0, 1, 2, 2, 1, 3, 0, 0};
  Handle pipe = d3d9_pipeline(rig, false, false);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  Handle ib = rig.buffer(kAllocIndexBuffer, bytes_of(indices, sizeof indices));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.bind_index_buffer(2, graphics::IndexType::Uint16);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw_indexed(6);
  w.end_pass();
  rig.render(w, {target, vb, ib});
  rig.read(target, out);
}

void scene_viewport_scissor(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[3] = {{-1, -1, 0.5f, 1, 0, 0, 1}, {3, -1, 0.5f, 0, 1, 0, 1}, {-1, 3, 0.5f, 0, 0, 1, 1}};
  Handle pipe = d3d9_pipeline(rig, false, false);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.set_viewport({4, 4, 24, 24, 0, 1});
  w.set_scissor({8, 6, 12, 14});
  w.draw(3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

// A triangle that winds counter-clockwise on the left, one that winds clockwise on the right; back faces are culled, with each winding being the front in turn (the top and the
// bottom half of the picture, each with its own scissor).
void culling(Rig& rig, uint32_t target_flags, ArrayView<uint32_t> out) {
  const Vertex v[6] = {{-0.9f, -0.8f, 0.5f, 1, 0, 0, 1}, {-0.1f, -0.8f, 0.5f, 1, 0, 0, 1}, {-0.5f, 0.8f, 0.5f, 1, 0, 0, 1},
                       {0.1f, -0.8f, 0.5f, 0, 0, 1, 1}, {0.5f, 0.8f, 0.5f, 0, 0, 1, 1}, {0.9f, -0.8f, 0.5f, 0, 0, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc ccw = colour_pipeline_desc(vs, ps), cw = ccw;
  ccw.cull_mode = cw.cull_mode = graphics::CullMode::Back;
  ccw.front_face = graphics::FrontFace::CounterClockwise, cw.front_face = graphics::FrontFace::Clockwise;
  Handle first = rig.pipeline(ccw), second = rig.pipeline(cw);
  Handle target = rig.image(target_flags, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.set_scissor({0, 0, kSize, kSize / 2});
  w.bind_pipeline(first);
  w.draw(6);
  w.set_scissor({0, kSize / 2, kSize, kSize / 2});
  w.bind_pipeline(second);
  w.draw(6);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

void scene_culling(Rig& rig, ArrayView<uint32_t> out) { culling(rig, kAllocRenderTarget, out); }
void scene_culling_texture(Rig& rig, ArrayView<uint32_t> out) { culling(rig, kAllocRenderTarget | kAllocTexture, out); }
void scene_culling_back_buffer(Rig& rig, ArrayView<uint32_t> out) { culling(rig, kAllocRenderTarget | kAllocPresent, out); }

void scene_copy_region(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[3] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  Handle pipe = d3d9_pipeline(rig, false, false);
  Handle source = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 1, kNoAllocation, {0, 0.3f, 0, 1});
  w.end_pass();
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  w.copy_region(1, 2, 3, 0, 8, 8, 16, 16);
  rig.render(w, {source, target, vb});
  rig.read(target, out);
}

// --- Direct3D 10 and 11 features (a device says whether it has them) ---------------------------------------------------------------------------------------------------

void scene_stencil(Rig& rig, ArrayView<uint32_t> out) {
  // A small triangle writes 1 to the stencil plane (and the picture); then a big one is drawn only where the plane says 1.
  const Vertex v[6] = {{-0.4f, -0.5f, 0.5f, 1, 0, 0, 1}, {0.6f, -0.3f, 0.5f, 1, 0, 0, 1}, {-0.1f, 0.7f, 0.5f, 1, 0, 0, 1},
                       {-1, -1, 0.5f, 0, 1, 0, 1}, {3, -1, 0.5f, 0, 1, 0, 1}, {-1, 3, 0.5f, 0, 1, 0, 1}};
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc mark = colour_pipeline_desc(vs, ps), test = mark;
  mark.stencil.enable = true;
  mark.stencil.compare = graphics::CompareOp::Always, mark.stencil.pass_op = graphics::StencilOp::Replace, mark.stencil.reference = 1;
  test.stencil.enable = true;
  test.stencil.compare = graphics::CompareOp::Equal, test.stencil.reference = 1;
  if (rig.step("stencil", rig.caps().stencil ? status::kSuccess : status::kNotSupported) == false) return;
  Handle first = rig.pipeline(mark), second = rig.pipeline(test);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle depth = rig.image(kAllocDepthStencil, Format::D24S8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, 1, {0, 0, 0, 1});
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_pipeline(first);
  w.draw(3, 0);
  w.bind_pipeline(second);
  w.draw(3, 3);
  w.end_pass();
  rig.render(w, {target, depth, vb});
  rig.read(target, out);
}

void scene_depth_compare(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureBlendState)) return void(rig.step("depth comparison", status::kNotSupported));
  // Greater: the later, nearer-than-nothing triangle only shows where it is farther than what is there (the clear is 0).
  const Vertex v[6] = {{-0.9f, -0.9f, 0.4f, 1, 0, 0, 1}, {0.7f, -0.9f, 0.4f, 1, 0, 0, 1}, {-0.9f, 0.7f, 0.4f, 1, 0, 0, 1},
                       {-0.5f, -0.5f, 0.7f, 0, 1, 0, 1}, {0.9f, -0.5f, 0.7f, 0, 1, 0, 1}, {-0.5f, 0.9f, 0.7f, 0, 1, 0, 1}};
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.depth_test = pd.depth_write = true;
  pd.depth_compare = graphics::CompareOp::Greater;
  Handle pipe = rig.pipeline(pd);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle depth = rig.image(kAllocDepthStencil, Format::D32Float, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  w.begin_pass(0, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 0.0f, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(6);
  w.end_pass();
  rig.render(w, {target, depth, vb});
  rig.read(target, out);
}

void scene_lines_points(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureLines)) return void(rig.step("lines", status::kNotSupported));
  const Vertex v[6] = {{-0.8f, -0.6f, 0.5f, 1, 0, 0, 1}, {0.8f, 0.6f, 0.5f, 1, 1, 0, 1}, {-0.8f, 0.7f, 0.5f, 0, 1, 0, 1}, {0.7f, -0.7f, 0.5f, 0, 0, 1, 1}, {0.0f, 0.0f, 0.5f, 1, 1, 1, 1}, {0.5f, 0.3f, 0.5f, 1, 0, 1, 1}};
  Handle pipe = d3d9_pipeline(rig, false, false);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.set_topology(Topology::LineList);
  w.draw(4);
  w.set_topology(Topology::PointList);
  w.draw(2, 4);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

void scene_blend_state(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureBlendState)) return void(rig.step("blend state", status::kNotSupported));
  const Vertex v[6] = {{-1, -1, 0.5f, 0.2f, 0.4f, 0.8f, 1}, {3, -1, 0.5f, 0.2f, 0.4f, 0.8f, 1}, {-1, 3, 0.5f, 0.2f, 0.4f, 0.8f, 1},
                       {-0.7f, -0.7f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}, {0.8f, -0.7f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}, {0.0f, 0.8f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}};
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc plain = colour_pipeline_desc(vs, ps), custom = plain;
  custom.blend.custom = custom.blend.enable = true;
  custom.blend.src_color = graphics::BlendFactor::One, custom.blend.dst_color = graphics::BlendFactor::One;
  custom.blend.color_op = graphics::BlendOp::ReverseSubtract;
  custom.blend.src_alpha = graphics::BlendFactor::Zero, custom.blend.dst_alpha = graphics::BlendFactor::One;
  custom.blend.write_mask = 0x7;
  Handle first = rig.pipeline(plain), second = rig.pipeline(custom);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_pipeline(first);
  w.draw(3, 0);
  w.bind_pipeline(second);
  w.draw(3, 3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

// ps_cbuf.dxbc: a constant buffer of 64 registers read through two integer indices (computed from the texture coordinate); the first four registers are also vs.dxbc's matrix.
void scene_dxbc_constant_buffer(Rig& rig, ArrayView<uint32_t> out) {
  if (rig.caps().max_push_constant_bytes < 1024) return void(rig.step("constant buffer of 1024 bytes", status::kNotSupported));
  auto vert = [](float x, float y, float u, float v) { return Vertex{x, y, 0.5f, u / 2, v / 2, 0.5f, 1}; };
  const Vertex v[6] = {vert(-1, -1, 0, 0), vert(1, -1, 1, 0), vert(-1, 1, 0, 1), vert(-1, 1, 0, 1), vert(1, -1, 1, 0), vert(1, 1, 1, 1)};
  float block[64 * 4];
  for (uint32_t j = 0; j < 64; ++j) {
    block[j * 4 + 0] = static_cast<float>(j) / 64.0f, block[j * 4 + 1] = 1.0f - static_cast<float>(j) / 64.0f, block[j * 4 + 2] = static_cast<float>(j & 1), block[j * 4 + 3] = 0.25f;
  }
  for (uint32_t i = 0; i < 16; ++i) block[i] = kIdentity[i];
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_cbuf, sizeof b::k_ps_cbuf, 0);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(block, sizeof block));
  w.draw(6);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

// ps_math.dxbc: arithmetic, a branch, clip and the rounding family on two colours and a texture coordinate; its constants are the first two rows of the block (the identity here).
void scene_dxbc_math(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[3] = {{-1, -1, 0.5f, 0.9f, 0.3f, 0.2f, 1}, {3, -1, 0.5f, 0.1f, 0.8f, 0.4f, 1}, {-1, 3, 0.5f, 0.4f, 0.2f, 0.9f, 1}};
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_math, sizeof b::k_ps_math, 0);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

// ps_system.dxbc: Load, SampleLevel, GetDimensions and the pixel's SV_Position and SV_IsFrontFace.
void texel_access(Rig& rig, uint32_t target_flags, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureTexelAccess | graphics::kFeatureSystemInputs)) return void(rig.step("texel access", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_system, sizeof b::k_ps_system, 1);
  Handle target = rig.image(target_flags, Format::A8R8G8B8, kSize, kSize);
  Handle texture = checker(rig);
  textured_quad(rig, pipe, texture, target, graphics::Filter::Nearest, out);
}
void scene_texel_access(Rig& rig, ArrayView<uint32_t> out) { texel_access(rig, kAllocRenderTarget, out); }
void scene_texel_access_back_buffer(Rig& rig, ArrayView<uint32_t> out) { texel_access(rig, kAllocRenderTarget | kAllocPresent, out); }

// The colour steps with the instance, not the vertex: three instances of one triangle, the last one's colour (record 2's) is what stays.
void scene_instanced_colour(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureInstancing)) return void(rig.step("instancing", status::kNotSupported));
  const Vertex v[3] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.attributes[1].per_instance = true;
  Handle pipe = rig.pipeline(pd);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw_instanced(3, 3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

void scene_instancing(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureInstancing | graphics::kFeatureSystemInputs)) return void(rig.step("instancing", status::kNotSupported));
  // vs_ids.dxbc: position + per-instance offset; the colour says which instance (red) and whether the vertex is the third (green). Record n of the buffer is vertex n's position
  // and instance n's offset.
  const float records[3][5] = {{-0.2f, -0.2f, 0.5f, -0.5f, -0.5f}, {0.2f, -0.2f, 0.5f, 0.5f, -0.4f}, {0.0f, 0.2f, 0.5f, 0.0f, 0.5f}};
  namespace b = conformance_blobs;
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs_ids, sizeof b::k_vs_ids));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 20, pd.attribute_count = 2, pd.varying_count = 1;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float2, 12};
  pd.attributes[1].per_instance = true;
  pd.cull_mode = graphics::CullMode::None;
  Handle pipe = rig.pipeline(pd);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(records, sizeof records));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.draw_instanced(3, 3);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

struct Entry {
  const char* name;
  Scene run;
  bool edges_vary = false;   // lines and points: which pixels a line covers is up to the rasterizer, so a pixel may also match any of its neighbours
};

// Two render targets (SV_Target0 / SV_Target1 of ps_mrt.dxbc): the first holds the vertex colour, the second the same with red and blue swapped. The picture compared is
// the first target in the even columns and the second in the odd ones, so a target the device did not fill (or filled with the other's output) shows.
void scene_multiple_targets(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureMultipleTargets)) return void(rig.step("multiple render targets", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[3] = {{-0.9f, -0.9f, 0.5f, 1, 0.2f, 0, 1}, {0.9f, -0.9f, 0.5f, 0, 1, 0.3f, 1}, {0.0f, 0.9f, 0.5f, 0.1f, 0.2f, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_mrt, sizeof b::k_ps_mrt));
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  pd.color_target_count = 2;
  Handle pipe = rig.pipeline(pd);
  Handle first = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize), second = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  w.begin_pass_targets(0, {1}, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1.0f, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  rig.render(w, {first, second, vb});
  ArrayView<uint32_t> other = allocate_view<uint32_t>(rig.allocator(), uint64_t{kSize} * kSize);
  if (other.empty()) return void(rig.step("memory", status::kNoMemory));
  rig.read(first, out);
  rig.read(second, other);
  for (uint32_t i = 1; i < kSize * kSize; i += 2) out[i] = other[i];
  release_view(rig.allocator(), other);
}

// A float render target (half or single precision): two triangles are added into it (One + One), so where they overlap the colour is above 1 - which a clamping target would lose - and
// the target is then sampled as a texture, scaled by 0.4 (ps_scale.dxbc), into an ordinary target.
void float_target_scene(Rig& rig, ArrayView<uint32_t> out, Format format) {
  if (!rig.has(graphics::kFeatureFloatFormats | graphics::kFeatureBlendState)) return void(rig.step("float formats", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex tri[6] = {{-0.8f, -0.8f, 0.5f, 1.5f, 0.25f, 0.1f, 1.0f}, {0.8f, -0.8f, 0.5f, 1.5f, 0.25f, 0.1f, 1.0f}, {0.0f, 0.8f, 0.5f, 1.5f, 0.25f, 0.1f, 1.0f},
                         {-0.8f, 0.6f, 0.5f, 1.0f, 0.75f, 0.4f, 0.5f}, {0.8f, 0.6f, 0.5f, 1.0f, 0.75f, 0.4f, 0.5f}, {0.0f, -0.9f, 0.5f, 1.0f, 0.75f, 0.4f, 0.5f}};
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc add = colour_pipeline_desc(vs, ps);
  add.blend.custom = add.blend.enable = true;
  add.blend.src_color = add.blend.dst_color = add.blend.src_alpha = add.blend.dst_alpha = graphics::BlendFactor::One;
  Handle first = rig.pipeline(add);
  Handle second = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_scale, sizeof b::k_ps_scale, 1);
  Handle texture = rig.image(kAllocRenderTarget | kAllocTexture, format, kSize, kSize);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle tvb = rig.buffer(kAllocVertexBuffer, bytes_of(tri, sizeof tri));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 0});
  w.bind_pipeline(first);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(6);
  w.end_pass();
  rig.render(w, {texture, target, tvb});
  // (the vertex shader makes the texture coordinate from the colour: u / 2, v / 2)
  auto vert = [](float x, float y, float u, float v) { return Vertex{x, y, 0.5f, u / 2, v / 2, 1, 1}; };
  const Vertex quad[6] = {vert(-1, -1, 0, 0), vert(1, -1, 1, 0), vert(-1, 1, 0, 1), vert(-1, 1, 0, 1), vert(1, -1, 1, 0), vert(1, 1, 1, 1)};
  Handle qvb = rig.buffer(kAllocVertexBuffer, bytes_of(quad, sizeof quad));
  graphics::SamplerDesc sampler;
  sampler.filter = graphics::Filter::Nearest;
  sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
  CommandWriter w2(rig.allocator());
  begin(w2, 0, kNoAllocation, {0, 0, 0, 1});
  w2.bind_pipeline(second);
  w2.bind_vertex_buffer(1);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.bind_texture(0, 2, sampler);
  w2.draw(6);
  w2.end_pass();
  rig.render(w2, {target, qvb, texture});
  rig.read(target, out);
}
void scene_half_float_target(Rig& rig, ArrayView<uint32_t> out) { float_target_scene(rig, out, Format::A16B16G16R16F); }
void scene_float_target(Rig& rig, ArrayView<uint32_t> out) { float_target_scene(rig, out, Format::A32B32G32R32F); }

const Entry kScenes[] = {
    {"Direct3D 9 vertex colours", scene_vertex_colours},
    {"Direct3D 9 Shader Model 3", scene_sm3},
    {"Direct3D 9 Shader Model 1", scene_sm1},
    {"Direct3D 9 depth test", scene_depth},
    {"Direct3D 9 alpha blend", scene_blend},
    {"Direct3D 9 indexed draw", scene_indexed},
    {"Direct3D 9 viewport and scissor", scene_viewport_scissor},
    {"Direct3D 9 culling and winding", scene_culling},
    {"Direct3D 9 culling into a texture", scene_culling_texture},
    {"Direct3D 9 culling into a back buffer", scene_culling_back_buffer},
    {"Direct3D 9 copy between targets", scene_copy_region},
    {"Direct3D 10 constant buffer (DXBC)", scene_dxbc_constants},
    {"Direct3D 10 texture, point filter (DXBC)", scene_dxbc_texture},
    {"Direct3D 10 texture, linear filter (DXBC)", scene_dxbc_texture_linear},
    {"Direct3D 10 pixel shader arithmetic (DXBC)", scene_dxbc_math},
    {"Direct3D 10 indexed constant buffer, 1 KiB (DXBC)", scene_dxbc_constant_buffer},
    {"Direct3D 10 Load, SampleLevel, SV_Position, SV_IsFrontFace (DXBC)", scene_texel_access},
    {"Direct3D 10 the same into a back buffer", scene_texel_access_back_buffer},
    {"Direct3D 10 triangle strip", scene_strip},
    {"Direct3D 11 render to texture", scene_render_to_texture},
    {"Direct3D 10 blend state", scene_blend_state},
    {"Direct3D 10 depth comparison", scene_depth_compare},
    {"Direct3D 10 stencil", scene_stencil},
    {"Direct3D 10 lines and points", scene_lines_points, true},
    {"Direct3D 10 per-instance attribute", scene_instanced_colour},
    {"Direct3D 10 instancing", scene_instancing},
    {"Direct3D 10 multiple render targets (DXBC)", scene_multiple_targets},
    {"Direct3D 10 half-float render target and texture", scene_half_float_target},
    {"Direct3D 10 float render target and texture", scene_float_target},
};

bool channels_close(uint32_t a, uint32_t b) {
  for (uint32_t shift = 0; shift < 32; shift += 8) {
    const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
    if ((d < 0 ? -d : d) > static_cast<int32_t>(kTolerance)) return false;
  }
  return true;
}

bool pixel_matches(ArrayView<uint32_t> device, ArrayView<uint32_t> reference, uint64_t i, bool any_neighbour) {
  if (channels_close(device[i], reference[i])) return true;
  if (!any_neighbour) return false;
  const int32_t x = static_cast<int32_t>(i % kSize), y = static_cast<int32_t>(i / kSize);
  for (int32_t dy = -1; dy <= 1; ++dy)
    for (int32_t dx = -1; dx <= 1; ++dx) {
      const int32_t nx = x + dx, ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(kSize) || ny >= static_cast<int32_t>(kSize)) continue;
      if (channels_close(device[i], reference[uint64_t{static_cast<uint32_t>(ny)} * kSize + static_cast<uint32_t>(nx)])) return true;
    }
  return false;
}

}  // namespace

uint32_t run_conformance(allocators::IAllocator& allocator, Kernel& kernel, uint32_t adapter_index, uint32_t reference_index, SceneReport report, void* context) {
  uint32_t bad = 0;
  ArrayView<uint32_t> device_pixels = allocate_view<uint32_t>(allocator, kPixels), reference_pixels = allocate_view<uint32_t>(allocator, kPixels);
  if (!device_pixels.empty() && !reference_pixels.empty()) {
    for (const Entry& scene : kScenes) {
      SceneResult result;
      result.name = scene.name;
      device_pixels.fill(0xDEADBEEFu);
      reference_pixels.fill(0xDEADBEEFu);
      NtStatus status = status::kSuccess;
      const char* where = "";
      {
        Rig rig(allocator, kernel, adapter_index);
        scene.run(rig, device_pixels);
        status = rig.status(), where = rig.where();
      }
      {
        Rig reference(allocator, kernel, reference_index);
        scene.run(reference, reference_pixels);
        result.reference_failed = !reference.ok();
      }
      if (status == status::kNotSupported) {
        result.verdict = SceneVerdict::Refused, result.status = status, result.step = where;
      } else if (status != status::kSuccess) {
        result.verdict = SceneVerdict::Broken, result.status = status, result.step = where;
        ++bad;
      } else if (!result.reference_failed) {
        // (a scene that draws nothing in the reference proves nothing: both sides black would be "the same")
        uint32_t lit = 0;
        for (uint64_t i = 0; i < kPixels; ++i) lit += reference_pixels[i] != 0xFF000000u;
        if (lit < 16) {
          result.verdict = SceneVerdict::Broken, result.status = status::kInvalidParameter, result.step = "the reference drew nothing";
          ++bad;
        }
        for (uint64_t i = 0; i < kPixels && result.verdict == SceneVerdict::Same; ++i)
          if (!pixel_matches(device_pixels, reference_pixels, i, scene.edges_vary) && result.bad_pixels++ == 0)
            result.first_bad = static_cast<uint32_t>(i), result.device_value = device_pixels[i], result.reference_value = reference_pixels[i];
        if (result.bad_pixels > kAllowedBad) result.verdict = SceneVerdict::Differs, ++bad;
      }
      result.device_picture = device_pixels.data(), result.reference_picture = reference_pixels.data();
      if (report) report(context, result);
    }
  }
  if (!device_pixels.empty()) release_view(allocator, device_pixels);
  if (!reference_pixels.empty()) release_view(allocator, reference_pixels);
  return bad;
}

}  // namespace dxgk
