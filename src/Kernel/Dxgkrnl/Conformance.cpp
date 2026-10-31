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
  Kernel& kernel() { return kernel_; }
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
  Handle create(const AllocationDesc& d) {
    Handle out = 0;
    step("create_allocation", ok() ? kernel_.create_allocation(kProcess, device_, d, &out) : status_);
    return out;
  }
  Handle root_signature(ConstByteView blob) {
    Handle h = 0;
    step("create_root_signature", ok() ? kernel_.create_root_signature(kProcess, device_, blob, &h) : status_);
    return h;
  }
  Handle descriptor_heap(DescriptorHeapType type, uint32_t count) {
    Handle h = 0;
    step("create_descriptor_heap", ok() ? kernel_.create_descriptor_heap(kProcess, device_, type, count, &h) : status_);
    return h;
  }
  void descriptor(Handle heap, uint32_t index, const DescriptorDesc& d) { step("write_descriptor", ok() ? kernel_.write_descriptor(kProcess, heap, index, d) : status_); }
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
  // Reads `out.size()` little-endian words from the start of a buffer.
  void read_words(Handle a, ArrayView<uint32_t> out) {
    LockResult lock;
    if (!step("lock (read words)", ok() ? kernel_.lock(kProcess, a, kLockReadOnly, 0, &lock) : status_)) return;
    ByteReader in(ConstByteView(lock.data.data(), lock.data.size()));
    for (uint32_t& w : out) w = in.le32();
    kernel_.unlock(kProcess, a);
    step("read words", in.ok() ? status::kSuccess : status::kInvalidParameter);
  }
  // Reads the little-endian 64-bit number `index` of a buffer.
  uint64_t read_u64(Handle a, uint32_t index) {
    LockResult lock;
    if (!step("lock (read numbers)", ok() ? kernel_.lock(kProcess, a, kLockReadOnly, 0, &lock) : status_)) return 0;
    ByteReader in(ConstByteView(lock.data.data(), lock.data.size()));
    in.skip(uint64_t{index} * 8);
    const uint64_t value = in.le64();
    kernel_.unlock(kProcess, a);
    step("read numbers", in.ok() ? status::kSuccess : status::kInvalidParameter);
    return value;
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

// gather4 (dxil_ps_gather.hlsl): the four texels of the footprint, green and red, over the checker texture; a linear sampler (gather ignores the filter, not the address mode).
void scene_dxil_gather(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_dxil_ps_gather, sizeof b::k_dxil_ps_gather, 1);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle texture = checker(rig);
  textured_quad(rig, pipe, texture, target, graphics::Filter::Linear, out);
}

// Derivatives (dxil_ps_deriv_show.hlsl): ddx_fine / ddy_fine of values computed from the interpolated colour and texture coordinate, shown as colours; the quad's pairs on the CPU renderer
// against the hardware's.
void scene_dxil_derivatives(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_dxil_ps_deriv_show, sizeof b::k_dxil_ps_deriv_show, 1);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle texture = checker(rig);
  textured_quad(rig, pipe, texture, target, graphics::Filter::Nearest, out);
}

// sample_d and sample_b (dxil_ps_grad.hlsl) over a texture of one level: the level is clamped to it, so the picture is the plain lookup; what the scene checks is that both reach the device.
void scene_dxil_grad(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_dxil_ps_grad, sizeof b::k_dxil_ps_grad, 1);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle texture = checker(rig);
  textured_quad(rig, pipe, texture, target, graphics::Filter::Linear, out);
}

// CalculateLevelOfDetail (dxil_ps_lod_show.hlsl): the level the hardware works out for the coordinate against the interpreter's, from the quad.
void scene_dxil_lod(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_dxil_ps_lod_show, sizeof b::k_dxil_ps_lod_show, 1);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle texture = checker(rig);
  textured_quad(rig, pipe, texture, target, graphics::Filter::Nearest, out);
}

// A texture array indexed by a variable (dxil_ps_array_show.hlsl): the left half of the quad samples the first texture, the right half the second.
void scene_dxil_texture_array(Rig& rig, ArrayView<uint32_t> out) {
  if (graphics::kMaxTextureUnits < 2) return void(rig.step("two texture units", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_dxil_ps_array_show, sizeof b::k_dxil_ps_array_show, 2);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle first = checker(rig);
  uint32_t texels[16];
  for (uint32_t i = 0; i < 16; ++i) texels[i] = ((i % 4 + i / 4) & 1) ? 0xFF20F060u : 0xFFF02060u;
  Handle second = rig.image(kAllocTexture, Format::A8R8G8B8, 4, 4);
  rig.write(second, bytes_of(texels, sizeof texels));
  auto vert = [](float x, float y, float u, float v) { return Vertex{x, y, 0.5f, u / 2, v / 2, 1, 1}; };
  const Vertex v[6] = {vert(-0.9f, -0.9f, 0, 0), vert(0.9f, -0.9f, 1, 0), vert(-0.9f, 0.9f, 0, 1), vert(-0.9f, 0.9f, 0, 1), vert(0.9f, -0.9f, 1, 0), vert(0.9f, 0.9f, 1, 1)};
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  graphics::SamplerDesc sampler;
  sampler.filter = graphics::Filter::Nearest;
  sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_texture(0, 2, sampler);
  w.bind_texture(1, 3, sampler);
  w.draw(6);
  w.end_pass();
  rig.render(w, {target, vb, first, second});
  rig.read(target, out);
}

// sample_c (dxil_ps_cmp_show.hlsl): a texture whose red grows from the left (0.1) to the right (0.9) compared with references that grow down the quad and a fixed one; the comparison is
// filtered, so the edges between passing and failing texels are blends.
void scene_dxil_compare(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_dxil_ps_cmp_show, sizeof b::k_dxil_ps_cmp_show, 1);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  uint32_t texels[16];
  for (uint32_t i = 0; i < 16; ++i) texels[i] = 0xFF000000u | ((26 + (i % 4) * 68) << 16);
  Handle texture = rig.image(kAllocTexture, Format::A8R8G8B8, 4, 4);
  rig.write(texture, bytes_of(texels, sizeof texels));
  textured_quad(rig, pipe, texture, target, graphics::Filter::Linear, out);
}

// Double precision (dxil_ps_double_show.hlsl): arithmetic, a select, a loop-carried sum, fma and conversions on doubles, shown as colours.
void scene_dxil_double(Rig& rig, ArrayView<uint32_t> out) {
  const Vertex v[3] = {{-1, -1, 0.5f, 0.9f, 0.3f, 0.2f, 1}, {3, -1, 0.5f, 0.1f, 0.8f, 0.4f, 1}, {-1, 3, 0.5f, 0.4f, 0.2f, 0.9f, 1}};
  namespace b = conformance_blobs;
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_dxil_ps_double_show, sizeof b::k_dxil_ps_double_show, 0);
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
  uint32_t allowed_bad = kAllowedBad;   // pixels that may differ (a scene that is nothing but the placement of points is held to less)
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

// Independent blending: two overlapping triangles into two targets that start at dark grey. Target 0 adds (One + One) so the overlap is brighter than either triangle; target 1 does
// not blend, so the overlap is the second triangle alone. Even columns of the picture are target 0, odd ones target 1.
void scene_independent_blend(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureMultipleTargets | graphics::kFeatureBlendState | graphics::kFeatureIndependentBlend)) return void(rig.step("independent blend", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[6] = {{-0.9f, -0.9f, 0.5f, 0.5f, 0.1f, 0.1f, 1}, {0.7f, -0.9f, 0.5f, 0.5f, 0.1f, 0.1f, 1}, {-0.1f, 0.9f, 0.5f, 0.5f, 0.1f, 0.1f, 1},
                       {-0.7f, 0.5f, 0.5f, 0.1f, 0.4f, 0.3f, 1}, {0.9f, 0.5f, 0.5f, 0.1f, 0.4f, 0.3f, 1}, {0.1f, -0.9f, 0.5f, 0.1f, 0.4f, 0.3f, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_mrt, sizeof b::k_ps_mrt));
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  pd.color_target_count = 2;
  pd.blend.custom = pd.blend.enable = true;
  pd.blend.src_color = pd.blend.dst_color = pd.blend.src_alpha = pd.blend.dst_alpha = graphics::BlendFactor::One;
  pd.independent_blend = true;   // (target 1: the default state, no blending)
  Handle pipe = rig.pipeline(pd);
  Handle first = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize), second = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  w.begin_pass_targets(0, {1}, kNoAllocation, kPassClearColor, {0.1f, 0.1f, 0.1f, 1}, 1.0f, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(6);
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

// Occlusion queries: a quad on the right half is drawn first (near), then three queried quads behind it - one in the open (16 x 16 pixels), one half behind the near quad (128 pixels
// show) and one wholly behind it (none). The counts are then drawn as the colour of three stripes, so that a wrong count shows in the picture.
void scene_occlusion_query(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureOcclusionQueries)) return void(rig.step("occlusion queries", status::kNotSupported));
  auto quad = [](Vertex* v, float x0, float y0, float x1, float y1, float z, float r, float g, float b) {
    v[0] = {x0, y0, z, r, g, b, 1}, v[1] = {x1, y0, z, r, g, b, 1}, v[2] = {x0, y1, z, r, g, b, 1};
    v[3] = {x0, y1, z, r, g, b, 1}, v[4] = {x1, y0, z, r, g, b, 1}, v[5] = {x1, y1, z, r, g, b, 1};
  };
  Vertex v[24];
  quad(v + 0, 0.0f, -1.0f, 1.0f, 1.0f, 0.2f, 0, 0, 0);          // near, right half
  quad(v + 6, -1.0f, -1.0f, 0.0f, 0.0f, 0.5f, 0, 0, 0);         // in the open: 16 x 16 pixels
  quad(v + 12, -0.5f, 0.0f, 0.5f, 1.0f, 0.5f, 0, 0, 0);         // half behind the near quad: 8 x 16 pixels show
  quad(v + 18, 0.25f, -1.0f, 0.75f, 0.0f, 0.5f, 0, 0, 0);       // wholly behind it
  Handle pipe = d3d9_pipeline(rig, true, false);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle depth = rig.image(kAllocDepthStencil, Format::D32Float, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  AllocationDesc rd;
  rd.flags = kAllocQueryResult, rd.size = 3 * 8;
  Handle results = rig.create(rd);
  CommandWriter w(rig.allocator());
  begin(w, 0, 1, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(6);
  for (uint32_t q = 0; q < 3; ++q) {
    w.begin_query(3, q * 8);
    w.draw(6, 6 + q * 6);
    w.end_query();
  }
  w.end_pass();
  rig.render(w, {target, depth, vb, results});
  uint64_t counts[3];
  for (uint32_t q = 0; q < 3; ++q) counts[q] = rig.read_u64(results, q);
  // (the stripes: pixels 0 .. 8, 12 .. 20 and 24 .. 32)
  Vertex s[18];
  for (uint32_t q = 0; q < 3; ++q) {
    const float shade = counts[q] >= 256 ? 1.0f : static_cast<float>(counts[q]) / 256.0f;
    quad(s + q * 6, -1.0f + 0.75f * static_cast<float>(q), -1.0f, -0.5f + 0.75f * static_cast<float>(q), 1.0f, 0.5f, shade, 0.25f, 0.25f);
  }
  Handle svb = rig.buffer(kAllocVertexBuffer, bytes_of(s, sizeof s));
  Handle flat = d3d9_pipeline(rig, false, false);
  CommandWriter w2(rig.allocator());
  begin(w2, 0, kNoAllocation, {0, 0, 0, 1});
  w2.bind_pipeline(flat);
  w2.bind_vertex_buffer(1);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.draw(18);
  w2.end_pass();
  rig.render(w2, {target, svb});
  rig.read(target, out);
}

// A volume, an array or a cube texture of 4 x 4 layers, each layer a different colour with a gradient across it (red from the left, darker towards the bottom), sampled by pixel shaders
// that declare Texture3D / Texture2DArray / TextureCube (ps_tex3d / ps_texarray / ps_texcube). The vertex colour is the texture coordinate (vs_vol).
Handle layered_texture(Rig& rig, graphics::TextureKind kind, uint32_t layers, uint32_t (*base)(uint32_t layer)) {
  AllocationDesc d;
  d.flags = kAllocTexture, d.format = Format::A8R8G8B8, d.width = d.height = 4, d.kind = kind, d.layers = layers;
  Handle texture = rig.create(d);
  ByteBuffer bytes(rig.allocator());
  for (uint32_t layer = 0; layer < layers; ++layer)
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x) {
        const uint32_t color = base(layer);
        const uint32_t shade = (255 - 30 * y) * (100 + 50 * x) / 255;   // 100 .. 250
        auto channel = [&](uint32_t shift) { return (((color >> shift) & 0xFF) * shade / 255) & 0xFF; };
        bytes.put32(0xFF000000u | (channel(16) << 16) | (channel(8) << 8) | channel(0));
      }
  rig.write(texture, bytes.view());
  return texture;
}

// Draws the quads (6 vertices each; the vertex colours are the coordinates) with the given pixel shader and texture, nearest filtered.
void sample_layered(Rig& rig, ArrayView<uint32_t> out, const uint8_t* ps, uint32_t ps_size, Handle texture, const Vertex* vertices, uint32_t quads) {
  namespace b = conformance_blobs;
  Handle p = dxbc_pipeline(rig, b::k_vs_vol, sizeof b::k_vs_vol, ps, ps_size, 1);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(vertices, sizeof(Vertex) * 6 * quads));
  graphics::SamplerDesc sampler;
  sampler.filter = graphics::Filter::Nearest;
  sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(p);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_texture(0, 2, sampler);
  w.draw(6 * quads);
  w.end_pass();
  rig.render(w, {target, vb, texture});
  rig.read(target, out);
}

