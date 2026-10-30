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

#include "Kernel/Dxgkrnl/Adapter.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/Mipmaps.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace dxgk {

namespace {

constexpr uint64_t kMaxShaderBytes = 256 * 1024;
constexpr uint32_t kMaxVertexStride = 2048;
constexpr float kMaxCoordinate = 1.0e6f;
constexpr int32_t kMaxScissor = 1 << 16;

bool finite_within(float v, float limit) { return v == v && v >= -limit && v <= limit; }

// The backing of an allocation is allocated 16-byte aligned, so it is a whole number of aligned words (false would mean a bug in VidMm).
bool as_words(ByteView bytes, ArrayView<uint32_t>* out) { return bytes.try_cast<uint32_t>(out); }

// The device takes shader text as a `const char*`. The bytes are the shader's own copy, NUL terminated by the caller, and char has no alignment requirement.
const char* text_pointer(ByteView text) { return reinterpret_cast<const char*>(text.data()); }

uint32_t vertex_format_bytes(graphics::VertexFormat f) { return (static_cast<uint32_t>(f) + 1) * 4; }

// Copies a w x h block of pixels between the system-memory backings of two allocations of one format (they may be the same allocation, the blocks overlapping).
// The GPU's newer contents are read back first, and the destination's device object is marked stale. The caller has checked the rectangles.
NtStatus copy_region_cpu(VidMm& mm, Allocation& dst, uint32_t dx, uint32_t dy, Allocation& src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
  if (!mm.read_back(src) || !mm.read_back(dst)) return status::kInvalidDeviceState;
  const uint32_t bpp = pixel_bytes(dst.desc().format);   // (the formats are equal)
  const uint64_t row_bytes = uint64_t{w} * bpp;
  const bool bottom_up = &dst == &src && dy > sy;   // (overlapping rows: copy the ones that would be overwritten last)
  for (uint32_t i = 0; i < h; ++i) {
    const uint32_t y = bottom_up ? h - 1 - i : i;
    ByteView to;
    ConstByteView from;
    if (!dst.writable_backing().try_subview((uint64_t{dy + y} * dst.desc().width + dx) * bpp, row_bytes, &to) ||
        !src.backing().try_subview((uint64_t{sy + y} * src.desc().width + sx) * bpp, row_bytes, &from))
      return status::kInvalidParameter;
    to.copy_from(from);
  }
  mm.note_cpu_write(dst);
  return status::kSuccess;
}

}  // namespace

ShaderObject::~ShaderObject() {
  if (module_) device_.destroy_shader_module(module_);
  release_view(allocator_, text_);
  release_view(allocator_, tokens_);
}

PipelineObject::~PipelineObject() {
  if (pipeline_) device_.destroy_pipeline(pipeline_);
}

// --- Running a command stream ---------------------------------------------------------------------------------------------------------------------------------------

// Walks a command stream once, either only to check it (against the allocation list and the pipelines the process owns) or to replay it on the device. Checking and replaying
// share the walk so that they cannot disagree about what is valid; replaying additionally checks what can change between the two (the contents of an index buffer) and does
// what the checks promised: puts the allocations on the GPU, records the device commands and submits them.
class Adapter::StreamRunner {
 public:
  StreamRunner(Adapter& adapter, ConstByteView commands, ArrayView<const SharedPointer<Allocation>> allocations)
      : adapter_(adapter), mm_(adapter.vidmm_), commands_(commands), allocations_(allocations), pending_(adapter.allocator_), transient_(adapter.allocator_) {}
  ~StreamRunner() { release_transient(); }

  NtStatus validate(IPipelineLookup& lookup, List<SharedPointer<PipelineObject>>& pipelines) {
    exec_ = false;
    lookup_ = &lookup;
    collected_ = &pipelines;
    return walk();
  }

  NtStatus execute(ArrayView<const SharedPointer<PipelineObject>> pipelines) {
    exec_ = true;
    replay_ = pipelines;
    for (const SharedPointer<Allocation>& a : allocations_) mm_.pin(*a);
    NtStatus s = walk();
    if (cb_) {
      adapter_.device_.destroy_command_buffer(cb_);   // (only after a failure: a good run flushed it)
      cb_ = nullptr;
    }
    for (const SharedPointer<Allocation>& a : allocations_) mm_.unpin(*a);
    return s;
  }

 private:
  static constexpr NtStatus kBad = status::kInvalidParameter;

  Allocation* alloc(uint32_t index) const { return index < allocations_.size() ? allocations_[index].get() : nullptr; }
  static bool exact(const ByteReader& p) { return p.ok() && p.at_end(); }
  static bool is_extra_target(Allocation* const* extras, uint32_t count, const Allocation* a) {
    for (uint32_t n = 0; n < count; ++n)
      if (extras[n] == a) return true;
    return false;
  }
  static bool is_extra_target(const Allocation* const* extras, uint32_t count, const Allocation* a) {
    for (uint32_t n = 0; n < count; ++n)
      if (extras[n] == a) return true;
    return false;
  }

  NtStatus walk() {
    ByteReader in(commands_);
    while (!in.at_end()) {
      const uint32_t header = in.le32();
      const uint32_t words = header >> 16;
      if (!in.ok() || words < 1 || words > kMaxPacketWords) return kBad;
      ConstByteView body = in.take(uint64_t{words - 1} * 4);
      if (!in.ok()) return kBad;
      ByteReader p(body);
      const NtStatus s = packet(static_cast<Op>(header & 0xFFFF), p);
      if (s != status::kSuccess) return s;
    }
    if (in_pass_) return kBad;   // a pass left open
    return exec_ ? flush() : status::kSuccess;
  }

  // The device command buffer being recorded (made when the first command needs it).
  NtStatus ensure_cb() {
    if (cb_) return status::kSuccess;
    cb_ = adapter_.device_.create_command_buffer();
    if (!cb_) return status::kNoMemory;
    device_topology_ = graphics::PrimitiveTopology::TriangleList;
    cb_->begin();
    return status::kSuccess;
  }
  // Runs what has been recorded so far. Needed before the CPU touches an allocation the recorded commands use.
  NtStatus flush() {
    if (!cb_) return status::kSuccess;
    const bool ended = cb_->end();
    bool done = ended && adapter_.device_.submit(cb_);
    if (done) done = store_query_results();
    adapter_.device_.destroy_command_buffer(cb_);
    cb_ = nullptr;
    pending_.clear();
    next_slot_ = 0;
    release_transient();
    return done ? status::kSuccess : status::kInvalidDeviceState;
  }

  // Writes the counts of the queries the device ran into the allocations they name (the CPU's copy: these allocations never go to the GPU).
  bool store_query_results() {
    for (uint64_t i = 0; i < pending_.count(); ++i) {
      const PendingQuery& q = pending_[i];
      uint64_t samples = 0;
      ByteView where;
      if (!cb_->occlusion_result(q.slot, &samples) || !q.allocation->writable_backing().try_subview(q.offset, 8, &where)) return false;
      ByteWriter out(where);
      out.le64(samples);
      mm_.note_cpu_write(*q.allocation);
    }
    return true;
  }

  NtStatus resident(Allocation& a) { return mm_.make_resident(a); }

