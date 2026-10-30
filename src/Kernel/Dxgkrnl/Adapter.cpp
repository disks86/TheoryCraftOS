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

  NtStatus validate(IPipelineLookup& lookup, List<SharedPointer<PipelineObject>>& pipelines, List<SharedPointer<RootSignatureObject>>& signatures, List<SharedPointer<DescriptorHeapObject>>& heaps) {
    exec_ = false;
    lookup_ = &lookup;
    collected_ = &pipelines;
    collected_signatures_ = &signatures;
    collected_heaps_ = &heaps;
    const NtStatus s = walk();
    // (the states the barriers left allocations in count once the whole stream is good)
    if (s == status::kSuccess)
      for (uint64_t i = 0; i < shadow_.count() && i < allocations_.size(); ++i) allocations_[i]->set_state(shadow_[i]);
    return s;
  }

  NtStatus execute(ArrayView<const SharedPointer<PipelineObject>> pipelines, ArrayView<const SharedPointer<RootSignatureObject>> signatures, ArrayView<const SharedPointer<DescriptorHeapObject>> heaps) {
    exec_ = true;
    replay_ = pipelines;
    replay_signatures_ = signatures;
    replay_heaps_ = heaps;
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
    device_patch_points_ = 0;
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
      case Op::SetStreamOutput: return set_stream_output(p);
      case Op::ResolveRegion: return resolve(p);
      case Op::SetRootSignature: return set_root_signature(p);
      case Op::SetDescriptorHeaps: return set_descriptor_heaps(p);
      case Op::SetRootConstants: return set_root_constants(p);
      case Op::SetRootDescriptor: return set_root_descriptor(p);
      case Op::SetRootTable: return set_root_table(p);
      case Op::ResourceBarrier: return resource_barrier(p);
      case Op::Dispatch: return dispatch(p);
      case Op::SetTopology: {
        const uint32_t topology = p.le32();
        const bool adjacency = topology == static_cast<uint32_t>(Topology::LineListAdjacency) || topology == static_cast<uint32_t>(Topology::TriangleListAdjacency);
        if (!exact(p) || (!adjacency && topology > kPatchListBase + graphics::kMaxPatchControlPoints)) return kBad;
        if (adjacency ? !(adapter_.caps_.features & graphics::kFeatureGeometryShaders) : (topology > static_cast<uint32_t>(Topology::PointList) && !(adapter_.caps_.features & graphics::kFeatureTessellation))) return kBad;
        topology_ = static_cast<Topology>(topology);
        return status::kSuccess;
      }
    }
    return kBad;
  }

  NtStatus begin_query(ByteReader& p) {
    const uint32_t index = p.le32(), offset = p.le32();
    uint32_t kind = 0;
    if (!p.at_end()) kind = p.le32();
    if (!exact(p) || !in_pass_ || query_open_ || !(adapter_.caps_.features & graphics::kFeatureOcclusionQueries)) return kBad;
    // (the kind in the low byte, the output stream of a stream output query above it)
    const uint32_t stream = kind >> 8;
    kind &= 0xFF;
    const uint32_t features = adapter_.caps_.features;
    const bool so_kind = kind == static_cast<uint32_t>(graphics::QueryKind::SoPrimitivesWritten) || kind == static_cast<uint32_t>(graphics::QueryKind::SoPrimitivesNeeded);
    if (kind > static_cast<uint32_t>(graphics::QueryKind::TimestampDisjoint) || stream >= (so_kind ? graphics::kMaxGeometryStreams : 1u)) return kBad;
    if (kind == static_cast<uint32_t>(graphics::QueryKind::Timestamp) || kind == static_cast<uint32_t>(graphics::QueryKind::TimestampFrequency) || kind == static_cast<uint32_t>(graphics::QueryKind::TimestampDisjoint) ? !(features & graphics::kFeatureTimestampQueries)
        : so_kind ? (features & (graphics::kFeatureStatisticsQueries | graphics::kFeatureStreamOutput)) != (graphics::kFeatureStatisticsQueries | graphics::kFeatureStreamOutput)
        : (kind != 0 && !(features & graphics::kFeatureStatisticsQueries))) return kBad;
    Allocation* a = alloc(index);
    if (!a || !a->is_query_result() || offset % 8 || offset > a->desc().size - 8) return kBad;
    if (queries_in_stream_ >= graphics::kMaxOcclusionQueries) return kBad;
    ++queries_in_stream_;
    query_open_ = true;
    if (!exec_) return status::kSuccess;
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    if (!pending_.push_back({a, offset, next_slot_})) return status::kNoMemory;
    cb_->begin_query(next_slot_++, static_cast<graphics::QueryKind>(kind), stream);
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
    if (!color || !color->is_image() || !(color->desc().flags & kAllocRenderTarget) || !use(color, state::kRenderTarget)) return kBad;
    if (depth_index != kNoAllocation && (!depth || !depth->is_image() || !(depth->desc().flags & kAllocDepthStencil))) return kBad;
    if (depth && !use(depth, state::kDepthWrite)) return kBad;
    if (depth && (depth->desc().width != color->desc().width || depth->desc().height != color->desc().height || depth->desc().samples != color->desc().samples)) return kBad;
    for (uint32_t n = 0; n < extra_count; ++n) {
      const Allocation* e = extras[n];
      if (!e->is_image() || !(e->desc().flags & kAllocRenderTarget) || e == color || e == depth || !use(extras[n], state::kRenderTarget)) return kBad;
      if (e->desc().width != color->desc().width || e->desc().height != color->desc().height || e->desc().format != color->desc().format || e->desc().samples != color->desc().samples) return kBad;
      for (uint32_t k = 0; k < n; ++k)
        if (extras[k] == e) return kBad;
    }
    if (flags & ~(kPassClearColor | kPassClearDepth | kPassClearStencil)) return kBad;
    // (the clear colour of an integer target is four integers as float bits: any bit pattern)
    if (!is_integer(color->desc().format) && (!finite_within(clear.x, 1e6f) || !finite_within(clear.y, 1e6f) || !finite_within(clear.z, 1e6f) || !finite_within(clear.w, 1e6f))) return kBad;
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

  // Resolves a multisampled target into an ordinary image of the same format and size: the samples of a colour target averaged (or sample 0 taken), a depth buffer's sample 0 (or, with the
  // adapter's kFeatureResolveModes, its least, greatest or mean). The mode is an optional third word (a graphics::ResolveMode, 0 average when it is left out).
  NtStatus resolve(ByteReader& p) {
    const uint32_t to_index = p.le32(), from_index = p.le32();
    uint32_t mode = 0;
    if (!p.at_end()) mode = p.le32();
    if (!exact(p) || in_pass_ || !(adapter_.caps_.features & graphics::kFeatureMultisample) || mode > static_cast<uint32_t>(graphics::ResolveMode::Max)) return kBad;
    Allocation* to = alloc(to_index);
    Allocation* from = alloc(from_index);
    if (!to || !from || !to->is_image() || !from->is_image() || to->is_staging() || to == from || to->is_depth() != from->is_depth() || !use(to, state::kResolveDest) || !use(from, state::kResolveSource)) return kBad;
    const bool depth = to->is_depth();
    if (from->desc().samples < 2 || to->desc().samples != 1 || to->mip_levels() != 1 || to->layers() != 1) return kBad;
    if (to->desc().format != from->desc().format || to->desc().width != from->desc().width || to->desc().height != from->desc().height) return kBad;
    if (!(from->desc().flags & (depth ? kAllocDepthStencil : kAllocRenderTarget))) return kBad;
    // (which modes the adapter has: the average of a colour image, the first sample of a depth image, all of them with kFeatureResolveModes)
    const bool plain = depth ? mode == static_cast<uint32_t>(graphics::ResolveMode::Sample0) : mode == static_cast<uint32_t>(graphics::ResolveMode::Average);
    if (depth && !(adapter_.caps_.features & graphics::kFeatureDepthResolve)) return kBad;
    if (!plain && !(adapter_.caps_.features & graphics::kFeatureResolveModes)) return kBad;
    unbind_textures_of(to);
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*to);
    if (s == status::kSuccess) s = resident(*from);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->resolve_image(to->image(), from->image(), static_cast<graphics::ResolveMode>(mode));
    mm_.note_gpu_write(*to);
    return status::kSuccess;
  }

  // After a draw: the buffers its pipeline's stream output wrote have newer contents on the device than anywhere else.
  void note_stream_output_written() {
    if (!pipeline_ || !pipeline_->has_stream_output()) return;
    for (uint32_t b = 0; b < graphics::kMaxStreamOutputBuffers; ++b)
      if ((pipeline_->stream_output_buffers() >> b & 1) && so_bound_[b] && so_alloc_[b]) mm_.note_gpu_write(*so_alloc_[b]);
  }

  NtStatus set_stream_output(ByteReader& p) {
    const uint32_t slot = p.le32(), index = p.le32(), offset = p.le32();
    if (!exact(p) || !(adapter_.caps_.features & graphics::kFeatureStreamOutput) || slot >= graphics::kMaxStreamOutputBuffers) return kBad;
    if (index == kNoAllocation) {
      so_bound_[slot] = false;
      so_alloc_[slot] = nullptr;
      if (!exec_) return status::kSuccess;
      const NtStatus s = ensure_cb();
      if (s != status::kSuccess) return s;
      cb_->bind_stream_output(slot, nullptr, 0);
      return status::kSuccess;
    }
    Allocation* a = alloc(index);
    const bool append = offset == kStreamOutputAppendOffset;
    if (!a || !(a->desc().flags & kAllocStreamOutput) || (!append && (offset % 4 || offset > a->desc().size)) || !use(a, state::kStreamOut)) return kBad;
    so_bound_[slot] = true;
    so_alloc_[slot] = a;
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_stream_output(slot, a->buffer(), append ? graphics::kStreamOutputAppend : offset);
    return status::kSuccess;
  }

  NtStatus bind_uav(ByteReader& p) {
    const uint32_t slot = p.le32(), index = p.le32(), offset = p.le32(), size = p.le32();
    if (!exact(p) || in_pass_ || !(adapter_.caps_.features & graphics::kFeatureCompute) || slot >= graphics::kMaxStorageBuffers) return kBad;
    Allocation* a = alloc(index);
    if (!a || !(a->desc().flags & kAllocUnorderedAccess) || !use(a, state::kUnorderedAccess) || size == 0 || offset % 4 || size % 4 || offset > a->desc().size || size > a->desc().size - offset) return kBad;
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
    if (const NtStatus bound = apply_root_arguments()) return bound;
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
    if (!a || !a->is_buffer() || !(a->desc().flags & kAllocVertexBuffer) || offset > a->desc().size || !use(a, state::kVertexAndConstantBuffer)) return kBad;
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
    if (!a || !a->is_buffer() || !(a->desc().flags & kAllocIndexBuffer) || offset > a->desc().size || offset % index_bytes || !use(a, state::kIndexBuffer)) return kBad;
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
    if (!a || !a->is_image() || !(a->desc().flags & kAllocTexture) || a->is_staging() || !use(a, state::kPixelShaderResource | state::kNonPixelShaderResource)) return kBad;
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
    if (const NtStatus bound = apply_root_arguments()) return bound;
    const uint32_t count = p.le32(), first = p.le32();
    const int32_t vertex_offset = indexed ? static_cast<int32_t>(p.le32()) : 0;
    uint32_t instance_count = 1, first_instance = 0;
    if (instanced) {
      instance_count = p.le32();
      first_instance = p.le32();
    }
    if (!exact(p) || !in_pass_ || !pipeline_) return kBad;
    if (instanced && (!(adapter_.caps_.features & graphics::kFeatureInstancing) || instance_count == 0 || instance_count > (1u << 20))) return kBad;
    if (topology_ >= Topology::LineList && topology_ <= Topology::PointList && !(adapter_.caps_.features & graphics::kFeatureLines)) return kBad;
    // Patches go with a hull and a domain shader that take that many control points, and those shaders go with patches (and with no geometry shader).
    if (is_patch_list(topology_) != pipeline_->has_tessellation()) return kBad;
    if (pipeline_->has_stream_output())   // (every buffer the declaration writes needs a target)
      for (uint32_t b = 0; b < graphics::kMaxStreamOutputBuffers; ++b)
        if ((pipeline_->stream_output_buffers() >> b & 1) && !so_bound_[b]) return kBad;
    if (is_adjacency(topology_) && !pipeline_->has_geometry()) return kBad;   // (only a geometry shader takes the neighbours)
    if (pipeline_->has_tessellation() && (patch_points(topology_) != pipeline_->tess_input_points() || pipeline_->has_geometry())) return kBad;
    for (uint32_t u = 0; u < pipeline_->texture_count(); ++u)
      if (!texture_bound_[u] || (texture_alloc_[u]->desc().samples > 1 ? graphics::TextureKind::Texture2DMS : texture_alloc_[u]->texture_kind()) != pipeline_->texture_kind(u)) return kBad;   // (unbound, or a volume where the shader reads a 2D texture, ...)
    if (pipeline_->color_target_count() > pass_target_count_) return kBad;
    if (pipeline_->blends()) {   // (an integer target takes no blending)
      if (is_integer(pass_color_->desc().format)) return kBad;
      for (uint32_t n = 0; n + 1 < pass_target_count_; ++n)
        if (pass_extra_[n] && is_integer(pass_extra_[n]->desc().format)) return kBad;
    }
    if (pipeline_->has_geometry()) {   // (the geometry shader takes the primitives the draw makes)
      const graphics::GeometryPrimitive made = topology_ == Topology::LineListAdjacency ? graphics::GeometryPrimitive::LineAdjacency
                                               : topology_ == Topology::TriangleListAdjacency ? graphics::GeometryPrimitive::TriangleAdjacency
                                               : topology_ == Topology::LineList || topology_ == Topology::LineStrip ? graphics::GeometryPrimitive::Line
                                               : topology_ == Topology::PointList ? graphics::GeometryPrimitive::Point : graphics::GeometryPrimitive::Triangle;
      if (made != pipeline_->geometry_input()) return kBad;
    }
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
        if (topology_ != Topology::TriangleList && topology_ != Topology::LineList && topology_ != Topology::PointList && !is_patch_list(topology_) && raw == restart) continue;   // (ends a strip: names no vertex)
        const int64_t v = int64_t{raw} + vertex_offset;
        if (!in.ok() || v < 0) return kBad;
        if (v > highest) highest = v;
      }
      if (vertex_offset_ + static_cast<uint64_t>(highest + 1) * stride > vertex_->desc().size) return kBad;
    }
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    if (topology_ == Topology::TriangleStrip || topology_ == Topology::TriangleFan || topology_ == Topology::LineStrip) return draw_expanded(indexed, count, first, vertex_offset, instance_count, first_instance);
    if (is_patch_list(topology_)) {
      set_device_topology(graphics::PrimitiveTopology::PatchList);
      if (device_patch_points_ != patch_points(topology_)) {
        cb_->set_patch_control_points(patch_points(topology_));
        device_patch_points_ = patch_points(topology_);
      }
    } else {
      set_device_topology(topology_ == Topology::LineListAdjacency ? graphics::PrimitiveTopology::LineListAdjacency : topology_ == Topology::TriangleListAdjacency ? graphics::PrimitiveTopology::TriangleListAdjacency : topology_ == Topology::LineList ? graphics::PrimitiveTopology::LineList : topology_ == Topology::PointList ? graphics::PrimitiveTopology::PointList : graphics::PrimitiveTopology::TriangleList);
    }
    if (indexed && index_rebind_) {   // (an expanded draw left its own indices bound)
      cb_->bind_index_buffer(index_->buffer(), index_bytes_ == 4 ? graphics::IndexType::Uint32 : graphics::IndexType::Uint16, index_offset_);
      index_rebind_ = false;
    }
    if (indexed) cb_->draw_indexed(count, first, vertex_offset, instance_count, first_instance);
    else cb_->draw(count, first, instance_count, first_instance);
    note_stream_output_written();
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
    note_stream_output_written();
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
    if (!dst || !src || !dst->is_image() || !src->is_image() || dst->is_depth() || src->is_depth() || !use(dst, state::kCopyDest) || !use(src, state::kCopySource)) return kBad;
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

  // --- Direct3D 12 binding -----------------------------------------------------------------------------------------------------------------------------------

  uint64_t slot_of(const Allocation* a) const {
    for (uint64_t i = 0; i < allocations_.size(); ++i)
      if (allocations_[i].get() == a) return i;
    return ~uint64_t{0};
  }
  bool ensure_shadow() {
    if (shadow_.count() == allocations_.size()) return true;
    shadow_.clear();
    for (const SharedPointer<Allocation>& a : allocations_)
      if (!shadow_.push_back(a->state())) return false;
    return true;
  }
  uint32_t state_of(const Allocation* a) {
    const uint64_t slot = slot_of(a);
    return ensure_shadow() && slot < shadow_.count() ? shadow_[slot] : a->state();
  }
  // Whether `a` may be used for `needed` (any one of the bits) in the state the stream has left it in; an allocation that does not track states always may.
  // (Replaying a stream that was checked does not check again: the states are those it left.)
  bool use(const Allocation* a, uint32_t needed) {
    return exec_ || !a->state_tracked() || state_allows(state_of(a), needed);
  }
  // The index of an allocation in the list (what the lowered bindings are called with).
  bool index_of(const Allocation* a, uint32_t* index) const {
    const uint64_t slot = slot_of(a);
    if (slot == ~uint64_t{0}) return false;
    *index = static_cast<uint32_t>(slot);
    return true;
  }

  NtStatus set_root_signature(ByteReader& p) {
    const Handle handle = p.le32();
    if (!exact(p)) return kBad;
    SharedPointer<RootSignatureObject> found;
    if (exec_) {
      if (replay_signature_cursor_ >= replay_signatures_.size()) return kBad;
      found = replay_signatures_[replay_signature_cursor_++];
    } else {
      found = lookup_->find_root_signature(handle);
      if (!found || !collected_signatures_->push_back(found)) return found ? status::kNoMemory : status::kInvalidHandle;
    }
    signature_ = found;
    for (RootArgument& a : arguments_) a = RootArgument();
    return status::kSuccess;
  }

  NtStatus set_descriptor_heaps(ByteReader& p) {
    const uint32_t count = p.le32();
    if (!p.ok() || count == 0 || count > 2) return kBad;
    SharedPointer<DescriptorHeapObject> views, samplers;
    for (uint32_t i = 0; i < count; ++i) {
      const Handle handle = p.le32();
      if (!p.ok()) return kBad;
      SharedPointer<DescriptorHeapObject> found;
      if (exec_) {
        if (replay_heap_cursor_ >= replay_heaps_.size()) return kBad;
        found = replay_heaps_[replay_heap_cursor_++];
      } else {
        found = lookup_->find_descriptor_heap(handle);
        if (!found || !collected_heaps_->push_back(found)) return found ? status::kNoMemory : status::kInvalidHandle;
      }
      SharedPointer<DescriptorHeapObject>& slot = found->type() == DescriptorHeapType::Sampler ? samplers : views;
      if (slot) return kBad;   // (two of a kind)
      slot = found;
    }
    if (!exact(p)) return kBad;
    view_heap_ = views;
    sampler_heap_ = samplers;
    return status::kSuccess;
  }

  const RootParameter* parameter(uint32_t index, RootParameterType type) const {
    if (!signature_ || index >= signature_->desc.parameter_count) return nullptr;
    const RootParameter& rp = signature_->desc.parameters[index];
    return rp.type == type ? &rp : nullptr;
  }

  NtStatus set_root_constants(ByteReader& p) {
    const uint32_t index = p.le32(), first = p.le32(), count = p.le32();
    const RootParameter* rp = parameter(index, RootParameterType::Constants);
    if (!p.ok() || !rp || count == 0 || first > rp->constant_count || count > rp->constant_count - first) return kBad;
    RootArgument& arg = arguments_[index];
    for (uint32_t i = 0; i < count; ++i) arg.values[first + i] = p.le32();
    if (!exact(p)) return kBad;
    arg.set = true;
    return status::kSuccess;
  }

  NtStatus set_root_descriptor(ByteReader& p) {
    const uint32_t index = p.le32(), allocation = p.le32(), offset = p.le32();
    if (!exact(p) || !signature_ || index >= signature_->desc.parameter_count) return kBad;
    const RootParameter& rp = signature_->desc.parameters[index];
    Allocation* a = alloc(allocation);
    if (!a || offset % 4 || offset >= a->desc().size) return kBad;
    if (rp.type == RootParameterType::Cbv) {
      if (!a->is_buffer() || a->is_query_result() || !use(a, state::kVertexAndConstantBuffer)) return kBad;
    } else if (rp.type == RootParameterType::Uav) {
      if (!(a->desc().flags & kAllocUnorderedAccess) || !use(a, state::kUnorderedAccess)) return kBad;
    } else {
      return kBad;   // (a shader resource view of a buffer: no device has buffer shader resources)
    }
    arguments_[index].set = true;
    arguments_[index].allocation = a;
    arguments_[index].offset = offset;
    return status::kSuccess;
  }

  NtStatus set_root_table(ByteReader& p) {
    const uint32_t index = p.le32(), first = p.le32();
    const RootParameter* rp = parameter(index, RootParameterType::Table);
    if (!exact(p) || !rp) return kBad;
    const DescriptorHeapObject* heap = rp->sampler_table ? sampler_heap_.get() : view_heap_.get();
    if (!heap || first >= heap->count()) return kBad;
    if (rp->table_size != kUnboundedDescriptors && rp->table_size > heap->count() - first) return kBad;
    arguments_[index].set = true;
    arguments_[index].first = first;
    return status::kSuccess;
  }

  NtStatus resource_barrier(ByteReader& p) {
    const uint32_t count = p.le32();
    if (!p.ok() || count == 0) return kBad;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t type = p.le32(), index = p.le32(), before = p.le32(), after = p.le32(), flags = p.le32();
      if (!p.ok() || type > 2 || flags > 2) return kBad;
      Allocation* a = index == kNoAllocation ? nullptr : alloc(index);
      if (index != kNoAllocation && !a) return kBad;
      if (type == 2) {
        // An aliasing barrier: `index` is the allocation going out of use and the "before" word the index of the one coming into use (either may be kNoAllocation: nothing to do), both placed in
        // one heap. What the GPU holds for the first goes to the heap's memory and the second is uploaded from it.
        Allocation* incoming = before == kNoAllocation ? nullptr : alloc(before);
        if (before != kNoAllocation && !incoming) return kBad;
        if (!a || !incoming) continue;
        if (in_pass_ || !a->placed() || a->heap().get() != incoming->heap().get() || a == incoming) return kBad;
        if (exec_) {
          NtStatus s = flush();   // (what was recorded runs first)
          if (s == status::kSuccess && !mm_.switch_alias(*a, *incoming)) s = status::kInvalidDeviceState;
          if (s != status::kSuccess) return s;
        }
        continue;
      }
      if (exec_) continue;   // (the rest was checked when the stream was queued)
      if (type != 0) {
        // (an unordered access barrier: for an allocation that is in that state; the devices order a dispatch's writes themselves. An aliasing barrier: nothing to do.)
        if (type == 1 && a && a->state_tracked() && !(state_of(a) & state::kUnorderedAccess)) return kBad;
        continue;
      }
      if (!a || ((before | after) & ~kStateMask) || before == after) return kBad;
      if ((after & state::kWrite) && (after & (after - 1))) return kBad;   // (a state that writes stands alone)
      if (!a->state_tracked()) continue;
      if (flags == 1) {
        if (state_of(a) != before) return kBad;
        continue;   // (a split barrier begins: the change is made at its end)
      }
      if (state_of(a) != before) return kBad;
      if (!ensure_shadow()) return status::kNoMemory;
      const uint64_t slot = slot_of(a);
      if (slot < shadow_.count()) shadow_[slot] = after;
    }
    return exact(p) ? status::kSuccess : kBad;
  }

  // What a draw or dispatch binds from the root arguments: b0 (the first constant buffer: root constants, a root constant buffer view or a table's first view) as the push constants, the shader
  // resource views of tables as the texture units t<n> (with the sampler s<n> of a table or a static sampler, else s0, else the defaults), the unordered access views as storage slots u<n>.
  NtStatus apply_root_arguments() {
    if (!signature_) return status::kSuccess;
    const RootSignatureDesc& sig = signature_->desc;
    bool constants_found = false;
    uint8_t block[graphics::kMaxPushConstantBytes] = {};
    uint32_t block_bytes = 0;
    auto sampler_for = [&](uint32_t unit) {
      const graphics::SamplerDesc* found = nullptr;
      const graphics::SamplerDesc* zero = nullptr;
      for (uint32_t i = 0; i < sig.sampler_count; ++i) {
        if (sig.samplers[i].space != 0) continue;
        if (sig.samplers[i].shader_register == unit) found = &sig.samplers[i].sampler;
        if (sig.samplers[i].shader_register == 0) zero = &sig.samplers[i].sampler;
      }
      for (uint32_t i = 0; i < sig.parameter_count; ++i) {
        const RootParameter& rp = sig.parameters[i];
        if (rp.type != RootParameterType::Table || !rp.sampler_table || !arguments_[i].set || !sampler_heap_) continue;
        for (uint32_t r = 0; r < rp.range_count; ++r) {
          const DescriptorRange& range = rp.ranges[r];
          if (range.space != 0 || range.type != RangeType::Sampler) continue;
          for (uint32_t k : {unit, 0u}) {
            if (k < range.base_register || (range.count != kUnboundedDescriptors && k - range.base_register >= range.count)) continue;
            const uint64_t at = uint64_t{arguments_[i].first} + range.offset + (k - range.base_register);
            if (at < sampler_heap_->count() && sampler_heap_->at(at).kind == DescriptorKind::Sampler) {
              if (k == unit) return sampler_heap_->at(at).sampler;
              if (!zero) zero = &sampler_heap_->at(at).sampler;
            }
          }
        }
      }
      return found ? *found : zero ? *zero : graphics::SamplerDesc();
    };
    for (uint32_t i = 0; i < sig.parameter_count; ++i) {
      const RootParameter& rp = sig.parameters[i];
      const RootArgument& arg = arguments_[i];
      if (rp.type == RootParameterType::Constants) {
        if (rp.space != 0 || rp.shader_register != 0) return kBad;   // (one constant buffer)
        if (!arg.set) return kBad;
        const uint32_t bytes = (rp.constant_count * 4 + 15) / 16 * 16;
        if (constants_found || bytes > adapter_.caps_.max_push_constant_bytes) return kBad;
        constants_found = true;
        block_bytes = bytes;
        ByteWriter w(ByteView(block, sizeof block));
        for (uint32_t k = 0; k < rp.constant_count; ++k) w.le32(arg.values[k]);
      } else if (rp.type == RootParameterType::Cbv) {
        if (rp.space != 0 || rp.shader_register != 0 || constants_found || !arg.set) return kBad;
        constants_found = true;
        const uint64_t avail = arg.allocation->desc().size - arg.offset;
        block_bytes = static_cast<uint32_t>(avail < adapter_.caps_.max_push_constant_bytes ? avail / 16 * 16 : adapter_.caps_.max_push_constant_bytes);
        if (block_bytes == 0 || block_bytes > sizeof block) return kBad;
        ByteView out(block, block_bytes);
        if (!out.copy_from(arg.allocation->backing().subview(arg.offset, block_bytes))) return kBad;
      } else if (rp.type == RootParameterType::Uav) {
        if (rp.space != 0 || !arg.set || rp.shader_register >= graphics::kMaxStorageBuffers) return kBad;
        const NtStatus s = lower_uav(rp.shader_register, arg.allocation, arg.offset, arg.allocation->desc().size - arg.offset);
        if (s != status::kSuccess) return s;
      } else if (rp.type == RootParameterType::Table && !rp.sampler_table) {
        if (!arg.set || !view_heap_) return kBad;
        for (uint32_t r = 0; r < rp.range_count; ++r) {
          const DescriptorRange& range = rp.ranges[r];
          if (range.space != 0) return kBad;
          const uint64_t available = view_heap_->count() - arg.first;
          const uint64_t count = range.count == kUnboundedDescriptors ? (available > range.offset ? available - range.offset : 0) : range.count;
          for (uint64_t k = 0; k < count; ++k) {
            const uint64_t at = uint64_t{arg.first} + range.offset + k;
            if (at >= view_heap_->count()) return kBad;
            const Descriptor& d = view_heap_->at(at);
            const uint32_t reg = range.base_register + static_cast<uint32_t>(k);
            if (d.kind == DescriptorKind::None) continue;   // (a descriptor that was never written: not used by a shader that is right)
            if (range.type == RangeType::Srv && d.kind == DescriptorKind::Srv) {
              if (reg >= graphics::kMaxTextureUnits) return kBad;
              const NtStatus s = lower_texture(reg, d.allocation.get(), sampler_for(reg));
              if (s != status::kSuccess) return s;
            } else if (range.type == RangeType::Uav && d.kind == DescriptorKind::Uav) {
              if (reg >= graphics::kMaxStorageBuffers) return kBad;
              const NtStatus s = lower_uav(reg, d.allocation.get(), d.offset, d.size);
              if (s != status::kSuccess) return s;
            } else if (range.type == RangeType::Cbv && d.kind == DescriptorKind::Cbv) {
              if (reg != 0 || constants_found) return kBad;
              constants_found = true;
              block_bytes = static_cast<uint32_t>(d.size < adapter_.caps_.max_push_constant_bytes ? d.size / 16 * 16 : adapter_.caps_.max_push_constant_bytes);
              if (block_bytes == 0 || block_bytes > sizeof block || !use(d.allocation.get(), state::kVertexAndConstantBuffer)) return kBad;
              ByteView out(block, block_bytes);
              if (!out.copy_from(d.allocation->backing().subview(d.offset, block_bytes))) return kBad;
            } else if (d.kind != DescriptorKind::Srv && d.kind != DescriptorKind::Uav && d.kind != DescriptorKind::Cbv) {
              return kBad;
            } else if ((range.type == RangeType::Srv) != (d.kind == DescriptorKind::Srv) || (range.type == RangeType::Uav) != (d.kind == DescriptorKind::Uav)) {
              return kBad;   // (a descriptor of another kind than its range)
            }
          }
        }
      }
    }
    if (constants_found && (!constants_sent_ || block_bytes != last_constants_bytes_ || __builtin_memcmp(block, last_constants_, block_bytes) != 0)) {
      ByteBuffer packet(adapter_.allocator_);
      packet.put32(block_bytes);
      packet.append(ConstByteView(block, block_bytes));
      ByteReader reader(packet.view());
      const NtStatus s = push_constants(reader);
      if (s != status::kSuccess) return s;
      constants_sent_ = true;
      last_constants_bytes_ = block_bytes;
      ByteView(last_constants_, sizeof last_constants_).copy_from(ConstByteView(block, block_bytes));
    }
    return status::kSuccess;
  }

  NtStatus lower_texture(uint32_t unit, Allocation* a, const graphics::SamplerDesc& sampler) {
    uint32_t index = 0;
    if (!a || !index_of(a, &index)) return kBad;   // (the allocation is in the list or the draw is refused)
    ByteBuffer packet(adapter_.allocator_);
    for (uint32_t w : {unit, index, static_cast<uint32_t>(sampler.filter), static_cast<uint32_t>(sampler.address_u), static_cast<uint32_t>(sampler.address_v), static_cast<uint32_t>(sampler.mip_filter),
                       __builtin_bit_cast(uint32_t, sampler.lod_bias), __builtin_bit_cast(uint32_t, sampler.min_lod), __builtin_bit_cast(uint32_t, sampler.max_lod)})
      packet.put32(w);
    ByteReader reader(packet.view());
    return bind_texture(reader);
  }

  NtStatus lower_uav(uint32_t slot, Allocation* a, uint64_t offset, uint64_t size) {
    uint32_t index = 0;
    if (!a || !index_of(a, &index) || offset > 0xFFFFFFFFu || size > 0xFFFFFFFFu) return kBad;
    ByteBuffer packet(adapter_.allocator_);
    for (uint32_t w : {slot, index, static_cast<uint32_t>(offset), static_cast<uint32_t>(size)}) packet.put32(w);
    ByteReader reader(packet.view());
    return bind_uav(reader);
  }

  bool constants_sent_ = false;
  uint32_t last_constants_bytes_ = 0;
  alignas(16) uint8_t last_constants_[graphics::kMaxPushConstantBytes] = {};

  Adapter& adapter_;
  VidMm& mm_;
  ConstByteView commands_;
  ArrayView<const SharedPointer<Allocation>> allocations_;
  bool exec_ = false;
  IPipelineLookup* lookup_ = nullptr;
  List<SharedPointer<PipelineObject>>* collected_ = nullptr;
  ArrayView<const SharedPointer<PipelineObject>> replay_;
  uint64_t replay_cursor_ = 0;
  List<SharedPointer<RootSignatureObject>>* collected_signatures_ = nullptr;
  List<SharedPointer<DescriptorHeapObject>>* collected_heaps_ = nullptr;
  ArrayView<const SharedPointer<RootSignatureObject>> replay_signatures_;
  ArrayView<const SharedPointer<DescriptorHeapObject>> replay_heaps_;
  uint64_t replay_signature_cursor_ = 0, replay_heap_cursor_ = 0;
  // Direct3D 12 binding: the root signature set, the heaps, and the arguments of its parameters.
  SharedPointer<RootSignatureObject> signature_;
  SharedPointer<DescriptorHeapObject> view_heap_, sampler_heap_;
  struct RootArgument {
    bool set = false;
    uint32_t values[kMaxRootDwords] = {};
    Allocation* allocation = nullptr;   // a root descriptor: the buffer and the offset of its range
    uint64_t offset = 0;
    uint32_t first = 0;                 // a table: its first descriptor in the heap
  } arguments_[kMaxRootParameters];
  List<uint32_t> shadow_{adapter_.allocator_};   // the resource state of each allocation of the list, as the barriers so far leave it (empty until one is asked)
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
  bool so_bound_[graphics::kMaxStreamOutputBuffers] = {};
  Allocation* so_alloc_[graphics::kMaxStreamOutputBuffers] = {};
  bool uav_bound_[graphics::kMaxStorageBuffers] = {};
  Allocation* uav_alloc_[graphics::kMaxStorageBuffers] = {};
  Allocation* vertex_ = nullptr;
  uint64_t vertex_offset_ = 0;
  Allocation* index_ = nullptr;
  uint64_t index_offset_ = 0;
  uint32_t index_bytes_ = 2;
  Topology topology_ = Topology::TriangleList;
  uint32_t device_patch_points_ = 0;
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
      if (stage != graphics::ShaderStage::Compute && stage != graphics::ShaderStage::Geometry && stage != graphics::ShaderStage::Hull && stage != graphics::ShaderStage::Domain) {
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
      if (stage == graphics::ShaderStage::Compute || stage == graphics::ShaderStage::Geometry || stage == graphics::ShaderStage::Hull || stage == graphics::ShaderStage::Domain) {
        // (nothing but the interpreter and the TGSI text runs a compute or geometry shader)
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
      shader->geometry_input_ = program->geometry_input();
      shader->geometry_streams_ = program->geometry_streams();
      shader->tess_input_points_ = program->tess_input_points();
      shader->tess_output_points_ = program->tess_output_points();
      shader->tess_domain_ = program->tess_domain();
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

NtStatus Adapter::create_pipeline(const PipelineDesc& d, const SharedPointer<ShaderObject>& vertex, const SharedPointer<ShaderObject>& fragment, SharedPointer<PipelineObject>* out,
                                  const SharedPointer<ShaderObject>& geometry, const SharedPointer<ShaderObject>& hull, const SharedPointer<ShaderObject>& domain) {
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
  if ((d.multisample || d.alpha_to_coverage) && !(caps_.features & graphics::kFeatureMultisample)) return status::kNotSupported;
  if (d.alpha_to_coverage && !d.multisample) return status::kInvalidParameter;
  if (hull || domain) {
    if (!hull || !domain || hull->stage() != graphics::ShaderStage::Hull || domain->stage() != graphics::ShaderStage::Domain || hull->tess_output_points() != domain->tess_input_points() || hull->tess_domain() != domain->tess_domain()) return status::kInvalidParameter;
    if (!(caps_.features & graphics::kFeatureTessellation)) return status::kNotSupported;
    if (geometry) return status::kNotSupported;   // (a geometry shader after the tessellator is not made)
  }
  if (d.stream_output_count) {
    // (stream output takes the outputs of the geometry shader, or the domain shader, or the vertex shader; the entries name streams the geometry shader has (the others have one), outputs and
    // components that exist, and a buffer takes only one stream; each buffer's record is as long as its entries need, in whole words)
    if (!(caps_.features & graphics::kFeatureStreamOutput)) return status::kNotSupported;
    if (d.stream_output_count > graphics::kMaxStreamOutputEntries || d.rasterized_stream < -1 || d.rasterized_stream >= static_cast<int32_t>(graphics::kMaxGeometryStreams) || (!geometry && d.rasterized_stream > 0)) return status::kInvalidParameter;
    const uint32_t stream_count = geometry ? geometry->geometry_streams() : 1;
    int32_t stream_of_buffer[graphics::kMaxStreamOutputBuffers] = {-1, -1, -1, -1};
    uint32_t bytes[graphics::kMaxStreamOutputBuffers] = {};
    for (uint32_t e = 0; e < d.stream_output_count; ++e) {
      const graphics::StreamOutputEntry& entry = d.stream_output[e];
      if (entry.stream >= stream_count || entry.output > graphics::kMaxVaryings || entry.component_count == 0 || entry.start_component + entry.component_count > 4 || entry.buffer >= graphics::kMaxStreamOutputBuffers)
        return status::kInvalidParameter;
      if (stream_of_buffer[entry.buffer] >= 0 && stream_of_buffer[entry.buffer] != entry.stream) return status::kInvalidParameter;
      stream_of_buffer[entry.buffer] = entry.stream;
      bytes[entry.buffer] += 4u * entry.component_count;
    }
    for (uint32_t b = 0; b < graphics::kMaxStreamOutputBuffers; ++b)
      if (stream_of_buffer[b] >= 0 && (d.stream_output_stride[b] < bytes[b] || d.stream_output_stride[b] % 4 || d.stream_output_stride[b] > 2048)) return status::kInvalidParameter;
  }
  if (geometry && (geometry->stage() != graphics::ShaderStage::Geometry || !(caps_.features & graphics::kFeatureGeometryShaders))) return geometry && geometry->stage() != graphics::ShaderStage::Geometry ? status::kInvalidParameter : status::kNotSupported;
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
  pd.alpha_to_coverage = d.alpha_to_coverage;
  pd.stream_output_count = d.stream_output_count;
  for (uint32_t e = 0; e < d.stream_output_count; ++e) pd.stream_output[e] = d.stream_output[e];
  for (uint32_t b = 0; b < graphics::kMaxStreamOutputBuffers; ++b) pd.stream_output_stride[b] = d.stream_output_stride[b];
  pd.rasterized_stream = d.rasterized_stream;
  pd.geometry_shader = geometry ? geometry->module() : nullptr;
  pd.hull_shader = hull ? hull->module() : nullptr;
  pd.domain_shader = domain ? domain->module() : nullptr;
  for (uint32_t i = 0; i < graphics::kMaxColorTargets - 1; ++i) pd.target_blend[i] = d.target_blend[i];
  pd.depth_compare = d.depth_compare;
  pd.color_target_count = d.color_target_count;

  SharedPointer<PipelineObject> pipeline = SharedPointer<PipelineObject>::make(allocator_, device_);
  if (!pipeline) return status::kNoMemory;
  pipeline->pipeline_ = device_.create_graphics_pipeline(pd);
  if (!pipeline->pipeline_) return status::kNotSupported;
  pipeline->vertex_ = vertex;
  pipeline->fragment_ = fragment;
  pipeline->geometry_ = geometry;
  pipeline->hull_ = hull;
  pipeline->domain_ = domain;
  if (hull) pipeline->tess_input_points_ = hull->tess_input_points();
  pipeline->blends_ = d.alpha_blend || d.blend.enable || (d.independent_blend && (d.target_blend[0].enable || d.target_blend[1].enable || d.target_blend[2].enable));
  if (geometry) pipeline->geometry_input_ = geometry->geometry_input();
  for (uint32_t e = 0; e < d.stream_output_count; ++e) pipeline->stream_output_buffers_ |= 1u << d.stream_output[e].buffer;
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
  const NtStatus v = runner.validate(pipelines, s->pipelines, s->signatures, s->heaps);
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
  return runner.execute(ArrayView<const SharedPointer<PipelineObject>>(s.pipelines.view().data(), s.pipelines.count()), ArrayView<const SharedPointer<RootSignatureObject>>(s.signatures.view().data(), s.signatures.count()),
                        ArrayView<const SharedPointer<DescriptorHeapObject>>(s.heaps.view().data(), s.heaps.count()));
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