// A quad over the pixels [x0, x1) x [y0, y1); the vertex colour of a corner is the coordinate (c(u, v) for u across and v down, each 0 .. 1).
template <typename Coord>
void quad_pixels(Vertex* v, float x0, float y0, float x1, float y1, Coord coord) {
  auto at = [&](float px, float py, float u, float vv) {
    const graphics::Vec3 c = coord(u, vv);
    return Vertex{px / 16.0f - 1.0f, py / 16.0f - 1.0f, 0.5f, c.x, c.y, c.z, 1.0f};
  };
  v[0] = at(x0, y0, 0, 0), v[1] = at(x1, y0, 1, 0), v[2] = at(x0, y1, 0, 1);
  v[3] = at(x0, y1, 0, 1), v[4] = at(x1, y0, 1, 0), v[5] = at(x1, y1, 1, 1);
}

// The other queries: the occlusion predicate of a quad that shows and of one that is hidden, the vertices and primitives that draws took, and (only whether they are more than none, as a device
// may reuse vertex results and shade helper pixels) the vertex shader and pixel shader runs. The picture is a stripe for each, as bright as its result.
void scene_statistics_queries(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureOcclusionQueries | graphics::kFeatureStatisticsQueries)) return void(rig.step("statistics queries", status::kNotSupported));
  auto quad = [](Vertex* v, float x0, float y0, float x1, float y1, float z, float r, float g, float b) {
    v[0] = {x0, y0, z, r, g, b, 1}, v[1] = {x1, y0, z, r, g, b, 1}, v[2] = {x0, y1, z, r, g, b, 1};
    v[3] = {x0, y1, z, r, g, b, 1}, v[4] = {x1, y0, z, r, g, b, 1}, v[5] = {x1, y1, z, r, g, b, 1};
  };
  Vertex v[24];
  quad(v + 0, 0.0f, -1.0f, 1.0f, 1.0f, 0.2f, 0, 0, 0);
  quad(v + 6, -1.0f, -1.0f, 0.0f, 0.0f, 0.5f, 0, 0, 0);
  quad(v + 12, -0.5f, 0.0f, 0.5f, 1.0f, 0.5f, 0, 0, 0);
  quad(v + 18, 0.25f, -1.0f, 0.75f, 0.0f, 0.5f, 0, 0, 0);
  Handle pipe = d3d9_pipeline(rig, true, false);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle depth = rig.image(kAllocDepthStencil, Format::D32Float, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  AllocationDesc rd;
  rd.flags = kAllocQueryResult, rd.size = 6 * 8;
  Handle results = rig.create(rd);
  CommandWriter w(rig.allocator());
  begin(w, 0, 1, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(6);
  // (kind, first vertex, vertex count)
  static const uint32_t kQueries[6][3] = {{1, 6, 6}, {1, 18, 6}, {2, 6, 12}, {3, 6, 18}, {4, 6, 6}, {5, 6, 6}};
  for (uint32_t q = 0; q < 6; ++q) {
    w.begin_query(3, q * 8, kQueries[q][0]);
    w.draw(kQueries[q][2], kQueries[q][1]);
    w.end_query();
  }
  w.end_pass();
  rig.render(w, {target, depth, vb, results});
  uint64_t counts[6];
  for (uint32_t q = 0; q < 6; ++q) counts[q] = rig.read_u64(results, q);
  Vertex s[36];
  for (uint32_t q = 0; q < 6; ++q) {
    const float value = q < 2 ? static_cast<float>(counts[q]) : q == 2 ? static_cast<float>(counts[q]) / 16.0f : q == 3 ? static_cast<float>(counts[q]) / 8.0f : counts[q] > 0 ? 1.0f : 0.0f;
    const float x0 = (1.0f + 5.0f * static_cast<float>(q)) / 16.0f - 1.0f;
    quad(s + q * 6, x0, -1.0f, x0 + 4.0f / 16.0f, 1.0f, 0.5f, value > 1.0f ? 1.0f : value, 0.25f, 0.25f);
  }
  Handle svb = rig.buffer(kAllocVertexBuffer, bytes_of(s, sizeof s));
  Handle flat = d3d9_pipeline(rig, false, false);
  CommandWriter w2(rig.allocator());
  begin(w2, 0, kNoAllocation, {0, 0, 0, 1});
  w2.bind_pipeline(flat);
  w2.bind_vertex_buffer(1);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.draw(36);
  w2.end_pass();
  rig.render(w2, {target, svb});
  rig.read(target, out);
}

void scene_texture_3d(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureTextureKinds)) return void(rig.step("texture kinds", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle texture = layered_texture(rig, graphics::TextureKind::Texture3D, 4, [](uint32_t layer) -> uint32_t { return layer == 0 ? 0xFF0000u : layer == 1 ? 0x00FF00u : layer == 2 ? 0x0000FFu : 0xFFFF00u; });
  // Top half: one slice (depth 0.6: the third) across u and v. Bottom half: v fixed, the depth running down the picture.
  Vertex v[12];
  quad_pixels(v, 0, 0, 32, 16, [](float u, float vv) { return graphics::Vec3{u, vv, 0.6f}; });
  quad_pixels(v + 6, 0, 16, 32, 32, [](float u, float vv) { return graphics::Vec3{u, 0.3f, vv}; });
  sample_layered(rig, out, b::k_ps_tex3d, sizeof b::k_ps_tex3d, texture, v, 2);
}

void scene_texture_array(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureTextureKinds)) return void(rig.step("texture kinds", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle texture = layered_texture(rig, graphics::TextureKind::Texture2DArray, 3, [](uint32_t layer) -> uint32_t { return layer == 0 ? 0xFF8000u : layer == 1 ? 0x00FF80u : 0x8000FFu; });
  // Three bands, one layer each (the layer is the third coordinate: 0, 1, 2). Their edges are off the pixel grid so that no pixel centre falls on a texel boundary, where devices may round either way.
  Vertex v[18];
  for (uint32_t i = 0; i < 3; ++i)
    quad_pixels(v + 6 * i, static_cast<float>(i * 10) + 1.2f, 0, static_cast<float>(i * 10) + 10.8f, 32, [i](float u, float vv) { return graphics::Vec3{u, vv, static_cast<float>(i)}; });
  sample_layered(rig, out, b::k_ps_texarray, sizeof b::k_ps_texarray, texture, v, 3);
}

void scene_texture_cube(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureTextureKinds)) return void(rig.step("texture kinds", status::kNotSupported));
  namespace b = conformance_blobs;
  // +X red, -X cyan, +Y green, -Y magenta, +Z blue, -Z yellow.
  Handle texture = layered_texture(rig, graphics::TextureKind::TextureCube, 6, [](uint32_t face) -> uint32_t {
    static const uint32_t kColours[6] = {0xFF0000u, 0x00FFFFu, 0x00FF00u, 0xFF00FFu, 0x0000FFu, 0xFFFF00u};
    return kColours[face];
  });
  // Six quads in a grid of 3 x 2 (10 x 16 pixels each), each looking at one face: the direction is the face's axis, with the other two components running over -0.8 .. 0.8.
  Vertex v[36];
  for (uint32_t face = 0; face < 6; ++face) {
    const float x0 = static_cast<float>(1 + (face % 3) * 10), y0 = static_cast<float>(face / 3 * 16);
    quad_pixels(v + 6 * face, x0, y0, x0 + 10, y0 + 16, [face](float u, float vv) {
      const float a = -0.8f + 1.6f * u, c = -0.8f + 1.6f * vv, axis = face % 2 ? -1.0f : 1.0f;
      return face < 2 ? graphics::Vec3{axis, c, a} : face < 4 ? graphics::Vec3{a, axis, c} : graphics::Vec3{a, c, axis};
    });
  }
  sample_layered(rig, out, b::k_ps_texcube, sizeof b::k_ps_texcube, texture, v, 6);
}