  void unbind_textures_of(const Allocation* a) {
    for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u)
      if (texture_bound_[u] && texture_alloc_[u] == a) texture_bound_[u] = false;
  }

  NtStatus packet(Op op, ByteReader& p) {
    switch (op) {
      case Op::BeginPass: return begin_pass(p);
      case Op::EndPass: {
        if (!exact(p) || !in_pass_ || query_open_) return kBad;
        in_pass_ = false;
        if (exec_) cb_->end_render_pass();
        return status::kSuccess;
      }
      case Op::BindPipeline: return bind_pipeline(p);
      case Op::BindVertexBuffer: return bind_vertex_buffer(p);
      case Op::BindIndexBuffer: return bind_index_buffer(p);
      case Op::PushConstants: return push_constants(p);
      case Op::BindTexture: return bind_texture(p);
      case Op::SetViewport: return set_viewport(p);
      case Op::SetScissor: return set_scissor(p);
      case Op::Draw: return draw(p, false, false);
      case Op::DrawIndexed: return draw(p, true, false);
      case Op::DrawInstanced: return draw(p, false, true);
      case Op::DrawIndexedInstanced: return draw(p, true, true);
      case Op::CopyRegion: return copy_region(p);
      case Op::GenerateMipmaps: return generate_mipmaps(p);
      case Op::BeginQuery: return begin_query(p);
      case Op::EndQuery: return end_query(p);
      case Op::BindUav: return bind_uav(p);
      case Op::ResolveRegion: return resolve(p);
      case Op::Dispatch: return dispatch(p);
      case Op::SetTopology: {
        const uint32_t topology = p.le32();
        if (!exact(p) || topology > static_cast<uint32_t>(Topology::PointList)) return kBad;
        topology_ = static_cast<Topology>(topology);
        return status::kSuccess;
      }
    }
    return kBad;
  }

  NtStatus begin_query(ByteReader& p) {
    const uint32_t index = p.le32(), offset = p.le32();
    if (!exact(p) || !in_pass_ || query_open_ || !(adapter_.caps_.features & graphics::kFeatureOcclusionQueries)) return kBad;
    Allocation* a = alloc(index);
    if (!a || !a->is_query_result() || offset % 8 || offset > a->desc().size - 8) return kBad;
    if (queries_in_stream_ >= graphics::kMaxOcclusionQueries) return kBad;
    ++queries_in_stream_;
    query_open_ = true;
    if (!exec_) return status::kSuccess;
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    if (!pending_.push_back({a, offset, next_slot_})) return status::kNoMemory;
    cb_->begin_occlusion_query(next_slot_++);
    return status::kSuccess;
  }

  NtStatus end_query(ByteReader& p) {
    if (!exact(p) || !query_open_) return kBad;
    query_open_ = false;
    if (exec_) cb_->end_occlusion_query();
    return status::kSuccess;
  }

  NtStatus begin_pass(ByteReader& p) {
    const uint32_t color_index = p.le32(), depth_index = p.le32(), flags = p.le32();
    graphics::Vec4 clear;
    clear.x = bits_float(p.le32()), clear.y = bits_float(p.le32()), clear.z = bits_float(p.le32()), clear.w = bits_float(p.le32());
    const float clear_depth = bits_float(p.le32());
    const uint32_t clear_stencil = p.le32();
    // Further colour targets, one word each (the adapter's kFeatureMultipleTargets).
    Allocation* extras[graphics::kMaxColorTargets - 1] = {};
    uint32_t extra_count = 0;
    while (!p.at_end() && p.ok() && extra_count < graphics::kMaxColorTargets - 1) {
      extras[extra_count] = alloc(p.le32());
      if (!extras[extra_count]) return kBad;
      ++extra_count;
    }
    if (!exact(p) || in_pass_) return kBad;
    if (extra_count && !(adapter_.caps_.features & graphics::kFeatureMultipleTargets)) return kBad;
    Allocation* color = alloc(color_index);
    Allocation* depth = depth_index == kNoAllocation ? nullptr : alloc(depth_index);
    if (!color || !color->is_image() || !(color->desc().flags & kAllocRenderTarget)) return kBad;
    if (depth_index != kNoAllocation && (!depth || !depth->is_image() || !(depth->desc().flags & kAllocDepthStencil))) return kBad;
    if (depth && (depth->desc().width != color->desc().width || depth->desc().height != color->desc().height || depth->desc().samples != color->desc().samples)) return kBad;
    for (uint32_t n = 0; n < extra_count; ++n) {
      const Allocation* e = extras[n];
      if (!e->is_image() || !(e->desc().flags & kAllocRenderTarget) || e == color || e == depth) return kBad;
      if (e->desc().width != color->desc().width || e->desc().height != color->desc().height || e->desc().format != color->desc().format || e->desc().samples != color->desc().samples) return kBad;
      for (uint32_t k = 0; k < n; ++k)
        if (extras[k] == e) return kBad;
    }
    if (flags & ~(kPassClearColor | kPassClearDepth | kPassClearStencil)) return kBad;
    if (!finite_within(clear.x, 1e6f) || !finite_within(clear.y, 1e6f) || !finite_within(clear.z, 1e6f) || !finite_within(clear.w, 1e6f)) return kBad;
    if (!(clear_depth >= 0.0f && clear_depth <= 1.0f) || clear_stencil > 255) return kBad;
    // A render pass may not draw into an image that is bound as a texture.
    for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u)
      if (texture_bound_[u] && (texture_alloc_[u] == color || texture_alloc_[u] == depth || is_extra_target(extras, extra_count, texture_alloc_[u]))) return kBad;
    in_pass_ = true;
    pass_color_ = color;
    pass_depth_ = depth;
    pass_target_count_ = 1 + extra_count;
    for (uint32_t n = 0; n < graphics::kMaxColorTargets - 1; ++n) pass_extra_[n] = n < extra_count ? extras[n] : nullptr;
    if (!exec_) return status::kSuccess;

    NtStatus s = resident(*color);
    if (s == status::kSuccess && depth) s = resident(*depth);
    for (uint32_t n = 0; n < extra_count && s == status::kSuccess; ++n) s = resident(*extras[n]);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    graphics::RenderPassBegin pass;
    pass.color = color->image();
    pass.depth = depth ? depth->image() : nullptr;
    pass.clear_color = (flags & kPassClearColor) != 0;
    pass.clear_color_value = clear;
    pass.clear_depth = (flags & kPassClearDepth) != 0;
    pass.clear_depth_value = clear_depth;
    pass.clear_stencil = (flags & kPassClearStencil) != 0;
    pass.clear_stencil_value = static_cast<uint8_t>(clear_stencil);
    for (uint32_t n = 0; n < extra_count; ++n) pass.extra_colors[n] = extras[n]->image();
    cb_->begin_render_pass(pass);
    mm_.note_gpu_write(*color);
    for (uint32_t n = 0; n < extra_count; ++n) mm_.note_gpu_write(*extras[n]);
    if (depth) mm_.note_gpu_write(*depth);
    return status::kSuccess;
  }

  NtStatus bind_pipeline(ByteReader& p) {
    const Handle handle = p.le32();
    if (!exact(p)) return kBad;
    SharedPointer<PipelineObject> found;
    if (exec_) {
      if (replay_cursor_ >= replay_.size()) return kBad;
      found = replay_[replay_cursor_++];
    } else {
      found = lookup_->find_pipeline(handle);
      if (!found || !collected_->push_back(found)) return found ? status::kNoMemory : status::kInvalidHandle;
    }
    if (found->compute()) {
      // (a compute pipeline is bound between passes and dispatched; it leaves the graphics pipeline alone)
      if (in_pass_) return kBad;
      compute_ = found.get();
      if (exec_) {
        const NtStatus s = ensure_cb();
        if (s != status::kSuccess) return s;
        cb_->bind_compute_pipeline(compute_->pipeline());
      }
      return status::kSuccess;
    }
    pipeline_ = found.get();
    if (exec_) {
      const NtStatus s = ensure_cb();
      if (s != status::kSuccess) return s;
      cb_->bind_pipeline(pipeline_->pipeline());
    }
    return status::kSuccess;
  }

  // Averages the samples of a multisampled colour target into an ordinary image of the same format and size.
  NtStatus resolve(ByteReader& p) {
    const uint32_t to_index = p.le32(), from_index = p.le32();
    if (!exact(p) || in_pass_ || !(adapter_.caps_.features & graphics::kFeatureMultisample)) return kBad;
    Allocation* to = alloc(to_index);
    Allocation* from = alloc(from_index);
    if (!to || !from || !to->is_image() || !from->is_image() || to->is_depth() || from->is_depth() || to->is_staging() || to == from) return kBad;
    if (from->desc().samples < 2 || to->desc().samples != 1 || to->mip_levels() != 1 || to->layers() != 1) return kBad;
    if (to->desc().format != from->desc().format || to->desc().width != from->desc().width || to->desc().height != from->desc().height) return kBad;
    if (!(from->desc().flags & kAllocRenderTarget)) return kBad;
    unbind_textures_of(to);
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*to);
    if (s == status::kSuccess) s = resident(*from);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->resolve_image(to->image(), from->image());
    mm_.note_gpu_write(*to);
    return status::kSuccess;
  }

  NtStatus bind_uav(ByteReader& p) {
    const uint32_t slot = p.le32(), index = p.le32(), offset = p.le32(), size = p.le32();
    if (!exact(p) || in_pass_ || !(adapter_.caps_.features & graphics::kFeatureCompute) || slot >= graphics::kMaxStorageBuffers) return kBad;
    Allocation* a = alloc(index);
    if (!a || !(a->desc().flags & kAllocUnorderedAccess) || size == 0 || offset % 4 || size % 4 || offset > a->desc().size || size > a->desc().size - offset) return kBad;
    uav_bound_[slot] = true;
    uav_alloc_[slot] = a;
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_storage_buffer(slot, a->buffer(), offset, size);
    return status::kSuccess;
  }

  NtStatus dispatch(ByteReader& p) {
    const uint32_t x = p.le32(), y = p.le32(), z = p.le32();
    if (!exact(p) || in_pass_ || !compute_) return kBad;
    if (x > graphics::kMaxDispatchGroups || y > graphics::kMaxDispatchGroups || z > graphics::kMaxDispatchGroups) return kBad;
    const uint64_t per_group = uint64_t{compute_->thread_group(0)} * compute_->thread_group(1) * compute_->thread_group(2);
    if (uint64_t{x} * y * z * per_group > graphics::kMaxDispatchThreads) return kBad;
    for (uint32_t slot = 0; slot < graphics::kMaxStorageBuffers; ++slot)
      if ((compute_->storage_mask() >> slot & 1) && !uav_bound_[slot]) return kBad;
    if (x == 0 || y == 0 || z == 0 || !exec_) return status::kSuccess;
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->dispatch(x, y, z);
    for (uint32_t slot = 0; slot < graphics::kMaxStorageBuffers; ++slot)
      if (uav_bound_[slot]) mm_.note_gpu_write(*uav_alloc_[slot]);
    return status::kSuccess;
  }

  NtStatus bind_vertex_buffer(ByteReader& p) {
    const uint32_t index = p.le32(), offset = p.le32();
    if (!exact(p)) return kBad;
    Allocation* a = alloc(index);
    if (!a || !a->is_buffer() || !(a->desc().flags & kAllocVertexBuffer) || offset > a->desc().size) return kBad;
    vertex_ = a;
    vertex_offset_ = offset;
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_vertex_buffer(a->buffer(), offset);
    return status::kSuccess;
  }

  NtStatus bind_index_buffer(ByteReader& p) {
    const uint32_t index = p.le32(), type = p.le32(), offset = p.le32();
    if (!exact(p) || type > 1) return kBad;
    Allocation* a = alloc(index);
    const uint32_t index_bytes = type == 1 ? 4 : 2;
    if (!a || !a->is_buffer() || !(a->desc().flags & kAllocIndexBuffer) || offset > a->desc().size || offset % index_bytes) return kBad;
    index_ = a;
    index_offset_ = offset;
    index_bytes_ = index_bytes;
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_index_buffer(a->buffer(), type == 1 ? graphics::IndexType::Uint32 : graphics::IndexType::Uint16, offset);
    return status::kSuccess;
  }

  NtStatus push_constants(ByteReader& p) {
    const uint32_t count = p.le32();
    if (!p.ok() || count == 0 || count > adapter_.caps_.max_push_constant_bytes || count % 16) return kBad;
    ConstByteView data = p.take(count);
    if (!exact(p)) return kBad;
    if (!exec_) return status::kSuccess;
    alignas(16) uint8_t block[graphics::kMaxPushConstantBytes] = {};   // (the device reads vec4s: an aligned copy)
    ByteView(block, sizeof(block)).copy_from(data);
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->push_constants(block, count);
    return status::kSuccess;
  }

  NtStatus bind_texture(ByteReader& p) {
    const uint32_t unit = p.le32(), index = p.le32(), filter = p.le32(), au = p.le32(), av = p.le32(), mip = p.le32();
    const float bias = bits_float(p.le32()), min_lod = bits_float(p.le32()), max_lod = bits_float(p.le32());
    if (!exact(p) || unit >= graphics::kMaxTextureUnits || filter > 1 || au > 1 || av > 1 || mip > 2) return kBad;
    if (!finite_within(bias, 100.0f) || !finite_within(min_lod, 100.0f) || !finite_within(max_lod, 1000.0f)) return kBad;
    Allocation* a = alloc(index);
    if (!a || !a->is_image() || !(a->desc().flags & kAllocTexture) || a->is_staging()) return kBad;
    if (in_pass_ && (a == pass_color_ || a == pass_depth_ || is_extra_target(pass_extra_, pass_target_count_ - 1, a))) return kBad;
    texture_bound_[unit] = true;
    texture_alloc_[unit] = a;
    if (!exec_) return status::kSuccess;
    graphics::SamplerDesc sampler;
    sampler.filter = static_cast<graphics::Filter>(filter);
    sampler.address_u = static_cast<graphics::AddressMode>(au);
    sampler.address_v = static_cast<graphics::AddressMode>(av);
    sampler.mip_filter = static_cast<graphics::MipFilter>(mip);
    sampler.lod_bias = bias;
    sampler.min_lod = min_lod;
    sampler.max_lod = max_lod;
    NtStatus s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_texture(unit, a->image(), sampler);
    return status::kSuccess;
  }

  NtStatus set_viewport(ByteReader& p) {
    graphics::Viewport v;
    v.x = bits_float(p.le32()), v.y = bits_float(p.le32()), v.width = bits_float(p.le32()), v.height = bits_float(p.le32());
    v.min_depth = bits_float(p.le32()), v.max_depth = bits_float(p.le32());
    if (!exact(p)) return kBad;
    if (!finite_within(v.x, kMaxCoordinate) || !finite_within(v.y, kMaxCoordinate) || !finite_within(v.width, kMaxCoordinate) || !finite_within(v.height, kMaxCoordinate) ||
        v.width < 0 || v.height < 0 || !finite_within(v.min_depth, 1.0f) || !finite_within(v.max_depth, 1.0f))
      return kBad;
    if (!exec_) return status::kSuccess;
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->set_viewport(v);
    return status::kSuccess;
  }

  NtStatus set_scissor(ByteReader& p) {
    const int32_t x = static_cast<int32_t>(p.le32()), y = static_cast<int32_t>(p.le32());
    const uint32_t w = p.le32(), h = p.le32();
    if (!exact(p) || x < -kMaxScissor || x > kMaxScissor || y < -kMaxScissor || y > kMaxScissor || w > static_cast<uint32_t>(kMaxScissor) || h > static_cast<uint32_t>(kMaxScissor)) return kBad;
    if (!exec_) return status::kSuccess;
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->set_scissor({x, y, w, h});
    return status::kSuccess;
  }

  // The reads a draw does must stay inside the buffers bound: the vertices [first, first + count) of the vertex buffer, and for an indexed draw the indices and (when
  // replaying, as the buffer can change after the stream was checked) the vertices they name.
  NtStatus draw(ByteReader& p, bool indexed, bool instanced) {
    const uint32_t count = p.le32(), first = p.le32();
    const int32_t vertex_offset = indexed ? static_cast<int32_t>(p.le32()) : 0;
    uint32_t instance_count = 1, first_instance = 0;
    if (instanced) {
      instance_count = p.le32();
      first_instance = p.le32();
    }
    if (!exact(p) || !in_pass_ || !pipeline_) return kBad;
    if (instanced && (!(adapter_.caps_.features & graphics::kFeatureInstancing) || instance_count == 0 || instance_count > (1u << 20))) return kBad;
    if (topology_ >= Topology::LineList && !(adapter_.caps_.features & graphics::kFeatureLines)) return kBad;
    for (uint32_t u = 0; u < pipeline_->texture_count(); ++u)
      if (!texture_bound_[u] || texture_alloc_[u]->texture_kind() != pipeline_->texture_kind(u)) return kBad;   // (unbound, or a volume where the shader reads a 2D texture, ...)
    if (pipeline_->color_target_count() > pass_target_count_) return kBad;
    const bool vertices = pipeline_->attribute_count() > 0;
    if (vertices && !vertex_) return kBad;
    const uint64_t stride = pipeline_->vertex_stride();
    if (indexed) {
      if (!index_) return kBad;
      const uint64_t end = uint64_t{first} + count;   // (< 2^33, times a 4-byte index < 2^35)
      if (index_offset_ + end * index_bytes_ > index_->desc().size) return kBad;
    } else if (vertices) {
      const uint64_t end = uint64_t{first} + count;   // (times a stride of at most 2048: < 2^44)
      if (vertex_offset_ + end * stride > vertex_->desc().size) return kBad;
    }
    // The per-instance attributes are read from the instances' elements of the vertex buffer.
    if (pipeline_->has_per_instance()) {
      const uint64_t end = uint64_t{first_instance} + instance_count;
      if (vertex_offset_ + end * stride > vertex_->desc().size) return kBad;
    }
    if (count == 0) return status::kSuccess;
    if (!exec_) return status::kSuccess;

    if (indexed && vertices) {
      // The index values: every vertex the draw can reach must be inside the vertex buffer.
      ByteReader in(index_->backing());
      in.skip(index_offset_ + uint64_t{first} * index_bytes_);
      int64_t highest = -1;
      const uint32_t restart = index_bytes_ == 4 ? 0xFFFFFFFFu : 0xFFFFu;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t raw = index_bytes_ == 4 ? in.le32() : in.le16();
        if (topology_ != Topology::TriangleList && topology_ != Topology::LineList && topology_ != Topology::PointList && raw == restart) continue;   // (ends a strip: names no vertex)
        const int64_t v = int64_t{raw} + vertex_offset;
        if (!in.ok() || v < 0) return kBad;
        if (v > highest) highest = v;
      }
      if (vertex_offset_ + static_cast<uint64_t>(highest + 1) * stride > vertex_->desc().size) return kBad;
    }
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    if (topology_ == Topology::TriangleStrip || topology_ == Topology::TriangleFan || topology_ == Topology::LineStrip) return draw_expanded(indexed, count, first, vertex_offset, instance_count, first_instance);
    set_device_topology(topology_ == Topology::LineList ? graphics::PrimitiveTopology::LineList : topology_ == Topology::PointList ? graphics::PrimitiveTopology::PointList : graphics::PrimitiveTopology::TriangleList);
    if (indexed && index_rebind_) {   // (an expanded draw left its own indices bound)
      cb_->bind_index_buffer(index_->buffer(), index_bytes_ == 4 ? graphics::IndexType::Uint32 : graphics::IndexType::Uint16, index_offset_);
      index_rebind_ = false;
    }
    if (indexed) cb_->draw_indexed(count, first, vertex_offset, instance_count, first_instance);
    else cb_->draw(count, first, instance_count, first_instance);
    return status::kSuccess;
  }

  void set_device_topology(graphics::PrimitiveTopology topology) {
    if (topology == device_topology_) return;
    cb_->set_primitive_topology(topology);
    device_topology_ = topology;
  }

  // A strip or fan as a list: the triangles (or, for a line strip, the lines) are worked out from the vertex numbers (or the indices) and drawn with a buffer of indices made for
  // this draw, which lives until the commands run. The winding of every second triangle of a strip is turned so that all face one way.
  NtStatus draw_expanded(bool indexed, uint32_t count, uint32_t first, int32_t vertex_offset, uint32_t instance_count, uint32_t first_instance) {
    constexpr uint64_t kMaxExpandedIndices = 3u << 20;
    const bool fan = topology_ == Topology::TriangleFan;
    const bool lines = topology_ == Topology::LineStrip;
    const uint32_t restart = index_bytes_ == 4 ? 0xFFFFFFFFu : 0xFFFFu;
    ByteBuffer out(adapter_.allocator_);
    uint32_t run[3];   // the vertices of the strip so far (for a fan: the first, the previous and the current)
    uint32_t seen = 0;
    ByteReader in(indexed ? index_->backing() : ConstByteView());
    if (indexed) in.skip(index_offset_ + uint64_t{first} * index_bytes_);
    uint64_t emitted = 0;
    strip_triangle_ = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v;
      if (indexed) {
        v = index_bytes_ == 4 ? in.le32() : in.le16();
        if (!in.ok()) return kBad;
        if (v == restart) {
          seen = 0;
          strip_triangle_ = 0;
          continue;
        }
      } else {
        v = first + i;
      }
      if (lines) {
        // (a line from each vertex to the next)
        if (seen == 0) {
          run[0] = v;
          seen = 1;
          continue;
        }
        if (emitted + 2 > kMaxExpandedIndices) return kBad;
        if (!out.put32(run[0]) || !out.put32(v)) return status::kNoMemory;
        emitted += 2;
        run[0] = v;
        continue;
      }
      if (seen < 3) {
        run[seen++] = v;
        if (seen < 3) continue;
      } else if (fan) {
        run[1] = run[2], run[2] = v;
      } else {
        run[0] = run[1], run[1] = run[2], run[2] = v;
      }
      // (in a strip the triangle at an odd position has its first two vertices swapped, so that all face one way)
      uint32_t a = run[0], b = run[1], c = run[2];
      if (!fan && (strip_triangle_ & 1)) {
        const uint32_t t = a;
        a = b, b = t;
      }
      ++strip_triangle_;
      if (emitted + 3 > kMaxExpandedIndices) return kBad;
      if (!out.put32(a) || !out.put32(b) || !out.put32(c)) return status::kNoMemory;
      emitted += 3;
    }
    strip_triangle_ = 0;
    if (emitted == 0) return status::kSuccess;
    graphics::IBuffer* buffer = adapter_.device_.create_buffer(out.size(), graphics::kBufferIndex);
    if (!buffer) return status::kNoMemory;
    if (!transient_.push_back(buffer)) {
      adapter_.device_.destroy_buffer(buffer);
      return status::kNoMemory;
    }
    ByteView mapped = buffer->map();
    if (mapped.size() < out.size()) return status::kNoMemory;
    mapped.first(out.size()).copy_from(out.view());
    buffer->unmap();
    set_device_topology(lines ? graphics::PrimitiveTopology::LineList : graphics::PrimitiveTopology::TriangleList);
    cb_->bind_index_buffer(buffer, graphics::IndexType::Uint32, 0);
    cb_->draw_indexed(static_cast<uint32_t>(emitted), 0, indexed ? vertex_offset : 0, instance_count, first_instance);
    index_rebind_ = indexed || index_ != nullptr;
    return status::kSuccess;
  }

  void release_transient() {
    for (graphics::IBuffer* b : transient_.view()) adapter_.device_.destroy_buffer(b);
    transient_.clear();
  }

  NtStatus copy_region(ByteReader& p) {
    const uint32_t di = p.le32(), dx = p.le32(), dy = p.le32(), si = p.le32(), sx = p.le32(), sy = p.le32(), w = p.le32(), h = p.le32();
    if (!exact(p) || in_pass_ || w == 0 || h == 0) return kBad;
    Allocation* dst = alloc(di);
    Allocation* src = alloc(si);
    if (!dst || !src || !dst->is_image() || !src->is_image() || dst->is_depth() || src->is_depth()) return kBad;
    if (dst->desc().format != src->desc().format || dst->mip_levels() != 1 || src->mip_levels() != 1 || dst->desc().samples != 1 || src->desc().samples != 1) return kBad;
    if (uint64_t{dx} + w > dst->desc().width || uint64_t{dy} + h > dst->desc().height || uint64_t{sx} + w > src->desc().width || uint64_t{sy} + h > src->desc().height) return kBad;
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s == status::kSuccess) s = copy_region_cpu(mm_, *dst, dx, dy, *src, sx, sy, w, h);
    unbind_textures_of(dst);
    return s;
  }

  NtStatus generate_mipmaps(ByteReader& p) {
    const uint32_t index = p.le32();
    if (!exact(p) || in_pass_) return kBad;
    Allocation* a = alloc(index);
    if (!a || !a->is_image() || !(a->desc().flags & kAllocTexture) || a->mip_levels() < 2) return kBad;
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    for (uint32_t level = 1; level < a->mip_levels(); ++level) {
      uint64_t from_offset = 0, from_bytes = 0, to_offset = 0, to_bytes = 0;
      ArrayView<uint32_t> from, to;
      if (!a->level_range(level - 1, &from_offset, &from_bytes) || !a->level_range(level, &to_offset, &to_bytes) || !as_words(a->writable_backing().subview(from_offset, from_bytes), &from) ||
          !as_words(a->writable_backing().subview(to_offset, to_bytes), &to))
        return status::kInvalidDeviceState;
      // (the box filter averages each byte of a pixel on its own, so the order of the channels does not matter)
      graphics::mip_box_downsample(ArrayView<const uint32_t>(from.data(), from.size()), graphics::mip_extent(a->desc().width, level - 1), graphics::mip_extent(a->desc().height, level - 1), to,
                                   graphics::mip_extent(a->desc().width, level), graphics::mip_extent(a->desc().height, level));
    }
    mm_.note_cpu_write(*a);
    unbind_textures_of(a);
    return status::kSuccess;
  }

  Adapter& adapter_;
  VidMm& mm_;
  ConstByteView commands_;
  ArrayView<const SharedPointer<Allocation>> allocations_;
  bool exec_ = false;
  IPipelineLookup* lookup_ = nullptr;
  List<SharedPointer<PipelineObject>>* collected_ = nullptr;
  ArrayView<const SharedPointer<PipelineObject>> replay_;
  uint64_t replay_cursor_ = 0;
  graphics::ICommandBuffer* cb_ = nullptr;
  // The state a stream builds up, to check the draws.
  bool in_pass_ = false;
  struct PendingQuery {
    Allocation* allocation;
    uint64_t offset;
    uint32_t slot;
  };
  bool query_open_ = false;
  uint32_t queries_in_stream_ = 0;       // the queries begun so far (at most kMaxOcclusionQueries in a stream)
  uint32_t next_slot_ = 0;               // the slot of the next query in the command buffer being recorded
  List<PendingQuery> pending_;           // the queries recorded in it: where their counts go
  const Allocation* pass_color_ = nullptr;
  const Allocation* pass_extra_[graphics::kMaxColorTargets - 1] = {};
  uint32_t pass_target_count_ = 1;
  const Allocation* pass_depth_ = nullptr;
  PipelineObject* pipeline_ = nullptr;
  PipelineObject* compute_ = nullptr;
  bool uav_bound_[graphics::kMaxStorageBuffers] = {};
  Allocation* uav_alloc_[graphics::kMaxStorageBuffers] = {};
  Allocation* vertex_ = nullptr;
  uint64_t vertex_offset_ = 0;
  Allocation* index_ = nullptr;
  uint64_t index_offset_ = 0;
  uint32_t index_bytes_ = 2;
  Topology topology_ = Topology::TriangleList;
  graphics::PrimitiveTopology device_topology_ = graphics::PrimitiveTopology::TriangleList;   // what the recorded commands have set so far
  uint32_t strip_triangle_ = 0;      // the position of the triangle being made in its strip (for the winding)
  bool index_rebind_ = false;        // the device's bound indices are a draw's own, not the stream's
  List<graphics::IBuffer*> transient_;   // index buffers made for expanded draws, destroyed when the commands have run
  bool texture_bound_[graphics::kMaxTextureUnits] = {};
  const Allocation* texture_alloc_[graphics::kMaxTextureUnits] = {};
};

// --- The adapter ----------------------------------------------------------------------------------------------------------------------------------------------------

Adapter::Adapter(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device, const AdapterConfig& config, uint32_t index, Luid luid)
    : allocator_(allocator), device_(device), display_(config.display), index_(index), luid_(luid), vidmm_(allocator, device, config.local_memory_budget, config.system_memory_limit), contexts_(allocator) {
  const graphics::DeviceInfo info = device.info();
  caps_.max_texture_dimension = kMaxTextureDimension;
  caps_.max_vertex_attributes = graphics::kMaxVertexAttributes;
  caps_.max_varyings = graphics::kMaxVaryings;
  caps_.max_texture_units = graphics::kMaxTextureUnits;
  caps_.max_push_constant_bytes = info.max_push_constant_bytes < graphics::kMaxPushConstantBytes ? info.max_push_constant_bytes : graphics::kMaxPushConstantBytes;
  caps_.max_vertex_stride = kMaxVertexStride;
  caps_.accelerated = info.accelerated;
  caps_.features = info.features;
  const char* name = config.name ? config.name : info.name;
  for (uint32_t i = 0; name && name[i] && i + 1 < sizeof(caps_.name); ++i) caps_.name[i] = name[i];

  // What the device accepts beyond the basics, found by trying: a stencil plane, a mipmapped texture.
  graphics::ImageDesc probe;
  probe.extent = {2, 2};
  probe.format = graphics::Format::D32Float;
  probe.usage = graphics::kImageDepthAttachment;
  probe.stencil = true;
  if (graphics::IImage* image = device.create_image(probe)) {
    caps_.stencil = true;
    device.destroy_image(image);
  }
  probe.format = graphics::Format::B8G8R8A8Unorm;
  probe.usage = graphics::kImageSampled;
  probe.stencil = false;
  probe.mip_levels = 2;
  if (graphics::IImage* image = device.create_image(probe)) {
    caps_.mipmaps = true;
    device.destroy_image(image);
  }
  probe.mip_levels = 1;
  if (graphics::IImage* image = device.create_image(probe)) {
    caps_.textures = true;
    device.destroy_image(image);
  }
  // Whether the device takes shaders that are not its own fixed ones: the built-in shader with the names of the fixed kernels taken away (what is left is the function, the TGSI text
  // and the Direct3D 9 tokens, which a device may or may not understand).
  graphics::ShaderModuleDesc vertex = graphics::colored_vertex_shader(), fragment = graphics::colored_fragment_shader();
  vertex.qpu_program = graphics::QpuProgram::None, vertex.eu_program = graphics::EuProgram::None;
  fragment.qpu_program = graphics::QpuProgram::None, fragment.eu_program = graphics::EuProgram::None;
  if (graphics::IShaderModule* module = device.create_shader_module(vertex)) {
    caps_.programmable_vertex = true;
    device.destroy_shader_module(module);
  }
  if (graphics::IShaderModule* module = device.create_shader_module(fragment)) {
    caps_.programmable_pixel = true;
    device.destroy_shader_module(module);
  }
}