// The same textures read with Load, SampleLevel and GetDimensions (ps_texel3d / ps_texelarray / ps_texelcube): the picture is the texel loaded, the texel sampled and the size.
void scene_texel_3d(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureTextureKinds | graphics::kFeatureTexelAccess)) return void(rig.step("texture kinds", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle texture = layered_texture(rig, graphics::TextureKind::Texture3D, 4, [](uint32_t layer) -> uint32_t { return layer == 0 ? 0xFF0000u : layer == 1 ? 0x00FF00u : layer == 2 ? 0x0000FFu : 0xFFFF00u; });
  Vertex v[12];
  quad_pixels(v, 0, 0, 32, 16, [](float u, float vv) { return graphics::Vec3{u * 0.97f + 0.01f, vv * 0.97f + 0.01f, 0.6f}; });
  quad_pixels(v + 6, 0, 16, 32, 32, [](float u, float vv) { return graphics::Vec3{u * 0.97f + 0.01f, 0.3f, vv * 0.97f + 0.01f}; });
  sample_layered(rig, out, b::k_ps_texel3d, sizeof b::k_ps_texel3d, texture, v, 2);
}

void scene_texel_array(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureTextureKinds | graphics::kFeatureTexelAccess)) return void(rig.step("texture kinds", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle texture = layered_texture(rig, graphics::TextureKind::Texture2DArray, 3, [](uint32_t layer) -> uint32_t { return layer == 0 ? 0xFF8000u : layer == 1 ? 0x00FF80u : 0x8000FFu; });
  Vertex v[18];
  for (uint32_t i = 0; i < 3; ++i)
    quad_pixels(v + 6 * i, static_cast<float>(i * 10) + 1.2f, 0, static_cast<float>(i * 10) + 10.8f, 32, [i](float u, float vv) { return graphics::Vec3{u * 0.97f + 0.01f, vv * 0.97f + 0.01f, static_cast<float>(i)}; });
  sample_layered(rig, out, b::k_ps_texelarray, sizeof b::k_ps_texelarray, texture, v, 3);
}

void scene_texel_cube(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureTextureKinds | graphics::kFeatureTexelAccess)) return void(rig.step("texture kinds", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle texture = layered_texture(rig, graphics::TextureKind::TextureCube, 6, [](uint32_t face) -> uint32_t {
    static const uint32_t kColours[6] = {0xFF0000u, 0x00FFFFu, 0x00FF00u, 0xFF00FFu, 0x0000FFu, 0xFFFF00u};
    return kColours[face];
  });
  Vertex v[36];
  for (uint32_t face = 0; face < 6; ++face) {
    const float x0 = static_cast<float>(1 + (face % 3) * 10), y0 = static_cast<float>(face / 3 * 16);
    quad_pixels(v + 6 * face, x0, y0, x0 + 10, y0 + 16, [face](float u, float vv) {
      const float a = -0.8f + 1.6f * u, c = -0.8f + 1.6f * vv, axis = face % 2 ? -1.0f : 1.0f;
      return face < 2 ? graphics::Vec3{axis, c, a} : face < 4 ? graphics::Vec3{a, axis, c} : graphics::Vec3{a, c, axis};
    });
  }
  sample_layered(rig, out, b::k_ps_texelcube, sizeof b::k_ps_texelcube, texture, v, 6);
}

// A compute shader (cs_fill.hlsl): 4 x 8 groups of 8 x 4 threads, each writing a pixel of a 32 x 32 picture into a structured buffer from its SV_DispatchThreadID, SV_GroupID and
// SV_GroupIndex, a structured buffer of uint4 it writes and reads back, a constant from the constant buffer, and two atomic counters that must end at 1024. The picture is the buffer.
void compute_fill(Rig& rig, ArrayView<uint32_t> out, const uint8_t* code, uint32_t size) {
  if (!rig.has(graphics::kFeatureCompute)) return void(rig.step("compute", status::kNotSupported));
  Handle cs = rig.shader("create_shader (compute)", ShaderKind::Dxbc, graphics::ShaderStage::Compute, bytes_of(code, size));
  PipelineDesc pd;
  pd.compute_shader = cs;
  Handle pipe = rig.pipeline(pd);
  auto storage = [&](uint64_t size) {
    AllocationDesc d;
    d.flags = kAllocUnorderedAccess, d.size = size;
    return rig.create(d);
  };
  Handle picture = storage(kPixels * 4), structured = storage(kPixels * 16), counters = storage(16);
  const uint32_t constants[4] = {0x00102030u, 0, 0, 0};
  CommandWriter w(rig.allocator());
  w.bind_pipeline(pipe);
  w.push_constants(bytes_of(constants, sizeof constants));
  w.bind_uav(0, 0, 0, kPixels * 4);
  w.bind_uav(1, 1, 0, kPixels * 16);
  w.bind_uav(2, 2, 0, 16);
  w.dispatch(kSize / 8, kSize / 4, 1);
  rig.render(w, {picture, structured, counters});
  rig.read_words(picture, out);
  // (a counter that did not reach 1024 - an atomic lost - shows as a green cast)
  if (rig.read_u64(counters, 0) != (uint64_t{1024} | (uint64_t{1024} << 32)))
    for (uint32_t& pixel : out) pixel ^= 0x0000FF00u;
}

// Direct3D 12 binding: the compute scene again with a root signature (b0 as four root constants, u0 and u1 as root descriptors, u2 from a descriptor table) and resource states (the
// picture is made in the common state and moved to unordered access).
void scene_d3d12_compute(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureCompute)) return void(rig.step("compute", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle cs = rig.shader("create_shader (compute)", ShaderKind::Dxbc, graphics::ShaderStage::Compute, bytes_of(b::k_cs_fill, sizeof b::k_cs_fill));
  PipelineDesc pd;
  pd.compute_shader = cs;
  Handle pipe = rig.pipeline(pd);
  auto storage = [&](uint64_t size, uint32_t extra) {
    AllocationDesc d;
    d.flags = kAllocUnorderedAccess | extra, d.size = size, d.initial_state = state::kCommon;
    return rig.create(d);
  };
  Handle picture = storage(kPixels * 4, kAllocTrackState), structured = storage(kPixels * 16, 0), counters = storage(16, 0);
  // (root signature version 1.0: four parameters - constants b0 x 4; UAV u0; UAV u1; a table with the UAV u2 -, no static samplers)
  const uint32_t blob[] = {1, 4, 24, 0, 32 * 4, 0,
                           1, 0, 18 * 4, 4, 0, 21 * 4, 4, 0, 23 * 4, 0, 0, 25 * 4,
                           0, 0, 4, 0, 0, 1, 0, 1, 27 * 4, 1, 1, 2, 0, 0};
  Handle signature = rig.root_signature(bytes_of(blob, sizeof blob));
  Handle heap = rig.descriptor_heap(DescriptorHeapType::CbvSrvUav, 2);
  DescriptorDesc uav;
  uav.kind = DescriptorKind::Uav, uav.allocation = counters;
  rig.descriptor(heap, 1, uav);
  const uint32_t constants[4] = {0x00102030u, 0, 0, 0};
  CommandWriter w(rig.allocator());
  w.transition(0, state::kCommon, state::kUnorderedAccess);
  w.set_root_signature(signature);
  w.set_descriptor_heaps({heap});
  w.set_root_constants(0, 0, bytes_of(constants, sizeof constants));
  w.set_root_descriptor(1, 0, 0);
  w.set_root_descriptor(2, 1, 0);
  w.set_root_table(3, 1);
  w.bind_pipeline(pipe);
  w.dispatch(kSize / 8, kSize / 4, 1);
  w.transition(0, state::kUnorderedAccess, state::kCopySource);
  rig.render(w, {picture, structured, counters});
  rig.read_words(picture, out);
  if (rig.read_u64(counters, 0) != (uint64_t{1024} | (uint64_t{1024} << 32)))
    for (uint32_t& pixel : out) pixel ^= 0x0000FF00u;
}

// Direct3D 12 binding for drawing: the volume texture scene with a root signature (a table with t0, b0 as 16 root constants, a static point sampler), a descriptor heap, and a render target
// and a texture that move between states with barriers.
void scene_d3d12_textures(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureTextureKinds)) return void(rig.step("texture kinds", status::kNotSupported));
  namespace b = conformance_blobs;
  AllocationDesc td;
  td.flags = kAllocTexture | kAllocTrackState, td.format = Format::A8R8G8B8, td.width = 4, td.height = 4, td.kind = graphics::TextureKind::Texture3D, td.layers = 4, td.initial_state = state::kCopyDest;
  Handle texture = rig.create(td);
  ByteBuffer bytes(rig.allocator());
  for (uint32_t layer = 0; layer < 4; ++layer)
    for (uint32_t texel = 0; texel < 16; ++texel) bytes.put32(0xFF000000u | (layer == 0 ? 0xFF0000u : layer == 1 ? 0x00FF00u : layer == 2 ? 0x0000FFu : 0xFFFF00u));
  rig.write(texture, bytes.view());
  Vertex v[12];
  quad_pixels(v, 0, 0, 32, 16, [](float u, float vv) { return graphics::Vec3{u, vv, 0.6f}; });
  quad_pixels(v + 6, 0, 16, 32, 32, [](float u, float vv) { return graphics::Vec3{u, 0.3f, vv}; });
  Handle p = dxbc_pipeline(rig, b::k_vs_vol, sizeof b::k_vs_vol, b::k_ps_tex3d, sizeof b::k_ps_tex3d, 1);
  AllocationDesc rd;
  rd.flags = kAllocRenderTarget | kAllocTrackState, rd.format = Format::A8R8G8B8, rd.width = kSize, rd.height = kSize, rd.initial_state = state::kCommon;
  Handle target = rig.create(rd);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  // (root signature version 1.0: a table with the SRV t0, then b0 as 16 constants; a static point sampler with clamped addresses)
  const uint32_t blob[] = {1, 2, 24, 1, 22 * 4, 0,
                           0, 0, 12 * 4, 1, 0, 19 * 4,
                           1, 14 * 4, 0, 1, 0, 0, 0,
                           0, 0, 16,
                           0, 3, 3, 3, 0, 1, 8, 0, 0, __builtin_bit_cast(uint32_t, 1000.0f), 0, 0, 0};
  Handle signature = rig.root_signature(bytes_of(blob, sizeof blob));
  Handle heap = rig.descriptor_heap(DescriptorHeapType::CbvSrvUav, 3);
  DescriptorDesc srv;
  srv.kind = DescriptorKind::Srv, srv.allocation = texture;
  rig.descriptor(heap, 2, srv);
  CommandWriter w(rig.allocator());
  w.transition(2, state::kCopyDest, state::kPixelShaderResource);
  w.transition(0, state::kCommon, state::kRenderTarget);
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.set_root_signature(signature);
  w.set_descriptor_heaps({heap});
  w.set_root_constants(1, 0, bytes_of(kIdentity, sizeof kIdentity));
  w.set_root_table(0, 2);
  w.bind_pipeline(p);
  w.bind_vertex_buffer(1);
  w.draw(12);
  w.end_pass();
  rig.render(w, {target, vb, texture});
  rig.read(target, out);
}

// Direct3D 12 resource heaps: a render target and a texture placed at the same place in one heap. A triangle is drawn into the render target, an aliasing barrier passes the use to the
// texture (what the GPU holds for the target goes to the heap's memory, the texture is uploaded from it) and the texture is drawn onto the target.
void scene_d3d12_heap(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle first = d3d9_pipeline(rig, false, false);
  Handle second = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_tex, sizeof b::k_ps_tex, 1);
  AllocationDesc hd;
  hd.flags = kAllocHeap, hd.size = 8192;
  Handle heap = rig.create(hd);
  AllocationDesc id;
  id.flags = kAllocRenderTarget, id.format = Format::A8R8G8B8, id.width = id.height = kSize, id.heap = heap, id.heap_offset = 256;
  Handle placed_target = rig.create(id);
  id.flags = kAllocTexture;
  Handle placed_texture = rig.create(id);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  const Vertex tri[3] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  Handle tvb = rig.buffer(kAllocVertexBuffer, bytes_of(tri, sizeof tri));
  auto vert = [](float x, float y, float u, float v) { return Vertex{x, y, 0.5f, u / 2, v / 2, 1, 1}; };
  const Vertex quad[6] = {vert(-0.6f, -0.6f, 0, 0), vert(0.9f, -0.6f, 1, 0), vert(-0.6f, 0.9f, 0, 1), vert(-0.6f, 0.9f, 0, 1), vert(0.9f, -0.6f, 1, 0), vert(0.9f, 0.9f, 1, 1)};
  Handle qvb = rig.buffer(kAllocVertexBuffer, bytes_of(quad, sizeof quad));
  graphics::SamplerDesc sampler;
  sampler.filter = graphics::Filter::Linear;
  sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
  CommandWriter w(rig.allocator());   // (list: target 0, texture 1, screen 2, triangle 3, quad 4)
  begin(w, 0, kNoAllocation, {0.1f, 0.1f, 0.1f, 1});
  w.bind_pipeline(first);
  w.bind_vertex_buffer(3);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  w.resource_barrier({CommandWriter::Barrier{2, 0, 1, 0, 0}});
  begin(w, 2, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(second);
  w.bind_vertex_buffer(4);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_texture(0, 1, sampler);
  w.draw(6);
  w.end_pass();
  rig.render(w, {placed_target, placed_texture, target, tvb, qvb});
  rig.read(target, out);
}

// DXIL (Shader Model 6) shaders: a vertex shader with a constant buffer (a matrix and a tint) and three pixel shaders - one with a lot of arithmetic, one with branches and a discard, one with
// loops - each drawing a triangle in a third of the picture. The kernel translates the bitcode to the tokens of the same shader, so this checks that what comes out runs on the GPU as it does on the CPU.
void scene_dxil(Rig& rig, ArrayView<uint32_t> out) {
  namespace b = conformance_blobs;
  Handle first = dxbc_pipeline(rig, b::k_dxil_vs, sizeof b::k_dxil_vs, b::k_dxil_ps_math, sizeof b::k_dxil_ps_math, 0);
  Handle second = dxbc_pipeline(rig, b::k_dxil_vs, sizeof b::k_dxil_vs, b::k_dxil_ps_flow, sizeof b::k_dxil_ps_flow, 0);
  Handle third = dxbc_pipeline(rig, b::k_dxil_vs, sizeof b::k_dxil_vs, b::k_dxil_ps_loop, sizeof b::k_dxil_ps_loop, 0);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  // (positions in the cell, colours, so that the varyings span ranges that take the branches both ways)
  Vertex v[9];
  for (uint32_t cell = 0; cell < 3; ++cell) {
    const float x0 = -0.97f + 0.65f * static_cast<float>(cell), x1 = x0 + 0.6f;
    v[3 * cell] = {x0, -0.9f, 0.5f, 0.9f, 0.1f, 0.1f, 1.0f};
    v[3 * cell + 1] = {x1, -0.9f, 0.5f, 0.1f, 0.9f, 0.4f, 1.0f};
    v[3 * cell + 2] = {(x0 + x1) * 0.5f, 0.9f, 0.5f, 0.2f, 0.3f, 0.9f, 0.6f};
  }
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  float constants[20];
  for (uint32_t k = 0; k < 16; ++k) constants[k] = kIdentity[k];
  constants[16] = 1.0f, constants[17] = 0.9f, constants[18] = 1.0f, constants[19] = 1.0f;   // (the tint)
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(constants, sizeof constants));
  w.bind_pipeline(first);
  w.draw(3, 0);
  w.bind_pipeline(second);
  w.draw(3, 3);
  w.bind_pipeline(third);
  w.draw(3, 6);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

void scene_compute(Rig& rig, ArrayView<uint32_t> out) { compute_fill(rig, out, conformance_blobs::k_cs_fill, sizeof conformance_blobs::k_cs_fill); }
// The same shader as DXIL (cs_6_0).
void scene_dxil_compute(Rig& rig, ArrayView<uint32_t> out) { compute_fill(rig, out, conformance_blobs::k_dxil_cs_fill, sizeof conformance_blobs::k_dxil_cs_fill); }

// Group-shared memory and barriers (cs_shared.hlsl): 16 groups of 8 x 8 threads, each writing a slot of a 64-word shared array, reading its neighbour's after a barrier, and the 64 slots added
// by a reduction with a barrier in its loop. The picture is the buffer: red the neighbour's value, green the sum, blue the thread.
void compute_shared(Rig& rig, ArrayView<uint32_t> out, const uint8_t* code, uint32_t size) {
  if (!rig.has(graphics::kFeatureCompute)) return void(rig.step("compute", status::kNotSupported));
  Handle cs = rig.shader("create_shader (compute)", ShaderKind::Dxbc, graphics::ShaderStage::Compute, bytes_of(code, size));
  PipelineDesc pd;
  pd.compute_shader = cs;
  Handle pipe = rig.pipeline(pd);
  AllocationDesc d;
  d.flags = kAllocUnorderedAccess, d.size = kPixels * 4;
  Handle picture = rig.create(d);
  CommandWriter w(rig.allocator());
  w.bind_pipeline(pipe);
  w.bind_uav(0, 0, 0, kPixels * 4);
  w.dispatch(kSize / 8, kSize / 8, 1);
  rig.render(w, {picture});
  rig.read_words(picture, out);
}