Adapter::~Adapter() {
  for (uint64_t i = 0; i < contexts_.count(); ++i) contexts_[i]->queue.clear();
}

NtStatus Adapter::create_shader(const ShaderDesc& desc, SharedPointer<ShaderObject>* out) {
  SharedPointer<ShaderObject> shader = SharedPointer<ShaderObject>::make(allocator_, allocator_, device_);
  if (!shader) return status::kNoMemory;
  graphics::ShaderModuleDesc md;
  NtStatus no_functions = status::kSuccess;   // why a D3d9Bytecode shader has no native function, if it has none
  switch (desc.kind) {
    case ShaderKind::ColoredVertex: md = graphics::colored_vertex_shader(); break;
    case ShaderKind::ColoredFragment: md = graphics::colored_fragment_shader(); break;
    case ShaderKind::TexturedVertex: md = graphics::textured_vertex_shader(); break;
    case ShaderKind::TexturedFragment: md = graphics::textured_fragment_shader(); break;
    case ShaderKind::Tgsi: {
      if (desc.code.empty() || desc.code.size() > kMaxShaderBytes) return status::kInvalidParameter;
      for (uint8_t b : desc.code)
        if (b == 0) return status::kInvalidParameter;
      shader->text_ = allocate_view<uint8_t>(allocator_, desc.code.size() + 1);
      if (shader->text_.empty()) return status::kNoMemory;
      shader->text_.copy_from(desc.code);
      shader->text_[desc.code.size()] = 0;
      md.stage = desc.stage;
      md.tgsi = text_pointer(shader->text_);
      break;
    }
    case ShaderKind::D3d9Tokens: {
      if (desc.code.empty() || desc.code.size() > kMaxShaderBytes || desc.code.size() % 4) return status::kInvalidParameter;
      shader->tokens_ = allocate_view<uint32_t>(allocator_, desc.code.size() / 4);
      if (shader->tokens_.empty()) return status::kNoMemory;
      ByteReader in(desc.code);
      for (uint32_t& token : shader->tokens_) token = in.le32();
      md.stage = desc.stage;
      md.d3d9_tokens = shader->tokens_.data();
      md.d3d9_token_count = static_cast<uint32_t>(shader->tokens_.size());
      break;
    }
    case ShaderKind::D3d9Bytecode:
    case ShaderKind::Dxbc: {
      UniquePointer<D3d9Program> program;
      const NtStatus s = desc.kind == ShaderKind::Dxbc ? D3d9Program::compile_dxbc(allocator_, desc.code, &program) : D3d9Program::compile(allocator_, desc.code, &program);
      if (s != status::kSuccess) return s;
      if (program->constant_bytes() > caps_.max_push_constant_bytes) return status::kNotSupported;
      if (program->required_features() & ~caps_.features) return status::kNotSupported;   // (system values, texel access: the device must have them)   // (more constants than this device is given)
      // A GPU device (virgl) takes the shader as TGSI text; the others run it through the interpreter's native function. Both are given: each device uses what it understands.
      // A shader the text cannot say (see D3d9Tgsi.cpp) is still fine for the interpreter.
      const graphics::ShaderStage stage = program->stage();
      String text(allocator_);
      const NtStatus t = program->emit_tgsi(&text);
      if (t == status::kNoMemory) return t;
      if (t == status::kSuccess) {
        shader->text_ = allocate_view<uint8_t>(allocator_, text.size() + 1);
        if (shader->text_.empty()) return status::kNoMemory;
        for (uint64_t i = 0; i < text.size(); ++i) shader->text_[i] = static_cast<uint8_t>(text[i]);
        shader->text_[text.size()] = 0;
        md.tgsi = text_pointer(shader->text_);
      }
      md.stage = stage;
      // The devices without a shader compiler: SVGA3D takes the (rewritten) Direct3D 9 tokens, Intel Gen9 runs a vertex function and one of two fixed pixel kernels.
      if (stage != graphics::ShaderStage::Compute) {
        List<uint32_t> svga(allocator_);
        bool have_svga = false;
        if (desc.kind == ShaderKind::D3d9Bytecode) {
          have_svga = program->emit_svga_tokens(desc.code, &svga) == status::kSuccess;
        } else {
          // (a program made from DXBC goes through the Direct3D 9 stream it can be written as)
          List<uint32_t> d3d9(allocator_);
          ByteBuffer bytes(allocator_);
          UniquePointer<D3d9Program> second;
          if (program->emit_d3d9_tokens(&d3d9) == status::kSuccess) {
            for (uint64_t i = 0; i < d3d9.count(); ++i) bytes.put32(d3d9[i]);
            have_svga = D3d9Program::compile(allocator_, bytes.view(), &second) == status::kSuccess && second->emit_svga_tokens(bytes.view(), &svga) == status::kSuccess;
          }
        }
        if (have_svga) {
          shader->tokens_ = allocate_view<uint32_t>(allocator_, svga.count());
          if (shader->tokens_.empty()) return status::kNoMemory;
          for (uint64_t i = 0; i < svga.count(); ++i) shader->tokens_[i] = svga[i];
          md.d3d9_tokens = shader->tokens_.data();
          md.d3d9_token_count = static_cast<uint32_t>(shader->tokens_.size());
        }
      }
      if (stage == graphics::ShaderStage::Compute) {
        // (nothing but the interpreter and the TGSI text runs a compute shader)
      } else if (stage == graphics::ShaderStage::Fragment) {
        uint8_t varying = 0;
        const FragmentKernel kernel = program->classify_fragment(&varying);
        if (kernel != FragmentKernel::None) {
          md.eu_program = kernel == FragmentKernel::Colored ? graphics::EuProgram::ColoredFragment : graphics::EuProgram::TexturedFragment;
          md.eu_varying = varying;
          if (kernel == FragmentKernel::Colored) md.qpu_program = graphics::QpuProgram::ColoredFragment;
        }
      } else {
        // The Raspberry Pi's vertex kernel is the matrix transform of the built-in shader; a Direct3D 9 shader that is exactly that runs on it (the matrix as rows if it takes dp4s).
        uint8_t colour = 0;
        const VertexKernel kernel = program->classify_vertex(&colour);
        if (kernel != VertexKernel::None) {
          md.qpu_program = graphics::QpuProgram::ColoredVertex;
          md.qpu_matrix_rows = kernel == VertexKernel::MatrixRows;
          md.qpu_flip_y = true;
        }
      }
      for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) shader->texture_kinds_[u] = program->texture_kind(u);
      for (uint32_t k = 0; k < 3; ++k) shader->thread_group_[k] = program->thread_group(k);
      for (uint32_t u = 0; u < graphics::kMaxStorageBuffers; ++u)
        if (program->storage_kind(u) != D3d9Program::BufferKind::None) shader->storage_mask_ |= 1u << u;
      const NtStatus b = shader->d3d9_.bind(util::move(program), &md);
      if (b != status::kSuccess && !md.tgsi) return b;
      no_functions = b;   // (out of trampolines is no matter to a device that takes the text)
      break;
    }
    default: return status::kInvalidParameter;
  }
  shader->stage_ = md.stage;
  shader->module_ = device_.create_shader_module(md);
  if (!shader->module_) return no_functions != status::kSuccess ? no_functions : status::kNotSupported;
  *out = shader;
  return status::kSuccess;
}

NtStatus Adapter::create_compute_pipeline(const SharedPointer<ShaderObject>& shader, SharedPointer<PipelineObject>* out) {
  if (!shader || shader->stage() != graphics::ShaderStage::Compute) return status::kInvalidParameter;
  if (!(caps_.features & graphics::kFeatureCompute)) return status::kNotSupported;
  SharedPointer<PipelineObject> pipeline = SharedPointer<PipelineObject>::make(allocator_, device_);
  if (!pipeline) return status::kNoMemory;
  pipeline->pipeline_ = device_.create_compute_pipeline(shader->module());
  if (!pipeline->pipeline_) return status::kNotSupported;
  pipeline->compute_ = true;
  for (uint32_t k = 0; k < 3; ++k) pipeline->thread_group_[k] = shader->thread_group_[k];
  pipeline->storage_mask_ = shader->storage_mask_;
  pipeline->vertex_ = shader;   // (kept alive for as long as the pipeline is)
  *out = pipeline;
  return status::kSuccess;
}

NtStatus Adapter::create_pipeline(const PipelineDesc& d, const SharedPointer<ShaderObject>& vertex, const SharedPointer<ShaderObject>& fragment, SharedPointer<PipelineObject>* out) {
  if (!vertex || !fragment || vertex->stage() != graphics::ShaderStage::Vertex || fragment->stage() != graphics::ShaderStage::Fragment) return status::kInvalidParameter;
  if (d.vertex_stride > kMaxVertexStride || d.attribute_count > graphics::kMaxVertexAttributes || d.varying_count > graphics::kMaxVaryings || d.texture_count > graphics::kMaxTextureUnits)
    return status::kInvalidParameter;
  if (d.attribute_count > 0 && d.vertex_stride == 0) return status::kInvalidParameter;
  for (uint32_t i = 0; i < d.attribute_count; ++i) {
    const graphics::VertexAttribute& a = d.attributes[i];
    if (a.location >= graphics::kMaxVertexAttributes || static_cast<uint32_t>(a.format) > static_cast<uint32_t>(graphics::VertexFormat::Float4)) return status::kInvalidParameter;
    if (a.offset > d.vertex_stride || vertex_format_bytes(a.format) > d.vertex_stride - a.offset) return status::kInvalidParameter;
  }
  const graphics::StencilState& st = d.stencil;
  if (static_cast<uint32_t>(d.cull_mode) > 2 || static_cast<uint32_t>(d.front_face) > 1 || static_cast<uint32_t>(st.compare) > 7 || static_cast<uint32_t>(st.fail_op) > 7 ||
      static_cast<uint32_t>(st.depth_fail_op) > 7 || static_cast<uint32_t>(st.pass_op) > 7)
    return status::kInvalidParameter;
  if (st.enable && !caps_.stencil) return status::kNotSupported;
  // The state beyond the base interface, for the devices that have it.
  const graphics::BlendState& bs = d.blend;
  if (static_cast<uint32_t>(d.depth_compare) > 7 || static_cast<uint32_t>(bs.src_color) > 10 || static_cast<uint32_t>(bs.dst_color) > 10 || static_cast<uint32_t>(bs.src_alpha) > 10 ||
      static_cast<uint32_t>(bs.dst_alpha) > 10 || static_cast<uint32_t>(bs.color_op) > 4 || static_cast<uint32_t>(bs.alpha_op) > 4 || bs.write_mask > 0xF)
    return status::kInvalidParameter;
  for (const graphics::BlendState& t : d.target_blend)
    if (static_cast<uint32_t>(t.src_color) > 10 || static_cast<uint32_t>(t.dst_color) > 10 || static_cast<uint32_t>(t.src_alpha) > 10 || static_cast<uint32_t>(t.dst_alpha) > 10 ||
        static_cast<uint32_t>(t.color_op) > 4 || static_cast<uint32_t>(t.alpha_op) > 4 || t.write_mask > 0xF)
      return status::kInvalidParameter;
  if (d.independent_blend && !(caps_.features & graphics::kFeatureIndependentBlend)) return status::kNotSupported;
  if (d.multisample && !(caps_.features & graphics::kFeatureMultisample)) return status::kNotSupported;
  if ((bs.custom || d.depth_compare != graphics::CompareOp::LessEqual) && !(caps_.features & graphics::kFeatureBlendState)) return status::kNotSupported;
  for (uint32_t i = 0; i < d.attribute_count; ++i)
    if (d.attributes[i].per_instance && !(caps_.features & graphics::kFeatureInstancing)) return status::kNotSupported;
  if (d.color_target_count == 0 || d.color_target_count > graphics::kMaxColorTargets) return status::kInvalidParameter;
  if (d.color_target_count > 1 && !(caps_.features & graphics::kFeatureMultipleTargets)) return status::kNotSupported;

  graphics::GraphicsPipelineDesc pd;
  pd.vertex_shader = vertex->module();
  pd.fragment_shader = fragment->module();
  pd.vertex_stride = d.vertex_stride;
  pd.attribute_count = d.attribute_count;
  for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
  pd.varying_count = d.varying_count;
  pd.texture_count = d.texture_count;
  pd.cull_mode = d.cull_mode;
  pd.front_face = d.front_face;
  pd.depth_test = d.depth_test;
  pd.depth_write = d.depth_write;
  pd.alpha_blend = d.alpha_blend;
  pd.stencil = st;
  pd.blend = bs;
  pd.independent_blend = d.independent_blend;
  pd.multisample = d.multisample;
  for (uint32_t i = 0; i < graphics::kMaxColorTargets - 1; ++i) pd.target_blend[i] = d.target_blend[i];
  pd.depth_compare = d.depth_compare;
  pd.color_target_count = d.color_target_count;

  SharedPointer<PipelineObject> pipeline = SharedPointer<PipelineObject>::make(allocator_, device_);
  if (!pipeline) return status::kNoMemory;
  pipeline->pipeline_ = device_.create_graphics_pipeline(pd);
  if (!pipeline->pipeline_) return status::kNotSupported;
  pipeline->vertex_ = vertex;
  pipeline->fragment_ = fragment;
  pipeline->stride_ = d.vertex_stride;
  for (uint32_t i = 0; i < d.attribute_count; ++i) pipeline->per_instance_ = pipeline->per_instance_ || d.attributes[i].per_instance;
  pipeline->attribute_count_ = d.attribute_count;
  pipeline->texture_count_ = d.texture_count;
  for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) pipeline->texture_kinds_[u] = fragment->texture_kind(u);
  pipeline->color_target_count_ = d.color_target_count;
  *out = pipeline;
  return status::kSuccess;
}