void scene_compute_shared(Rig& rig, ArrayView<uint32_t> out) { compute_shared(rig, out, conformance_blobs::k_cs_shared, sizeof conformance_blobs::k_cs_shared); }
// Buffer shader resource views (dxil_cs_srv.hlsl): a structured buffer of uint4 (t0) and a byte address buffer (t1), read-only, combined into the picture buffer (u0).
void scene_dxil_compute_srv(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureCompute)) return void(rig.step("compute", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle cs = rig.shader("create_shader (compute)", ShaderKind::Dxbc, graphics::ShaderStage::Compute, bytes_of(b::k_dxil_cs_srv, sizeof b::k_dxil_cs_srv));
  PipelineDesc pd;
  pd.compute_shader = cs;
  Handle pipe = rig.pipeline(pd);
  ByteBuffer structured(rig.allocator()), raw(rig.allocator());
  for (uint32_t i = 0; i < kPixels * 4; ++i) structured.put32(0x01010101u * (i % 251) + i * 7);
  for (uint32_t i = 0; i < kPixels; ++i) raw.put32(0x9E3779B9u * (i + 1));
  Handle src = rig.buffer(kAllocUnorderedAccess, structured.view()), bytes = rig.buffer(kAllocUnorderedAccess, raw.view());
  AllocationDesc d;
  d.flags = kAllocUnorderedAccess, d.size = kPixels * 4;
  Handle picture = rig.create(d);
  CommandWriter w(rig.allocator());
  w.bind_pipeline(pipe);
  w.bind_uav(0, 2, 0, kPixels * 4);
  w.bind_uav(graphics::kMaxUavBuffers, 0, 0, kPixels * 16);
  w.bind_uav(graphics::kMaxUavBuffers + 1, 1, 0, kPixels * 4);
  w.dispatch(kSize / 8, kSize / 4, 1);
  rig.render(w, {src, bytes, picture});
  rig.read_words(picture, out);
}
void scene_dxil_compute_shared(Rig& rig, ArrayView<uint32_t> out) { compute_shared(rig, out, conformance_blobs::k_dxil_cs_shared, sizeof conformance_blobs::k_dxil_cs_shared); }

// Multisampling (4 samples): a far red rectangle and a near green one that overlaps it, both with edges between the pixels' sample positions, drawn into 4x colour and depth images (depth
// tested per sample) and resolved. Vertical and horizontal edges are covered the same by every device that uses the standard 4x pattern, whatever the order of the samples.
void scene_multisample(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureMultisample | graphics::kFeatureBlendState)) return void(rig.step("multisampling", status::kNotSupported));
  auto rect = [](Vertex* v, float x0, float y0, float x1, float y1, float z, float r, float g, float b) {
    auto at = [&](float px, float py) { return Vertex{px / 16.0f - 1.0f, py / 16.0f - 1.0f, z, r, g, b, 1}; };
    v[0] = at(x0, y0), v[1] = at(x1, y0), v[2] = at(x0, y1);
    v[3] = at(x0, y1), v[4] = at(x1, y0), v[5] = at(x1, y1);
  };
  Vertex v[12];
  rect(v, 4.3f, 3.4f, 20.6f, 22.7f, 0.6f, 1, 0, 0);
  rect(v + 6, 12.4f, 9.2f, 27.8f, 28.6f, 0.3f, 0, 1, 0);
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.depth_test = pd.depth_write = true;
  pd.multisample = true;
  Handle pipe = rig.pipeline(pd);
  AllocationDesc color_desc, depth_desc;
  color_desc.flags = kAllocRenderTarget, color_desc.format = Format::A8R8G8B8, color_desc.width = color_desc.height = kSize, color_desc.samples = 4;
  depth_desc.flags = kAllocDepthStencil, depth_desc.format = Format::D32Float, depth_desc.width = depth_desc.height = kSize, depth_desc.samples = 4;
  Handle multisampled = rig.create(color_desc), depth = rig.create(depth_desc);
  // Resolved twice: into an image that is only drawn into (the even columns of the picture) and into one that is also a texture (the odd ones), which devices may store the other way up.
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize), textured = rig.image(kAllocRenderTarget | kAllocTexture, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  w.begin_pass(0, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1.0f, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(4);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(12);
  w.end_pass();
  w.resolve_region(2, 0);
  w.resolve_region(3, 0);
  rig.render(w, {multisampled, depth, target, textured, vb});
  ArrayView<uint32_t> other = allocate_view<uint32_t>(rig.allocator(), uint64_t{kSize} * kSize);
  if (other.empty()) return void(rig.step("memory", status::kNoMemory));
  rig.read(target, out);
  rig.read(textured, other);
  for (uint32_t i = 1; i < kSize * kSize; i += 2) out[i] = other[i];
  release_view(rig.allocator(), other);
}

// A geometry shader (gs_copy.hlsl): every triangle comes out as itself and, as a second strip, a half-size copy moved to the lower right with the red and blue swapped.
void geometry_scene(Rig& rig, ArrayView<uint32_t> out, const uint8_t* gs_code, uint32_t gs_size) {
  if (!rig.has(graphics::kFeatureGeometryShaders)) return void(rig.step("geometry shaders", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[3] = {{-0.9f, -0.9f, 0.5f, 1, 0, 0, 1}, {0.9f, -0.9f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.9f, 0.5f, 0, 0, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle gs = rig.shader("create_shader (geometry)", ShaderKind::Dxbc, graphics::ShaderStage::Geometry, bytes_of(gs_code, gs_size));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  pd.geometry_shader = gs;
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

void scene_geometry_shader(Rig& rig, ArrayView<uint32_t> out) { geometry_scene(rig, out, conformance_blobs::k_gs_copy, sizeof conformance_blobs::k_gs_copy); }
// The same shader as DXIL (gs_6_0).
void scene_dxil_geometry(Rig& rig, ArrayView<uint32_t> out) { geometry_scene(rig, out, conformance_blobs::k_dxil_gs_copy, sizeof conformance_blobs::k_dxil_gs_copy); }

// The geometry shader that runs twice for each primitive (gs_copy_inst2: gs_copy with an instance count of 2): the same triangles drawn twice over, so the same picture as gs_copy's.
void scene_geometry_instances(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureGeometryShaders)) return void(rig.step("geometry shaders", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[3] = {{-0.9f, -0.9f, 0.5f, 1, 0, 0, 1}, {0.9f, -0.9f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.9f, 0.5f, 0, 0, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle gs = rig.shader("create_shader (geometry)", ShaderKind::Dxbc, graphics::ShaderStage::Geometry, bytes_of(b::k_gs_copy_inst2, sizeof b::k_gs_copy_inst2));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  pd.geometry_shader = gs;
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

// Stream output (gs_streams.hlsl): a geometry shader with two streams of points writes, for each of the three vertices of a triangle, the position and colour (stream 0, 32 bytes a vertex) and a
// vector made of both (stream 1, 16 bytes) into two buffers, with nothing drawn (stream -1). The picture is made from the buffers: a small square at the position of each record in stream 0, coloured
// by the record of stream 1 (the vector 0.5 * q + 0.5), so a position that is turned over, a colour that is not the vertex's or a record that is in the wrong place shows.
void scene_stream_output(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureStreamOutput | graphics::kFeatureGeometryShaders)) return void(rig.step("stream output", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[3] = {{-0.5f, -0.5f, 0.5f, 1, 0, 0, 1}, {0.5f, -0.5f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.5f, 0.5f, 0, 0, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle gs = rig.shader("create_shader (geometry)", ShaderKind::Dxbc, graphics::ShaderStage::Geometry, bytes_of(b::k_gs_streams, sizeof b::k_gs_streams));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  pd.geometry_shader = gs;
  pd.stream_output_count = 3;
  pd.stream_output[0] = {0, 0, 0, 4, 0}, pd.stream_output[1] = {0, 1, 0, 4, 0}, pd.stream_output[2] = {1, 4, 0, 4, 1};
  pd.stream_output_stride[0] = 32, pd.stream_output_stride[1] = 16;
  pd.rasterized_stream = -1;
  Handle pipe = rig.pipeline(pd);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  AllocationDesc od;
  od.flags = kAllocStreamOutput, od.size = 3 * 32;
  Handle first = rig.create(od);
  od.size = 3 * 16;
  Handle second = rig.create(od);
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.set_stream_output(0, 2);
  w.set_stream_output(1, 3);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  rig.render(w, {target, vb, first, second});
  uint32_t f0[24], f1[12];
  rig.read_words(first, ArrayView<uint32_t>(f0, 24));
  rig.read_words(second, ArrayView<uint32_t>(f1, 12));
  auto f = [](uint32_t bits) { return __builtin_bit_cast(float, bits); };
  auto shade = [](float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; };
  Vertex squares[18];
  for (uint32_t i = 0; i < 3; ++i) {
    const float x = f(f0[8 * i]), y = f(f0[8 * i + 1]), r = shade(f(f1[4 * i]) * 0.5f + 0.5f), g = shade(f(f1[4 * i + 1]) * 0.5f + 0.5f), bl = shade(f(f1[4 * i + 2]) * 0.5f + 0.5f);
    const float h = 0.2f;
    Vertex* q = squares + 6 * i;
    q[0] = {x - h, y - h, 0.5f, r, g, bl, 1}, q[1] = {x + h, y - h, 0.5f, r, g, bl, 1}, q[2] = {x - h, y + h, 0.5f, r, g, bl, 1};
    q[3] = q[2], q[4] = q[1], q[5] = {x + h, y + h, 0.5f, r, g, bl, 1};
  }
  Handle flat = d3d9_pipeline(rig, false, false);
  Handle svb = rig.buffer(kAllocVertexBuffer, bytes_of(squares, sizeof squares));
  CommandWriter w2(rig.allocator());
  begin(w2, 0, kNoAllocation, {0, 0, 0, 1});
  w2.bind_pipeline(flat);
  w2.bind_vertex_buffer(1);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.draw(18);
  w2.end_pass();
  rig.render(w2, {target, svb});
  rig.read(target, out);
}


// A picture made of stream output records: a small square for each record of a buffer of positions (4 floats) and colours (4 floats), coloured by the vector in the second buffer (0.5 * q + 0.5),
// shifted by `shift(i)`.
template <typename Shift>
uint32_t squares_from_records(const uint32_t* positions, const uint32_t* vectors, uint32_t records, Shift shift, Vertex* squares) {
  auto f = [](uint32_t bits) { return __builtin_bit_cast(float, bits); };
  auto shade = [](float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; };
  for (uint32_t i = 0; i < records; ++i) {
    const float x = f(positions[8 * i]), y = f(positions[8 * i + 1]) + shift(i), r = shade(f(vectors[4 * i]) * 0.5f + 0.5f), g = shade(f(vectors[4 * i + 1]) * 0.5f + 0.5f), bl = shade(f(vectors[4 * i + 2]) * 0.5f + 0.5f);
    const float h = 0.2f;
    Vertex* q = squares + 6 * i;
    q[0] = {x - h, y - h, 0.5f, r, g, bl, 1}, q[1] = {x + h, y - h, 0.5f, r, g, bl, 1}, q[2] = {x - h, y + h, 0.5f, r, g, bl, 1};
    q[3] = q[2], q[4] = q[1], q[5] = {x + h, y + h, 0.5f, r, g, bl, 1};
  }
  return 6 * records;
}

// Appending across passes and submits, the statistics of stream output and timestamps. Two rounds of three records go into buffers made for seven (the second round after the first, appended in
// another pass of the same commands); a third round, in other commands, has room for one record: one primitive is written, three were needed. The squares of the second round are moved down so
// that a round that overwrote the first shows. Stripes at the bottom: written (1 of 4), needed (3 of 4), and whether the timestamps went forwards.
void scene_stream_output_append(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureStreamOutput | graphics::kFeatureGeometryShaders | graphics::kFeatureOcclusionQueries | graphics::kFeatureStatisticsQueries | graphics::kFeatureTimestampQueries))
    return void(rig.step("stream output append", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[3] = {{-0.5f, -0.5f, 0.5f, 1, 0, 0, 1}, {0.5f, -0.5f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.5f, 0.5f, 0, 0, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle gs = rig.shader("create_shader (geometry)", ShaderKind::Dxbc, graphics::ShaderStage::Geometry, bytes_of(b::k_gs_streams, sizeof b::k_gs_streams));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  pd.geometry_shader = gs;
  pd.stream_output_count = 3;
  pd.stream_output[0] = {0, 0, 0, 4, 0}, pd.stream_output[1] = {0, 1, 0, 4, 0}, pd.stream_output[2] = {1, 4, 0, 4, 1};
  pd.stream_output_stride[0] = 32, pd.stream_output_stride[1] = 16;
  pd.rasterized_stream = -1;
  Handle pipe = rig.pipeline(pd);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  AllocationDesc od;
  od.flags = kAllocStreamOutput, od.size = 7 * 32;
  Handle first = rig.create(od);
  od.size = 7 * 16;
  Handle second = rig.create(od);
  AllocationDesc rd;
  rd.flags = kAllocQueryResult, rd.size = 6 * 8;
  Handle results = rig.create(rd);
  auto pass = [&](CommandWriter& w, uint32_t offset, bool queries) {
    begin(w, 0, kNoAllocation, {0, 0, 0, 1});
    w.set_stream_output(0, 2, offset);
    w.set_stream_output(1, 3, offset);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
    if (queries) {
      w.begin_query(4, 0, static_cast<uint32_t>(graphics::QueryKind::Timestamp));
      w.end_query();
      w.begin_query(4, 8, static_cast<uint32_t>(graphics::QueryKind::SoPrimitivesWritten));
      w.draw(3);
      w.end_query();
      w.begin_query(4, 16, static_cast<uint32_t>(graphics::QueryKind::SoPrimitivesNeeded));
      w.draw(3);
      w.end_query();
      w.begin_query(4, 24, static_cast<uint32_t>(graphics::QueryKind::Timestamp));
      w.end_query();
      w.begin_query(4, 32, static_cast<uint32_t>(graphics::QueryKind::TimestampDisjoint));
      w.end_query();
      w.begin_query(4, 40, static_cast<uint32_t>(graphics::QueryKind::TimestampFrequency));
      w.end_query();
    } else {
      w.draw(3);
    }
    w.end_pass();
  };
  CommandWriter w(rig.allocator());
  pass(w, 0, false);
  pass(w, kStreamOutputAppendOffset, false);
  rig.render(w, {target, vb, first, second, results});
  CommandWriter w2(rig.allocator());
  pass(w2, kStreamOutputAppendOffset, true);
  rig.render(w2, {target, vb, first, second, results});
  // (the first query of the third round took a record, the second found none left: so 1 written by it, then none, and 3 needed by each)
  const uint64_t t0 = rig.read_u64(results, 0), written = rig.read_u64(results, 1), needed = rig.read_u64(results, 2), t1 = rig.read_u64(results, 3);
  uint32_t f0[56], f1[28];
  rig.read_words(first, ArrayView<uint32_t>(f0, 56));
  rig.read_words(second, ArrayView<uint32_t>(f1, 28));
  Vertex squares[7 * 6 + 4 * 6];
  const uint32_t n = squares_from_records(f0, f1, 7, [](uint32_t i) { return i >= 3 && i < 6 ? -0.5f : 0.0f; }, squares);
  // (stripes: the numbers as the height of a bar, in the bottom quarter)
  auto bar = [&](uint32_t k, float value, float r, float g, float bl) {
    const float x0 = -0.9f + 0.6f * static_cast<float>(k), x1 = x0 + 0.4f, top = -1.0f + 0.5f * value;
    Vertex* q = squares + n + 6 * k;
    q[0] = {x0, -1.0f, 0.4f, r, g, bl, 1}, q[1] = {x1, -1.0f, 0.4f, r, g, bl, 1}, q[2] = {x0, top, 0.4f, r, g, bl, 1};
    q[3] = q[2], q[4] = q[1], q[5] = {x1, top, 0.4f, r, g, bl, 1};
  };
  bar(0, static_cast<float>(written) / 4.0f, 1, 0.3f, 0.3f);
  bar(1, static_cast<float>(needed) / 4.0f, 0.3f, 1, 0.3f);
  // (the fourth bar: the clock was not interrupted and runs in nanoseconds)
  Vertex* last = squares + n + 18;
  const float tick = rig.read_u64(results, 5) == 1000000000u && rig.read_u64(results, 4) == 0 ? 1.0f : 0.1f;
  last[0] = {0.9f - 0.4f, -1.0f, 0.4f, 1, 1, 0.3f, 1}, last[1] = {0.9f, -1.0f, 0.4f, 1, 1, 0.3f, 1}, last[2] = {0.9f - 0.4f, -1.0f + 0.5f * tick, 0.4f, 1, 1, 0.3f, 1};
  last[3] = last[2], last[4] = last[1], last[5] = {0.9f, -1.0f + 0.5f * tick, 0.4f, 1, 1, 0.3f, 1};
  bar(2, t1 > t0 && t0 != 0 ? 1.0f : 0.1f, 0.3f, 0.3f, 1);
  Handle flat = d3d9_pipeline(rig, false, false);
  Handle svb = rig.buffer(kAllocVertexBuffer, bytes_of(squares, sizeof(Vertex) * (n + 24)));
  CommandWriter w3(rig.allocator());
  begin(w3, 0, kNoAllocation, {0, 0, 0, 1});
  w3.bind_pipeline(flat);
  w3.bind_vertex_buffer(1);
  w3.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w3.draw(n + 24);
  w3.end_pass();
  rig.render(w3, {target, svb});
  rig.read(target, out);
}

// Stream output of a vertex shader and of a domain shader: the three vertices of a triangle come out as the records they were (squares, as with the geometry shader), and the triangles the
// tessellator made of a patch are drawn again from the records (position and colour of each vertex).
void scene_stream_output_stages(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureStreamOutput | graphics::kFeatureTessellation | graphics::kFeatureOcclusionQueries | graphics::kFeatureStatisticsQueries))
    return void(rig.step("stream output stages", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[3] = {{-0.5f, -0.25f, 0.5f, 1, 0, 0, 1}, {0.5f, -0.25f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.75f, 0.5f, 0, 0, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  Handle hs = rig.shader("create_shader (hull)", ShaderKind::Dxbc, graphics::ShaderStage::Hull, bytes_of(b::k_hs_tri_cb, sizeof b::k_hs_tri_cb));
  Handle ds = rig.shader("create_shader (domain)", ShaderKind::Dxbc, graphics::ShaderStage::Domain, bytes_of(b::k_ds_noise, sizeof b::k_ds_noise));
  PipelineDesc vp = colour_pipeline_desc(vs, ps);
  vp.varying_count = 3;
  vp.stream_output_count = 2;
  vp.stream_output[0] = {0, 0, 0, 4, 0}, vp.stream_output[1] = {0, 1, 0, 4, 0};
  vp.stream_output_stride[0] = 32;
  vp.rasterized_stream = -1;
  Handle vertex_pipe = rig.pipeline(vp);
  PipelineDesc tp = vp;
  tp.hull_shader = hs, tp.domain_shader = ds;
  Handle domain_pipe = rig.pipeline(tp);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  AllocationDesc od;
  od.flags = kAllocStreamOutput, od.size = 3 * 32;
  Handle vertex_out = rig.create(od);
  od.size = 64 * 3 * 32;
  Handle domain_out = rig.create(od);
  AllocationDesc rd;
  rd.flags = kAllocQueryResult, rd.size = 8;
  Handle results = rig.create(rd);
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.set_stream_output(0, 2);
  w.bind_pipeline(vertex_pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  float constants[24];
  for (uint32_t k = 0; k < 16; ++k) constants[k] = kIdentity[k];
  for (uint32_t k = 0; k < 8; ++k) constants[16 + k] = k < 4 ? 3.0f : 0.0f;
  w.set_stream_output(0, 3);
  w.bind_pipeline(domain_pipe);
  w.push_constants(bytes_of(constants, sizeof constants));
  w.set_topology(patch_list(3));
  w.begin_query(4, 0, static_cast<uint32_t>(graphics::QueryKind::SoPrimitivesWritten));
  w.draw(3);
  w.end_query();
  w.end_pass();
  rig.render(w, {target, vb, vertex_out, domain_out, results});
  const uint64_t triangles = rig.read_u64(results, 0);
  const uint32_t count = triangles > 64 ? 64 : static_cast<uint32_t>(triangles);
  uint32_t f0[24], f1[64 * 24];
  rig.read_words(vertex_out, ArrayView<uint32_t>(f0, 24));
  rig.read_words(domain_out, ArrayView<uint32_t>(f1, 24 * count));
  auto f = [](uint32_t bits) { return __builtin_bit_cast(float, bits); };
  Vertex drawn[3 * 6 + 64 * 3];
  uint32_t n = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    const float x = f(f0[8 * i]) * 0.5f - 0.5f, y = f(f0[8 * i + 1]) * 0.5f - 0.5f, h = 0.1f;
    const float r = f(f0[8 * i + 4]), g = f(f0[8 * i + 5]), bl = f(f0[8 * i + 6]);
    Vertex* q = drawn + n;
    q[0] = {x - h, y - h, 0.5f, r, g, bl, 1}, q[1] = {x + h, y - h, 0.5f, r, g, bl, 1}, q[2] = {x - h, y + h, 0.5f, r, g, bl, 1};
    q[3] = q[2], q[4] = q[1], q[5] = {x + h, y + h, 0.5f, r, g, bl, 1};
    n += 6;
  }
  for (uint32_t i = 0; i < 3 * count; ++i) {
    const float x = f(f1[8 * i]) * 0.5f + 0.4f, y = f(f1[8 * i + 1]) * 0.5f + 0.2f;
    drawn[n++] = {x, y, 0.5f, f(f1[8 * i + 4]), f(f1[8 * i + 5]), f(f1[8 * i + 6]), 1};
  }
  Handle flat = d3d9_pipeline(rig, false, false);
  Handle svb = rig.buffer(kAllocVertexBuffer, bytes_of(drawn, sizeof(Vertex) * n));
  CommandWriter w2(rig.allocator());
  begin(w2, 0, kNoAllocation, {0, 0, 0, 1});
  w2.bind_pipeline(flat);
  w2.bind_vertex_buffer(1);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.draw(n);
  w2.end_pass();
  rig.render(w2, {target, svb});
  rig.read(target, out);
}

// A stream other than the first drawn: gs_streams_draw1 puts a point at each vertex in stream 1 (moved by half a pixel to the centre of a pixel) and a vector in stream 0, which only the buffer
// gets. The picture is the three points.
void scene_stream_output_draw_stream1(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureStreamOutput | graphics::kFeatureGeometryShaders)) return void(rig.step("stream output draw stream", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[3] = {{-0.5f, -0.5f, 0.5f, 1, 0, 0, 1}, {0.5f, -0.5f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.5f, 0.5f, 0, 0, 1, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle gs = rig.shader("create_shader (geometry)", ShaderKind::Dxbc, graphics::ShaderStage::Geometry, bytes_of(b::k_gs_streams_draw1, sizeof b::k_gs_streams_draw1));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.varying_count = 3;
  pd.geometry_shader = gs;
  pd.stream_output_count = 2;
  pd.stream_output[0] = {0, 4, 0, 4, 0}, pd.stream_output[1] = {1, 0, 0, 4, 1};   // (stream 0's vector made of the position and colour; stream 1's position)
  pd.stream_output_stride[0] = 16, pd.stream_output_stride[1] = 16;
  pd.rasterized_stream = 1;
  Handle pipe = rig.pipeline(pd);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  AllocationDesc od;
  od.flags = kAllocStreamOutput, od.size = 3 * 16;
  Handle first = rig.create(od);
  Handle second = rig.create(od);
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.set_stream_output(0, 2);
  w.set_stream_output(1, 3);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  rig.render(w, {target, vb, first, second});
  // (stream 0's vectors, position + colour, drawn as squares on the points: a pixel alone is little to compare)
  uint32_t g0[12];
  rig.read_words(first, ArrayView<uint32_t>(g0, 12));
  Vertex squares[18];
  for (uint32_t i = 0; i < 3; ++i) {
    const float x = __builtin_bit_cast(float, g0[4 * i]) * 0.5f, y = __builtin_bit_cast(float, g0[4 * i + 1]) * 0.5f - 0.25f, h = 0.15f;
    Vertex* q = squares + 6 * i;
    q[0] = {x - h, y - h, 0.5f, 1, 1, 0.5f, 1}, q[1] = {x + h, y - h, 0.5f, 1, 1, 0.5f, 1}, q[2] = {x - h, y + h, 0.5f, 1, 1, 0.5f, 1};
    q[3] = q[2], q[4] = q[1], q[5] = {x + h, y + h, 0.5f, 1, 1, 0.5f, 1};
  }
  Handle flat = d3d9_pipeline(rig, false, false);
  Handle svb = rig.buffer(kAllocVertexBuffer, bytes_of(squares, sizeof squares));
  CommandWriter w2(rig.allocator());
  w2.begin_pass(0, kNoAllocation, 0, {0, 0, 0, 1}, 1.0f, 0);   // (the points stay)
  w2.bind_pipeline(flat);
  w2.bind_vertex_buffer(1);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.draw(18);
  w2.end_pass();
  rig.render(w2, {target, svb});
  rig.read(target, out);
}

// Primitives with adjacency: a triangle with the three vertices beside it (the shader draws the even ones, with some of the colour of the odd one after each), and a line with the vertex
// before and after (the shader makes a quad of its four).
void scene_geometry_adjacency(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureGeometryShaders)) return void(rig.step("geometry shaders", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex v[10] = {{-0.9f, -0.9f, 0.5f, 1, 0, 0, 1}, {0.0f, 0.0f, 0.5f, 0, 0, 1, 1}, {-0.1f, -0.9f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.0f, 0.5f, 1, 1, 1, 1}, {-0.5f, 0.9f, 0.5f, 0, 0, 1, 1}, {0.0f, 0.0f, 0.5f, 1, 1, 0, 1},
                        {0.1f, -0.9f, 0.5f, 1, 0, 0, 1}, {0.9f, -0.9f, 0.5f, 0, 1, 0, 1}, {0.1f, 0.9f, 0.5f, 0, 0, 1, 1}, {0.9f, 0.9f, 0.5f, 1, 1, 0, 1}};
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  Handle triangles = 0, lines = 0;
  for (uint32_t k = 0; k < 2; ++k) {
    const uint8_t* code = k == 0 ? b::k_gs_tri_adj : b::k_gs_line_adj;
    const uint32_t size = k == 0 ? sizeof b::k_gs_tri_adj : sizeof b::k_gs_line_adj;
    Handle gs = rig.shader("create_shader (geometry)", ShaderKind::Dxbc, graphics::ShaderStage::Geometry, bytes_of(code, size));
    PipelineDesc pd = colour_pipeline_desc(vs, ps);
    pd.varying_count = 3;
    pd.geometry_shader = gs;
    (k == 0 ? triangles : lines) = rig.pipeline(pd);
  }
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_pipeline(triangles);
  w.set_topology(Topology::TriangleListAdjacency);
  w.draw(6, 0);
  w.bind_pipeline(lines);
  w.set_topology(Topology::LineListAdjacency);
  w.draw(4, 6);
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

// Draws a full-target quad (the vertex shader makes the texture coordinate from the colour: u / 2, v / 2) with a pixel shader that reads the texture in unit 0, nearest filtered, into `target`.
void show_texture(Rig& rig, Handle target, Handle texture, const uint8_t* ps, uint32_t ps_size) {
  namespace b = conformance_blobs;
  auto vert = [](float x, float y, float u, float v) { return Vertex{x, y, 0.5f, u / 2, v / 2, 1, 1}; };
  const Vertex quad[6] = {vert(-1, -1, 0, 0), vert(1, -1, 1, 0), vert(-1, 1, 0, 1), vert(-1, 1, 0, 1), vert(1, -1, 1, 0), vert(1, 1, 1, 1)};
  Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, ps, ps_size, 1);
  Handle qvb = rig.buffer(kAllocVertexBuffer, bytes_of(quad, sizeof quad));
  graphics::SamplerDesc sampler;
  sampler.filter = graphics::Filter::Nearest;
  sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.bind_texture(0, 2, sampler);
  w.draw(6);
  w.end_pass();
  rig.render(w, {target, qvb, texture});
}

// sRGB: two triangles are drawn into an sRGB render target, the second half transparent (so the blend happens on linear values and the result is encoded), and the target is then sampled
// (decoded to linear values again) into an ordinary one. The even columns of the picture are the sRGB image as it is stored, the odd ones what the sampler made of it.
void scene_srgb(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureSrgbFormats | graphics::kFeatureBlendState)) return void(rig.step("sRGB formats", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex tri[6] = {{-0.9f, -0.9f, 0.5f, 0.2f, 0.5f, 0.8f, 1.0f}, {0.9f, -0.9f, 0.5f, 0.2f, 0.5f, 0.8f, 1.0f}, {-0.9f, 0.9f, 0.5f, 0.2f, 0.5f, 0.8f, 1.0f},
                         {-0.5f, -0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 0.5f}, {0.5f, -0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 0.5f}, {-0.5f, 0.5f, 0.5f, 1.0f, 0.5f, 0.0f, 0.5f}};
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.alpha_blend = true;
  Handle pipe = rig.pipeline(pd);
  Handle texture = rig.image(kAllocRenderTarget | kAllocTexture, Format::A8R8G8B8Srgb, kSize, kSize);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle tvb = rig.buffer(kAllocVertexBuffer, bytes_of(tri, sizeof tri));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(6);
  w.end_pass();
  rig.render(w, {texture, target, tvb});
  show_texture(rig, target, texture, b::k_ps_tex, sizeof b::k_ps_tex);
  ArrayView<uint32_t> raw = allocate_view<uint32_t>(rig.allocator(), uint64_t{kSize} * kSize);
  if (raw.empty()) return void(rig.step("memory", status::kNoMemory));
  rig.read(target, out);
  rig.read(texture, raw);
  for (uint32_t i = 0; i < kSize * kSize; i += 2) out[i] = raw[i];
  release_view(rig.allocator(), raw);
}

// 16-bit normalised: a colour of 1/10000 .. 3/10000 (far below what 8 bits keep) is added twenty times into a 16-bit target and into an 8-bit one, and both are shown a hundred times
// brighter. The 8-bit one stays black (the even columns), the 16-bit one has gathered it (the odd ones).
void scene_unorm16(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureUnorm16Formats | graphics::kFeatureBlendState)) return void(rig.step("16-bit formats", status::kNotSupported));
  namespace b = conformance_blobs;
  Vertex tri[60];
  for (uint32_t i = 0; i < 20; ++i) {
    tri[3 * i] = {-0.9f, -0.9f, 0.5f, 0.0001f, 0.0002f, 0.0003f, 1.0f};
    tri[3 * i + 1] = {0.9f, -0.9f, 0.5f, 0.0001f, 0.0002f, 0.0003f, 1.0f};
    tri[3 * i + 2] = {0.0f, 0.9f, 0.5f, 0.0001f, 0.0002f, 0.0003f, 1.0f};
  }
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc add = colour_pipeline_desc(vs, ps);
  add.blend.custom = add.blend.enable = true;
  add.blend.src_color = add.blend.dst_color = add.blend.src_alpha = add.blend.dst_alpha = graphics::BlendFactor::One;
  Handle pipe = rig.pipeline(add);
  Handle narrow = rig.image(kAllocRenderTarget | kAllocTexture, Format::A8R8G8B8, kSize, kSize), wide = rig.image(kAllocRenderTarget | kAllocTexture, Format::A16B16G16R16, kSize, kSize);
  Handle shown_narrow = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize), shown_wide = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle tvb = rig.buffer(kAllocVertexBuffer, bytes_of(tri, sizeof tri));
  for (Handle image : {narrow, wide}) {
    CommandWriter w(rig.allocator());
    begin(w, 0, kNoAllocation, {0, 0, 0, 0});
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
    w.draw(60);
    w.end_pass();
    rig.render(w, {image, tvb});
  }
  show_texture(rig, shown_narrow, narrow, b::k_ps_scale100, sizeof b::k_ps_scale100);
  show_texture(rig, shown_wide, wide, b::k_ps_scale100, sizeof b::k_ps_scale100);
  ArrayView<uint32_t> other = allocate_view<uint32_t>(rig.allocator(), uint64_t{kSize} * kSize);
  if (other.empty()) return void(rig.step("memory", status::kNoMemory));
  rig.read(shown_narrow, out);
  rig.read(shown_wide, other);
  for (uint32_t i = 1; i < kSize * kSize; i += 2) out[i] = other[i];
  release_view(rig.allocator(), other);
}

// A multisampled image read by a shader (ps_ms.hlsl: Texture2DMS Load, sample (x mod 4) of the pixel at x, y): two rectangles with edges off the pixel grid drawn into a 4x multisampled
// target (the colours of its samples are what the picture shows, one sample a column), then the target itself is bound to the pixel shader as a texture.
void scene_multisample_resource(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureMultisample | graphics::kFeatureTexelAccess | graphics::kFeatureSystemInputs)) return void(rig.step("multisampling", status::kNotSupported));
  namespace b = conformance_blobs;
  auto rect = [](Vertex* v, float x0, float y0, float x1, float y1, float z, float r, float g, float bl) {
    auto at = [&](float px, float py) { return Vertex{px / 16.0f - 1.0f, py / 16.0f - 1.0f, z, r, g, bl, 1}; };
    v[0] = at(x0, y0), v[1] = at(x1, y0), v[2] = at(x0, y1);
    v[3] = at(x0, y1), v[4] = at(x1, y0), v[5] = at(x1, y1);
  };
  Vertex v[12];
  rect(v, 4.3f, 3.4f, 20.6f, 22.7f, 0.6f, 1, 0, 0);
  rect(v + 6, 12.4f, 9.2f, 27.8f, 28.6f, 0.3f, 0, 1, 0);
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.depth_test = pd.depth_write = true;
  pd.multisample = true;
  Handle pipe = rig.pipeline(pd);
  AllocationDesc color_desc, depth_desc;
  color_desc.flags = kAllocRenderTarget | kAllocTexture, color_desc.format = Format::A8R8G8B8, color_desc.width = color_desc.height = kSize, color_desc.samples = 4;
  depth_desc.flags = kAllocDepthStencil, depth_desc.format = Format::D32Float, depth_desc.width = depth_desc.height = kSize, depth_desc.samples = 4;
  Handle multisampled = rig.create(color_desc), depth = rig.create(depth_desc);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  w.begin_pass(0, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1.0f, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(12);
  w.end_pass();
  rig.render(w, {multisampled, depth, vb});
  // The second pass: a quad over the target, the pixel shader loading the samples.
  auto vert = [](float x, float y) { return Vertex{x, y, 0.5f, 1, 1, 1, 1}; };
  const Vertex quad[6] = {vert(-1, -1), vert(1, -1), vert(-1, 1), vert(-1, 1), vert(1, -1), vert(1, 1)};
  Handle show = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_ms, sizeof b::k_ps_ms, 1);
  Handle qvb = rig.buffer(kAllocVertexBuffer, bytes_of(quad, sizeof quad));
  graphics::SamplerDesc sampler;
  CommandWriter w2(rig.allocator());
  begin(w2, 0, kNoAllocation, {0, 0, 0, 1});
  w2.bind_pipeline(show);
  w2.bind_vertex_buffer(1);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.bind_texture(0, 2, sampler);
  w2.draw(6);
  w2.end_pass();
  rig.render(w2, {target, qvb, multisampled});
  rig.read(target, out);
}

// Depth resolve: two rectangles with edges off the pixel grid (the near one at depth 0.3, the far one at 0.7, behind it where they overlap) are drawn into a 4x multisampled depth buffer, which
// is resolved (sample 0, or the least or greatest of the samples) into a plain one; then the plain depth buffer is read by drawing over it with the depth test: a quad at depth 0.5 that
// shows green where the stored depth is greater (0.7 and the cleared 1), and one at 0.85 over it that shows blue where it is the cleared 1, so the picture says which of the three depths each pixel
// holds: black (0.3), green (0.7), blue (1).
void depth_resolve_scene(Rig& rig, ArrayView<uint32_t> out, uint32_t mode) {
  if (!rig.has(graphics::kFeatureMultisample | graphics::kFeatureDepthResolve | (mode == 1 ? 0u : graphics::kFeatureResolveModes))) return void(rig.step("depth resolve", status::kNotSupported));
  auto rect = [](Vertex* v, float x0, float y0, float x1, float y1, float z, float r, float g, float b) {
    auto at = [&](float px, float py) { return Vertex{px / 16.0f - 1.0f, py / 16.0f - 1.0f, z, r, g, b, 1}; };
    v[0] = at(x0, y0), v[1] = at(x1, y0), v[2] = at(x0, y1);
    v[3] = at(x0, y1), v[4] = at(x1, y0), v[5] = at(x1, y1);
  };
  Vertex v[12];
  rect(v, 4.3f, 3.4f, 20.6f, 22.7f, 0.3f, 1, 0, 0);
  rect(v + 6, 12.4f, 9.2f, 27.8f, 28.6f, 0.7f, 0, 1, 0);
  Vertex probes[12];
  rect(probes, 0, 0, 32, 32, 0.5f, 0, 1, 0);
  rect(probes + 6, 0, 0, 32, 32, 0.85f, 0, 0, 1);
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc draw = colour_pipeline_desc(vs, ps);
  draw.depth_test = draw.depth_write = true;
  draw.multisample = true;
  Handle draw_pipe = rig.pipeline(draw);
  PipelineDesc probe = colour_pipeline_desc(vs, ps);
  probe.depth_test = true;
  probe.depth_compare = graphics::CompareOp::Less;
  Handle probe_pipe = rig.pipeline(probe);
  AllocationDesc color_desc, depth_desc;
  color_desc.flags = kAllocRenderTarget, color_desc.format = Format::A8R8G8B8, color_desc.width = color_desc.height = kSize, color_desc.samples = 4;
  depth_desc.flags = kAllocDepthStencil, depth_desc.format = Format::D32Float, depth_desc.width = depth_desc.height = kSize, depth_desc.samples = 4;
  Handle ms_color = rig.create(color_desc), ms_depth = rig.create(depth_desc);
  Handle plain_depth = rig.image(kAllocDepthStencil, Format::D32Float, kSize, kSize);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v)), pvb = rig.buffer(kAllocVertexBuffer, bytes_of(probes, sizeof probes));
  CommandWriter w(rig.allocator());
  w.begin_pass(0, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1.0f, 0);
  w.bind_pipeline(draw_pipe);
  w.bind_vertex_buffer(3);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(12);
  w.end_pass();
  w.resolve_region(2, 1, mode);
  rig.render(w, {ms_color, ms_depth, plain_depth, vb});
  CommandWriter w2(rig.allocator());
  w2.begin_pass(0, 1, kPassClearColor, {0, 0, 0, 1}, 1.0f, 0);
  w2.bind_pipeline(probe_pipe);
  w2.bind_vertex_buffer(2);
  w2.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w2.draw(12);
  w2.end_pass();
  rig.render(w2, {target, plain_depth, pvb});
  rig.read(target, out);
}
void scene_depth_resolve_sample0(Rig& rig, ArrayView<uint32_t> out) { depth_resolve_scene(rig, out, 1); }
void scene_depth_resolve_min(Rig& rig, ArrayView<uint32_t> out) { depth_resolve_scene(rig, out, 2); }
void scene_depth_resolve_max(Rig& rig, ArrayView<uint32_t> out) { depth_resolve_scene(rig, out, 3); }

// Alpha to coverage: five bands of whole pixels, all the same orange, with alphas 0, 1, 0, 1 and 1: into a 4x multisampled target the alpha 0 bands keep none of their samples and the
// others all, so the resolved picture is black, orange, black, orange, orange. (A GPU gives a fraction of an alpha a number of samples that varies from pixel to pixel, which a picture cannot
// compare; the CPU renderer's own rounding is tested in test_alpha_to_coverage.)
void scene_alpha_to_coverage(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureMultisample)) return void(rig.step("multisampling", status::kNotSupported));
  Vertex v[30];
  static const float kAlpha[5] = {0.0f, 1.0f, 0.0f, 1.0f, 1.0f};   // (only the ends: in between, a GPU dithers the number of samples from pixel to pixel)
  for (uint32_t i = 0; i < 5; ++i) {
    const float x0 = static_cast<float>(i * 6) / 16.0f - 1.0f, x1 = static_cast<float>(i * 6 + 6) / 16.0f - 1.0f, y0 = -1.0f, y1 = 1.0f;
    Vertex* p = v + i * 6;
    p[0] = {x0, y0, 0.5f, 1.0f, 0.5f, 0.2f, kAlpha[i]}, p[1] = {x1, y0, 0.5f, 1.0f, 0.5f, 0.2f, kAlpha[i]}, p[2] = {x0, y1, 0.5f, 1.0f, 0.5f, 0.2f, kAlpha[i]};
    p[3] = p[2], p[4] = p[1], p[5] = {x1, y1, 0.5f, 1.0f, 0.5f, 0.2f, kAlpha[i]};
  }
  Handle vs = rig.vertex_shader(bytes_of(kVertexTokens, sizeof kVertexTokens), ShaderKind::D3d9Bytecode);
  Handle ps = rig.pixel_shader(bytes_of(kPixelTokens, sizeof kPixelTokens), ShaderKind::D3d9Bytecode);
  PipelineDesc pd = colour_pipeline_desc(vs, ps);
  pd.multisample = true;
  pd.alpha_to_coverage = true;
  Handle pipe = rig.pipeline(pd);
  AllocationDesc color_desc;
  color_desc.flags = kAllocRenderTarget, color_desc.format = Format::A8R8G8B8, color_desc.width = color_desc.height = kSize, color_desc.samples = 4;
  Handle multisampled = rig.create(color_desc);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof v));
  CommandWriter w(rig.allocator());
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1.0f, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(30);
  w.end_pass();
  w.resolve_region(1, 0);
  rig.render(w, {multisampled, target, vb});
  rig.read(target, out);
}

// Integers: a triangle's colour is written by the pixel shader as four integers (colour * 255) into an R32G32B32A32_UINT target cleared to (20, 40, 60, 255), and a second pass reads them
// back with Load (an integer texture is not sampled) and shows them divided by 255.
void scene_integer(Rig& rig, ArrayView<uint32_t> out) {
  if (!rig.has(graphics::kFeatureIntegerFormats)) return void(rig.step("integer formats", status::kNotSupported));
  namespace b = conformance_blobs;
  const Vertex tri[3] = {{-0.9f, -0.9f, 0.5f, 0.2f, 0.5f, 0.8f, 1.0f}, {0.9f, -0.9f, 0.5f, 0.2f, 0.5f, 0.8f, 1.0f}, {0.0f, 0.9f, 0.5f, 0.2f, 0.5f, 0.8f, 1.0f}};
  Handle first = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, b::k_ps_int_write, sizeof b::k_ps_int_write, 0);
  Handle texture = rig.image(kAllocRenderTarget | kAllocTexture, Format::A32B32G32R32Uint, kSize, kSize);
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle tvb = rig.buffer(kAllocVertexBuffer, bytes_of(tri, sizeof tri));
  CommandWriter w(rig.allocator());
  // (the clear colour: the bits of the integers)
  begin(w, 0, kNoAllocation, {__builtin_bit_cast(float, 20u), __builtin_bit_cast(float, 40u), __builtin_bit_cast(float, 60u), __builtin_bit_cast(float, 255u)});
  w.bind_pipeline(first);
  w.bind_vertex_buffer(2);
  w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
  w.draw(3);
  w.end_pass();
  rig.render(w, {texture, target, tvb});
  show_texture(rig, target, texture, b::k_ps_int_load, sizeof b::k_ps_int_load);
  rig.read(target, out);
}

// Render target formats: a triangle is drawn into a 16 x 16 target of each format (the clear colour and the colour of the triangle are the same for all), and each target is then shown in a
// quarter of the picture through a pixel shader that reads it back (Load for the integers, Sample for the others).
struct FormatBlob {
  const uint8_t* code;
  uint32_t size;
};
struct FormatCase {
  Format format;
  FormatBlob write, show;
  float colour[4];       // the triangle's colour (what the vertices carry)
  bool integer;
};
#define FORMAT_BLOB(name) FormatBlob{conformance_blobs::k_##name, sizeof conformance_blobs::k_##name}

void formats_scene(Rig& rig, ArrayView<uint32_t> out, const FormatCase* cases, uint32_t count, uint32_t feature) {
  if (!rig.has(feature)) return void(rig.step("render target formats", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  for (uint32_t i = 0; i < count; ++i) {
    const FormatCase& c = cases[i];
    const Vertex tri[3] = {{-0.9f, -0.9f, 0.5f, c.colour[0], c.colour[1], c.colour[2], c.colour[3]}, {0.9f, -0.9f, 0.5f, c.colour[0], c.colour[1], c.colour[2], c.colour[3]},
                           {0.0f, 0.9f, 0.5f, c.colour[0], c.colour[1], c.colour[2], c.colour[3]}};
    Handle texture = rig.image(kAllocRenderTarget | kAllocTexture, c.format, 16, 16);
    Handle write = rig.pixel_shader(bytes_of(c.write.code, c.write.size));
    PipelineDesc wd = colour_pipeline_desc(vs, write);
    wd.varying_count = 3;
    Handle first = rig.pipeline(wd);
    Handle tvb = rig.buffer(kAllocVertexBuffer, bytes_of(tri, sizeof tri));
    CommandWriter w(rig.allocator());
    // (the clear colour of an integer target is the bits of the integers)
    const graphics::Vec4 clear = c.integer ? graphics::Vec4{__builtin_bit_cast(float, 20u), __builtin_bit_cast(float, 40u), __builtin_bit_cast(float, 60u), __builtin_bit_cast(float, 255u)} : graphics::Vec4{0, 0, 0, 0};
    begin(w, 0, kNoAllocation, clear);
    w.bind_pipeline(first);
    w.bind_vertex_buffer(2);
    w.push_constants(bytes_of(kIdentity, sizeof kIdentity));
    w.draw(3);
    w.end_pass();
    rig.render(w, {texture, target, tvb});
    // The target in its quarter of the picture.
    auto vert = [](float x, float y, float u, float v) { return Vertex{x, y, 0.5f, u / 2, v / 2, 1, 1}; };
    const Vertex quad[6] = {vert(-1, -1, 0, 0), vert(1, -1, 1, 0), vert(-1, 1, 0, 1), vert(-1, 1, 0, 1), vert(1, -1, 1, 0), vert(1, 1, 1, 1)};
    Handle pipe = dxbc_pipeline(rig, b::k_vs, sizeof b::k_vs, c.show.code, c.show.size, 1);
    Handle qvb = rig.buffer(kAllocVertexBuffer, bytes_of(quad, sizeof quad));
    graphics::SamplerDesc sampler;
    sampler.filter = graphics::Filter::Nearest;
    sampler.address_u = sampler.address_v = graphics::AddressMode::ClampToEdge;
    CommandWriter show(rig.allocator());
    show.begin_pass(0, kNoAllocation, i == 0 ? kPassClearColor : 0u, {0, 0, 0, 1}, 1.0f, 0);
    const float x = static_cast<float>((i & 1) * 16), y = static_cast<float>((i >> 1) * 16);
    show.set_viewport({x, y, 16, 16, 0.0f, 1.0f});
    show.set_scissor({static_cast<int32_t>(x), static_cast<int32_t>(y), 16, 16});
    show.bind_pipeline(pipe);
    show.bind_vertex_buffer(1);
    show.push_constants(bytes_of(kIdentity, sizeof kIdentity));
    show.bind_texture(0, 2, sampler);
    show.draw(6);
    show.end_pass();
    rig.render(show, {target, qvb, texture});
  }
  rig.read(target, out);
}

#define FORMAT_SCENE(name, feature, ...)                                      \
  void name(Rig& rig, ArrayView<uint32_t> out) {                              \
    static const FormatCase cases[] = {__VA_ARGS__};                          \
    formats_scene(rig, out, cases, sizeof cases / sizeof cases[0], feature);  \
  }
// Integers of every width and sign: the writer makes colour * 255 (minus 100 for the signed ones) as integers, the reader shows them back as (value (+ 100)) / 255.
FORMAT_SCENE(scene_integer_formats, graphics::kFeatureIntegerFormats,
             {Format::A16B16G16R16Uint, FORMAT_BLOB(ps_int_write), FORMAT_BLOB(ps_uint_loadq), {0.2f, 0.5f, 0.8f, 1.0f}, true},
             {Format::A16B16G16R16Sint, FORMAT_BLOB(ps_sint_write), FORMAT_BLOB(ps_sint_loadq), {0.3f, 0.6f, 0.9f, 1.0f}, true},
             {Format::A8B8G8R8Uint, FORMAT_BLOB(ps_int_write), FORMAT_BLOB(ps_uint_loadq), {0.9f, 0.4f, 0.1f, 1.0f}, true},
             {Format::A32B32G32R32Sint, FORMAT_BLOB(ps_sint_write), FORMAT_BLOB(ps_sint_loadq), {0.1f, 0.7f, 0.4f, 1.0f}, true})
// Signed and 10-bit normalised: a negative colour into a snorm target (shown with its green negated), and 10 bit colours with two bits of alpha (0.7 is kept as 2/3).
FORMAT_SCENE(scene_normalised_formats, graphics::kFeatureSnormFormats,
             {Format::A16B16G16R16Snorm, FORMAT_BLOB(ps_color), FORMAT_BLOB(ps_snorm_show), {0.3f, -0.6f, 0.9f, 1.0f}, false},
             {Format::A16B16G16R16Snorm, FORMAT_BLOB(ps_color), FORMAT_BLOB(ps_snorm_show), {-0.2f, -0.9f, 0.45f, 0.5f}, false},
             {Format::A2B10G10R10, FORMAT_BLOB(ps_color), FORMAT_BLOB(ps_show), {0.2f, 0.5f, 0.8f, 0.7f}, false},
             {Format::A2B10G10R10, FORMAT_BLOB(ps_color), FORMAT_BLOB(ps_show), {0.05f, 0.95f, 0.5f, 0.3f}, false})

// Tessellation: a few patches drawn into one picture, each in its own cell (a single one fills the picture), with the tessellation factors in the constants after the matrix. The domain shaders move every point by an amount of its own, so a point that is not where the tessellator should put it, or a
// triangle that joins other points, shows in the picture.
struct TessBlob {
  const uint8_t* code;
  uint32_t size;
};
struct TessCase {
  TessBlob hull, domain;
  uint32_t points;           // control points of a patch: 3 (triangle), 4 (quad), 2 (isoline)
  float f0[4], f1[4];        // the constants after the matrix: the hull shaders read their factors there (a triangle's inside factor is the fourth of the first)
  graphics::CullMode cull;
};
#define TESS_BLOB(name) TessBlob{conformance_blobs::k_##name, sizeof conformance_blobs::k_##name}

void tess_scene(Rig& rig, ArrayView<uint32_t> out, const TessCase* cases, uint32_t count) {
  if (!rig.has(graphics::kFeatureTessellation)) return void(rig.step("tessellation", status::kNotSupported));
  namespace b = conformance_blobs;
  Handle vs = rig.vertex_shader(bytes_of(b::k_vs, sizeof b::k_vs));
  Handle ps = rig.pixel_shader(bytes_of(b::k_ps_color, sizeof b::k_ps_color));
  Handle target = rig.image(kAllocRenderTarget, Format::A8R8G8B8, kSize, kSize);
  if (count == 0 || count > 4) return void(rig.step("tessellation (cases)", status::kInvalidParameter));
  // The patches: the control points in the corners of a cell (red, green, blue, yellow).
  Vertex v[4 * 4];
  for (uint32_t i = 0; i < count; ++i) {
    const float h = count == 1 ? 0.85f : 0.42f;
    const float cx = count == 1 ? 0.0f : (i & 1 ? 0.5f : -0.5f), cy = count == 1 ? 0.0f : (i & 2 ? 0.5f : -0.5f);
    Vertex* p = v + i * 4;
    const uint32_t n = cases[i].points;
    if (n == 3) {
      p[0] = {cx - h, cy - h, 0.5f, 1, 0, 0, 1}, p[1] = {cx + h, cy - h, 0.5f, 0, 1, 0, 1}, p[2] = {cx, cy + h, 0.5f, 0, 0, 1, 1};
    } else if (n == 4) {
      p[0] = {cx - h, cy - h, 0.5f, 1, 0, 0, 1}, p[1] = {cx + h, cy - h, 0.5f, 0, 1, 0, 1}, p[2] = {cx - h, cy + h, 0.5f, 0, 0, 1, 1}, p[3] = {cx + h, cy + h, 0.5f, 1, 1, 0, 1};
    } else {
      p[0] = {cx - h, cy, 0.5f, 1, 0, 0, 1}, p[1] = {cx + h, cy, 0.5f, 0, 1, 0, 1};
    }
  }
  Handle vb = rig.buffer(kAllocVertexBuffer, bytes_of(v, sizeof(Vertex) * 4 * count));
  CommandWriter w(rig.allocator());
  begin(w, 0, kNoAllocation, {0, 0, 0, 1});
  for (uint32_t i = 0; i < count; ++i) {
    const TessCase& c = cases[i];
    Handle hs = rig.shader("create_shader (hull)", ShaderKind::Dxbc, graphics::ShaderStage::Hull, bytes_of(c.hull.code, c.hull.size));
    Handle ds = rig.shader("create_shader (domain)", ShaderKind::Dxbc, graphics::ShaderStage::Domain, bytes_of(c.domain.code, c.domain.size));
    PipelineDesc pd = colour_pipeline_desc(vs, ps);
    pd.varying_count = 3;
    pd.hull_shader = hs;
    pd.domain_shader = ds;
    pd.cull_mode = c.cull;
    Handle pipe = rig.pipeline(pd);
    float constants[24];
    for (uint32_t k = 0; k < 16; ++k) constants[k] = kIdentity[k];
    for (uint32_t k = 0; k < 4; ++k) constants[16 + k] = c.f0[k], constants[20 + k] = c.f1[k];
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(bytes_of(constants, sizeof constants));
    w.set_topology(patch_list(c.points));
    w.draw(c.points, i * 4);
  }
  w.end_pass();
  rig.render(w, {target, vb});
  rig.read(target, out);
}

#define TESS_SCENE(name, ...)                                         \
  void name(Rig& rig, ArrayView<uint32_t> out) {                      \
    static constexpr TessCase cases[] = {__VA_ARGS__};                \
    tess_scene(rig, out, cases, sizeof cases / sizeof cases[0]);      \
  }
constexpr auto kNoCull = graphics::CullMode::None;

// Triangles of different factors (odd and even, edges that differ, an inside factor of 1), every one cut in rings of its own.
TESS_SCENE(scene_tess_triangle,
           {TESS_BLOB(hs_tri_cb), TESS_BLOB(ds_noise), 3, {5, 5, 5, 5}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_tri_cb), TESS_BLOB(ds_noise), 3, {4, 4, 4, 4}, {0, 0, 0, 0}, kNoCull},
           {TESS_BLOB(hs_tri_cb), TESS_BLOB(ds_noise), 3, {3, 6, 2, 4}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_tri_cb), TESS_BLOB(ds_noise), 3, {1, 7, 4, 1}, {0, 0, 0, 0}, kNoCull})
// Quads: even and odd rings, edges that differ from the inside, and a strip of quads left in the middle.
TESS_SCENE(scene_tess_quad,
           {TESS_BLOB(hs_quad_cb), TESS_BLOB(ds_quad), 4, {4, 4, 4, 4}, {4, 4, 0, 0}, kNoCull}, {TESS_BLOB(hs_quad_cb), TESS_BLOB(ds_quad), 4, {5, 5, 5, 5}, {5, 5, 0, 0}, kNoCull},
           {TESS_BLOB(hs_quad_cb), TESS_BLOB(ds_quad), 4, {2, 3, 5, 8}, {4, 3, 0, 0}, kNoCull}, {TESS_BLOB(hs_quad_cb), TESS_BLOB(ds_quad), 4, {3, 3, 3, 3}, {7, 3, 0, 0}, kNoCull})
// Isolines drawn as lines and as points, and the points of a triangle and of a quad.
TESS_SCENE(scene_tess_lines_points,
           {TESS_BLOB(hs_iso_cb), TESS_BLOB(ds_iso), 2, {4, 6, 0, 0}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_iso_point_cb), TESS_BLOB(ds_iso), 2, {3, 5, 0, 0}, {0, 0, 0, 0}, kNoCull},
           {TESS_BLOB(hs_tri_point_cb), TESS_BLOB(ds_noise), 3, {5, 5, 5, 5}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_quad_point_cb), TESS_BLOB(ds_quad), 4, {4, 4, 4, 4}, {3, 3, 0, 0}, kNoCull})
// Which way the triangles wind: with back faces culled, the clockwise output of a triangle patch and of a quad patch is drawn or not as the counter-clockwise output is not or is.
TESS_SCENE(scene_tess_winding,
           {TESS_BLOB(hs_tri_cb), TESS_BLOB(ds_noise), 3, {3, 3, 3, 3}, {0, 0, 0, 0}, graphics::CullMode::Back}, {TESS_BLOB(hs_tri_ccw_cb), TESS_BLOB(ds_noise), 3, {3, 3, 3, 3}, {0, 0, 0, 0}, graphics::CullMode::Back},
           {TESS_BLOB(hs_quad_cb), TESS_BLOB(ds_quad), 4, {3, 3, 3, 3}, {3, 3, 0, 0}, graphics::CullMode::Back}, {TESS_BLOB(hs_quad_ccw_cb), TESS_BLOB(ds_quad), 4, {3, 3, 3, 3}, {3, 3, 0, 0}, graphics::CullMode::Back})
// Power-of-two partitioning: factors that are rounded up to 4, 8, 2 and 4.
TESS_SCENE(scene_tess_pow2,
           {TESS_BLOB(hs_tri_pow2_cb), TESS_BLOB(ds_noise), 3, {3, 5, 2, 3}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_tri_pow2_cb), TESS_BLOB(ds_noise), 3, {6, 6, 6, 6}, {0, 0, 0, 0}, kNoCull})

// Fractional partitioning: the factors are not whole numbers; the points of an edge are placed between those of the whole factors on either side of it.
TESS_SCENE(scene_tess_fractional_odd,
           {TESS_BLOB(hs_tri_odd_cb), TESS_BLOB(ds_noise), 3, {2.5f, 3.7f, 5.2f, 4.4f}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_quad_odd_cb), TESS_BLOB(ds_quad), 4, {3.3f, 2.2f, 6.6f, 4.1f}, {3.7f, 5.5f, 0, 0}, kNoCull},
           {TESS_BLOB(hs_iso_odd_cb), TESS_BLOB(ds_iso), 2, {3, 5.5f, 0, 0}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_tri_odd_cb), TESS_BLOB(ds_noise), 3, {1, 1, 1, 1.6f}, {0, 0, 0, 0}, kNoCull})