NtStatus Adapter::create_context(const SharedPointer<DeviceObject>& device, ContextPriority priority, SharedPointer<ContextObject>* out) {
  if (static_cast<uint32_t>(priority) > 2) return status::kInvalidParameter;
  SharedPointer<ContextObject> context = SharedPointer<ContextObject>::make(allocator_, allocator_, device, priority);
  if (!context) return status::kNoMemory;
  if (!contexts_.push_back(context)) return status::kNoMemory;
  *out = context;
  return status::kSuccess;
}

void Adapter::destroy_context(const SharedPointer<ContextObject>& context) {
  context->lost = true;
  // (fence signals the context still owes are carried out, so that nobody waits for work that will never run)
  for (UniquePointer<Submission>& s : context->queue.view())
    if (s->kind == SubmissionKind::Signal) s->sync->signal(s->value);
  context->queue.clear();
  context->queued_bytes = 0;
  for (uint64_t i = 0; i < contexts_.count(); ++i)
    if (contexts_[i].get() == context.get()) {
      contexts_.remove_at(i);
      break;
    }
}

NtStatus Adapter::queue(ContextObject& context, UniquePointer<Submission> submission) {
  if (context.lost) return status::kDeviceRemoved;
  const uint64_t bytes = submission->commands.size();
  if (context.queue.count() >= kMaxQueuedSubmissions || bytes > kMaxQueuedBytes - context.queued_bytes) return status::kGraphicsTryAgainLater;
  if (!context.queue.push_back(util::move(submission))) return status::kNoMemory;
  context.queued_bytes += bytes;
  return status::kSuccess;
}