TESS_SCENE(scene_tess_fractional_even,
           {TESS_BLOB(hs_tri_even_cb), TESS_BLOB(ds_noise), 3, {2.5f, 3.7f, 5.2f, 4.4f}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_quad_even_cb), TESS_BLOB(ds_quad), 4, {3.3f, 2.2f, 6.6f, 4.1f}, {3.7f, 5.5f, 0, 0}, kNoCull},
           {TESS_BLOB(hs_iso_even_cb), TESS_BLOB(ds_iso), 2, {3, 5.5f, 0, 0}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(hs_tri_even_cb), TESS_BLOB(ds_noise), 3, {1, 1, 1, 1.6f}, {0, 0, 0, 0}, kNoCull})

// The same with DXIL (Shader Model 6) hull and domain shaders: a triangle patch and a quad patch with different factors.
TESS_SCENE(scene_dxil_tess,
           {TESS_BLOB(dxil_hs_tri_cb), TESS_BLOB(dxil_ds_noise), 3, {5, 5, 5, 5}, {0, 0, 0, 0}, kNoCull}, {TESS_BLOB(dxil_hs_tri_cb), TESS_BLOB(dxil_ds_noise), 3, {3, 6, 2, 4}, {0, 0, 0, 0}, kNoCull},
           {TESS_BLOB(dxil_hs_quad_cb), TESS_BLOB(dxil_ds_quad), 4, {4, 4, 4, 4}, {4, 4, 0, 0}, kNoCull}, {TESS_BLOB(dxil_hs_quad_cb), TESS_BLOB(dxil_ds_quad), 4, {2, 3, 5, 8}, {4, 3, 0, 0}, kNoCull})

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
    {"Direct3D 10 geometry shader (DXBC)", scene_geometry_shader},
    {"Direct3D 11 instanced geometry (DXBC)", scene_geometry_instances},
    {"Direct3D 10 adjacency primitives (DXBC)", scene_geometry_adjacency},
    {"Direct3D 12 root signature, descriptors and states: compute (DXBC)", scene_d3d12_compute},
    {"Direct3D 12 root signature, descriptors and states: textures (DXBC)", scene_d3d12_textures},
    {"Direct3D 12 resource heap: a target and a texture aliased (DXBC)", scene_d3d12_heap},
    {"Direct3D 12 DXIL shaders (Shader Model 6)", scene_dxil},
    {"Direct3D 12 DXIL compute shader (Shader Model 6)", scene_dxil_compute},
    {"Direct3D 12 DXIL geometry shader (Shader Model 6)", scene_dxil_geometry},
    {"Direct3D 12 DXIL hull and domain shaders (Shader Model 6)", scene_dxil_tess},
    {"Direct3D 12 DXIL gather4 (Shader Model 6)", scene_dxil_gather},
    {"Direct3D 12 DXIL derivatives (Shader Model 6)", scene_dxil_derivatives},
    {"Direct3D 12 DXIL sample_d and sample_b (Shader Model 6)", scene_dxil_grad},
    {"Direct3D 12 DXIL CalculateLevelOfDetail (Shader Model 6)", scene_dxil_lod},
    {"Direct3D 12 DXIL comparison sampling (Shader Model 6)", scene_dxil_compare},
    {"Direct3D 12 DXIL double precision (Shader Model 6)", scene_dxil_double},
    {"Direct3D 12 DXIL textures picked by a variable index (Shader Model 6)", scene_dxil_texture_array},
    {"Direct3D 12 DXIL buffer shader resource views (Shader Model 6)", scene_dxil_compute_srv},
    {"Direct3D 12 DXIL group-shared memory, barriers and a loop (Shader Model 6)", scene_dxil_compute_shared},
    {"Direct3D 11 stream output of two streams (DXBC)", scene_stream_output},
    {"Direct3D 11 stream output appending, statistics and timestamps (DXBC)", scene_stream_output_append},
    {"Direct3D 11 stream output of vertex and domain shaders (DXBC)", scene_stream_output_stages},
    {"Direct3D 11 stream output drawing the second stream (DXBC)", scene_stream_output_draw_stream1},
    {"Direct3D 11 tessellation, triangle domain (DXBC)", scene_tess_triangle, false, 6},
    {"Direct3D 11 tessellation, quad domain (DXBC)", scene_tess_quad, false, 6},
    {"Direct3D 11 tessellation, isolines and points (DXBC)", scene_tess_lines_points, true},
    {"Direct3D 11 tessellation, winding (DXBC)", scene_tess_winding},
    {"Direct3D 11 tessellation, power-of-two partitioning (DXBC)", scene_tess_pow2, false, 6},
    {"Direct3D 11 tessellation, fractional odd partitioning (DXBC)", scene_tess_fractional_odd, true},
    {"Direct3D 11 tessellation, fractional even partitioning (DXBC)", scene_tess_fractional_even, true},
    {"Direct3D 10 sRGB render target and texture", scene_srgb},
    {"Direct3D 10 16-bit normalised render target and texture", scene_unorm16},
    {"Direct3D 10 integer render target and Load (DXBC)", scene_integer},
    {"Direct3D 10 integer formats of every width and sign (DXBC)", scene_integer_formats},
    {"Direct3D 10 signed and 10-bit normalised formats (DXBC)", scene_normalised_formats},
    {"Direct3D 10 4x multisampling", scene_multisample},
    {"Direct3D 10 alpha to coverage", scene_alpha_to_coverage},
    {"Direct3D 12 depth resolve, first sample", scene_depth_resolve_sample0},
    {"Direct3D 12 depth resolve, least sample", scene_depth_resolve_min},
    {"Direct3D 12 depth resolve, greatest sample", scene_depth_resolve_max},
    {"Direct3D 10 multisampled image as a shader resource (DXBC)", scene_multisample_resource},
    {"Direct3D 11 compute shader and unordered access views (DXBC)", scene_compute},
    {"Direct3D 11 group-shared memory and barriers (DXBC)", scene_compute_shared},
    {"Direct3D 10 independent blend per render target (DXBC)", scene_independent_blend},
    {"Direct3D 10 half-float render target and texture", scene_half_float_target},
    {"Direct3D 10 float render target and texture", scene_float_target},
    {"Direct3D 10 occlusion queries", scene_occlusion_query},
    {"Direct3D 10 occlusion predicate and pipeline statistics", scene_statistics_queries},
    {"Direct3D 10 volume texture (DXBC)", scene_texture_3d},
    {"Direct3D 10 texture array (DXBC)", scene_texture_array},
    {"Direct3D 10 cube texture (DXBC)", scene_texture_cube},
    {"Direct3D 10 Load, SampleLevel, GetDimensions of a 3D image (DXBC)", scene_texel_3d},
    {"Direct3D 10 Load, SampleLevel, GetDimensions of an image array (DXBC)", scene_texel_array},
    {"Direct3D 10 SampleLevel, GetDimensions of a cube map (DXBC)", scene_texel_cube},
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
        if (result.bad_pixels > scene.allowed_bad) result.verdict = SceneVerdict::Differs, ++bad;
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