NtStatus Adapter::queue_render(ContextObject& context, ConstByteView commands, ArrayView<const SharedPointer<Allocation>> allocations, IPipelineLookup& pipelines) {
  if (context.lost) return status::kDeviceRemoved;
  if (commands.empty() || commands.size() > kMaxCommandBytes || commands.size() % 4) return status::kInvalidParameter;
  for (const SharedPointer<Allocation>& a : allocations)
    if (!a) return status::kInvalidParameter;
  UniquePointer<Submission> s = UniquePointer<Submission>::make(allocator_, allocator_);
  if (!s) return status::kNoMemory;
  s->kind = SubmissionKind::Render;
  if (!s->commands.append(commands)) return status::kNoMemory;
  for (const SharedPointer<Allocation>& a : allocations)
    if (!s->allocations.push_back(a)) return status::kNoMemory;
  StreamRunner runner(*this, s->commands.view(), ArrayView<const SharedPointer<Allocation>>(s->allocations.view().data(), s->allocations.count()));
  const NtStatus v = runner.validate(pipelines, s->pipelines);
  if (v != status::kSuccess) return v;
  return queue(context, util::move(s));
}

NtStatus Adapter::queue_signal(ContextObject& context, const SharedPointer<SyncObject>& sync, uint64_t value) {
  if (!sync) return status::kInvalidHandle;
  UniquePointer<Submission> s = UniquePointer<Submission>::make(allocator_, allocator_);
  if (!s) return status::kNoMemory;
  s->kind = SubmissionKind::Signal;
  s->sync = sync;
  s->value = value;
  return queue(context, util::move(s));
}

NtStatus Adapter::queue_wait(ContextObject& context, const SharedPointer<SyncObject>& sync, uint64_t value) {
  if (!sync) return status::kInvalidHandle;
  UniquePointer<Submission> s = UniquePointer<Submission>::make(allocator_, allocator_);
  if (!s) return status::kNoMemory;
  s->kind = SubmissionKind::Wait;
  s->sync = sync;
  s->value = value;
  return queue(context, util::move(s));
}

NtStatus Adapter::queue_present(ContextObject& context, const PresentArgs& args, const SharedPointer<Allocation>& source, const SharedPointer<Allocation>& destination) {
  if (!source || !source->is_image() || source->is_depth() || source->is_staging()) return status::kInvalidParameter;
  PresentArgs a = args;
  if (a.source_rect.width == 0 || a.source_rect.height == 0) a.source_rect = {0, 0, source->desc().width, source->desc().height};
  if (destination) {
    const AllocationDesc& sd = source->desc();
    const AllocationDesc& dd = destination->desc();
    if (!destination->is_image() || destination->is_depth() || !(dd.flags & kAllocPresent) || dd.format != sd.format || source->mip_levels() != 1 || destination->mip_levels() != 1)
      return status::kInvalidParameter;
    if (a.source_rect.x < 0 || a.source_rect.y < 0 || a.dest_x < 0 || a.dest_y < 0) return status::kInvalidParameter;
    const graphics::Rect2D r = a.source_rect;
    if (uint64_t{static_cast<uint32_t>(r.x)} + r.width > sd.width || uint64_t{static_cast<uint32_t>(r.y)} + r.height > sd.height) return status::kInvalidParameter;
    if (uint64_t{static_cast<uint32_t>(a.dest_x)} + r.width > dd.width || uint64_t{static_cast<uint32_t>(a.dest_y)} + r.height > dd.height) return status::kInvalidParameter;
  } else if (!(source->desc().flags & kAllocPresent)) {
    return status::kInvalidParameter;
  }
  UniquePointer<Submission> s = UniquePointer<Submission>::make(allocator_, allocator_);
  if (!s) return status::kNoMemory;
  s->kind = SubmissionKind::Present;
  s->present = a;
  if (!s->allocations.push_back(source) || (destination && !s->allocations.push_back(destination))) return status::kNoMemory;
  return queue(context, util::move(s));
}

NtStatus Adapter::execute_render(Submission& s) {
  StreamRunner runner(*this, s.commands.view(), ArrayView<const SharedPointer<Allocation>>(s.allocations.view().data(), s.allocations.count()));
  return runner.execute(ArrayView<const SharedPointer<PipelineObject>>(s.pipelines.view().data(), s.pipelines.count()));
}

NtStatus Adapter::execute_present(Submission& s) {
  Allocation& source = *s.allocations[0];
  Allocation* destination = s.allocations.count() > 1 ? s.allocations[1].get() : nullptr;
  Allocation& shown = destination ? *destination : source;
  vidmm_.pin(source);
  vidmm_.pin(shown);
  NtStatus st = status::kSuccess;
  if (destination) {
    const graphics::Rect2D r = s.present.source_rect;
    st = copy_region_cpu(vidmm_, *destination, static_cast<uint32_t>(s.present.dest_x), static_cast<uint32_t>(s.present.dest_y), source, static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y), r.width, r.height);
  }
  if (st == status::kSuccess) st = vidmm_.make_resident(shown);
  if (st == status::kSuccess && !device_.present(shown.image())) st = status::kInvalidDeviceState;
  vidmm_.unpin(shown);
  vidmm_.unpin(source);
  return st;
}

bool Adapter::runnable(const ContextObject& context) const {
  const Submission& head = *context.queue[0];
  return head.kind != SubmissionKind::Wait || head.sync->satisfied(head.value);
}

void Adapter::fault(ContextObject& context, NtStatus why) {
  context.lost = true;
  DeviceState& state = context.device->state;
  state.execution = why == status::kGraphicsNoVideoMemory || why == status::kNoMemory ? DeviceExecState::ErrorOutOfMemory : why == status::kInvalidParameter ? DeviceExecState::PageFault : DeviceExecState::Hung;
  ++state.reset_count;
  state.last_fault = why;
  for (UniquePointer<Submission>& s : context.queue.view())
    if (s->kind == SubmissionKind::Signal) s->sync->signal(s->value);
  context.queue.clear();
  context.queued_bytes = 0;
}

void Adapter::run_head(ContextObject& context) {
  UniquePointer<Submission> s = util::move(context.queue[0]);
  context.queue.remove_at(0);
  context.queued_bytes -= s->commands.size();
  NtStatus st = status::kSuccess;
  switch (s->kind) {
    case SubmissionKind::Render: st = execute_render(*s); break;
    case SubmissionKind::Present: st = execute_present(*s); break;
    case SubmissionKind::Signal: s->sync->signal(s->value); break;
    case SubmissionKind::Wait: s->sync->consume(); break;
  }
  if (st != status::kSuccess) fault(context, st);
}

bool Adapter::pump() {
  bool any = false;
  for (;;) {
    ContextObject* picked = nullptr;
    const uint64_t n = contexts_.count();
    for (int priority = 2; priority >= 0 && !picked; --priority) {
      for (uint64_t k = 0; k < n; ++k) {
        const uint64_t index = (next_[priority] + k) % n;
        ContextObject* c = contexts_[index].get();
        if (static_cast<int>(c->priority) != priority || c->queue.count() == 0 || !runnable(*c)) continue;
        picked = c;
        next_[priority] = static_cast<uint32_t>(index + 1);
        break;
      }
    }
    if (!picked) return any;
    run_head(*picked);
    any = true;
  }
}

bool Adapter::allocation_in_use(const Allocation& a) const {
  for (uint64_t i = 0; i < contexts_.count(); ++i)
    for (const UniquePointer<Submission>& s : contexts_[i]->queue.view())
      for (const SharedPointer<Allocation>& held : s->allocations.view())
        if (held.get() == &a) return true;
  return false;
}

}  // namespace dxgk
