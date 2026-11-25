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
#include "Graphics/WidePixels.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/Mipmaps.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Hardware/VideoCodec/VideoProcess.hpp"
#include "Kernel/Dxgkrnl/VideoEncode.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace dxgk {

namespace {
StreamTrace g_stream_trace = nullptr;
}
void set_stream_trace(StreamTrace trace) { g_stream_trace = trace; }

namespace {

constexpr uint64_t kMaxShaderBytes = 256 * 1024;
constexpr uint32_t kMaxVertexStride = 2048;
constexpr float kMaxCoordinate = 1.0e6f;
constexpr int32_t kMaxScissor = 1 << 16;

bool finite_within(float v, float limit) { return v == v && v >= -limit && v <= limit; }

// The device takes shader text as a `const char*`. The bytes are the shader's own copy, NUL terminated by the caller, and char has no alignment requirement.
const char* text_pointer(ByteView text) { return reinterpret_cast<const char*>(text.data()); }


// Copies a w x h block of pixels (of a block-compressed format: of whole blocks, x, y, w and h in pixels) of `layers` layers between the system-memory backings of two allocations whose
// pixels are as many bytes: from layer src_layer .. of level src_level to dst_layer .. of dst_level (they may be the same allocation, the blocks overlapping). The GPU's newer contents are read
// back first, and the destination's device object is marked stale. The caller has checked the rectangles.
NtStatus copy_subresources_cpu(VidMm& mm, Allocation& dst, uint32_t dst_level, uint32_t dst_layer, uint32_t dx, uint32_t dy, Allocation& src, uint32_t src_level, uint32_t src_layer, uint32_t sx,
                               uint32_t sy, uint32_t w, uint32_t h, uint32_t layers) {
  if (!mm.read_back(src) || !mm.read_back(dst)) return status::kInvalidDeviceState;
  const Format f = src.desc().format;
  const uint32_t unit = is_compressed(f) ? 4 : 1;
  const uint64_t row_bytes = image_row_bytes(f, w), dst_pitch = image_row_bytes(f, graphics::mip_extent(dst.desc().width, dst_level)),
                 src_pitch = image_row_bytes(f, graphics::mip_extent(src.desc().width, src_level));
  const uint32_t rows = image_rows(f, h);
  const bool same = &dst == &src && dst_level == src_level;
  const bool layers_down = same && dst_layer > src_layer;                 // (overlapping layers: the ones that would be overwritten first are copied last)
  for (uint32_t k = 0; k < layers; ++k) {
    const uint32_t layer = layers_down ? layers - 1 - k : k;
    uint64_t dst_at = 0, src_at = 0, bytes = 0;
    if (!dst.subresource_range(dst_level, dst_layer + layer, &dst_at, &bytes) || !src.subresource_range(src_level, src_layer + layer, &src_at, &bytes)) return status::kInvalidParameter;
    const bool bottom_up = same && dst_layer == src_layer && dy > sy;       // (overlapping rows: copy the ones that would be overwritten last)
    for (uint32_t i = 0; i < rows; ++i) {
      const uint32_t row = bottom_up ? rows - 1 - i : i;
      ByteView to;
      ConstByteView from;
      if (!dst.writable_backing().try_subview(dst_at + uint64_t{dy / unit + row} * dst_pitch + image_row_bytes(f, dx), row_bytes, &to) ||
          !src.backing().try_subview(src_at + uint64_t{sy / unit + row} * src_pitch + image_row_bytes(f, sx), row_bytes, &from))
        return status::kInvalidParameter;
      to.copy_from(from);
    }
  }
  mm.note_cpu_write(dst);
  return status::kSuccess;
}
NtStatus copy_region_cpu(VidMm& mm, Allocation& dst, uint32_t dx, uint32_t dy, Allocation& src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
  return copy_subresources_cpu(mm, dst, 0, 0, dx, dy, src, 0, 0, sx, sy, w, h, 1);
}

// GenerateMipmaps on the backing: every level of every layer from the level before it by the box filter (graphics::mip_box_downsample's: each channel the mean of the 2 x 2 texels at
// twice the place, rounded to the nearest; a volume's 2 x 2 x 2 of the two slices at twice the slice), the 8-bit formats by their codes (an sRGB image's chain is of its stored codes), the
// wide ones by their values. The caller has read the GPU's contents back.
bool generate_levels_cpu(Allocation& a) {
  const AllocationDesc& d = a.desc();
  const bool wide = is_wide(d.format), volume = d.kind == graphics::TextureKind::Texture3D;
  const graphics::Format wide_format = device_wide_format(d.format);
  const uint32_t bpp = pixel_bytes(d.format), taps = volume ? 8 : 4;
  for (uint32_t level = 1; level < d.mip_levels; ++level) {
    const uint32_t sw = graphics::mip_extent(d.width, level - 1), sh = graphics::mip_extent(d.height, level - 1);
    const uint32_t dw = graphics::mip_extent(d.width, level), dh = graphics::mip_extent(d.height, level);
    const uint32_t below = a.level_layers(level - 1);
    for (uint32_t layer = 0; layer < a.level_layers(level); ++layer) {
      const uint32_t z0 = volume ? (2 * layer < below ? 2 * layer : below - 1) : layer, z1 = volume ? (2 * layer + 1 < below ? 2 * layer + 1 : below - 1) : layer;
      uint64_t to_at = 0, from_a = 0, from_b = 0, bytes = 0;
      if (!a.subresource_range(level, layer, &to_at, &bytes) || !a.subresource_range(level - 1, z0, &from_a, &bytes) || !a.subresource_range(level - 1, z1, &from_b, &bytes)) return false;
      for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x) {
          const uint32_t y0 = 2 * y < sh ? 2 * y : sh - 1, y1 = 2 * y + 1 < sh ? 2 * y + 1 : sh - 1, x0 = 2 * x < sw ? 2 * x : sw - 1, x1 = 2 * x + 1 < sw ? 2 * x + 1 : sw - 1;
          const uint64_t at[4] = {uint64_t{y0} * sw + x0, uint64_t{y0} * sw + x1, uint64_t{y1} * sw + x0, uint64_t{y1} * sw + x1};
          ByteView out;
          if (!a.writable_backing().try_subview(to_at + (uint64_t{y} * dw + x) * bpp, bpp, &out)) return false;
          if (wide) {
            graphics::Vec4 sum = {0, 0, 0, 0};
            for (uint32_t k = 0; k < taps; ++k) {
              ConstByteView texel;
              if (!a.backing().try_subview((k < 4 ? from_a : from_b) + at[k % 4] * bpp, bpp, &texel)) return false;
              ByteReader in(texel);
              sum = sum + graphics::decode_wide_pixel(wide_format, in);
            }
            ByteWriter w(out);
            graphics::encode_wide_pixel(wide_format, sum * (1.0f / static_cast<float>(taps)), w);
            if (!w.ok()) return false;
            continue;
          }
          uint32_t sum[4] = {};
          for (uint32_t k = 0; k < taps; ++k) {
            ConstByteView texel;
            if (!a.backing().try_subview((k < 4 ? from_a : from_b) + at[k % 4] * bpp, bpp, &texel)) return false;
            for (uint32_t c = 0; c < 4; ++c) sum[c] += texel[c];
          }
          for (uint32_t c = 0; c < 4; ++c) out[c] = static_cast<uint8_t>((sum[c] + taps / 2) / taps);
        }
    }
  }
  return true;
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
      : adapter_(adapter), mm_(adapter.vidmm_), commands_(commands), allocations_(allocations), queries_(adapter.allocator_), transient_(adapter.allocator_) {}
  
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

  // The video decoders the stream's DecodeFrames name: collected while it is checked, given back in the same order when it runs.
  void collect_decoders(List<SharedPointer<VideoDecoderObject>>* decoders) { collected_decoders_ = decoders; }
  void replay_decoders(ArrayView<const SharedPointer<VideoDecoderObject>> decoders) { replay_decoders_ = decoders; }

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
  static constexpr uint32_t kPipelineStatistics1Values = graphics::kPipelineStatisticCount + 3;   // (and the amplification and mesh shader invocations and mesh primitives: none)
  static constexpr uint32_t kMaxStreamQueryValues = kPipelineStatistics1Values;
  struct StreamQuery {
    Allocation* allocation = nullptr;
    uint64_t offset = 0;
    uint32_t kind = 0;        // (a graphics::QueryKind, kQueryKindPipelineStatistics1 or kQueryKindSoStatistics)
    uint32_t stream = 0;
    bool accumulate = false;
    bool open = true;
    bool written = false;
    bool segment = false;     // (it has device queries in the command buffer being recorded, from first_slot on)
    uint32_t first_slot = 0;
    uint64_t sums[kMaxStreamQueryValues] = {};   // what its segments counted so far
    int64_t vertex_adjust = 0;   // what the strips and fans drawn as lists while it was open take from (or add to) the vertices and vertex shader runs the device counted
  };

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
    uint32_t commands = 0;
    while (!in.at_end()) {
      const uint32_t header = in.le32();
      const uint32_t words = header >> 16;
      if (!in.ok() || words < 1 || words > kMaxPacketWords) return kBad;
      ConstByteView body = in.take(uint64_t{words - 1} * 4);
      if (!in.ok()) return kBad;
      ByteReader p(body);
      const NtStatus s = packet(static_cast<Op>(header & 0xFFFF), p);
      if (s != status::kSuccess) {
        if (g_stream_trace) g_stream_trace(header & 0xFFFF, s, commands, refused_line_);
        return s;
      }
      ++commands;
    }
    if (in_pass_) return kBad;   // a pass left open
    for (const StreamQuery& q : queries_.view())
      if (q.open) return kBad;   // a query left open
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
    raster_sent_ = false;   // (a new command buffer has the rasterizer state's defaults: the next draw sends the stream's)
    // (the queries still open go on in this command buffer: a segment each)
    for (StreamQuery& q : queries_.view())
      if (q.open) begin_segment(q);
    return status::kSuccess;
  }
  // Runs what has been recorded so far. Needed before the CPU touches an allocation the recorded commands use.
  NtStatus flush() {
    if (!cb_) return store_query_results() ? status::kSuccess : status::kInvalidDeviceState;
    // (the open queries' segments end with the command buffer; the next one begins new ones)
    for (StreamQuery& q : queries_.view())
      if (q.open && q.segment) end_segment(q);
    const bool ended = cb_->end();
    bool done = ended && adapter_.device_.submit(cb_);
    if (done) done = collect_segments();
    if (done) done = store_query_results();
    adapter_.device_.destroy_command_buffer(cb_);
    cb_ = nullptr;
    for (StreamQuery& q : queries_.view()) q.segment = false;
    next_slot_ = 0;
    constants_sent_ = false;   // (the next command buffer has none of the root arguments' constants: the next draw or dispatch sends them again)
    release_transient();
    return done ? status::kSuccess : status::kInvalidDeviceState;
  }

  // Runs what was recorded (flush) and opens a command buffer that has what the stream bound outside a pass and does not bind again at the next draw or dispatch: the compute pipeline (the
  // constants go again with the next one). What a mirror or a write-back needs the device's contents for, in the middle of a stream.
  NtStatus flush_keeping_state() {
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    constants_sent_ = false;
    if (!compute_) return status::kSuccess;
    s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_compute_pipeline(compute_->pipeline());
    return status::kSuccess;
  }

  // --- Queries ---
  // A query of the stream is open from its BeginQuery to its EndQuery, across passes and the command buffers a flush in the middle of the stream cuts it into: each command buffer it is
  // open in has a segment of it - the device queries it is made of (query_device_slots) - and what the segments counted is added up (query_sum) and written when it has ended.

  // The values a kernel query kind writes (8 bytes each), and the device queries one segment of it takes.
  static uint32_t query_values(uint32_t kind) {
    return kind == static_cast<uint32_t>(graphics::QueryKind::PipelineStatistics) ? graphics::kPipelineStatisticCount
           : kind == kQueryKindPipelineStatistics1 ? kPipelineStatistics1Values
           : kind == kQueryKindSoStatistics ? 2u : 1u;
  }
  static uint32_t query_device_slots(uint32_t kind) { return kind == kQueryKindSoStatistics ? 2u : 1u; }

  // Begins a segment of `q` in the command buffer being recorded.
  void begin_segment(StreamQuery& q) {
    q.first_slot = next_slot_;
    q.segment = true;
    next_slot_ += query_device_slots(q.kind);
    if (q.kind == kQueryKindSoStatistics) {
      cb_->begin_query(q.first_slot, graphics::QueryKind::SoPrimitivesWritten, q.stream);
      cb_->begin_query(q.first_slot + 1, graphics::QueryKind::SoPrimitivesNeeded, q.stream);
      return;
    }
    const uint32_t device_kind = q.kind == kQueryKindPipelineStatistics1 ? static_cast<uint32_t>(graphics::QueryKind::PipelineStatistics) : q.kind;
    cb_->begin_query(q.first_slot, static_cast<graphics::QueryKind>(device_kind), q.stream);
  }
  void end_segment(StreamQuery& q) {
    for (uint32_t k = 0; k < query_device_slots(q.kind); ++k) cb_->end_query(q.first_slot + k);
  }
  // After the device ran a command buffer: what each segment in it counted, added to its query.
  bool collect_segments() {
    for (StreamQuery& q : queries_.view()) {
      if (!q.segment) continue;
      using graphics::QueryKind;
      if (q.kind == kQueryKindSoStatistics) {
        uint64_t written = 0, needed = 0;
        if (!cb_->query_value(q.first_slot, 0, &written) || !cb_->query_value(q.first_slot + 1, 0, &needed)) return false;
        q.sums[0] += written;
        q.sums[1] += needed;
        continue;
      }
      const uint32_t values = q.kind == kQueryKindPipelineStatistics1 ? graphics::kPipelineStatisticCount : query_values(q.kind);
      for (uint32_t k = 0; k < values; ++k) {
        uint64_t v = 0;
        if (!cb_->query_value(q.first_slot, k, &v)) return false;
        // (a time is the last segment's - the time the query ended -, a frequency the same in every one; counts, predicates and the disjoint flag add up)
        if (q.kind == static_cast<uint32_t>(QueryKind::Timestamp) || q.kind == static_cast<uint32_t>(QueryKind::TimestampFrequency)) q.sums[k] = v;
        else q.sums[k] += v;
      }
    }
    return true;
  }
  // Writes the results of the queries that ended into the allocations they name (the CPU's copy: these allocations never go to the GPU). A query begun with kQueryAccumulate adds
  // its counts to what is there (a predicate: 1 if either is not 0).
  bool store_query_results() {
    for (StreamQuery& q : queries_.view()) {
      if (q.open || q.written) continue;
      q.written = true;
      const uint32_t values = query_values(q.kind);
      ByteView where;
      if (!q.allocation->writable_backing().try_subview(q.offset, uint64_t{values} * 8, &where)) return false;
      const bool predicate = q.kind == static_cast<uint32_t>(graphics::QueryKind::OcclusionPredicate) || q.kind == static_cast<uint32_t>(graphics::QueryKind::TimestampDisjoint);
      // (the vertices and vertex shader runs of strips and fans, as Direct3D counts them)
      auto adjust = [&](uint32_t k) {
        const int64_t v = static_cast<int64_t>(q.sums[k]) + q.vertex_adjust;
        q.sums[k] = v > 0 ? static_cast<uint64_t>(v) : 0;
      };
      if (q.kind == static_cast<uint32_t>(graphics::QueryKind::IaVertices) || q.kind == static_cast<uint32_t>(graphics::QueryKind::VsInvocations)) adjust(0);
      if (q.kind == static_cast<uint32_t>(graphics::QueryKind::PipelineStatistics) || q.kind == kQueryKindPipelineStatistics1) {
        adjust(static_cast<uint32_t>(graphics::PipelineStatistic::IaVertices));
        adjust(static_cast<uint32_t>(graphics::PipelineStatistic::VsInvocations));
      }
      uint64_t out[kMaxStreamQueryValues] = {};
      ByteReader before(where);
      for (uint32_t k = 0; k < values; ++k) {
        const uint64_t there = before.le64();
        const uint64_t value = predicate ? (q.sums[k] ? 1 : 0) : q.sums[k];
        out[k] = !q.accumulate ? value : predicate ? ((there || value) ? 1 : 0) : there + value;
      }
      ByteWriter w(where);
      for (uint32_t k = 0; k < values; ++k) w.le64(out[k]);
      mm_.note_cpu_write(*q.allocation);
    }
    return true;
  }

  NtStatus resident(Allocation& a) { return mm_.make_resident(a); }

  void unbind_textures_of(const Allocation* a) {
    for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u)
      if (texture_bound_[u] && texture_alloc_[u] == a) texture_bound_[u] = false;
  }
  // Whether unit `u`'s view shows layers first .. first + count - 1 of level `level` of `a` (a pass that draws there may not sample them; one that draws into other levels or layers may).
  bool unit_sees(uint32_t u, const Allocation* a, uint32_t level, uint32_t first, uint32_t count) const {
    if (!texture_bound_[u] || texture_alloc_[u] != a) return false;
    const graphics::TextureView& v = texture_view_[u];
    if (level < v.base_level || level >= v.base_level + v.level_count) return false;
    if (a->texture_kind() == graphics::TextureKind::Texture3D) return true;
    return first < v.first_layer + v.layer_count && v.first_layer < first + count;
  }
  void unbind_textures_drawn(const Allocation* a, uint32_t level, uint32_t first, uint32_t count) {
    for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u)
      if (unit_sees(u, a, level, first, count)) texture_bound_[u] = false;
  }
  // Whether the open pass draws where unit `u`'s view of its image would show (bind_texture refuses such a view).
  bool drawn_in_pass(uint32_t u) const {
    for (uint32_t n = 0; n <= graphics::kMaxColorTargets; ++n) {
      const Allocation* t = n == graphics::kMaxColorTargets ? pass_depth_ : n == 0 ? pass_color_ : n < pass_target_count_ ? pass_extra_[n - 1] : nullptr;
      if (t && unit_sees(u, t, pass_level_[n], pass_first_layer_[n], pass_layer_count_[n])) return true;
    }
    return false;
  }
  // The textures a pipeline's shaders read (`mask`: bit u for unit u): bound, and through a view of the kind the shader declared (texture_view_fits; a multisampled image to a Texture2DMS
  // unit only). Replaying, a unit whose device binding is of another kind is bound again with the declared one.
  NtStatus fit_textures(const PipelineObject& p, uint32_t mask) {
    const bool views = (adapter_.caps_.features2 & graphics::kFeature2MipmappedLayers) != 0;
    for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) {
      if (!(mask >> u & 1)) continue;
      if (!texture_bound_[u]) return refuse(__LINE__);
      const Allocation* a = texture_alloc_[u];
      const graphics::TextureKind declared = p.texture_kind(u);
      graphics::TextureView view = texture_view_[u];
      if (a->desc().samples > 1) {
        if (declared != graphics::TextureKind::Texture2DMS) return refuse(__LINE__);
        continue;
      }
      view.kind = declared;
      // (unbound, or a volume where the shader reads a 2D texture, ...)
      if (views ? !graphics::texture_view_fits(a->texture_kind(), a->layers(), a->mip_levels(), view) : a->texture_kind() != declared) return refuse(__LINE__);
      if (!exec_ || !views || device_view_kind_[u] == declared) continue;
      const NtStatus s = ensure_cb();
      if (s != status::kSuccess) return s;
      cb_->bind_texture_view(u, a->image(), texture_sampler_[u], view);
      device_view_kind_[u] = declared;
    }
    return status::kSuccess;
  }

  NtStatus packet(Op op, ByteReader& p) {
    switch (op) {
      case Op::BeginPass: return begin_pass(p);
      case Op::EndPass: {
        if (!exact(p) || !in_pass_) return kBad;
        in_pass_ = false;
        if (!exec_) return status::kSuccess;
        cb_->end_render_pass();
        const NtStatus s = write_back_targets();
        return s == status::kSuccess ? write_back_images() : s;
      }
      case Op::BindUavImage: return bind_uav_image(p);
      case Op::BindTexelBuffer: return bind_texel_buffer(p);
      case Op::SetDepthBounds: return set_depth_bounds(p);
      case Op::SetSamplePositions: return set_sample_positions(p);
      case Op::SetViewInstance: return set_view_instance(p);
      case Op::SetShadingRate: return set_shading_rate(p);
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
      case Op::CopyBuffer: return copy_buffer(p);
      case Op::CopyBufferImage: return copy_buffer_image(p);
      case Op::CopyTiles: return copy_tiles(p);
      case Op::DecodeFrame: return decode_frame(p);
      case Op::VideoProcess: return video_process_packet(p);
      case Op::EstimateMotion: return estimate_motion_packet(p);
      case Op::ResolveMotionVectors: return resolve_motion_vectors_packet(p);
      case Op::EncodeFrame: return encode_frame_packet(p);
      case Op::ResolveEncoderMetadata: return resolve_encoder_metadata_packet(p);
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
    uint32_t word = 0;
    if (!p.at_end()) word = p.le32();
    if (!exact(p) || !(adapter_.caps_.features & graphics::kFeatureOcclusionQueries)) return kBad;
    // (the kind in the low byte, the output stream of a stream output query in bits 8-9, kQueryAccumulate; nothing else)
    const uint32_t kind = word & 0xFF, stream = word >> 8 & 0xFF;
    const bool accumulate = (word & kQueryAccumulate) != 0;
    if (word & ~(0xFFFFu | kQueryAccumulate)) return kBad;
    using graphics::QueryKind;
    const uint32_t features = adapter_.caps_.features;
    const bool so_kind = kind == static_cast<uint32_t>(QueryKind::SoPrimitivesWritten) || kind == static_cast<uint32_t>(QueryKind::SoPrimitivesNeeded) || kind == kQueryKindSoStatistics;
    const bool time_kind = kind == static_cast<uint32_t>(QueryKind::Timestamp) || kind == static_cast<uint32_t>(QueryKind::TimestampFrequency) || kind == static_cast<uint32_t>(QueryKind::TimestampDisjoint);
    if (kind > kQueryKindSoStatistics || stream >= (so_kind ? graphics::kMaxGeometryStreams : 1u)) return kBad;
    if (time_kind ? !(features & graphics::kFeatureTimestampQueries)
        : so_kind ? (features & (graphics::kFeatureStatisticsQueries | graphics::kFeatureStreamOutput)) != (graphics::kFeatureStatisticsQueries | graphics::kFeatureStreamOutput)
        : (kind != 0 && !(features & graphics::kFeatureStatisticsQueries))) return kBad;
    // (a time is not added to another)
    if (accumulate && (kind == static_cast<uint32_t>(QueryKind::Timestamp) || kind == static_cast<uint32_t>(QueryKind::TimestampFrequency))) return kBad;
    Allocation* a = alloc(index);
    const uint64_t bytes = uint64_t{query_values(kind)} * 8;
    if (!a || !a->is_query_result() || offset % 8 || bytes > a->desc().size || offset > a->desc().size - bytes) return kBad;
    // (no two open queries write the same bytes)
    for (const StreamQuery& q : queries_.view())
      if (q.open && q.allocation == a && offset < q.offset + uint64_t{query_values(q.kind)} * 8 && q.offset < offset + bytes) return kBad;
    if (query_slots_in_stream_ + query_device_slots(kind) > graphics::kMaxOcclusionQueries) return kBad;
    query_slots_in_stream_ += query_device_slots(kind);
    StreamQuery q;
    q.allocation = a;
    q.offset = offset;
    q.kind = kind;
    q.stream = stream;
    q.accumulate = accumulate;
    if (!exec_) return queries_.push_back(q) ? status::kSuccess : status::kNoMemory;
    const NtStatus s = ensure_cb();   // (before the query is in the list: ensure_cb begins segments of those already open)
    if (s != status::kSuccess) return s;
    if (!queries_.push_back(q)) return status::kNoMemory;
    begin_segment(queries_[queries_.count() - 1]);
    return status::kSuccess;
  }

  // EndQuery [allocation_index, offset]: the open query that writes there, or the one begun last that is still open.
  NtStatus end_query(ByteReader& p) {
    bool named = false;
    uint32_t index = 0, offset = 0;
    if (!p.at_end()) {
      index = p.le32(), offset = p.le32();
      named = true;
    }
    if (!exact(p)) return kBad;
    const Allocation* a = named ? alloc(index) : nullptr;
    if (named && !a) return kBad;
    uint64_t at = queries_.count();
    for (uint64_t k = 0; k < queries_.count(); ++k) {
      const StreamQuery& q = queries_[k];
      if (q.open && (!named || (q.allocation == a && q.offset == offset))) at = k;
    }
    if (at == queries_.count()) return kBad;
    StreamQuery& q = queries_[at];
    q.open = false;
    if (exec_ && cb_ && q.segment) end_segment(q);   // (without a command buffer since the last flush nothing more was counted)
    return status::kSuccess;
  }

  NtStatus begin_pass(ByteReader& p) {
    const uint32_t color_index = p.le32(), depth_index = p.le32(), flags = p.le32();
    graphics::Vec4 clear;
    clear.x = bits_float(p.le32()), clear.y = bits_float(p.le32()), clear.z = bits_float(p.le32()), clear.w = bits_float(p.le32());
    const float clear_depth = bits_float(p.le32());
    const uint32_t clear_stencil = p.le32();
    // Further colour targets, one word each (the adapter's kFeatureMultipleTargets), and with kPassLayers the first layer and count of each target and of the depth buffer after them.
    Allocation* extras[graphics::kMaxColorTargets - 1] = {};
    uint32_t extra_count = 0;
    // (with kPassLevels the level of each colour target and of the depth buffer after that: kFeature2MipmappedLayers)
    const bool layered = (flags & kPassLayers) != 0, leveled = (flags & kPassLevels) != 0;
    const uint64_t rest = p.ok() ? p.remaining() / 4 : 0;
    const uint64_t per_extra = 1 + (layered ? 2 : 0) + (leveled ? 1 : 0), fixed = (layered ? 4 : 0) + (leveled ? 2 : 0);
    if (rest < fixed || (rest - fixed) % per_extra) return kBad;
    const uint64_t extra_words = (rest - fixed) / per_extra;
    while (extra_count < extra_words && !p.at_end() && p.ok() && extra_count < adapter_.caps_.max_color_targets - 1) {
      extras[extra_count] = alloc(p.le32());
      if (!extras[extra_count]) return kBad;
      ++extra_count;
    }
    if (extra_count != extra_words) return kBad;
    uint32_t first_layer[graphics::kMaxColorTargets + 1] = {}, layer_count[graphics::kMaxColorTargets + 1] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    if (layered) {
      for (uint32_t n = 0; n <= extra_count; ++n) first_layer[n] = p.le32(), layer_count[n] = p.le32();
      first_layer[graphics::kMaxColorTargets] = p.le32(), layer_count[graphics::kMaxColorTargets] = p.le32();
      if (!(adapter_.caps_.features2 & graphics::kFeature2LayeredTargets)) return kBad;
    }
    uint32_t level[graphics::kMaxColorTargets + 1] = {};
    if (leveled) {
      for (uint32_t n = 0; n <= extra_count; ++n) level[n] = p.le32();
      level[graphics::kMaxColorTargets] = p.le32();
      if (!(adapter_.caps_.features2 & graphics::kFeature2MipmappedLayers)) return kBad;
    }
    if (!exact(p) || in_pass_) return kBad;
    if (extra_count && !(adapter_.caps_.features & graphics::kFeatureMultipleTargets)) return kBad;
    Allocation* color = alloc(color_index);
    Allocation* depth = depth_index == kNoAllocation ? nullptr : alloc(depth_index);
    if (!color || !color->is_image() || !(color->desc().flags & kAllocRenderTarget) || !use(color, state::kRenderTarget)) return kBad;
    if (depth_index != kNoAllocation && (!depth || !depth->is_image() || !(depth->desc().flags & kAllocDepthStencil))) return kBad;
    if (depth && !use(depth, state::kDepthWrite)) return kBad;
    // (every target's level - each a level it has - is of one size: the pass's)
    if (level[0] >= color->mip_levels() || (depth && level[graphics::kMaxColorTargets] >= depth->mip_levels())) return kBad;
    const uint32_t pass_w = graphics::mip_extent(color->desc().width, level[0]), pass_h = graphics::mip_extent(color->desc().height, level[0]);
    auto sized = [&](const Allocation* t, uint32_t l) { return graphics::mip_extent(t->desc().width, l) == pass_w && graphics::mip_extent(t->desc().height, l) == pass_h; };
    if (depth && (!sized(depth, level[graphics::kMaxColorTargets]) || depth->desc().samples != color->desc().samples)) return kBad;
    for (uint32_t n = 0; n < extra_count; ++n) {
      const Allocation* e = extras[n];
      if (!e->is_image() || !(e->desc().flags & kAllocRenderTarget) || e == color || e == depth || !use(extras[n], state::kRenderTarget)) return kBad;
      if (level[n + 1] >= e->mip_levels() || !sized(e, level[n + 1]) || e->desc().format != color->desc().format || e->desc().samples != color->desc().samples) return kBad;
      for (uint32_t k = 0; k < n; ++k)
        if (extras[k] == e) return kBad;
    }
    if (flags & ~(kPassClearColor | kPassClearDepth | kPassClearStencil | kPassLayers | kPassLevels)) return kBad;
    // (the layers of each target are layers its level has: a plain image's are layer 0 of 1, and so are those of the depth buffer of a pass without one)
    for (uint32_t n = 0; n <= graphics::kMaxColorTargets; ++n) {
      const Allocation* t = n == graphics::kMaxColorTargets ? depth : n == 0 ? color : n <= extra_count ? extras[n - 1] : nullptr;
      const uint32_t layers = t ? t->level_layers(level[n]) : 1;
      if (!t && level[n] != 0) return kBad;
      if (layer_count[n] == 0 || first_layer[n] >= layers || layer_count[n] > layers - first_layer[n]) return kBad;
    }
    // (the clear colour of an integer target is four integers as float bits: any bit pattern)
    if (!is_integer(color->desc().format) && (!finite_within(clear.x, 1e6f) || !finite_within(clear.y, 1e6f) || !finite_within(clear.z, 1e6f) || !finite_within(clear.w, 1e6f))) return kBad;
    if (!(clear_depth >= 0.0f && clear_depth <= 1.0f) || clear_stencil > 255) return kBad;
    // A render pass may not draw into an image it samples. A texture unit that still holds one of the targets from an earlier draw is let go of here (as a copy
    // into an image does): no draw of the pass can sample through it, since a draw needs its units bound (draw) and a target cannot be bound during the pass
    // (bind_texture). Refusing the stream instead refused ordinary render-to-texture in one command list - sample A, draw into B, then sample B, draw into A -
    // and made the layers above send a list before every such pass.
    // (a unit that shows only other levels or layers of a target than those the pass draws into stays: render into a level while sampling the one above it)
    unbind_textures_drawn(color, level[0], first_layer[0], layer_count[0]);
    if (depth) unbind_textures_drawn(depth, level[graphics::kMaxColorTargets], first_layer[graphics::kMaxColorTargets], layer_count[graphics::kMaxColorTargets]);
    for (uint32_t n = 0; n < extra_count; ++n) unbind_textures_drawn(extras[n], level[n + 1], first_layer[n + 1], layer_count[n + 1]);
    for (uint32_t n = 0; n <= graphics::kMaxColorTargets; ++n) pass_level_[n] = level[n], pass_first_layer_[n] = first_layer[n], pass_layer_count_[n] = layer_count[n];
    in_pass_ = true;
    pass_color_ = color;
    pass_depth_ = depth;
    pass_target_count_ = 1 + extra_count;
    for (uint32_t n = 0; n < graphics::kMaxColorTargets - 1; ++n) pass_extra_[n] = n < extra_count ? extras[n] : nullptr;
    if (!exec_) return status::kSuccess;

    // (a target that is a mirror - an integer view of a plane - starts with what its source has: the pass draws over it, and it goes back at the pass's end)
    NtStatus s = refresh_mirror(color, true);
    for (uint32_t n = 0; n < extra_count && s == status::kSuccess; ++n) s = refresh_mirror(extras[n], true);
    write_back_[0] = color->writes_back() ? color : nullptr;
    for (uint32_t n = 0; n + 1 < graphics::kMaxColorTargets; ++n) write_back_[n + 1] = n < extra_count && extras[n]->writes_back() ? extras[n] : nullptr;
    if (s == status::kSuccess) s = resident(*color);
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
    for (uint32_t n = 0; n <= graphics::kMaxColorTargets; ++n) pass.first_layer[n] = first_layer[n], pass.layer_count[n] = layer_count[n], pass.level[n] = level[n];
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
      bound_compute_ = true;
      if (exec_) {
        const NtStatus s = ensure_cb();
        if (s != status::kSuccess) return s;
        cb_->bind_compute_pipeline(compute_->pipeline());
      }
      return status::kSuccess;
    }
    pipeline_ = found.get();
    bound_compute_ = false;
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
    if (mode == kResolveCopySamples) return copy_samples(p, to_index, from_index);
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
    // (a colour image's least or greatest samples also with kFeature2ColorResolveModes)
    const bool colour_extreme = !depth && (mode == static_cast<uint32_t>(graphics::ResolveMode::Min) || mode == static_cast<uint32_t>(graphics::ResolveMode::Max));
    if (!plain && !(adapter_.caps_.features & graphics::kFeatureResolveModes) && !(colour_extreme && (adapter_.caps_.features2 & graphics::kFeature2ColorResolveModes))) return kBad;
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

  // ResolveRegion's copy of samples: a region of a multisampled image (colour, or depth with its stencil) into another of its format, samples and size kind (one level, one layer).
  NtStatus copy_samples(ByteReader& p, uint32_t to_index, uint32_t from_index) {
    const uint32_t sx = p.le32(), sy = p.le32(), w = p.le32(), h = p.le32(), dx = p.le32(), dy = p.le32();
    if (!exact(p) || in_pass_ || !(adapter_.caps_.features2 & graphics::kFeature2SampleCopies)) return refuse(__LINE__);
    Allocation* to = alloc(to_index);
    Allocation* from = alloc(from_index);
    if (!to || !from || to == from || !to->is_image() || !from->is_image() || to->is_staging() || from->is_staging() || !use(to, state::kResolveDest) || !use(from, state::kResolveSource)) return refuse(__LINE__);
    if (to->desc().format != from->desc().format || to->desc().samples != from->desc().samples || from->desc().samples < 2 || to->mip_levels() != 1 || from->mip_levels() != 1 ||
        to->layers() != 1 || from->layers() != 1)
      return refuse(__LINE__);
    if (w == 0 || h == 0 || sx > from->desc().width || w > from->desc().width - sx || sy > from->desc().height || h > from->desc().height - sy || dx > to->desc().width ||
        w > to->desc().width - dx || dy > to->desc().height || h > to->desc().height - dy)
      return refuse(__LINE__);
    unbind_textures_of(to);
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*to);
    if (s == status::kSuccess) s = resident(*from);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->copy_samples(to->image(), from->image(), {static_cast<int32_t>(sx), static_cast<int32_t>(sy), w, h}, dx, dy);
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
    const bool view = !graphics::storage_slot_writable(slot);   // (a shader resource view of a buffer: read-only, also by the shaders of a draw, and it may change between draws)
    const bool draws_write = (adapter_.caps_.features & (graphics::kFeatureFragmentStores | graphics::kFeatureVertexStores | graphics::kFeatureGeometryStores | graphics::kFeatureTessellationStores)) != 0;   // (pixel shaders write the unordered access ones: they are bound between draws too)
    if (!exact(p) || slot >= graphics::kMaxStorageBuffers || (in_pass_ && !view && !draws_write) || !(adapter_.caps_.features & (graphics::kFeatureCompute | (view ? graphics::kFeatureStorageInDraws : graphics::kFeatureFragmentStores | graphics::kFeatureVertexStores | graphics::kFeatureGeometryStores | graphics::kFeatureTessellationStores)))) return kBad;
    Allocation* a = alloc(index);
    const bool read_only = view;   // (the slots after the u# ones are shader resource views of buffers; after those the counters of the u# ones, written)
    if (!a || !(a->desc().flags & kAllocUnorderedAccess) || !use(a, read_only ? state::kNonPixelShaderResource | state::kPixelShaderResource : state::kUnorderedAccess) || size == 0 || offset % 4 || size % 4 || offset > a->desc().size || size > a->desc().size - offset) return kBad;
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
    if (const NtStatus bound = apply_root_arguments(true)) return bound;
    const uint32_t x = p.le32(), y = p.le32(), z = p.le32();
    if (!exact(p) || in_pass_ || !compute_) return kBad;
    if (x > graphics::kMaxDispatchGroups || y > graphics::kMaxDispatchGroups || z > graphics::kMaxDispatchGroups) return kBad;
    const uint64_t per_group = uint64_t{compute_->thread_group(0)} * compute_->thread_group(1) * compute_->thread_group(2);
    if (uint64_t{x} * y * z * per_group > graphics::kMaxDispatchThreads) return kBad;
    for (uint32_t slot = 0; slot < graphics::kMaxStorageBuffers; ++slot)
      if (compute_->storage_mask().test(slot) && !uav_bound_[slot]) return kBad;
    if (const NtStatus fitted = fit_textures(*compute_, compute_->texture_mask())) return fitted;   // (the textures the shader reads: bound, and of the kind it declared)
    if (!images_fit(*compute_)) return refuse(__LINE__);   // (the storage images it declares: bound, and of what it declared)
    if (both_planes(compute_->texture_mask())) return refuse(__LINE__);   // (a depth buffer's two planes at once)
    if (x == 0 || y == 0 || z == 0 || !exec_) return status::kSuccess;
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    align_planes(compute_->texture_mask(), graphics::kMaxTextureUnits);
    cb_->dispatch(x, y, z);
    for (uint32_t slot = 0; slot < graphics::kMaxStorageBuffers; ++slot)
      if (uav_bound_[slot] && graphics::storage_slot_writable(slot)) mm_.note_gpu_write(*uav_alloc_[slot]);
    note_images_written(*compute_);
    return write_back_images();
  }

  NtStatus bind_vertex_buffer(ByteReader& p) {
    const uint32_t index = p.le32(), offset = p.le32();
    uint32_t slot = 0;
    if (!p.at_end()) slot = p.le32();      // (a third word: the vertex buffer slot, below the device's DeviceLimits::vertex_buffers)
    if (!exact(p) || slot >= adapter_.caps_.max_vertex_buffers) return kBad;
    Allocation* a = alloc(index);
    if (!a || !a->is_buffer() || !(a->desc().flags & kAllocVertexBuffer) || offset > a->desc().size || !use(a, state::kVertexAndConstantBuffer)) return kBad;
    vertex_[slot] = a;
    vertex_offset_[slot] = offset;
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_vertex_buffer_slot(slot, a->buffer(), offset);
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
    const uint32_t view = p.ok() && !p.at_end() ? p.le32() : 0;   // (the optional tenth word: texture_view_word)
    // (optionally after it the view: the first level and the count of levels, the first layer and the count of layers, a count of 0 to the last; kFeature2MipmappedLayers)
    const bool has_view = p.ok() && !p.at_end();
    uint32_t base_level = 0, level_count = 0, first_layer = 0, layer_count = 0;
    if (has_view) base_level = p.le32(), level_count = p.le32(), first_layer = p.le32(), layer_count = p.le32();
    // (optionally after the view the sampler's part: anisotropy and a border colour of any value, kFeature2SamplerState - words 15 to 20)
    uint32_t anisotropy = 1, custom = 0;
    float border_rgba[4] = {0, 0, 0, 0};
    if (has_view && p.ok() && !p.at_end()) {
      anisotropy = p.le32(), custom = p.le32();
      for (float& c : border_rgba) c = bits_float(p.le32());
      if (!p.ok() || anisotropy < 1 || anisotropy > graphics::kMaxAnisotropy || custom > 1) return refuse(__LINE__);
      if (!(adapter_.caps_.features2 & graphics::kFeature2SamplerState)) return refuse(__LINE__);
    }
    graphics::AddressMode mode_u, mode_v;
    graphics::BorderColor border, no_border;
    graphics::CompareOp compare = graphics::CompareOp::Never;
    bool stencil = false;
    if (!exact(p) || unit >= adapter_.caps_.max_texture_units || filter > 1 || !decode_address_word(au, &mode_u, &border) || !decode_address_word(av, &mode_v, &no_border) || mip > 2 ||
        !decode_texture_view_word(view, &compare, &stencil))
      return refuse(__LINE__);
    if (!(adapter_.caps_.features & graphics::kFeatureAddressModes)) mode_u = base_address_mode(mode_u), mode_v = base_address_mode(mode_v);   // (the device has wrap and clamp alone)
    if (!finite_within(bias, 100.0f) || !finite_within(min_lod, 100.0f) || !finite_within(max_lod, 1000.0f)) return refuse(__LINE__);
    Allocation* a = alloc(index);
    if (!a || !a->is_image() || !(a->desc().flags & kAllocTexture) || a->is_staging() || !use(a, state::kPixelShaderResource | state::kNonPixelShaderResource)) return refuse(__LINE__);
    if (stencil && !(a->is_depth() && a->desc().format == Format::D24S8)) return refuse(__LINE__);   // (the stencil plane of a depth buffer that has one)
    // (the view: levels and layers the image has - all of them without one, or on a device that has no views)
    const bool views = (adapter_.caps_.features2 & graphics::kFeature2MipmappedLayers) != 0;
    if (level_count == 0 && base_level < a->mip_levels()) level_count = a->mip_levels() - base_level;
    if (layer_count == 0 && first_layer < a->layers()) layer_count = a->layers() - first_layer;
    if (base_level >= a->mip_levels() || level_count > a->mip_levels() - base_level || first_layer >= a->layers() || layer_count > a->layers() - first_layer) return refuse(__LINE__);
    if (a->texture_kind() == graphics::TextureKind::Texture3D && (first_layer != 0 || layer_count != a->layers())) return refuse(__LINE__);   // (a volume's view has all its slices)
    if (!views && (base_level != 0 || level_count != a->mip_levels() || first_layer != 0 || layer_count != a->layers())) return refuse(__LINE__);
    texture_bound_[unit] = true;
    texture_alloc_[unit] = a;
    texture_stencil_[unit] = stencil;
    texture_view_[unit] = graphics::TextureView{a->texture_kind(), base_level, level_count, first_layer, layer_count};
    if (in_pass_ && drawn_in_pass(unit)) {   // (a pass may not sample what it draws into; other levels and layers of its targets it may)
      texture_bound_[unit] = false;
      return refuse(__LINE__);
    }
    graphics::SamplerDesc sampler;
    sampler.filter = static_cast<graphics::Filter>(filter);
    sampler.address_u = mode_u;
    sampler.address_v = mode_v;
    sampler.border = border;
    sampler.mip_filter = static_cast<graphics::MipFilter>(mip);
    sampler.lod_bias = bias;
    sampler.min_lod = min_lod;
    sampler.max_lod = max_lod;
    sampler.compare = compare;
    sampler.stencil = stencil;
    sampler.max_anisotropy = static_cast<uint8_t>(anisotropy);
    sampler.custom_border = custom != 0;
    for (uint32_t c = 0; c < 4; ++c) sampler.border_rgba[c] = border_rgba[c];
    texture_sampler_[unit] = sampler;
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    if (views) {
      // (the image's own kind; a draw or dispatch binds it again of the kind its shader declares when that is another: fit_textures)
      graphics::TextureView whole = texture_view_[unit];
      if (a->desc().samples > 1) cb_->bind_texture(unit, a->image(), sampler);
      else cb_->bind_texture_view(unit, a->image(), sampler, whole);
      device_view_kind_[unit] = a->texture_kind();
    } else {
      cb_->bind_texture(unit, a->image(), sampler);
    }
    return status::kSuccess;
  }
  // Binds unit `u`'s image again on the device with `sampler`: through its view, of the kind the device's binding has, on a device with views (kFeature2MipmappedLayers).
  void rebind_unit(uint32_t u, const graphics::SamplerDesc& sampler) {
    const Allocation* a = texture_alloc_[u];
    if ((adapter_.caps_.features2 & graphics::kFeature2MipmappedLayers) && a->desc().samples <= 1) {
      graphics::TextureView view = texture_view_[u];
      view.kind = device_view_kind_[u];
      cb_->bind_texture_view(u, a->image(), sampler, view);
    } else {
      cb_->bind_texture(u, a->image(), sampler);
    }
  }
  // On a device whose textures read one plane at a time (no kFeature2DepthAndStencilPlanes), a unit the pipeline has but its shaders do not read that shows the other plane of a
  // depth buffer a read unit shows is made to show the read one (the device's texture takes the plane of the views bound to it).
  void align_planes(uint32_t declared, uint32_t units) {
    if (adapter_.caps_.features2 & graphics::kFeature2DepthAndStencilPlanes) return;
    for (uint32_t u = 0; u < units && u < graphics::kMaxTextureUnits; ++u) {
      if ((declared >> u & 1) || !texture_bound_[u] || !texture_alloc_[u]->is_depth()) continue;
      for (uint32_t v = 0; v < graphics::kMaxTextureUnits; ++v) {
        if (!(declared >> v & 1) || !texture_bound_[v] || texture_alloc_[v] != texture_alloc_[u] || texture_stencil_[v] == texture_stencil_[u]) continue;
        graphics::SamplerDesc sampler = texture_sampler_[u];
        sampler.stencil = texture_stencil_[v];
        rebind_unit(u, sampler);
        texture_stencil_[u] = sampler.stencil;
        texture_sampler_[u] = sampler;
        break;
      }
    }
  }

  // --- The optional rasterizer state of Direct3D 12 (depth bounds, sample positions, view instancing, coarse shading): kept for the stream, across passes and command buffers, and given
  // to the device's command buffer before the next draw (apply_raster).
  struct RasterState {
    bool depth_bounds = false;
    float depth_min = 0.0f, depth_max = 1.0f;
    uint32_t samples = 0, pixels = 0;
    graphics::SamplePosition positions[graphics::kMaxSamplePositions] = {};
    uint32_t view = 0, viewport_offset = 0, layer_offset = 0;
    uint32_t rate = 0;
    graphics::ShadingRateCombiner combiners[2] = {graphics::ShadingRateCombiner::Passthrough, graphics::ShadingRateCombiner::Passthrough};
    Allocation* rate_image = nullptr;
  };
  RasterState raster_;
  bool raster_sent_ = false;   // (the device's command buffer has raster_)
  bool raster_used_ = false;   // (the stream set any of it: a stream that did not leaves the device's defaults alone)

  NtStatus set_depth_bounds(ByteReader& p) {
    const uint32_t enable = p.le32();
    const float min = bits_float(p.le32()), max = bits_float(p.le32());
    if (!exact(p) || enable > 1 || !(min >= 0.0f && min <= 1.0f) || !(max >= 0.0f && max <= 1.0f)) return refuse(__LINE__);   // (NaN fails both)
    if (!(adapter_.caps_.features2 & graphics::kFeature2DepthBounds)) return refuse(__LINE__);
    raster_.depth_bounds = enable != 0, raster_.depth_min = min, raster_.depth_max = max;
    raster_used_ = true, raster_sent_ = false;
    return status::kSuccess;
  }
  NtStatus set_sample_positions(ByteReader& p) {
    const uint32_t samples = p.le32(), pixels = p.le32();
    if (!p.ok() || !(adapter_.caps_.features2 & graphics::kFeature2SamplePositions)) return refuse(__LINE__);
    RasterState next = raster_;
    if (pixels == 0) {
      if (!exact(p) || samples != 0) return refuse(__LINE__);
      next.samples = next.pixels = 0;
    } else {
      // (1, 2, 4, 8 or 16 samples - a count the adapter has, or one -, one pixel or a quad of four, at most 16 positions)
      if ((pixels != 1 && pixels != 4) || (samples != 1 && samples != 2 && samples != 4 && samples != 8 && samples != 16) || samples * pixels > graphics::kMaxSamplePositions) return refuse(__LINE__);
      if (samples > 1 && !(adapter_.caps_.sample_counts >> samples & 1)) return refuse(__LINE__);
      for (uint32_t k = 0; k < samples * pixels; ++k) {
        const uint32_t w = p.le32();
        const int8_t x = static_cast<int8_t>(w & 0xFF), y = static_cast<int8_t>(w >> 8 & 0xFF);
        if (!p.ok() || (w >> 16) != 0 || x < -8 || x > 7 || y < -8 || y > 7) return refuse(__LINE__);
        next.positions[k] = {x, y};
      }
      if (!exact(p)) return refuse(__LINE__);
      next.samples = samples, next.pixels = pixels;
    }
    raster_ = next;
    raster_used_ = true, raster_sent_ = false;
    return status::kSuccess;
  }
  NtStatus set_view_instance(ByteReader& p) {
    const uint32_t view = p.le32(), viewport = p.le32(), layer = p.le32();
    if (!exact(p) || view >= kMaxViewInstances || viewport >= graphics::kMaxViewports || layer >= 2048) return refuse(__LINE__);
    if (!(adapter_.caps_.features2 & graphics::kFeature2ViewInstancing)) return refuse(__LINE__);
    raster_.view = view, raster_.viewport_offset = viewport, raster_.layer_offset = layer;
    raster_used_ = true, raster_sent_ = false;
    return status::kSuccess;
  }
  NtStatus set_shading_rate(ByteReader& p) {
    const uint32_t rate = p.le32(), primitive = p.le32(), image = p.le32(), index = p.le32();
    const uint32_t last = static_cast<uint32_t>(graphics::ShadingRateCombiner::Sum);
    if (!exact(p) || !graphics::valid_shading_rate(rate) || primitive > last || image > last) return refuse(__LINE__);
    if (!(adapter_.caps_.features2 & graphics::kFeature2ShadingRate)) return refuse(__LINE__);
    Allocation* a = nullptr;
    if (index != kNoAllocation) {
      // (an R8_UINT texture of one level, one layer and one sample, which no shader of the draws writes: D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE)
      a = alloc(index);
      if (!a || !a->is_image() || a->is_staging() || a->is_depth() || a->desc().format != Format::R8Uint || a->desc().samples > 1 || a->mip_levels() != 1 || a->layers() != 1 ||
          !use(a, state::kShadingRateSource))
        return refuse(__LINE__);
    }
    raster_.rate = rate;
    raster_.combiners[0] = static_cast<graphics::ShadingRateCombiner>(primitive);
    raster_.combiners[1] = static_cast<graphics::ShadingRateCombiner>(image);
    raster_.rate_image = a;
    raster_used_ = true, raster_sent_ = false;
    return status::kSuccess;
  }
  // Before a draw: the device's command buffer gets the stream's rasterizer state if it does not have it (the shading-rate image made resident).
  NtStatus apply_raster() {
    if (!raster_used_ || raster_sent_) return status::kSuccess;
    if (raster_.rate_image) {
      const NtStatus s = resident(*raster_.rate_image);
      if (s != status::kSuccess) return s;
    }
    const uint32_t features2 = adapter_.caps_.features2;
    if (features2 & graphics::kFeature2DepthBounds) cb_->set_depth_bounds(raster_.depth_bounds, raster_.depth_min, raster_.depth_max);
    if (features2 & graphics::kFeature2SamplePositions) cb_->set_sample_positions(raster_.samples, raster_.pixels, ArrayView<const graphics::SamplePosition>(raster_.positions, raster_.samples * raster_.pixels));
    if (features2 & graphics::kFeature2ViewInstancing) cb_->set_view_instance(raster_.view, raster_.viewport_offset, raster_.layer_offset);
    if (features2 & graphics::kFeature2ShadingRate) {
      cb_->set_shading_rate(raster_.rate, raster_.combiners[0], raster_.combiners[1]);
      cb_->set_shading_rate_image(raster_.rate_image ? raster_.rate_image->image() : nullptr);
    }
    raster_sent_ = true;
    return status::kSuccess;
  }

  // One viewport, or several (kFeature2Viewports): six words each.
  NtStatus set_viewport(ByteReader& p) {
    graphics::Viewport list[graphics::kMaxViewports];
    uint32_t count = 0;
    while (p.ok() && !p.at_end()) {
      if (count == graphics::kMaxViewports) return kBad;
      graphics::Viewport& v = list[count++];
      v.x = bits_float(p.le32()), v.y = bits_float(p.le32()), v.width = bits_float(p.le32()), v.height = bits_float(p.le32());
      v.min_depth = bits_float(p.le32()), v.max_depth = bits_float(p.le32());
      if (!p.ok() || !finite_within(v.x, kMaxCoordinate) || !finite_within(v.y, kMaxCoordinate) || !finite_within(v.width, kMaxCoordinate) || !finite_within(v.height, kMaxCoordinate) ||
          v.width < 0 || v.height < 0 || !finite_within(v.min_depth, 1.0f) || !finite_within(v.max_depth, 1.0f))
        return kBad;
    }
    if (!exact(p) || count == 0) return kBad;
    if (count > 1 && !(adapter_.caps_.features2 & graphics::kFeature2Viewports)) return refuse(__LINE__);
    if (!exec_) return status::kSuccess;
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    if (count == 1) cb_->set_viewport(list[0]);
    else cb_->set_viewports(ArrayView<const graphics::Viewport>(list, count));
    return status::kSuccess;
  }

  // One scissor rectangle, or several (kFeature2Viewports): four words each.
  NtStatus set_scissor(ByteReader& p) {
    graphics::Rect2D list[graphics::kMaxViewports];
    uint32_t count = 0;
    while (p.ok() && !p.at_end()) {
      if (count == graphics::kMaxViewports) return kBad;
      const int32_t x = static_cast<int32_t>(p.le32()), y = static_cast<int32_t>(p.le32());
      const uint32_t w = p.le32(), h = p.le32();
      if (!p.ok() || x < -kMaxScissor || x > kMaxScissor || y < -kMaxScissor || y > kMaxScissor || w > static_cast<uint32_t>(kMaxScissor) || h > static_cast<uint32_t>(kMaxScissor)) return kBad;
      list[count++] = {x, y, w, h};
    }
    if (!exact(p) || count == 0) return kBad;
    if (count > 1 && !(adapter_.caps_.features2 & graphics::kFeature2Viewports)) return refuse(__LINE__);
    if (!exec_) return status::kSuccess;
    const NtStatus s = ensure_cb();
    if (s != status::kSuccess) return s;
    if (count == 1) cb_->set_scissor(list[0]);
    else cb_->set_scissors(ArrayView<const graphics::Rect2D>(list, count));
    return status::kSuccess;
  }

  // The reads a draw does must stay inside the buffers bound: the vertices [first, first + count) of the vertex buffer, and for an indexed draw the indices and (when
  // replaying, as the buffer can change after the stream was checked) the vertices they name.
  // A refusal that says where: the line is given to the trace of refused streams (set_stream_trace).
  NtStatus refuse(uint32_t line) {
    refused_line_ = line;
    return kBad;
  }
  uint32_t refused_line_ = 0;

  NtStatus draw(ByteReader& p, bool indexed, bool instanced) {
    if (const NtStatus bound = apply_root_arguments(false)) return bound;
    const uint32_t count = p.le32(), first = p.le32();
    const int32_t vertex_offset = indexed ? static_cast<int32_t>(p.le32()) : 0;
    uint32_t instance_count = 1, first_instance = 0;
    if (instanced) {
      instance_count = p.le32();
      first_instance = p.le32();
    }
    if (!exact(p) || !in_pass_ || !pipeline_) return refuse(__LINE__);
    if (instanced && (!(adapter_.caps_.features & graphics::kFeatureInstancing) || instance_count == 0 || instance_count > (1u << 20))) return refuse(__LINE__);
    if (topology_ >= Topology::LineList && topology_ <= Topology::PointList && !(adapter_.caps_.features & graphics::kFeatureLines)) return refuse(__LINE__);
    // Patches go with a hull and a domain shader that take that many control points, and those shaders go with patches (and with no geometry shader).
    if (is_patch_list(topology_) != pipeline_->has_tessellation()) return refuse(__LINE__);
    if (pipeline_->has_stream_output())   // (every buffer the declaration writes needs a target)
      for (uint32_t b = 0; b < graphics::kMaxStreamOutputBuffers; ++b)
        if ((pipeline_->stream_output_buffers() >> b & 1) && !so_bound_[b]) return refuse(__LINE__);
    if (is_adjacency(topology_) && !pipeline_->has_geometry()) return refuse(__LINE__);   // (only a geometry shader takes the neighbours)
    if (pipeline_->has_tessellation() && (patch_points(topology_) != pipeline_->tess_input_points() || pipeline_->has_geometry())) return refuse(__LINE__);
    if (const NtStatus fitted = fit_textures(*pipeline_, pipeline_->texture_count() >= 32 ? ~0u : (1u << pipeline_->texture_count()) - 1)) return fitted;   // (bound, and of the kinds the shaders declare)
    if (both_planes(pipeline_->declared_units())) return refuse(__LINE__);   // (a depth buffer's two planes at once, read by the shaders)
    if (pipeline_->color_target_count() > pass_target_count_) return refuse(__LINE__);
    for (uint32_t slot = 0; slot < graphics::kMaxStorageBuffers; ++slot)
      if (pipeline_->storage_mask().test(slot) && !uav_bound_[slot]) return refuse(__LINE__);   // (a buffer the shaders read must be bound)
    if (!images_fit(*pipeline_)) return refuse(__LINE__);   // (the storage images the shaders declare)
    if (pipeline_->blends()) {   // (an integer target takes no blending)
      if (is_integer(pass_color_->desc().format)) return refuse(__LINE__);
      for (uint32_t n = 0; n + 1 < pass_target_count_; ++n)
        if (pass_extra_[n] && is_integer(pass_extra_[n]->desc().format)) return refuse(__LINE__);
    }
    if (pipeline_->has_geometry()) {   // (the geometry shader takes the primitives the draw makes)
      const graphics::GeometryPrimitive made = topology_ == Topology::LineListAdjacency ? graphics::GeometryPrimitive::LineAdjacency
                                               : topology_ == Topology::TriangleListAdjacency ? graphics::GeometryPrimitive::TriangleAdjacency
                                               : topology_ == Topology::LineList || topology_ == Topology::LineStrip ? graphics::GeometryPrimitive::Line
                                               : topology_ == Topology::PointList ? graphics::GeometryPrimitive::Point : graphics::GeometryPrimitive::Triangle;
      if (made != pipeline_->geometry_input()) return refuse(__LINE__);
    }
    const bool vertices = pipeline_->attribute_count() > 0;
    // Every vertex buffer the attributes read is bound, and the vertices the draw reads (per vertex) and the instances (per instance) are inside it.
    for (uint32_t slot = 0; slot < graphics::kMaxVertexBuffers; ++slot) {
      if (!(pipeline_->vertex_slots() >> slot & 1)) continue;
      if (!vertex_[slot]) return refuse(__LINE__);
      const uint64_t stride = pipeline_->slot_stride(slot);
      if (pipeline_->per_instance_slots() >> slot & 1) {
        // (the last element an instance reads: the attributes that step slowest stop soonest; one that never steps reads first_instance)
        const uint32_t rate = pipeline_->slot_step_rate(slot);
        const uint64_t end = uint64_t{first_instance} + (rate ? (instance_count - 1) / rate : 0) + 1;
        if (vertex_offset_[slot] + end * stride > vertex_[slot]->desc().size) return refuse(__LINE__);
      }
      if (!indexed && (pipeline_->per_vertex_slots() >> slot & 1)) {
        const uint64_t end = uint64_t{first} + count;   // (times a stride of at most 2048: < 2^44)
        if (vertex_offset_[slot] + end * stride > vertex_[slot]->desc().size) return refuse(__LINE__);
      }
    }
    if (indexed) {
      if (!index_) return refuse(__LINE__);
      const uint64_t end = uint64_t{first} + count;   // (< 2^33, times a 4-byte index < 2^35)
      if (index_offset_ + end * index_bytes_ > index_->desc().size) return refuse(__LINE__);
    }
    if (count == 0) return status::kSuccess;
    if (!exec_) return status::kSuccess;

    if (indexed && vertices && pipeline_->per_vertex_slots()) {
      // The index values: every vertex the draw can reach must be inside the vertex buffers.
      ByteReader in(index_->backing());
      in.skip(index_offset_ + uint64_t{first} * index_bytes_);
      int64_t highest = -1;
      const uint32_t restart = index_bytes_ == 4 ? 0xFFFFFFFFu : 0xFFFFu;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t raw = index_bytes_ == 4 ? in.le32() : in.le16();
        if (topology_ != Topology::TriangleList && topology_ != Topology::LineList && topology_ != Topology::PointList && !is_patch_list(topology_) && raw == restart) continue;   // (ends a strip: names no vertex)
        const int64_t v = int64_t{raw} + vertex_offset;
        if (!in.ok() || v < 0) return refuse(__LINE__);
        if (v > highest) highest = v;
      }
      for (uint32_t slot = 0; slot < graphics::kMaxVertexBuffers; ++slot)
        if ((pipeline_->per_vertex_slots() >> slot & 1) && vertex_offset_[slot] + static_cast<uint64_t>(highest + 1) * pipeline_->slot_stride(slot) > vertex_[slot]->desc().size) return refuse(__LINE__);
    }
    NtStatus s = ensure_cb();
    if (s == status::kSuccess) s = apply_raster();
    if (s != status::kSuccess) return s;
    align_planes(pipeline_->declared_units(), pipeline_->texture_count());
    if (topology_ == Topology::TriangleStrip || topology_ == Topology::TriangleFan || topology_ == Topology::LineStrip) {
      const NtStatus expanded = draw_expanded(indexed, count, first, vertex_offset, instance_count, first_instance);
      if (expanded == status::kSuccess) note_storage_written();
      return expanded;
    }
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
    note_storage_written();
    return status::kSuccess;
  }

  // The unordered access buffers the pipeline's pixel shader may have written are newer on the device than in the system.
  void note_storage_written() {
    note_images_written(*pipeline_);   // (and the storage images the pixel shader declares)
    if (!pipeline_->writes_storage()) return;
    for (uint32_t slot = 0; slot < graphics::kMaxStorageBuffers; ++slot)
      if (graphics::storage_slot_writable(slot) && pipeline_->storage_mask().test(slot) && uav_bound_[slot]) mm_.note_gpu_write(*uav_alloc_[slot]);
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
    // (the input assembler of Direct3D reads the strip's `count` vertices, and its vertex shader runs once for each, where the device draws `emitted`: the open queries are told)
    const int64_t more = (int64_t{count} - static_cast<int64_t>(emitted)) * int64_t{instance_count};
    for (StreamQuery& q : queries_.view())
      if (q.open) q.vertex_adjust += more;
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
    // (optionally: the destination's level and first layer, the source's, and the number of layers - a volume's slices - from them)
    uint32_t dst_level = 0, dst_layer = 0, src_level = 0, src_layer = 0, layers = 1, planes = 0;
    if (p.ok() && !p.at_end()) dst_level = p.le32(), dst_layer = p.le32(), src_level = p.le32(), src_layer = p.le32(), layers = p.le32();
    if (p.ok() && !p.at_end()) planes = p.le32();   // (and after them the planes of a depth buffer)
    if (!exact(p) || in_pass_ || w == 0 || h == 0 || layers == 0 || planes > 3) return kBad;
    Allocation* dst = alloc(di);
    Allocation* src = alloc(si);
    if (!dst || !src || !dst->is_image() || !src->is_image() || dst->is_depth() != src->is_depth() || !use(dst, state::kCopyDest) || !use(src, state::kCopySource)) return kBad;
    if (dst->is_depth() || dst->desc().samples != 1 || src->desc().samples != 1)
      return copy_device_subresources(dst, dst_level, dst_layer, src, src_level, src_layer, layers, w, h, dx, dy, sx, sy, planes);
    if (planes) return kBad;   // (planes are a depth buffer's)
    // (formats whose pixels - or blocks - are the same bytes: one format, or two of one size and kind - what Direct3D's copies between the formats of one typeless group are)
    const Format df = dst->desc().format, sf = src->desc().format;
    if (df != sf && (is_compressed(df) != is_compressed(sf) || image_row_bytes(df, 4) != image_row_bytes(sf, 4))) return kBad;
    if (dst->desc().samples != 1 || src->desc().samples != 1) return kBad;
    if (dst_level >= dst->mip_levels() || src_level >= src->mip_levels()) return kBad;
    const uint32_t dw = graphics::mip_extent(dst->desc().width, dst_level), dh = graphics::mip_extent(dst->desc().height, dst_level);
    const uint32_t sw = graphics::mip_extent(src->desc().width, src_level), sh = graphics::mip_extent(src->desc().height, src_level);
    if (uint64_t{dx} + w > dw || uint64_t{dy} + h > dh || uint64_t{sx} + w > sw || uint64_t{sy} + h > sh) return kBad;
    if (dst_layer >= dst->level_layers(dst_level) || layers > dst->level_layers(dst_level) - dst_layer || src_layer >= src->level_layers(src_level) || layers > src->level_layers(src_level) - src_layer) return kBad;
    if (is_compressed(sf)) {   // (whole blocks: the rectangle starts on a block and ends on one or at the level's edge, in both)
      auto blocks = [](uint32_t x, uint32_t span, uint32_t edge) { return x % 4 == 0 && ((x + span) % 4 == 0 || x + span == edge); };
      if (!blocks(dx, w, dw) || !blocks(dy, h, dh) || !blocks(sx, w, sw) || !blocks(sy, h, sh)) return kBad;
    }
    const bool plain = dst_level == 0 && src_level == 0 && dst_layer == 0 && src_layer == 0 && layers == 1 && dst->mip_levels() == 1 && src->mip_levels() == 1 && !is_compressed(sf) &&
                       dst->layers() == 1 && src->layers() == 1;
    if (!plain && !(adapter_.caps_.features2 & graphics::kFeature2MipmappedLayers) && (is_compressed(sf) || dst->mip_levels() != 1 || src->mip_levels() != 1)) return kBad;
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s == status::kSuccess) s = copy_subresources_cpu(mm_, *dst, dst_level, dst_layer, dx, dy, *src, src_level, src_layer, sx, sy, w, h, layers);
    unbind_textures_of(dst);
    return s;
  }

  // CopyRegion of depth buffers and multisampled images, whose contents the device alone keeps (kFeature2DepthCopies): whole subresources - the rectangle is all of the destination's level
  // at 0, 0, and the source's level is that size - between images of one format and sample count, `layers` layers from each first one, the planes of a depth buffer asked for (0: all
  // it has).
  NtStatus copy_device_subresources(Allocation* dst, uint32_t dst_level, uint32_t dst_layer, Allocation* src, uint32_t src_level, uint32_t src_layer, uint32_t layers, uint32_t w, uint32_t h,
                                    uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t planes) {
    if (!(adapter_.caps_.features2 & graphics::kFeature2DepthCopies)) return kBad;
    const AllocationDesc &dd = dst->desc(), &sd = src->desc();
    if (dd.format != sd.format || dd.samples != sd.samples || dst->is_staging() || src->is_staging() || dst_level >= dst->mip_levels() || src_level >= src->mip_levels()) return kBad;
    const uint32_t width = graphics::mip_extent(dd.width, dst_level), height = graphics::mip_extent(dd.height, dst_level);
    if (dx || dy || sx || sy || w != width || h != height || graphics::mip_extent(sd.width, src_level) != width || graphics::mip_extent(sd.height, src_level) != height) return kBad;
    if (dst_layer >= dst->level_layers(dst_level) || layers > dst->level_layers(dst_level) - dst_layer || src_layer >= src->level_layers(src_level) || layers > src->level_layers(src_level) - src_layer)
      return kBad;
    // (the planes: a depth buffer's depth, and the stencil of one that has it; a colour image's one)
    const bool stencil = dd.format == Format::D24S8;
    if (planes == 0) planes = dst->is_depth() && stencil ? 3u : 1u;
    if ((planes & 2) && !stencil) return kBad;
    if (!dst->is_depth() && planes != 1) return kBad;
    if (dst == src && dst_level == src_level && dst_layer < src_layer + layers && src_layer < dst_layer + layers && dst_layer != src_layer) return kBad;   // (overlapping layers of one level)
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s == status::kSuccess) s = resident(*dst);
    if (s == status::kSuccess) s = resident(*src);
    if (s != status::kSuccess) return s;
    for (uint32_t k = 0; k < layers; ++k)
      if (!dst->image()->copy_subresource(*src->image(), dst_level, dst_layer + k, src_level, src_layer + k, planes)) return status::kInvalidDeviceState;
    mm_.note_gpu_write(*dst);
    unbind_textures_of(dst);
    return status::kSuccess;
  }

  // CopyBufferImage of a depth buffer of one sample (kFeature2DepthCopies): a rectangle of a level's layer, one plane - the depth as floats or 16- / 24-bit codes, or the stencil as bytes
  // (`layout`) -, rows `pitch` bytes apart in the buffer from `boff`. Into the image the rest of the plane, and the other plane, keep what they had.
  NtStatus copy_depth_buffer_image(uint32_t direction, Allocation* image, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h, Allocation* buffer, uint32_t boff,
                                   uint32_t pitch, NarrowLayout layout) {
    if (!(adapter_.caps_.features2 & graphics::kFeature2DepthCopies) || !is_depth_layout(layout)) return kBad;
    const bool stencil_plane = layout == NarrowLayout::Stencil8;
    if (stencil_plane && image->desc().format != Format::D24S8) return kBad;
    if (level >= image->mip_levels() || layer >= image->level_layers(level)) return kBad;
    const uint32_t level_w = graphics::mip_extent(image->desc().width, level), level_h = graphics::mip_extent(image->desc().height, level);
    if (uint64_t{x} + w > level_w || uint64_t{y} + h > level_h) return kBad;
    const uint32_t texel = narrow_pixel_bytes(layout);
    if (pitch < uint64_t{w} * texel || uint64_t{boff} + uint64_t{pitch} * (h - 1) + uint64_t{w} * texel > buffer->desc().size) return kBad;
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s == status::kSuccess) s = resident(*image);
    if (s != status::kSuccess) return s;
    if (!mm_.read_back(*buffer)) return status::kInvalidDeviceState;
    // (the whole plane of the subresource: read, changed in the rectangle - or copied out of it -, written back)
    const uint64_t count = uint64_t{level_w} * level_h;
    PlaneScratch scratch(adapter_.allocator_, stencil_plane ? 0 : count, stencil_plane ? count : 0);
    if (scratch.depth.size() + scratch.stencil.size() != count) return status::kNoMemory;
    graphics::IImage* device_image = image->image();
    if (!device_image->read_depth_subresource(level, layer, scratch.depth, scratch.stencil)) return status::kInvalidDeviceState;
    const bool to_image = direction == 0;
    ByteView bytes = buffer->writable_backing();
    for (uint32_t row = 0; row < h; ++row) {
      ByteView line;
      if (!bytes.try_subview(uint64_t{boff} + uint64_t{pitch} * row, uint64_t{w} * texel, &line)) return status::kInvalidDeviceState;
      for (uint32_t i = 0; i < w; ++i) {
        const uint64_t at = uint64_t{y + row} * level_w + x + i;
        ByteView cell = line.subview(uint64_t{i} * texel, texel);
        if (stencil_plane) {
          if (to_image) scratch.stencil[at] = cell[0];
          else cell[0] = scratch.stencil[at];
          continue;
        }
        if (to_image) {
          ByteReader in{ConstByteView(cell)};
          const uint32_t code = layout == NarrowLayout::DepthUnorm16 ? in.le16() : in.le32();
          scratch.depth[at] = layout == NarrowLayout::DepthFloat32 ? bits_float(code)
                              : layout == NarrowLayout::DepthUnorm16 ? static_cast<float>(code) / 65535.0f : static_cast<float>(static_cast<double>(code & 0xFFFFFFu) / 16777215.0);
        } else {
          ByteWriter out{cell};
          const float d = scratch.depth[at];
          const float v = !(d == d) ? 0.0f : d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
          if (layout == NarrowLayout::DepthFloat32) out.le32(__builtin_bit_cast(uint32_t, d));
          else if (layout == NarrowLayout::DepthUnorm16) out.le16(static_cast<uint16_t>(v * 65535.0f + 0.5f));
          else out.le32(static_cast<uint32_t>(static_cast<double>(v) * 16777215.0 + 0.5));   // (the high byte 0)
        }
      }
    }
    if (to_image) {
      if (!device_image->write_depth_subresource(level, layer, scratch.depth, scratch.stencil)) return status::kInvalidDeviceState;
      mm_.note_gpu_write(*image);
      unbind_textures_of(image);
    } else {
      mm_.note_cpu_write(*buffer);
    }
    return status::kSuccess;
  }
  // The planes of a depth subresource while a copy works on them (one of the two, the other empty), given back when it goes.
  struct PlaneScratch {
    PlaneScratch(allocators::IAllocator& a, uint64_t depth_count, uint64_t stencil_count) : allocator(a) {
      if (depth_count) depth = allocate_view<float>(a, depth_count);
      if (stencil_count) stencil = allocate_view<uint8_t>(a, stencil_count);
    }
    ~PlaneScratch() {
      release_view(allocator, depth);
      release_view(allocator, stencil);
    }
    PlaneScratch(const PlaneScratch&) = delete;
    PlaneScratch& operator=(const PlaneScratch&) = delete;
    allocators::IAllocator& allocator;
    ArrayView<float> depth;
    ArrayView<uint8_t> stencil;
  };

  // A plain buffer (not a heap, a reserved buffer or a query result): what CopyBuffer and CopyBufferImage move bytes of.
  // (the results of queries can be a source: Direct3D 12's ResolveQueryData)
  static bool copyable_buffer(const Allocation* a, bool allow_query = false) {
    return a && a->is_buffer() && !(a->desc().flags & kAllocHeap) && (allow_query || !(a->desc().flags & kAllocQueryResult));
  }

  NtStatus copy_buffer(ByteReader& p) {
    const uint32_t di = p.le32(), doff = p.le32(), si = p.le32(), soff = p.le32(), bytes = p.le32();
    if (!exact(p) || in_pass_) return kBad;
    Allocation* dst = alloc(di);
    Allocation* src = alloc(si);
    if (!copyable_buffer(dst) || !copyable_buffer(src, true) || !use(dst, state::kCopyDest) || !use(src, state::kCopySource)) return kBad;
    if (uint64_t{doff} + bytes > dst->desc().size || uint64_t{soff} + bytes > src->desc().size) return kBad;
    if (dst == src && doff < soff + uint64_t{bytes} && soff < doff + uint64_t{bytes}) return kBad;
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    if (!mm_.read_back(*src) || !mm_.read_back(*dst)) return status::kInvalidDeviceState;
    ByteView to;
    ConstByteView from;
    if (!dst->writable_backing().try_subview(doff, bytes, &to) || !src->backing().try_subview(soff, bytes, &from)) return status::kInvalidDeviceState;
    to.copy_from(from);
    mm_.note_cpu_write(*dst);
    return status::kSuccess;
  }

  // CopyTiles: the tiles' heap bytes to or from a plain buffer (VidMm::copy_tiles).
  NtStatus copy_tiles(ByteReader& p) {
    const uint32_t direction = p.le32(), ri = p.le32(), first = p.le32(), count = p.le32(), bi = p.le32(), offset = p.le32();
    if (!exact(p) || in_pass_ || direction > 1 || count == 0) return kBad;
    Allocation* reserved = alloc(ri);
    Allocation* buffer = alloc(bi);
    if (!reserved || !reserved->reserved() || !copyable_buffer(buffer) || buffer->reserved()) return kBad;
    if (!use(reserved, direction == 0 ? state::kCopyDest : state::kCopySource) || !use(buffer, direction == 0 ? state::kCopySource : state::kCopyDest)) return kBad;
    if (first > reserved->tile_count() || count > reserved->tile_count() - first || uint64_t{count} * kTileBytes > buffer->desc().size || offset > buffer->desc().size - uint64_t{count} * kTileBytes)
      return kBad;
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    if (!mm_.copy_tiles(*reserved, first, count, *buffer, offset, direction == 1)) return status::kInvalidDeviceState;
    if (direction == 0) unbind_textures_of(reserved);
    return status::kSuccess;
  }

  NtStatus copy_buffer_image(ByteReader& p) {
    const uint32_t direction = p.le32(), ii = p.le32(), level = p.le32(), layer = p.le32(), x = p.le32(), y = p.le32(), w = p.le32(), h = p.le32(), bi = p.le32(), boff = p.le32(), pitch = p.le32();
    const uint32_t narrow_word = p.at_end() ? 0 : p.le32();      // (the optional twelfth word)
    if (!exact(p) || in_pass_ || direction > 1 || w == 0 || h == 0 || narrow_word > static_cast<uint32_t>(NarrowLayout::Stencil8)) return kBad;
    const NarrowLayout narrow = static_cast<NarrowLayout>(narrow_word);
    Allocation* image = alloc(ii);
    Allocation* buffer = alloc(bi);
    if (!image || !image->is_image() || image->desc().samples != 1 || !copyable_buffer(buffer)) return kBad;
    if (!use(image, direction == 0 ? state::kCopyDest : state::kCopySource) || !use(buffer, direction == 0 ? state::kCopySource : state::kCopyDest)) return kBad;
    // (a depth buffer: a plane in one of the depth layouts, by the device; the depth layouts are a depth buffer's)
    if (image->is_depth()) return copy_depth_buffer_image(direction, image, level, layer, x, y, w, h, buffer, boff, pitch, narrow);
    if (is_depth_layout(narrow)) return kBad;
    // (a layer of a level: an array's slice or a volume's slice at that level)
    if (level >= image->mip_levels() || layer >= image->level_layers(level)) return kBad;
    const uint32_t level_w = graphics::mip_extent(image->desc().width, level);
    const uint32_t level_h = graphics::mip_extent(image->desc().height, level);
    // (a block-compressed image is copied in whole blocks: the rectangle starts on a block and ends on one or at the level's edge; its rows are rows of blocks)
    const Format format = image->desc().format;
    const uint32_t unit = is_compressed(format) ? 4 : 1;
    if (uint64_t{x} + w > level_w || uint64_t{y} + h > level_h || x % unit || y % unit || ((x + w) % unit && x + w != level_w) || ((y + h) % unit && y + h != level_h)) return kBad;
    // (a narrow buffer: of an 8-bit four-channel image, or of A16B16G16R16)
    if (narrow != NarrowLayout::None) {
      const bool eight = narrow == NarrowLayout::R8 || narrow == NarrowLayout::R8G8;
      if (eight ? (format != Format::A8B8G8R8 && format != Format::X8B8G8R8) : (format != Format::A16B16G16R16 && format != Format::A16B16G16R16Sint)) return kBad;
    }
    const uint64_t row_bytes = image_row_bytes(format, w), level_row_bytes = image_row_bytes(format, level_w), x_bytes = image_row_bytes(format, x);
    const uint64_t buffer_row_bytes = narrow == NarrowLayout::None ? row_bytes : uint64_t{w} * narrow_pixel_bytes(narrow);
    const uint32_t rows = image_rows(format, h), first_row = y / unit;
    if (pitch < buffer_row_bytes) return kBad;
    if (uint64_t{boff} + uint64_t{pitch} * (rows - 1) + buffer_row_bytes > buffer->desc().size) return kBad;
    uint64_t level_offset = 0, level_bytes = 0;
    if (!image->subresource_range(level, layer, &level_offset, &level_bytes)) return kBad;
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    if (!mm_.read_back(*image) || !mm_.read_back(*buffer)) return status::kInvalidDeviceState;
    for (uint32_t row = 0; row < rows; ++row) {
      const uint64_t image_at = level_offset + uint64_t{first_row + row} * level_row_bytes + x_bytes;
      const uint64_t buffer_at = uint64_t{boff} + uint64_t{pitch} * row;
      ByteView to;
      ConstByteView from;
      if (narrow != NarrowLayout::None) {
        ByteView image_row, buffer_row;
        if (!image->writable_backing().try_subview(image_at, row_bytes, &image_row) || !buffer->writable_backing().try_subview(buffer_at, buffer_row_bytes, &buffer_row))
          return status::kInvalidDeviceState;
        copy_narrow_row(direction == 0, narrow, w, image_row, buffer_row, format == Format::A16B16G16R16Sint);
        continue;
      }
      if (direction == 0) {
        if (!image->writable_backing().try_subview(image_at, row_bytes, &to) || !buffer->backing().try_subview(buffer_at, row_bytes, &from)) return status::kInvalidDeviceState;
      } else {
        if (!buffer->writable_backing().try_subview(buffer_at, row_bytes, &to) || !image->backing().try_subview(image_at, row_bytes, &from)) return status::kInvalidDeviceState;
      }
      to.copy_from(from);
    }
    if (direction == 0) {
      mm_.note_cpu_write(*image);
      unbind_textures_of(image);
    } else {
      mm_.note_cpu_write(*buffer);
    }
    return status::kSuccess;
  }

  // One row of a narrow copy: `to_image` widens the buffer's pixels into the image's (missing channels 0, the last the maximum), else the image's first channels go into the buffer.
  // (`integer`: the image is of an integer format, whose missing alpha is 1)
  static void copy_narrow_row(bool to_image, NarrowLayout narrow, uint32_t width, ByteView image_row, ByteView buffer_row, bool integer = false) {
    const uint32_t channels = narrow == NarrowLayout::R8 || narrow == NarrowLayout::R16 ? 1 : 2;
    const bool sixteen = narrow == NarrowLayout::R16 || narrow == NarrowLayout::R16G16;
    const uint32_t sample = sixteen ? 2 : 1, image_pixel = 4 * sample, buffer_pixel = channels * sample;
    for (uint32_t i = 0; i < width; ++i) {
      ByteView pixel = image_row.subview(uint64_t{i} * image_pixel, image_pixel);
      ByteView narrow_pixel = buffer_row.subview(uint64_t{i} * buffer_pixel, buffer_pixel);
      if (to_image) {
        pixel.fill(0);
        pixel.subview(0, buffer_pixel).copy_from(narrow_pixel);
        if (integer) {
          pixel.subview(3 * sample, sample).fill(0);
          pixel[3 * sample] = 1;
        } else {
          pixel.subview(3 * sample, sample).fill(0xFF);      // (alpha: the maximum)
        }
      } else {
        narrow_pixel.copy_from(pixel.subview(0, buffer_pixel));
      }
    }
  }

  // A plane the decoder made, written into an image the runtime keeps it in (DxgTypes.hpp VideoDecoderDesc): every sample of a channel becomes a channel of a pixel, the others 0 and
  // the last the maximum. `channels` 1 (luma) or 2 (chroma: U, V interleaved); `sixteen` samples of 16 bits. Only what both have is written.
  // (`field` 1 / 2: only the lines of the top / bottom field - a decoded field picture; the image keeps its other lines)
  static bool write_video_plane(VidMm& mm, Allocation& image, const uint8_t* plane, uint32_t stride, uint32_t width, uint32_t height, uint32_t channels, bool sixteen,
                                uint32_t field = 0) {
    if (!plane) return false;
    const uint32_t w = width < image.desc().width ? width : image.desc().width, h = height < image.desc().height ? height : image.desc().height;
    const uint32_t sample = sixteen ? 2 : 1, pixel = 4 * sample;
    const uint64_t pitch = image_row_bytes(image.desc().format, image.desc().width);
    if (!mm.read_back(image)) return false;
    // (the decoder's plane as a view of what it says: stride bytes a row, h rows)
    if (uint64_t{w} * channels * sample > stride) return false;
    const ConstByteView source(plane, uint64_t{stride} * (h - 1) + uint64_t{w} * channels * sample);
    for (uint32_t y = field ? field - 1 : 0; y < h; y += field ? 2 : 1) {
      ByteView row;
      if (!image.writable_backing().try_subview(pitch * y, uint64_t{w} * pixel, &row)) return false;
      const ConstByteView from = source.subview(uint64_t{stride} * y, uint64_t{w} * channels * sample);
      for (uint32_t x = 0; x < w; ++x) {
        ByteView to = row.subview(uint64_t{x} * pixel, pixel);
        to.fill(0);
        to.subview(0, channels * sample).copy_from(from.subview(uint64_t{x} * channels * sample, channels * sample));
        to.subview(3 * sample, sample).fill(0xFF);
      }
    }
    mm.note_cpu_write(image);
    return true;
  }

  // A packed surface's rows (DxvaDecodedPicture::frame.plane[0]: dxva_packed_row_bytes each) copied into the image it is kept in, whose pixels are the same bytes (DxgTypes.hpp
  // VideoDecoderDesc); what is outside either is left alone.
  static bool write_packed_surface(VidMm& mm, Allocation& image, const uint8_t* plane, uint32_t stride, uint64_t row_bytes, uint32_t height) {
    if (!plane || row_bytes > stride) return false;
    const uint64_t pitch = image_row_bytes(image.desc().format, image.desc().width);
    const uint64_t bytes = row_bytes < pitch ? row_bytes : pitch;
    const uint32_t h = height < image.desc().height ? height : image.desc().height;
    if (h == 0) return true;
    if (!mm.read_back(image)) return false;
    const ConstByteView source(plane, uint64_t{stride} * (h - 1) + row_bytes);
    for (uint32_t y = 0; y < h; ++y) {
      ByteView row;
      if (!image.writable_backing().try_subview(pitch * y, bytes, &row)) return false;
      row.copy_from(source.subview(uint64_t{stride} * y, bytes));
    }
    mm.note_cpu_write(image);
    return true;
  }

  // The video processor's format of a decoder's surface format, and a colour space it takes for it (one histograms are counted in: they do not depend on it).
  static hardware::VpFormat vp_format_of_surface(hardware::DxvaSurfaceFormat f) {
    using S = hardware::DxvaSurfaceFormat;
    switch (f) {
      case S::Nv12: return hardware::VpFormat::Nv12;
      case S::P010: return hardware::VpFormat::P010;
      case S::P016: return hardware::VpFormat::P016;
      case S::Ayuv: return hardware::VpFormat::Ayuv;
      case S::Y410: return hardware::VpFormat::Y410;
      case S::Y416: return hardware::VpFormat::Y416;
      case S::Yuy2: return hardware::VpFormat::Yuy2;
      case S::Y210: return hardware::VpFormat::Y210;
      case S::Y216: return hardware::VpFormat::Y216;
    }
    return hardware::VpFormat::Nv12;
  }
  static hardware::VpColorSpace video_default_space(hardware::VpFormat f) { return hardware::vp_is_yuv(f) ? hardware::kVpYcbcrStudioG22LeftP709 : hardware::kVpRgbFullG22P709; }

  NtStatus decode_frame(ByteReader& p) {
    const Handle handle = p.le32();
    const uint32_t bi = p.le32(), pp_off = p.le32(), pp_bytes = p.le32(), qm_off = p.le32(), qm_bytes = p.le32(), sc_off = p.le32(), sc_bytes = p.le32();
    const uint32_t si = p.le32(), bs_off = p.le32(), bs_bytes = p.le32(), li = p.le32(), ci = p.le32(), qi = p.le32(), q_off = p.le32(), output_keyed = p.le32(), key_count = p.le32();
    if (!p.ok() || in_pass_ || key_count > kMaxVideoDecodeSurfaces || output_keyed > 1) return refuse(__LINE__);
    uint32_t key_index[kMaxVideoDecodeSurfaces];
    for (uint32_t i = 0; i < key_count; ++i) key_index[i] = p.le32();
    // the optional parts: a conversion of the decoded picture into another surface, its histograms
    bool convert = false, histogram = false;
    uint32_t cv[7] = {}, hg[3 + 8] = {};
    while (p.ok() && !p.at_end()) {
      const uint32_t magic = p.le32();
      if (magic == kDecodeConversion && !convert && !histogram) {
        convert = true;
        for (uint32_t& v : cv) v = p.le32();
      } else if (magic == kDecodeHistogram && !histogram) {
        histogram = true;
        for (uint32_t& v : hg) v = p.le32();
      } else {
        return refuse(__LINE__);
      }
    }
    if (!exact(p)) return refuse(__LINE__);
    SharedPointer<VideoDecoderObject> decoder;
    if (exec_) {
      if (replay_decoder_cursor_ >= replay_decoders_.size()) return refuse(__LINE__);
      decoder = replay_decoders_[replay_decoder_cursor_++];
    } else {
      decoder = lookup_->find_video_decoder(handle);
      if (!decoder) return status::kInvalidHandle;
      if (!collected_decoders_ || !collected_decoders_->push_back(decoder)) return status::kNoMemory;
    }
    const VideoDecoderDesc& desc = decoder->desc();
    Allocation* buffer = alloc(bi);
    Allocation* bitstream = alloc(si);
    const hardware::DxvaSurfaceFormat surface = static_cast<hardware::DxvaSurfaceFormat>(desc.surface_format);
    const bool packed = hardware::dxva_surface_packed(surface);
    Allocation* luma = alloc(li);
    Allocation* chroma = packed ? nullptr : alloc(ci);
    if (packed && ci != kNoAllocation) return refuse(__LINE__);
    Allocation* status_buffer = qi == kNoAllocation ? nullptr : alloc(qi);
    if (!copyable_buffer(buffer) || !copyable_buffer(bitstream) || (qi != kNoAllocation && !copyable_buffer(status_buffer, true))) return refuse(__LINE__);
    if (uint64_t{pp_off} + pp_bytes > buffer->desc().size || uint64_t{qm_off} + qm_bytes > buffer->desc().size || uint64_t{sc_off} + sc_bytes > buffer->desc().size ||
        uint64_t{bs_off} + bs_bytes > bitstream->desc().size || (status_buffer && uint64_t{q_off} + sizeof(VideoDecodeStatus) > status_buffer->desc().size))
      return refuse(__LINE__);
    // (the images the surface format is kept in: 8-bit samples in A8B8G8R8 / X8B8G8R8 pixels, 16-bit in A16B16G16R16, Y410 in A2B10G10R10)
    using S = hardware::DxvaSurfaceFormat;
    const bool eight = surface == S::Nv12 || surface == S::Ayuv || surface == S::Yuy2;
    const Format plane_format = eight ? Format::A8B8G8R8 : surface == S::Y410 ? Format::A2B10G10R10 : Format::A16B16G16R16;
    auto plane_ok = [&](const Allocation* a, uint32_t w, uint32_t h) {
      return a && a->is_image() && a->desc().kind == graphics::TextureKind::Texture2D && a->mip_levels() == 1 && a->desc().samples == 1 && a->desc().width >= w && a->desc().height >= h &&
             (a->desc().format == plane_format || (eight && a->desc().format == Format::X8B8G8R8));
    };
    const bool pairs = surface == S::Yuy2 || surface == S::Y210 || surface == S::Y216;
    if (packed) {
      if (!plane_ok(luma, pairs ? (desc.width + 1) / 2 : desc.width, desc.height)) return refuse(__LINE__);
    } else if (!plane_ok(luma, desc.width, desc.height) || !plane_ok(chroma, (desc.width + 1) / 2, (desc.height + 1) / 2) || luma == chroma) {
      return refuse(__LINE__);
    }
    if (!use(luma, state::kVideoDecodeWrite) || (chroma && !use(chroma, state::kVideoDecodeWrite)) || !use(buffer, state::kVideoDecodeRead) || !use(bitstream, state::kVideoDecodeRead))
      return refuse(__LINE__);
    for (uint32_t i = 0; i < key_count; ++i)
      if (key_index[i] != kNoAllocation && !alloc(key_index[i])) return refuse(__LINE__);
    const hardware::VpFormat decoded_format = vp_format_of_surface(surface);
    Allocation* conv0 = nullptr;
    Allocation* conv1 = nullptr;
    hardware::VpFormat conv_format = hardware::VpFormat::Nv12;
    if (convert) {
      if (cv[0] > static_cast<uint32_t>(hardware::kVpLastFormat) || cv[5] == 0 || cv[6] == 0 || cv[5] > 16384 || cv[6] > 16384 ||
          !hardware::vp_color_space_supported(decoded_format, cv[2]))
        return refuse(__LINE__);
      conv_format = static_cast<hardware::VpFormat>(cv[0]);
      conv0 = alloc(cv[3]);
      conv1 = hardware::vp_plane_count(conv_format) == 2 ? alloc(cv[4]) : nullptr;
      hardware::VpImage probe;
      if (!video_image_of(conv_format, cv[1], conv0, conv1, &probe) || !hardware::vp_color_space_supported(conv_format, cv[1])) return refuse(__LINE__);
      if (conv0 == luma || conv0 == chroma || (conv1 && (conv1 == luma || conv1 == chroma))) return refuse(__LINE__);   // (the reference is not its own output)
      if (!use(conv0, state::kVideoDecodeWrite) || (conv1 && !use(conv1, state::kVideoDecodeWrite))) return refuse(__LINE__);
    }
    Allocation* histogram_buffers[4] = {};
    if (histogram) {
      const uint32_t components = hg[0], bins = hg[1];
      if ((components & ~hardware::vp_histogram_components(decoded_format)) || bins == 0 || bins > 65536 || hg[2] != 32) return refuse(__LINE__);
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t bi_c = hg[3 + 2 * c], off = hg[4 + 2 * c];
        if (!(components & (1u << c)) || bi_c == kNoAllocation) continue;
        Allocation* b = alloc(bi_c);
        if (!copyable_buffer(b) || !buffer_range(b, off, uint64_t{bins} * 4) || !use(b, state::kVideoDecodeWrite)) return refuse(__LINE__);
        histogram_buffers[c] = b;
      }
    }
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    if (!mm_.read_back(*buffer) || !mm_.read_back(*bitstream)) return status::kInvalidDeviceState;
    // (a key is the allocation's number: the same allocation from picture to picture, whatever index the program gives it)
    uint32_t keys[kMaxVideoDecodeSurfaces];
    for (uint32_t i = 0; i < key_count; ++i) keys[i] = key_index[i] == kNoAllocation ? 0xFFFFFFFFu : static_cast<uint32_t>(alloc(key_index[i])->id());
    hardware::DxvaPicture picture;
    picture.picture_parameters = buffer->backing().subview(pp_off, pp_bytes);
    picture.quantization_matrix = buffer->backing().subview(qm_off, qm_bytes);
    picture.slice_control = buffer->backing().subview(sc_off, sc_bytes);
    picture.bitstream = bitstream->backing().subview(bs_off, bs_bytes);
    picture.surface_keys = ArrayView<const uint32_t>(keys, key_count);
    if (output_keyed) picture.output_key = static_cast<uint32_t>(luma->id());
    hardware::DxvaDecodedPicture decoded;
    const hardware::DxvaResult result = decoder->decoder().decode(picture, &decoded);
    VideoDecodeStatus record;
    if (result == hardware::DxvaResult::Ok || result == hardware::DxvaResult::MissingReference) {
      const hardware::VideoFrame& f = decoded.frame;
      bool written = decoded.surface_format == surface;
      if (written && packed) {
        written = write_packed_surface(mm_, *luma, f.plane[0], f.stride[0], hardware::dxva_packed_row_bytes(surface, f.format.width), f.format.height);
      } else if (written) {
        // (an H.264 field picture writes its field's lines only: DxvaDecodedPicture::field)
        const bool sixteen = f.format.pixel_format == hardware::PixelFormat::P010;
        const uint32_t field = decoded.field <= 2 ? decoded.field : 0;
        written = sixteen == (surface != S::Nv12) && write_video_plane(mm_, *luma, f.plane[0], f.stride[0], f.format.width, f.format.height, 1, sixteen, field) &&
                  write_video_plane(mm_, *chroma, f.plane[1], f.stride[1], f.format.chroma_width(), f.format.chroma_height(), 2, sixteen, field);
      }
      if (!written) return status::kInvalidDeviceState;
      unbind_textures_of(luma);
      if (chroma) unbind_textures_of(chroma);
      if (convert || histogram) {
        hardware::VpImage picture;
        if (!video_image_of(decoded_format, convert ? cv[2] : video_default_space(decoded_format), luma, chroma, &picture)) return status::kInvalidDeviceState;
        if (convert) {
          if (!mm_.read_back(*conv0) || (conv1 && !mm_.read_back(*conv1))) return status::kInvalidDeviceState;
          hardware::VpImage target;
          if (!video_image_of(conv_format, cv[1], conv0, conv1, &target)) return status::kInvalidDeviceState;
          hardware::VpOutput output;
          output.image = &target;
          output.target = {0, 0, static_cast<int32_t>(cv[5]), static_cast<int32_t>(cv[6])};
          output.allocator = &adapter_.allocator_;
          hardware::VpStream stream;
          stream.image = &picture;
          stream.source = {0, 0, static_cast<int32_t>(desc.width), static_cast<int32_t>(desc.height)};
          stream.destination = output.target;
          if (hardware::video_process(output, ArrayView<const hardware::VpStream>(&stream, 1)) == hardware::VpResult::Ok) {
            mm_.note_cpu_write(*conv0);
            unbind_textures_of(conv0);
            if (conv1) { mm_.note_cpu_write(*conv1); unbind_textures_of(conv1); }
          }
        }
        if (histogram) {
          const uint32_t bins = hg[1];
          ArrayView<uint32_t> counts[4];
          bool ok = true;
          for (uint32_t c = 0; c < 4; ++c)
            if (histogram_buffers[c]) {
              counts[c] = allocate_view<uint32_t>(adapter_.allocator_, bins);
              ok = ok && !counts[c].empty();
            }
          uint32_t asked = 0;
          for (uint32_t c = 0; c < 4; ++c)
            if (histogram_buffers[c]) asked |= 1u << c;
          ok = ok && hardware::vp_histogram(picture, desc.width, desc.height, asked, counts) == hardware::VpResult::Ok;
          for (uint32_t c = 0; ok && c < 4; ++c) {
            Allocation* b = histogram_buffers[c];
            if (!b) continue;
            ByteView to;
            if (!mm_.read_back(*b) || !b->writable_backing().try_subview(hg[4 + 2 * c], uint64_t{bins} * 4, &to)) { ok = false; break; }
            ByteWriter writer(to);
            for (uint32_t v : counts[c]) writer.le32(v);
            mm_.note_cpu_write(*b);
          }
          for (ArrayView<uint32_t>& v : counts)
            if (!v.empty()) release_view(adapter_.allocator_, v);
          if (!ok) return status::kNoMemory;
        }
      }
      ++decoder->pictures;
      if (result == hardware::DxvaResult::MissingReference) { record.status = 1; record.macroblocks_affected = ~uint64_t{0}; }
    } else {
      ++decoder->failures;
      record.status = 2;
      record.macroblocks_affected = ~uint64_t{0};
    }
    if (status_buffer) {
      if (!mm_.read_back(*status_buffer)) return status::kInvalidDeviceState;
      ByteView to;
      if (!status_buffer->writable_backing().try_subview(q_off, sizeof record, &to)) return status::kInvalidDeviceState;
      ByteWriter w(to);
      w.le64(record.status);
      w.le64(record.macroblocks_affected);
      mm_.note_cpu_write(*status_buffer);
    }
    return status::kSuccess;
  }

  // A picture of the video processor's (hardware::VpImage) over the images it is kept in: a YUV one two planes of widened pixels, an RGB one one image. False when the
  // allocations are not what `format` is kept as.
  static bool video_image_of(hardware::VpFormat format, hardware::VpColorSpace space, Allocation* plane0, Allocation* plane1, hardware::VpImage* out) {
    if (!plane0 || !plane0->is_image() || plane0->desc().kind != graphics::TextureKind::Texture2D || plane0->mip_levels() != 1 || plane0->desc().samples != 1) return false;
    out->format = format;
    out->color_space = space;
    out->width = plane0->desc().width;
    out->height = plane0->desc().height;
    auto plane_of = [&](Allocation* a, hardware::VpPlane* p, uint32_t channels, uint32_t sample_bytes, std::initializer_list<uint8_t> offsets, uint32_t bits, bool half) {
      p->memory = a->writable_backing();      // (an input's is only read: the processor takes one kind of view for both)
      p->pitch = static_cast<uint32_t>(image_row_bytes(a->desc().format, a->desc().width));
      p->step = sample_bytes;
      p->width = a->desc().width;
      p->height = a->desc().height;
      uint32_t c = 0;
      for (uint8_t o : offsets) p->offset[c++] = o;
      p->channels = static_cast<uint8_t>(channels);
      p->bits = static_cast<uint8_t>(bits);
      p->half_float = half;
    };
    const Format f0 = plane0->desc().format;
    switch (format) {
      case hardware::VpFormat::Nv12:
      case hardware::VpFormat::P010:
      case hardware::VpFormat::P016: {
        const bool sixteen = format != hardware::VpFormat::Nv12;
        const Format want = sixteen ? Format::A16B16G16R16 : Format::A8B8G8R8;
        if (!plane1 || plane1 == plane0 || !plane1->is_image() || plane1->desc().kind != graphics::TextureKind::Texture2D || plane1->mip_levels() != 1 || plane1->desc().samples != 1) return false;
        if ((f0 != want && !(f0 == Format::X8B8G8R8 && !sixteen)) || (plane1->desc().format != want && !(plane1->desc().format == Format::X8B8G8R8 && !sixteen))) return false;
        if (plane1->desc().width < (out->width + 1) / 2 || plane1->desc().height < (out->height + 1) / 2) return false;
        plane_of(plane0, &out->plane[0], 1, sixteen ? 8 : 4, {0}, sixteen ? 16 : 8, false);
        plane_of(plane1, &out->plane[1], 2, sixteen ? 8 : 4, {0, static_cast<uint8_t>(sixteen ? 2 : 1)}, sixteen ? 16 : 8, false);
        return true;
      }
      case hardware::VpFormat::Bgra8: if (f0 != Format::A8R8G8B8) return false; plane_of(plane0, &out->plane[0], 4, 4, {2, 1, 0, 3}, 8, false); return true;
      case hardware::VpFormat::Bgrx8: if (f0 != Format::X8R8G8B8 && f0 != Format::A8R8G8B8) return false; plane_of(plane0, &out->plane[0], 3, 4, {2, 1, 0, 3}, 8, false); return true;
      case hardware::VpFormat::Rgba8: if (f0 != Format::A8B8G8R8 && f0 != Format::X8B8G8R8) return false; plane_of(plane0, &out->plane[0], f0 == Format::X8B8G8R8 ? 3 : 4, 4, {0, 1, 2, 3}, 8, false); return true;
      case hardware::VpFormat::Rgb10a2: if (f0 != Format::A2B10G10R10) return false; plane_of(plane0, &out->plane[0], 4, 4, {0, 1, 2, 3}, 10, false); return true;
      case hardware::VpFormat::Rgba16f: if (f0 != Format::A16B16G16R16F) return false; plane_of(plane0, &out->plane[0], 4, 8, {0, 2, 4, 6}, 16, true); return true;
      case hardware::VpFormat::P208: {
        // (NV12's widened planes, the chroma as high as the luma)
        if (!plane1 || plane1 == plane0 || !plane1->is_image() || plane1->desc().kind != graphics::TextureKind::Texture2D || plane1->mip_levels() != 1 || plane1->desc().samples != 1) return false;
        if ((f0 != Format::A8B8G8R8 && f0 != Format::X8B8G8R8) || (plane1->desc().format != Format::A8B8G8R8 && plane1->desc().format != Format::X8B8G8R8)) return false;
        if (plane1->desc().width < (out->width + 1) / 2 || plane1->desc().height < out->height) return false;
        plane_of(plane0, &out->plane[0], 1, 4, {0}, 8, false);
        plane_of(plane1, &out->plane[1], 2, 4, {0, 1}, 8, false);
        return true;
      }
      // The packed YUV formats: one image of their own pixels (VideoProcess.hpp VpPlane: channels Y, U, V, A - for 4:2:2 Y0, U, Y1, V of a pair of pixels).
      case hardware::VpFormat::Ayuv: if (f0 != Format::A8B8G8R8) return false; plane_of(plane0, &out->plane[0], 4, 4, {2, 1, 0, 3}, 8, false); return true;
      case hardware::VpFormat::Y410: if (f0 != Format::A2B10G10R10) return false; plane_of(plane0, &out->plane[0], 4, 4, {1, 0, 2, 3}, 10, false); return true;
      case hardware::VpFormat::Y416: if (f0 != Format::A16B16G16R16) return false; plane_of(plane0, &out->plane[0], 4, 8, {2, 0, 4, 6}, 16, false); return true;
      case hardware::VpFormat::Yuy2:
      case hardware::VpFormat::Y210:
      case hardware::VpFormat::Y216: {
        const bool deep = format != hardware::VpFormat::Yuy2;
        if (f0 != (deep ? Format::A16B16G16R16 : Format::A8B8G8R8)) return false;
        plane_of(plane0, &out->plane[0], 4, deep ? 8 : 4, deep ? std::initializer_list<uint8_t>{0, 2, 4, 6} : std::initializer_list<uint8_t>{0, 1, 2, 3}, deep ? 16 : 8, false);
        out->width = 2 * plane0->desc().width;      // (the picture is two pixels a pair: an odd width is the runtime's to clip by its rectangles)
        return true;
      }
    }
    return false;
  }

  NtStatus video_process_packet(ByteReader& p) {
    const uint32_t out_format = p.le32(), out_space = p.le32();
    hardware::VpOutput output;
    output.target.left = static_cast<int32_t>(p.le32());
    output.target.top = static_cast<int32_t>(p.le32());
    output.target.right = static_cast<int32_t>(p.le32());
    output.target.bottom = static_cast<int32_t>(p.le32());
    for (float& c : output.background) c = bits_float(p.le32());
    output.background_is_yuv = p.le32() != 0;
    const uint32_t fill = p.le32();
    output.alpha_fill_stream = p.le32();
    const uint32_t o0 = p.le32(), o1 = p.le32(), count = p.le32();
    if (!p.ok() || in_pass_ || out_format > static_cast<uint32_t>(hardware::kVpLastFormat) || fill > static_cast<uint32_t>(hardware::VpAlphaFill::SourceStream) || count > kMaxVideoProcessStreams)
      return refuse(__LINE__);
    output.alpha_fill = static_cast<hardware::VpAlphaFill>(fill);
    struct StreamWords { uint32_t w[kVideoProcessStreamWords]; } words[kMaxVideoProcessStreams];
    for (uint32_t s = 0; s < count; ++s)
      for (uint32_t& w : words[s].w) w = p.le32();
    // the optional part: the streams' interlacing, stereo, filters, luma keys and light levels, the display's light levels
    struct ExtWords { uint32_t w[kVideoProcessStreamExtWords]; } ext[kMaxVideoProcessStreams] = {};
    struct Ext2Words { uint32_t w[kVideoProcessStreamExt2Words]; } ext2[kMaxVideoProcessStreams] = {};
    const bool extended = p.ok() && !p.at_end();
    bool extended2 = false;
    if (extended) {
      const uint32_t magic = p.le32();
      if (magic != kVideoProcessExtension && magic != kVideoProcessExtension2) return refuse(__LINE__);
      extended2 = magic == kVideoProcessExtension2;
      output.hdr.max_mastering = bits_float(p.le32());
      output.hdr.min_mastering = bits_float(p.le32());
      output.hdr.max_content = bits_float(p.le32());
      output.hdr.max_frame_average = bits_float(p.le32());
      for (uint32_t s = 0; s < count; ++s) {
        for (uint32_t& w : ext[s].w) w = p.le32();
        if (extended2)
          for (uint32_t& w : ext2[s].w) w = p.le32();
      }
    }
    if (!exact(p)) return refuse(__LINE__);
    output.allocator = &adapter_.allocator_;
    const hardware::VpFormat ofmt = static_cast<hardware::VpFormat>(out_format);
    Allocation* out0 = alloc(o0);
    Allocation* out1 = hardware::vp_plane_count(ofmt) == 2 ? alloc(o1) : nullptr;
    hardware::VpImage out_image;
    if (!video_image_of(ofmt, out_space, out0, out1, &out_image)) return refuse(__LINE__);
    if (!use(out0, state::kVideoProcessWrite) || (out1 && !use(out1, state::kVideoProcessWrite))) return refuse(__LINE__);
    hardware::VpImage images[kMaxVideoProcessStreams], references[2 * kMaxVideoProcessStreams], windows[hardware::kVpMaxTelecineFrames * kMaxVideoProcessStreams];
    hardware::VpStream streams[kMaxVideoProcessStreams];
    constexpr uint32_t kInputsPerStream = 6 + 2 * hardware::kVpMaxTelecineFrames;
    Allocation* inputs[kInputsPerStream * kMaxVideoProcessStreams] = {};
    for (uint32_t s = 0; s < count; ++s) {
      const uint32_t* w = words[s].w;
      if (w[0] > static_cast<uint32_t>(hardware::kVpLastFormat) || w[10] > static_cast<uint32_t>(hardware::VpOrientation::Clockwise270FlipHorizontal)) return refuse(__LINE__);
      const hardware::VpFormat f = static_cast<hardware::VpFormat>(w[0]);
      Allocation* a0 = alloc(w[13]);
      Allocation* a1 = hardware::vp_plane_count(f) == 2 ? alloc(w[14]) : nullptr;
      if (!video_image_of(f, w[1], a0, a1, &images[s])) return refuse(__LINE__);
      if (a0 == out0 || a0 == out1 || (a1 && (a1 == out0 || a1 == out1))) return refuse(__LINE__);      // (a stream is not its own output)
      if (!use(a0, state::kVideoProcessRead) || (a1 && !use(a1, state::kVideoProcessRead))) return refuse(__LINE__);
      inputs[kInputsPerStream * s] = a0;
      inputs[kInputsPerStream * s + 1] = a1;
      streams[s].image = &images[s];
      streams[s].source = {static_cast<int32_t>(w[2]), static_cast<int32_t>(w[3]), static_cast<int32_t>(w[4]), static_cast<int32_t>(w[5])};
      streams[s].destination = {static_cast<int32_t>(w[6]), static_cast<int32_t>(w[7]), static_cast<int32_t>(w[8]), static_cast<int32_t>(w[9])};
      streams[s].orientation = static_cast<hardware::VpOrientation>(w[10]);
      streams[s].alpha_blend = w[11] != 0;
      streams[s].alpha = bits_float(w[12]);
      if (!extended) continue;
      const uint32_t* e = ext[s].w;
      hardware::VpStream& st = streams[s];
      if (e[0] > static_cast<uint32_t>(hardware::VpFieldOrder::BottomFieldFirst) || e[1] > 1 || e[2] > static_cast<uint32_t>(hardware::VpDeinterlace::MotionCompensated) ||
          e[7] > static_cast<uint32_t>(hardware::VpStereoFormat::MonoOffset) || e[8] > 1 || e[9] > 1 || e[18] > 1)
        return refuse(__LINE__);
      st.field_order = static_cast<hardware::VpFieldOrder>(e[0]);
      st.field = static_cast<uint8_t>(e[1]);
      st.deinterlace = static_cast<hardware::VpDeinterlace>(e[2]);
      for (uint32_t k = 0; k < 2; ++k) {   // the frames before and after: two planes each (the second kNoAllocation for RGB), pictures like the stream's
        const uint32_t i0 = e[3 + 2 * k], i1 = e[4 + 2 * k];
        if (i0 == kNoAllocation) continue;
        Allocation* r0 = alloc(i0);
        Allocation* r1 = hardware::vp_plane_count(f) == 2 ? alloc(i1) : nullptr;
        hardware::VpImage& ri = references[2 * s + k];
        if (!video_image_of(f, w[1], r0, r1, &ri) || ri.width != images[s].width || ri.height != images[s].height) return refuse(__LINE__);
        if (r0 == out0 || r0 == out1 || (r1 && (r1 == out0 || r1 == out1))) return refuse(__LINE__);
        if (!use(r0, state::kVideoProcessRead) || (r1 && !use(r1, state::kVideoProcessRead))) return refuse(__LINE__);
        inputs[kInputsPerStream * s + 2 + 2 * k] = r0;
        inputs[kInputsPerStream * s + 3 + 2 * k] = r1;
        (k ? st.future : st.past) = &ri;
      }
      st.stereo = static_cast<hardware::VpStereoFormat>(e[7]);
      st.stereo_frame = static_cast<uint8_t>(e[8]);
      st.stereo_flip = e[9] != 0;
      st.stereo_offset = static_cast<int32_t>(e[10]);
      st.brightness = bits_float(e[11]);
      st.contrast = bits_float(e[12]);
      st.hue = bits_float(e[13]);
      st.saturation = bits_float(e[14]);
      st.noise_reduction = bits_float(e[15]);
      st.edge_enhancement = bits_float(e[16]);
      st.anamorphic = bits_float(e[17]);
      st.luma_key = e[18] != 0;
      st.luma_lower = bits_float(e[19]);
      st.luma_upper = bits_float(e[20]);
      st.hdr.max_mastering = bits_float(e[21]);
      st.hdr.min_mastering = bits_float(e[22]);
      st.hdr.max_content = bits_float(e[23]);
      st.hdr.max_frame_average = bits_float(e[24]);
      if (!extended2) continue;
      // the second part: frame rate conversion, inverse telecine, the stereo adjustment, automatic processing, HDR10+ metadata
      const uint32_t* x = ext2[s].w;
      if (x[0] > static_cast<uint32_t>(hardware::VpTemporal::InverseTelecine) || x[2] > 255 || x[3] > 255 || x[4] > 255 || x[5] > hardware::kVpMaxTelecineFrames ||
          x[24] > hardware::kVpHdr10PlusMaxBytes)
        return refuse(__LINE__);
      st.temporal = static_cast<hardware::VpTemporal>(x[0]);
      st.blend = bits_float(x[1]);
      st.ivtc_cycle = static_cast<uint8_t>(x[2]);
      st.ivtc_drop = static_cast<uint8_t>(x[3]);
      st.ivtc_output = static_cast<uint8_t>(x[4]);
      for (uint32_t k = 0; k < x[5]; ++k) {   // (the inverse telecine window: pictures like the stream's)
        const uint32_t i0 = x[6 + 2 * k], i1 = x[7 + 2 * k];
        if (i0 == kNoAllocation) continue;
        Allocation* r0 = alloc(i0);
        Allocation* r1 = hardware::vp_plane_count(f) == 2 ? alloc(i1) : nullptr;
        hardware::VpImage& wi = windows[hardware::kVpMaxTelecineFrames * s + k];
        if (!video_image_of(f, w[1], r0, r1, &wi) || wi.width != images[s].width || wi.height != images[s].height) return refuse(__LINE__);
        if (r0 == out0 || r0 == out1 || (r1 && (r1 == out0 || r1 == out1))) return refuse(__LINE__);
        if (!use(r0, state::kVideoProcessRead) || (r1 && !use(r1, state::kVideoProcessRead))) return refuse(__LINE__);
        inputs[kInputsPerStream * s + 6 + 2 * k] = r0;
        inputs[kInputsPerStream * s + 7 + 2 * k] = r1;
        st.telecine[k] = &wi;
      }
      st.stereo_adjust = static_cast<int32_t>(x[22]);
      st.auto_processing = x[23];
      uint8_t bytes[hardware::kVpHdr10PlusMaxBytes] = {};
      for (uint32_t k = 0; k < x[24]; ++k) bytes[k] = static_cast<uint8_t>(x[25 + k / 4] >> (8 * (k % 4)));
      // (metadata that is not HDR10+ is not used: the static levels tone map the picture, as with none)
      if (x[24] && !hardware::vp_parse_hdr10plus(ConstByteView(bytes, x[24]), &st.hdr10plus)) st.hdr10plus = hardware::VpHdr10Plus{};
    }
    if (!exec_) return status::kSuccess;
    NtStatus st = flush();
    if (st != status::kSuccess) return st;
    for (Allocation* a : inputs)
      if (a && !mm_.read_back(*a)) return status::kInvalidDeviceState;
    if (!mm_.read_back(*out0) || (out1 && !mm_.read_back(*out1))) return status::kInvalidDeviceState;
    // (the backings were read back above: the views of them are current; the output's are written)
    if (!video_image_of(ofmt, out_space, out0, out1, &out_image)) return status::kInvalidDeviceState;
    output.image = &out_image;
    if (hardware::video_process(output, ArrayView<const hardware::VpStream>(streams, count)) == hardware::VpResult::Ok) {
      mm_.note_cpu_write(*out0);
      unbind_textures_of(out0);
      if (out1) { mm_.note_cpu_write(*out1); unbind_textures_of(out1); }
    }
    return status::kSuccess;
  }

  // --- Video encoding and motion estimation (VideoEncode.hpp) -----------------------------------------------------------------------------------------------------

  // Whether the allocation is an 8-bit widened plane (what a runtime keeps NV12's planes in), as the encoder reads it.
  static bool plane_image(Allocation* a) {
    return a && a->is_image() && a->desc().kind == graphics::TextureKind::Texture2D && a->mip_levels() == 1 && a->desc().samples == 1 && a->layers() == 1 &&
           (a->desc().format == Format::A8B8G8R8 || a->desc().format == Format::X8B8G8R8);
  }
  // (EncodeFrame also takes the 16-bit form P010 is kept in - A16B16G16R16 -: the HEVC and AV1 paths check the pixel size against the bit depth, VideoEncodeCodecs.hpp)
  static bool encode_plane_image(Allocation* a) {
    return plane_image(a) || (a && a->is_image() && a->desc().kind == graphics::TextureKind::Texture2D && a->mip_levels() == 1 && a->desc().samples == 1 &&
                              a->layers() == 1 && a->desc().format == Format::A16B16G16R16);
  }
  static VideoPlaneImage read_view(Allocation* a) {
    return {a->backing(), image_row_bytes(a->desc().format, a->desc().width), a->desc().width, a->desc().height};
  }
  static VideoWritablePlaneImage write_view(Allocation* a) {
    return {a->writable_backing(), image_row_bytes(a->desc().format, a->desc().width), a->desc().width, a->desc().height};
  }
  // A buffer range: `offset` and `bytes` inside the buffer (bytes ~0: to the end).
  static bool buffer_range(Allocation* a, uint32_t offset, uint64_t bytes) {
    return copyable_buffer(a) && offset <= a->desc().size && (bytes == ~uint64_t{0} || bytes <= a->desc().size - offset);
  }

  NtStatus estimate_motion_packet(ByteReader& p) {
    const uint32_t block = p.le32(), width = p.le32(), height = p.le32(), ci = p.le32(), ri = p.le32(), hi = p.le32(), h_off = p.le32(), mi = p.le32(), m_off = p.le32();
    if (!exact(p) || in_pass_ || (block != 8 && block != 16) || width == 0 || height == 0 || width > 16384 || height > 16384) return refuse(__LINE__);
    Allocation* cur = alloc(ci);
    Allocation* ref = alloc(ri);
    Allocation* hint = hi == kNoAllocation ? nullptr : alloc(hi);
    Allocation* heap = alloc(mi);
    if (!plane_image(cur) || !plane_image(ref) || cur->desc().width < width || cur->desc().height < height || ref->desc().width < width || ref->desc().height < height)
      return refuse(__LINE__);
    const uint64_t need = motion_vector_heap_bytes(width, height, block);
    if (!buffer_range(heap, m_off, need) || (hi != kNoAllocation && (!buffer_range(hint, h_off, need) || hint == heap))) return refuse(__LINE__);
    if (!use(cur, state::kVideoEncodeRead) || !use(ref, state::kVideoEncodeRead)) return refuse(__LINE__);
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    if (!mm_.read_back(*cur) || !mm_.read_back(*ref) || !mm_.read_back(*heap) || (hint && !mm_.read_back(*hint))) return status::kInvalidDeviceState;
    const ConstByteView hints = hint ? hint->backing().subview(h_off, need) : ConstByteView();
    if (!run_motion_estimation(adapter_.allocator_, read_view(cur), read_view(ref), width, height, block, hints, heap->writable_backing().subview(m_off, need)))
      return status::kNoMemory;
    mm_.note_cpu_write(*heap);
    return status::kSuccess;
  }

  NtStatus resolve_motion_vectors_packet(ByteReader& p) {
    const uint32_t mi = p.le32(), m_off = p.le32(), width = p.le32(), height = p.le32(), ii = p.le32(), x = p.le32(), y = p.le32();
    if (!exact(p) || in_pass_ || width == 0 || height == 0 || width > 16384 || height > 16384) return refuse(__LINE__);
    Allocation* heap = alloc(mi);
    Allocation* image = alloc(ii);
    if (!buffer_range(heap, m_off, kMotionVectorHeapHeader) || !image || !image->is_image() || image->desc().kind != graphics::TextureKind::Texture2D || image->mip_levels() != 1 ||
        image->layers() != 1 || image->desc().samples != 1 || (image->desc().format != Format::A16B16G16R16Sint && image->desc().format != Format::R16G16Sint))
      return refuse(__LINE__);
    if (!use(image, state::kVideoEncodeWrite)) return refuse(__LINE__);
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    if (!mm_.read_back(*heap) || !mm_.read_back(*image)) return status::kInvalidDeviceState;
    VideoWritablePlaneImage out = {image->writable_backing(), image_row_bytes(image->desc().format, image->desc().width), image->desc().width, image->desc().height};
    // (a heap that does not hold vectors of that size, or that do not fit at (x, y), leaves the image as it was)
    if (resolve_motion_vectors(heap->backing().subview(m_off, heap->desc().size - m_off), width, height, out, x, y, pixel_bytes(image->desc().format))) {
      mm_.note_cpu_write(*image);
      unbind_textures_of(image);
    }
    return status::kSuccess;
  }

  NtStatus encode_frame_packet(ByteReader& p) {
    const uint32_t ai = p.le32(), a_off = p.le32(), a_bytes = p.le32(), hi = p.le32(), h_off = p.le32(), il = p.le32(), ic = p.le32(), rl = p.le32(), rc = p.le32();
    const uint32_t bi = p.le32(), b_off = p.le32(), mi = p.le32(), m_off = p.le32(), count = p.le32();
    if (!p.ok() || in_pass_ || count > kVideoEncodeMaxReferences) return refuse(__LINE__);
    uint32_t ref_index[2 * kVideoEncodeMaxReferences];
    for (uint32_t i = 0; i < 2 * count; ++i) ref_index[i] = p.le32();
    if (!exact(p)) return refuse(__LINE__);
    Allocation* args = alloc(ai);
    Allocation* heap = hi == kNoAllocation ? nullptr : alloc(hi);
    Allocation* in_l = alloc(il);
    Allocation* in_c = alloc(ic);
    Allocation* rec_l = rl == kNoAllocation ? nullptr : alloc(rl);
    Allocation* rec_c = rc == kNoAllocation ? nullptr : alloc(rc);
    Allocation* bits = alloc(bi);
    Allocation* meta = alloc(mi);
    if (!buffer_range(args, a_off, a_bytes) || (hi != kNoAllocation && !buffer_range(heap, h_off, kVideoEncodeHeapBytes)) || !buffer_range(bits, b_off, ~uint64_t{0}) ||
        !buffer_range(meta, m_off, ~uint64_t{0}))
      return refuse(__LINE__);
    if (!encode_plane_image(in_l) || !encode_plane_image(in_c) || in_l == in_c || (rl == kNoAllocation) != (rc == kNoAllocation) || (rec_l && (!encode_plane_image(rec_l) || !encode_plane_image(rec_c) || rec_l == rec_c)))
      return refuse(__LINE__);
    if (bits == meta || (rec_l && (rec_l == in_l || rec_l == in_c || rec_c == in_l || rec_c == in_c))) return refuse(__LINE__);
    if (!use(in_l, state::kVideoEncodeRead) || !use(in_c, state::kVideoEncodeRead) || !use(bits, state::kVideoEncodeWrite) || !use(meta, state::kVideoEncodeWrite) ||
        (rec_l && (!use(rec_l, state::kVideoEncodeWrite) || !use(rec_c, state::kVideoEncodeWrite))))
      return refuse(__LINE__);
    Allocation* refs[2 * kVideoEncodeMaxReferences] = {};
    for (uint32_t i = 0; i < 2 * count; ++i) {
      refs[i] = alloc(ref_index[i]);
      // (a reference may be the reconstruction's own texture - a program passes its whole buffer of pictures -: the references are read before the reconstruction is written)
      if (!encode_plane_image(refs[i]) || !use(refs[i], state::kVideoEncodeRead)) return refuse(__LINE__);
    }
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    for (Allocation* a : {args, heap, in_l, in_c, rec_l, rec_c, bits, meta})
      if (a && !mm_.read_back(*a)) return status::kInvalidDeviceState;
    for (uint32_t i = 0; i < 2 * count; ++i)
      if (!mm_.read_back(*refs[i])) return status::kInvalidDeviceState;
    VideoPictureImages references[kVideoEncodeMaxReferences];
    for (uint32_t i = 0; i < count; ++i) references[i] = {read_view(refs[2 * i]), read_view(refs[2 * i + 1])};
    VideoEncodeJob job;
    job.params = args->backing().subview(a_off, a_bytes);
    if (heap) job.heap = heap->writable_backing().subview(h_off, heap->desc().size - h_off);   // (the rate controller's state, then H.264's reference store)
    job.input = {read_view(in_l), read_view(in_c)};
    job.has_reconstruction = rec_l != nullptr;
    if (rec_l) {
      job.reconstruction_luma = write_view(rec_l);
      job.reconstruction_chroma = write_view(rec_c);
    }
    job.references = ArrayView<const VideoPictureImages>(references, count);
    uint64_t reference_keys[kVideoEncodeMaxReferences];
    for (uint32_t i = 0; i < count; ++i) reference_keys[i] = refs[2 * i]->id();
    job.reference_keys = ArrayView<const uint64_t>(reference_keys, count);
    if (rec_l) job.reconstruction_key = rec_l->id();
    job.bitstream = bits->writable_backing().subview(b_off, bits->desc().size - b_off);
    job.metadata = meta->writable_backing().subview(m_off, meta->desc().size - m_off);
    if (!run_video_encode(adapter_.allocator_, job)) return status::kNoMemory;
    for (Allocation* a : {heap, bits, meta, rec_l, rec_c})
      if (a) mm_.note_cpu_write(*a);
    if (rec_l) {
      unbind_textures_of(rec_l);
      unbind_textures_of(rec_c);
    }
    return status::kSuccess;
  }

  NtStatus resolve_encoder_metadata_packet(ByteReader& p) {
    const uint32_t mi = p.le32(), m_off = p.le32(), oi = p.le32(), o_off = p.le32();
    // (then optionally the frame PSNR and subregion PSNR buffers: index and offset each, kNoAllocation for none)
    uint32_t fi = kNoAllocation, f_off = 0, si = kNoAllocation, s_off = 0;
    if (p.remaining() == 16) {
      fi = p.le32();
      f_off = p.le32();
      si = p.le32();
      s_off = p.le32();
    }
    if (!exact(p) || in_pass_) return refuse(__LINE__);
    Allocation* meta = alloc(mi);
    Allocation* out = alloc(oi);
    Allocation* fpsnr = fi == kNoAllocation ? nullptr : alloc(fi);
    Allocation* spsnr = si == kNoAllocation ? nullptr : alloc(si);
    if (!buffer_range(meta, m_off, ~uint64_t{0}) || !buffer_range(out, o_off, ~uint64_t{0}) || meta == out || !use(out, state::kVideoEncodeWrite)) return refuse(__LINE__);
    if ((fi != kNoAllocation && (!buffer_range(fpsnr, f_off, ~uint64_t{0}) || fpsnr == meta || fpsnr == out || !use(fpsnr, state::kVideoEncodeWrite))) ||
        (si != kNoAllocation && (!buffer_range(spsnr, s_off, ~uint64_t{0}) || spsnr == meta || spsnr == out || !use(spsnr, state::kVideoEncodeWrite))))
      return refuse(__LINE__);
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    if (!mm_.read_back(*meta) || !mm_.read_back(*out) || (fpsnr && !mm_.read_back(*fpsnr)) || (spsnr && spsnr != fpsnr && !mm_.read_back(*spsnr))) return status::kInvalidDeviceState;
    // (an output buffer too small for what is to be written is left as it was; the two PSNR records may share a buffer at different offsets)
    const ByteView fview = fpsnr ? fpsnr->writable_backing().subview(f_off, fpsnr->desc().size - f_off) : ByteView();
    const ByteView sview = spsnr ? spsnr->writable_backing().subview(s_off, spsnr->desc().size - s_off) : ByteView();
    if (resolve_video_encode_metadata(meta->backing().subview(m_off, meta->desc().size - m_off), out->writable_backing().subview(o_off, out->desc().size - o_off), fview, sview)) {
      mm_.note_cpu_write(*out);
      if (fpsnr) mm_.note_cpu_write(*fpsnr);
      if (spsnr && spsnr != fpsnr) mm_.note_cpu_write(*spsnr);
    }
    return status::kSuccess;
  }

  // Every level of the texture from level 0 (every layer of an array or a cube its own chain, a volume's levels from the two slices above each), on the CPU from the backing: the GPU's
  // contents are read back first (a target drawn into). Not of a block-compressed or integer texture (their levels are not made by filtering: Direct3D's GenerateMips refuses them).
  NtStatus generate_mipmaps(ByteReader& p) {
    const uint32_t index = p.le32();
    if (!exact(p) || in_pass_) return kBad;
    Allocation* a = alloc(index);
    if (!a || !a->is_image() || !(a->desc().flags & kAllocTexture) || a->mip_levels() < 2 || is_compressed(a->desc().format) || is_integer(a->desc().format) || a->desc().samples != 1) return kBad;
    if (!(adapter_.caps_.features2 & graphics::kFeature2MipmappedLayers) && (a->layers() != 1 || is_wide(a->desc().format))) return kBad;
    if (!exec_) return status::kSuccess;
    NtStatus s = flush();
    if (s != status::kSuccess) return s;
    if (!mm_.read_back(*a) || !generate_levels_cpu(*a)) return status::kInvalidDeviceState;
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
    if (!exact(p)) return refuse(__LINE__);
    SharedPointer<RootSignatureObject> found;
    if (exec_) {
      if (replay_signature_cursor_ >= replay_signatures_.size()) return refuse(__LINE__);
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
    if (!p.ok() || count == 0 || count > 2) return refuse(__LINE__);
    SharedPointer<DescriptorHeapObject> views, samplers;
    for (uint32_t i = 0; i < count; ++i) {
      const Handle handle = p.le32();
      if (!p.ok()) return refuse(__LINE__);
      SharedPointer<DescriptorHeapObject> found;
      if (exec_) {
        if (replay_heap_cursor_ >= replay_heaps_.size()) return refuse(__LINE__);
        found = replay_heaps_[replay_heap_cursor_++];
      } else {
        found = lookup_->find_descriptor_heap(handle);
        if (!found || !collected_heaps_->push_back(found)) return found ? status::kNoMemory : status::kInvalidHandle;
      }
      SharedPointer<DescriptorHeapObject>& slot = found->type() == DescriptorHeapType::Sampler ? samplers : views;
      if (slot) return refuse(__LINE__);   // (two of a kind)
      slot = found;
    }
    if (!exact(p)) return refuse(__LINE__);
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
    if (!p.ok() || !rp || count == 0 || first > rp->constant_count || count > rp->constant_count - first) return refuse(__LINE__);
    RootArgument& arg = arguments_[index];
    for (uint32_t i = 0; i < count; ++i) arg.values[first + i] = p.le32();
    if (!exact(p)) return refuse(__LINE__);
    arg.set = true;
    return status::kSuccess;
  }

  NtStatus set_root_descriptor(ByteReader& p) {
    const uint32_t index = p.le32(), allocation = p.le32(), offset = p.le32();
    if (!exact(p) || !signature_ || index >= signature_->desc.parameter_count) return refuse(__LINE__);
    const RootParameter& rp = signature_->desc.parameters[index];
    Allocation* a = alloc(allocation);
    if (!a || offset % 4 || offset >= a->desc().size) return refuse(__LINE__);
    if (rp.type == RootParameterType::Cbv) {
      if (!a->is_buffer() || a->is_query_result() || !use(a, state::kVertexAndConstantBuffer)) return refuse(__LINE__);
    } else if (rp.type == RootParameterType::Uav) {
      if (!(a->desc().flags & kAllocUnorderedAccess) || !use(a, state::kUnorderedAccess)) return refuse(__LINE__);
    } else if (rp.type == RootParameterType::Srv) {
      if (!(a->desc().flags & kAllocUnorderedAccess) || !use(a, state::kNonPixelShaderResource)) return refuse(__LINE__);   // (a buffer: a storage buffer the shader only reads)
    } else {
      return refuse(__LINE__);
    }
    arguments_[index].set = true;
    arguments_[index].allocation = a;
    arguments_[index].offset = offset;
    return status::kSuccess;
  }

  NtStatus set_root_table(ByteReader& p) {
    const uint32_t index = p.le32(), first = p.le32();
    const RootParameter* rp = parameter(index, RootParameterType::Table);
    if (!exact(p) || !rp) return refuse(__LINE__);
    const DescriptorHeapObject* heap = rp->sampler_table ? sampler_heap_.get() : view_heap_.get();
    if (!heap || first >= heap->count()) return refuse(__LINE__);
    if (rp->table_size != kUnboundedDescriptors && rp->table_size > heap->count() - first) return refuse(__LINE__);
    arguments_[index].set = true;
    arguments_[index].first = first;
    // (the mirrors the table's views show are brought up to date here, outside a pass, where what the GPU drew into their sources can be read back: a draw in a pass
    // copies only what the CPU has)
    if (exec_ && !in_pass_ && !rp->sampler_table) return refresh_table_mirrors(*rp, first);
    return status::kSuccess;
  }

  // Brings a mirror (VidMm::set_mirror) up to date before it is used. `may_flush`: what was recorded may be run first, so that sources the GPU drew into can be read back
  // (no pass open on the device, and nothing bound yet that the run would lose: a table set outside a pass, a pass about to begin); else those keep what the mirror last
  // copied of them.
  NtStatus refresh_mirror(Allocation* a, bool may_flush) {
    if (!exec_ || !a || !a->is_mirror()) return status::kSuccess;
    const bool read_back = may_flush && mm_.mirror_needs_read_back(*a);
    if (read_back) {
      const NtStatus s = flush_keeping_state();
      if (s != status::kSuccess) return s;
    }
    return mm_.refresh_mirror(*a, read_back) ? status::kSuccess : status::kInvalidDeviceState;
  }
  // (the images the pipeline bound last reads - a view's own or its array, as it declares the unit -, or both of a view's when none is bound yet)
  NtStatus refresh_table_mirrors(const RootParameter& rp, uint32_t first) {
    if (!view_heap_) return status::kSuccess;
    const PipelineObject* reads = bound_compute_ ? compute_ : pipeline_;
    for (uint32_t r = 0; r < rp.range_count; ++r) {
      const DescriptorRange& range = rp.ranges[r];
      if (range.type != RangeType::Srv && range.type != RangeType::Uav) continue;
      const uint64_t available = view_heap_->count() > first ? view_heap_->count() - first : 0;
      const uint64_t count = range.count == kUnboundedDescriptors ? (available > range.offset ? available - range.offset : 0) : range.count;
      for (uint64_t k = 0; k < count; ++k) {
        const uint64_t at = uint64_t{first} + range.offset + k;
        if (at >= view_heap_->count()) break;
        const Descriptor& d = view_heap_->at(at);
        if (d.kind == DescriptorKind::UavTexture) {   // (an unordered access view of a plane: its mirror starts with what the plane has)
          const NtStatus s = refresh_mirror(d.allocation.get(), true);
          if (s != status::kSuccess) return s;
          continue;
        }
        if (d.kind != DescriptorKind::Srv) continue;
        const uint64_t reg = range.base_register + k;
        NtStatus s = status::kSuccess;
        if (!reads) {
          s = refresh_mirror(d.allocation.get(), true);
          if (s == status::kSuccess) s = refresh_mirror(d.alternate.get(), true);
        } else if (reg < graphics::kMaxTextureUnits && (bound_compute_ ? (reads->texture_mask() >> reg & 1) != 0 : reg < reads->texture_count())) {
          s = refresh_mirror(view_image(d, reads->texture_kind(static_cast<uint32_t>(reg))), true);
        }
        if (s != status::kSuccess) return s;
      }
    }
    return status::kSuccess;
  }
  // After a pass: the targets that are mirrors that write back give their channels to their sources (the drawing is run first).
  NtStatus write_back_targets() {
    bool flushed = false;
    for (uint32_t t = 0; t < graphics::kMaxColorTargets; ++t) {
      Allocation* target = write_back_[t];
      write_back_[t] = nullptr;
      if (!target) continue;
      if (!flushed) {
        const NtStatus s = flush_keeping_state();
        if (s != status::kSuccess) return s;
        flushed = true;
      }
      if (!mm_.write_back_mirror(*target)) return status::kInvalidDeviceState;
    }
    return status::kSuccess;
  }
  // After a dispatch, or a pass whose pixel shaders wrote storage images: the images that are mirrors that write back (unordered access views of planes) give their channels to their
  // sources (what was recorded is run first).
  NtStatus write_back_images() {
    bool flushed = false;
    for (Allocation*& target : image_write_back_) {
      Allocation* a = target;
      target = nullptr;
      if (!a) continue;
      if (!flushed) {
        const NtStatus s = flush_keeping_state();
        if (s != status::kSuccess) return s;
        flushed = true;
      }
      if (!mm_.write_back_mirror(*a)) return status::kInvalidDeviceState;
    }
    return status::kSuccess;
  }
  Allocation* image_write_back_[graphics::kMaxStorageImages] = {};   // the storage images written since that are mirrors that write back
  // Whether the image bound to storage slot `slot` is what a shader that declared it as `dim` with `ret` values may be given: a plain image (or one layer of an array) for a 1D or 2D one,
  // an array or a plain image for an array one, a volume for a 3D one, a typed view of a buffer for a buffer; float values for a format that is not an integer one, integers for one that is.
  bool image_fits(uint32_t slot, D3d9Program::ImageDim dim, D3d9Program::ImageReturn ret) const {
    if (slot >= graphics::kMaxImageSlots || !image_bound_[slot]) return false;
    if (texel_bound_[slot] != (dim == D3d9Program::ImageDim::Buffer)) return false;
    if (texel_bound_[slot]) return (ret != D3d9Program::ImageReturn::Float) == graphics::texel_is_integer(texel_format_[slot]);
    const Allocation* a = image_alloc_[slot];
    const graphics::TextureKind kind = a->texture_kind();
    const bool integer = is_integer(image_format_[slot]);   // (the view's format)
    if ((ret != D3d9Program::ImageReturn::Float) != integer) return false;
    switch (dim) {
      case D3d9Program::ImageDim::Texture1D: case D3d9Program::ImageDim::Texture2D: return kind != graphics::TextureKind::Texture3D && image_view_[slot].layer_count == 1;
      case D3d9Program::ImageDim::Texture1DArray: case D3d9Program::ImageDim::Texture2DArray: return kind != graphics::TextureKind::Texture3D;
      case D3d9Program::ImageDim::Texture3D: return kind == graphics::TextureKind::Texture3D;
      default: return false;
    }
  }
  // The storage images a pipeline's shader declares: each bound and of what it declared.
  bool images_fit(const PipelineObject& p) const {
    for (uint32_t slot = 0; slot < graphics::kMaxImageSlots; ++slot)
      if (p.image_mask().test(slot) && !image_fits(slot, p.image_dim(slot), p.image_return(slot))) return false;
    return true;
  }
  // After a dispatch or a draw: the storage images its shader declares have newer contents on the device; those that are mirrors that write back are noted to give them back.
  void note_images_written(const PipelineObject& p) {
    for (uint32_t slot = 0; slot < graphics::kMaxStorageImages; ++slot) {   // (the u# ones: the views of t# are read)
      if (!p.image_mask().test(slot) || !image_bound_[slot]) continue;
      mm_.note_gpu_write(*image_alloc_[slot]);
      if (texel_bound_[slot]) continue;
      if (image_alloc_[slot]->writes_back()) image_write_back_[slot] = image_alloc_[slot];
    }
  }

  NtStatus bind_uav_image(ByteReader& p) {
    const uint32_t slot = p.le32(), index = p.le32(), level = p.le32(), first = p.le32(), count = p.le32();
    const uint32_t view_format = p.ok() && !p.at_end() ? p.le32() : 0;   // (the view's format: 0 the allocation's own)
    const uint32_t features2 = adapter_.caps_.features2;
    if (!exact(p) || slot >= graphics::kMaxStorageImages || !(features2 & graphics::kFeature2StorageImages) || (in_pass_ && !(features2 & graphics::kFeature2StorageImagesInDraws))) return refuse(__LINE__);
    if (index == kNoAllocation) {
      image_bound_[slot] = false;
      texel_bound_[slot] = false;
      image_alloc_[slot] = nullptr;
      if (!exec_) return status::kSuccess;
      const NtStatus s = ensure_cb();
      if (s != status::kSuccess) return s;
      cb_->bind_storage_image(slot, nullptr, graphics::StorageImageView());
      return status::kSuccess;
    }
    Allocation* a = alloc(index);
    graphics::Format view = graphics::Format::Undefined;
    if (!a || !a->is_image() || !(a->desc().flags & kAllocStorageImage) || level >= a->mip_levels() || count == 0 || first >= a->level_layers(level) || count > a->level_layers(level) - first ||
        !is_known_format(view_format) || !storage_view_format(a->desc().format, static_cast<Format>(view_format), &view) || !use(a, state::kUnorderedAccess))
      return refuse(__LINE__);
    image_bound_[slot] = true;
    texel_bound_[slot] = false;
    image_alloc_[slot] = a;
    image_view_[slot] = graphics::StorageImageView{level, first, count, view};
    image_format_[slot] = view_format ? static_cast<Format>(view_format) : a->desc().format;
    if (!exec_) return status::kSuccess;
    // (an unordered access view of a plane - a mirror - starts with what its source has; in a pass, with what the CPU has of it)
    NtStatus s = refresh_mirror(a, !in_pass_);
    if (s == status::kSuccess) s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_storage_image(slot, a->image(), image_view_[slot]);
    return status::kSuccess;
  }

  // BindTexelBuffer: a typed view of a buffer in image slot `slot` (a u# one, read and written, or a t# one from kTexelBufferSlotBase, read).
  NtStatus bind_texel_buffer(ByteReader& p) {
    const uint32_t slot = p.le32(), index = p.le32(), first = p.le32(), count = p.le32(), format_word = p.le32();
    const uint32_t features2 = adapter_.caps_.features2;
    if (!exact(p) || slot >= graphics::kMaxImageSlots || !(features2 & graphics::kFeature2TexelBuffers) || (in_pass_ && !(features2 & graphics::kFeature2StorageImagesInDraws))) return refuse(__LINE__);
    const bool writable = slot < graphics::kMaxStorageImages;
    if (index == kNoAllocation) {
      image_bound_[slot] = texel_bound_[slot] = false;
      image_alloc_[slot] = nullptr;
      if (!exec_) return status::kSuccess;
      const NtStatus s = ensure_cb();
      if (s != status::kSuccess) return s;
      cb_->bind_storage_texel_buffer(slot, nullptr, graphics::TexelBufferView());
      return status::kSuccess;
    }
    const graphics::TexelFormat format = static_cast<graphics::TexelFormat>(format_word);
    Allocation* a = alloc(index);
    const uint64_t bytes = graphics::texel_bytes(format);
    // (a format the slot takes, and elements that lie in the buffer: first * bytes + count * bytes within its size, worked out so that nothing wraps)
    if (!a || !a->is_buffer() || !(a->desc().flags & kAllocUnorderedAccess) || !graphics::texel_format_valid(format) || (writable && !graphics::texel_format_storable(format)) || count == 0 ||
        uint64_t{first} > a->desc().size / bytes || uint64_t{count} > a->desc().size / bytes - first || !use(a, writable ? state::kUnorderedAccess : state::kNonPixelShaderResource | state::kPixelShaderResource))
      return refuse(__LINE__);
    image_bound_[slot] = texel_bound_[slot] = true;
    image_alloc_[slot] = a;
    texel_format_[slot] = format;
    if (!exec_) return status::kSuccess;
    NtStatus s = resident(*a);
    if (s == status::kSuccess) s = ensure_cb();
    if (s != status::kSuccess) return s;
    cb_->bind_storage_texel_buffer(slot, a->buffer(), graphics::TexelBufferView{uint64_t{first} * bytes, count, format});
    return status::kSuccess;
  }

  NtStatus lower_texel_buffer(uint32_t slot, Allocation* a, uint64_t offset, uint64_t size, uint32_t format) {
    uint32_t index = 0;
    const uint32_t bytes = graphics::texel_bytes(static_cast<graphics::TexelFormat>(format));
    if (!a || !index_of(a, &index) || bytes == 0 || offset % bytes || size % bytes || offset / bytes > 0xFFFFFFFFu || size / bytes > 0xFFFFFFFFu) return refuse(__LINE__);
    ByteBuffer packet(adapter_.allocator_);
    for (uint32_t w : {slot, index, static_cast<uint32_t>(offset / bytes), static_cast<uint32_t>(size / bytes), format}) packet.put32(w);
    ByteReader reader(packet.view());
    return bind_texel_buffer(reader);
  }

  NtStatus lower_uav_image(uint32_t slot, Allocation* a, const graphics::StorageImageView& v, uint32_t view_format) {
    uint32_t index = 0;
    if (!a || !index_of(a, &index)) return refuse(__LINE__);
    ByteBuffer packet(adapter_.allocator_);
    for (uint32_t w : {slot, index, v.level, v.first_layer, v.layer_count, view_format}) packet.put32(w);
    ByteReader reader(packet.view());
    return bind_uav_image(reader);
  }

  Allocation* write_back_[graphics::kMaxColorTargets] = {};   // the pass's targets that are mirrors that write back (begin_pass)
  // The image a texture view gives a shader that declares `unit` of `kind`: the view's own, or its alternate array (Descriptor::alternate) for a shader that declares an array.
  static Allocation* view_image(const Descriptor& d, graphics::TextureKind kind) {
    if (d.alternate && kind == graphics::TextureKind::Texture2DArray && d.allocation->texture_kind() == graphics::TextureKind::Texture2D) return d.alternate.get();
    return d.allocation.get();
  }

  NtStatus resource_barrier(ByteReader& p) {
    const uint32_t count = p.le32();
    if (!p.ok() || count == 0) return refuse(__LINE__);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t type = p.le32(), index = p.le32(), before = p.le32(), after = p.le32(), flags = p.le32();
      if (!p.ok() || type > 2 || flags > 2) return refuse(__LINE__);
      Allocation* a = index == kNoAllocation ? nullptr : alloc(index);
      if (index != kNoAllocation && !a) return refuse(__LINE__);
      if (type == 2) {
        // An aliasing barrier: `index` is the allocation going out of use and the "before" word the index of the one coming into use (either may be kNoAllocation: nothing to do), both placed in
        // one heap. What the GPU holds for the first goes to the heap's memory and the second is uploaded from it.
        Allocation* incoming = before == kNoAllocation ? nullptr : alloc(before);
        if (before != kNoAllocation && !incoming) return refuse(__LINE__);
        if (!a || !incoming) continue;
        if (in_pass_ || !a->placed() || a->heap().get() != incoming->heap().get() || a == incoming) return refuse(__LINE__);
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
        if (type == 1 && a && a->state_tracked() && !(state_of(a) & state::kUnorderedAccess)) return refuse(__LINE__);
        continue;
      }
      if (!a || ((before | after) & ~kStateMask) || before == after) return refuse(__LINE__);
      if ((after & state::kWrite) && (after & (after - 1))) return refuse(__LINE__);   // (a state that writes stands alone)
      if (!a->state_tracked()) continue;
      if (flags == 1) {
        if (state_of(a) != before) return refuse(__LINE__);
        continue;   // (a split barrier begins: the change is made at its end)
      }
      if (state_of(a) != before) return refuse(__LINE__);
      if (!ensure_shadow()) return status::kNoMemory;
      const uint64_t slot = slot_of(a);
      if (slot < shadow_.count()) shadow_[slot] = after;
    }
    return exact(p) ? status::kSuccess : kBad;
  }

  // A view's clamp of the levels of detail (Direct3D's ResourceMinLODClamp; d3d11's SetResourceMinLOD goes the same way): `clamp` counts the image's levels, the lookups count the view's
  // from `base_level`, so the least level of detail of the sampler is raised to clamp - base_level, after the sampler's own MinLOD and MaxLOD (Direct3D clamps the level the sampler
  // gives, then takes the greater of it and the clamp: a MaxLOD below the clamp is raised with it). A sampler without levels (a MaxLOD of 0: no mip filter) then reads the clamp's
  // level, the nearest one; a clamp past the view's last level reads its last.
  static void clamp_view_lod(graphics::SamplerDesc* sampler, float clamp, uint32_t base_level) {
    const float relative = clamp - static_cast<float>(base_level);
    if (!(relative > 0.0f)) return;
    if (sampler->min_lod < relative) sampler->min_lod = relative;
    if (sampler->max_lod < relative) sampler->max_lod = relative;
    if (sampler->mip_filter == graphics::MipFilter::None) sampler->mip_filter = graphics::MipFilter::Nearest;
  }

  // What a draw or dispatch binds from the root arguments: b0 (the first constant buffer: root constants, a root constant buffer view or a table's first view) as the push constants, the shader
  // resource views of tables as the texture units t<n> (with the sampler s<n> of a table or a static sampler, else s0, else the defaults), the unordered access views as storage slots u<n>.
  NtStatus apply_root_arguments(bool compute) {
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
        if (rp.space != 0 || rp.shader_register != 0) return refuse(__LINE__);   // (one constant buffer)
        if (!arg.set) return refuse(__LINE__);
        const uint32_t bytes = (rp.constant_count * 4 + 15) / 16 * 16;
        if (constants_found || bytes > adapter_.caps_.max_push_constant_bytes) return refuse(__LINE__);
        constants_found = true;
        block_bytes = bytes;
        ByteWriter w(ByteView(block, sizeof block));
        for (uint32_t k = 0; k < rp.constant_count; ++k) w.le32(arg.values[k]);
      } else if (rp.type == RootParameterType::Cbv) {
        if (rp.space != 0 || rp.shader_register != 0 || constants_found || !arg.set) return refuse(__LINE__);
        constants_found = true;
        const uint64_t avail = arg.allocation->desc().size - arg.offset;
        block_bytes = static_cast<uint32_t>(avail < adapter_.caps_.max_push_constant_bytes ? avail / 16 * 16 : adapter_.caps_.max_push_constant_bytes);
        if (block_bytes == 0 || block_bytes > sizeof block) return refuse(__LINE__);
        ByteView out(block, block_bytes);
        if (!out.copy_from(arg.allocation->backing().subview(arg.offset, block_bytes))) return refuse(__LINE__);
      } else if (rp.type == RootParameterType::Uav) {
        if (rp.space != 0 || !arg.set || rp.shader_register >= graphics::kMaxUavBuffers) return refuse(__LINE__);
        const NtStatus s = lower_uav(rp.shader_register, arg.allocation, arg.offset, arg.allocation->desc().size - arg.offset);
        if (s != status::kSuccess) return s;
      } else if (rp.type == RootParameterType::Srv) {
        if (rp.space != 0 || !arg.set || rp.shader_register >= graphics::kMaxSrvBuffers) return refuse(__LINE__);
        const NtStatus s = lower_uav(graphics::kMaxUavBuffers + rp.shader_register, arg.allocation, arg.offset, arg.allocation->desc().size - arg.offset);
        if (s != status::kSuccess) return s;
      } else if (rp.type == RootParameterType::Table && !rp.sampler_table) {
        if (!arg.set || !view_heap_) return refuse(__LINE__);
        for (uint32_t r = 0; r < rp.range_count; ++r) {
          const DescriptorRange& range = rp.ranges[r];
          if (range.space != 0) return refuse(__LINE__);
          const uint64_t available = view_heap_->count() - arg.first;
          const uint64_t count = range.count == kUnboundedDescriptors ? (available > range.offset ? available - range.offset : 0) : range.count;
          for (uint64_t k = 0; k < count; ++k) {
            const uint64_t at = uint64_t{arg.first} + range.offset + k;
            if (at >= view_heap_->count()) return refuse(__LINE__);
            const Descriptor& d = view_heap_->at(at);
            const uint32_t reg = range.base_register + static_cast<uint32_t>(k);
            if (d.kind == DescriptorKind::None) continue;   // (a descriptor that was never written: not used by a shader that is right)
            const PipelineObject* reads = compute ? compute_ : pipeline_;
            if (range.type == RangeType::Srv && d.kind == DescriptorKind::Srv && d.allocation->is_buffer()) {
              if (reg >= graphics::kMaxSrvBuffers) return refuse(__LINE__);
              // (a typed view where a shader declares t<reg> a typed buffer; the bytes of a buffer where it declares a raw or structured one)
              if (reads && reg < graphics::kMaxTexelBufferViews && reads->image_mask().test(graphics::kTexelBufferSlotBase + reg)) {
                if (!d.texel_format) return refuse(__LINE__);
                const NtStatus s = lower_texel_buffer(graphics::kTexelBufferSlotBase + reg, d.allocation.get(), d.offset, d.size, d.texel_format);
                if (s != status::kSuccess) return s;
                continue;
              }
              const NtStatus s = lower_uav(graphics::kMaxUavBuffers + reg, d.allocation.get(), d.offset, d.size);
              if (s != status::kSuccess) return s;
            } else if (range.type == RangeType::Srv && d.kind == DescriptorKind::Srv) {
              if (reg >= adapter_.caps_.max_texture_units) return refuse(__LINE__);
              // (t<reg> goes to every unit the shaders read it through - its own, and those of its pairs with other samplers -, each with the sampler of its pair; a unit a
              // pair of another texture has is that pair's. Each shows the descriptor's levels and layers, and its plane of a depth buffer)
              for (uint32_t unit = 0; unit < adapter_.caps_.max_texture_units; ++unit) {
                if ((reads ? reads->unit_texture(unit) : unit) != reg) continue;
                Allocation* image = view_image(d, reads ? reads->texture_kind(unit) : graphics::TextureKind::Texture2D);
                graphics::SamplerDesc sampler = sampler_for(reads ? reads->unit_sampler(unit) : unit);
                sampler.stencil = d.stencil;   // (the view's plane of a depth buffer)
                clamp_view_lod(&sampler, d.min_lod_clamp, d.view.base_level);
                NtStatus s = refresh_mirror(image, false);
                if (s == status::kSuccess) s = lower_texture(unit, image, sampler, image == d.allocation.get() ? d.view : graphics::TextureView());
                if (s != status::kSuccess) return s;
              }
            } else if (range.type == RangeType::Uav && d.kind == DescriptorKind::Uav) {
              if (reg >= graphics::kMaxUavBuffers) return refuse(__LINE__);
              // (a typed view where a shader declares u<reg> a typed buffer, the bytes where it declares a raw or structured one; and the view's counter where a shader counts with it)
              if (reads && reg < graphics::kMaxStorageImages && reads->image_mask().test(reg)) {
                if (!d.texel_format) return refuse(__LINE__);
                const NtStatus s = lower_texel_buffer(reg, d.allocation.get(), d.offset, d.size, d.texel_format);
                if (s != status::kSuccess) return s;
                continue;
              }
              NtStatus s = lower_uav(reg, d.allocation.get(), d.offset, d.size);
              if (s == status::kSuccess && reads && reads->storage_mask().test(graphics::kCounterSlotBase + reg)) {
                if (!d.counter) return refuse(__LINE__);   // (imm_atomic_alloc / consume on a view without a counter)
                s = lower_uav(graphics::kCounterSlotBase + reg, d.counter.get(), d.counter_offset, 4);
              }
              if (s != status::kSuccess) return s;
            } else if (range.type == RangeType::Uav && d.kind == DescriptorKind::UavTexture) {
              // (a storage image: bound where a shader of the pipeline declares u<reg> a typed view of a texture)
              if (reg >= graphics::kMaxStorageImages || !reads || !reads->image_mask().test(reg)) continue;
              const NtStatus s = lower_uav_image(reg, d.allocation.get(), d.image, d.view_format);
              if (s != status::kSuccess) return s;
            } else if (range.type == RangeType::Cbv && d.kind == DescriptorKind::Cbv) {
              if (reg != 0 || constants_found) return refuse(__LINE__);
              constants_found = true;
              block_bytes = static_cast<uint32_t>(d.size < adapter_.caps_.max_push_constant_bytes ? d.size / 16 * 16 : adapter_.caps_.max_push_constant_bytes);
              if (block_bytes == 0 || block_bytes > sizeof block || !use(d.allocation.get(), state::kVertexAndConstantBuffer)) return refuse(__LINE__);
              ByteView out(block, block_bytes);
              if (!out.copy_from(d.allocation->backing().subview(d.offset, block_bytes))) return refuse(__LINE__);
            } else if (d.kind != DescriptorKind::Srv && d.kind != DescriptorKind::Uav && d.kind != DescriptorKind::Cbv && d.kind != DescriptorKind::UavTexture) {
              return refuse(__LINE__);
            } else if ((range.type == RangeType::Srv) != (d.kind == DescriptorKind::Srv) || (range.type == RangeType::Uav) != (d.kind == DescriptorKind::Uav || d.kind == DescriptorKind::UavTexture)) {
              return refuse(__LINE__);   // (a descriptor of another kind than its range)
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

  NtStatus lower_texture(uint32_t unit, Allocation* a, const graphics::SamplerDesc& sampler, const graphics::TextureView& view) {
    uint32_t index = 0;
    if (!a || !index_of(a, &index)) return refuse(__LINE__);   // (the allocation is in the list or the draw is refused)
    ByteBuffer packet(adapter_.allocator_);
    for (uint32_t w : {unit, index, static_cast<uint32_t>(sampler.filter), address_word(sampler.address_u, sampler.border), address_word(sampler.address_v), static_cast<uint32_t>(sampler.mip_filter),
                       __builtin_bit_cast(uint32_t, sampler.lod_bias), __builtin_bit_cast(uint32_t, sampler.min_lod), __builtin_bit_cast(uint32_t, sampler.max_lod), texture_view_word(sampler)})
      packet.put32(w);
    // (the descriptor's view: its levels and layers, on a device that has views - and all of the image before the sampler's part on one that has not)
    const bool extras = (sampler.max_anisotropy > 1 || sampler.custom_border) && (adapter_.caps_.features2 & graphics::kFeature2SamplerState);
    if (adapter_.caps_.features2 & graphics::kFeature2MipmappedLayers)
      for (uint32_t w : {view.base_level, view.level_count, view.first_layer, view.layer_count}) packet.put32(w);
    else if (extras)
      for (uint32_t w : {0u, 0u, 0u, 0u}) packet.put32(w);
    // (anisotropy and a border colour of any value: the optional part; on a device without them a descriptor's anisotropy is filtered without - the quality of anisotropic filtering is
    // the device's - and its border colour is the nearest of the three every device has)
    if (extras) {
      for (uint32_t w : {uint32_t{sampler.max_anisotropy}, sampler.custom_border ? 1u : 0u, __builtin_bit_cast(uint32_t, sampler.border_rgba[0]), __builtin_bit_cast(uint32_t, sampler.border_rgba[1]),
                         __builtin_bit_cast(uint32_t, sampler.border_rgba[2]), __builtin_bit_cast(uint32_t, sampler.border_rgba[3])})
        packet.put32(w);
    }
    ByteReader reader(packet.view());
    return bind_texture(reader);
  }

  NtStatus lower_uav(uint32_t slot, Allocation* a, uint64_t offset, uint64_t size) {
    uint32_t index = 0;
    if (!a || !index_of(a, &index) || offset > 0xFFFFFFFFu || size > 0xFFFFFFFFu) return refuse(__LINE__);
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
  List<SharedPointer<VideoDecoderObject>>* collected_decoders_ = nullptr;
  ArrayView<const SharedPointer<VideoDecoderObject>> replay_decoders_;
  uint64_t replay_decoder_cursor_ = 0;
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
  uint32_t query_slots_in_stream_ = 0;   // the device queries the stream's queries take (at most kMaxOcclusionQueries: a command buffer holds no more)
  uint32_t next_slot_ = 0;               // the slot of the next device query in the command buffer being recorded
  List<StreamQuery> queries_;            // the queries of the stream, in the order they began
  const Allocation* pass_color_ = nullptr;
  const Allocation* pass_extra_[graphics::kMaxColorTargets - 1] = {};
  uint32_t pass_target_count_ = 1;
  const Allocation* pass_depth_ = nullptr;
  PipelineObject* pipeline_ = nullptr;
  PipelineObject* compute_ = nullptr;
  bool bound_compute_ = false;   // the pipeline bound last is compute_ (whose textures a table set now is for)
  bool so_bound_[graphics::kMaxStreamOutputBuffers] = {};
  Allocation* so_alloc_[graphics::kMaxStreamOutputBuffers] = {};
  bool uav_bound_[graphics::kMaxStorageBuffers] = {};
  Allocation* uav_alloc_[graphics::kMaxStorageBuffers] = {};
  bool image_bound_[graphics::kMaxImageSlots] = {};
  bool texel_bound_[graphics::kMaxImageSlots] = {};   // ... a typed view of a buffer (BindTexelBuffer), not a texture's
  Allocation* image_alloc_[graphics::kMaxImageSlots] = {};
  graphics::StorageImageView image_view_[graphics::kMaxImageSlots];
  Format image_format_[graphics::kMaxImageSlots] = {};   // the format of each image's view
  graphics::TexelFormat texel_format_[graphics::kMaxImageSlots] = {};   // ... and of each typed view of a buffer
  Allocation* vertex_[graphics::kMaxVertexBuffers] = {};
  uint64_t vertex_offset_[graphics::kMaxVertexBuffers] = {};
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
  bool texture_stencil_[graphics::kMaxTextureUnits] = {};   // (the unit reads a depth buffer's stencil plane)
  // Whether the units `used` (bit n: unit n) read both planes of one depth buffer, which a device without kFeature2DepthAndStencilPlanes cannot do in one draw or dispatch.
  bool both_planes(uint32_t used) const {
    if (adapter_.caps_.features2 & graphics::kFeature2DepthAndStencilPlanes) return false;
    for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) {
      if (!(used >> u & 1) || !texture_bound_[u] || !texture_alloc_[u]->is_depth()) continue;
      for (uint32_t v = u + 1; v < graphics::kMaxTextureUnits; ++v)
        if ((used >> v & 1) && texture_bound_[v] && texture_alloc_[v] == texture_alloc_[u] && texture_stencil_[v] != texture_stencil_[u]) return true;
    }
    return false;
  }
  graphics::TextureView texture_view_[graphics::kMaxTextureUnits];        // the levels and layers each unit shows (BindTexture's view, counts made whole)
  graphics::SamplerDesc texture_sampler_[graphics::kMaxTextureUnits];   // (what the device was given for the unit)
  graphics::TextureKind device_view_kind_[graphics::kMaxTextureUnits] = {};   // the kind the device's binding of the unit has (kFeature2MipmappedLayers)
  // The open pass's level and layers of each target (kMaxColorTargets: the depth buffer).
  uint32_t pass_level_[graphics::kMaxColorTargets + 1] = {}, pass_first_layer_[graphics::kMaxColorTargets + 1] = {}, pass_layer_count_[graphics::kMaxColorTargets + 1] = {};
};

// --- The adapter ----------------------------------------------------------------------------------------------------------------------------------------------------

Adapter::Adapter(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device, const AdapterConfig& config, uint32_t index, Luid luid)
    : allocator_(allocator), device_(device), display_(config.display), index_(index), luid_(luid), vidmm_(allocator, device, config.local_memory_budget, config.system_memory_limit), contexts_(allocator) {
  const graphics::DeviceInfo info = device.info();
  caps_.max_texture_dimension = vidmm_.max_image_dimension();
  // What the device takes (DeviceInfo::limits, never more than the capacity of the structures).
  auto limit = [](uint32_t value, uint32_t capacity) { return value < capacity ? value : capacity; };
  caps_.max_vertex_attributes = limit(info.limits.vertex_attributes, graphics::kMaxVertexAttributes);
  caps_.max_varyings = limit(info.limits.varyings, graphics::kMaxVaryings);
  caps_.max_texture_units = limit(info.limits.texture_units, graphics::kMaxTextureUnits);
  caps_.max_color_targets = limit(info.limits.color_targets, graphics::kMaxColorTargets);
  caps_.max_vertex_buffers = limit(info.limits.vertex_buffers, graphics::kMaxVertexBuffers);
  caps_.max_push_constant_bytes = info.max_push_constant_bytes < graphics::kMaxPushConstantBytes ? info.max_push_constant_bytes : graphics::kMaxPushConstantBytes;
  caps_.max_vertex_stride = kMaxVertexStride;
  caps_.accelerated = info.accelerated;
  caps_.features = info.features;
  const char* name = config.name ? config.name : info.name;
  for (uint32_t i = 0; name && name[i] && i + 1 < sizeof(caps_.name); ++i) caps_.name[i] = name[i];
  caps_.sample_counts = info.sample_counts;
  caps_.blend_constant = info.blend_constant;
  caps_.features2 = info.features2;
  for (uint32_t f = kFirstExtraFormat; f <= kLastExtraFormat; ++f) caps_.format_caps[f - kFirstExtraFormat] = vidmm_.format_caps(static_cast<Format>(f));
  caps_.device_kind = static_cast<uint32_t>(info.kind);
  // The vendor the name says (the names are those of the devices in src/Graphics/Graphics3D); the virtual GPUs have their hypervisor's.
  auto name_has = [&](const char* part) {
    for (uint32_t i = 0; caps_.name[i]; ++i) {
      uint32_t k = 0;
      while (part[k] && caps_.name[i + k] == part[k]) ++k;
      if (!part[k]) return true;
    }
    return false;
  };
  if (info.kind == graphics::DeviceKind::Software) caps_.vendor_id = 0x10005, caps_.device_id = 1;
  else if (name_has("Intel")) caps_.vendor_id = 0x8086, caps_.device_id = 9;
  else if (name_has("Radeon") || name_has("AMD")) caps_.vendor_id = 0x1002, caps_.device_id = 0x694C;
  else if (name_has("NVIDIA")) caps_.vendor_id = 0x10DE, caps_.device_id = 1;
  else if (name_has("VMware")) caps_.vendor_id = 0x15AD, caps_.device_id = 0x0405;
  else if (name_has("virgl")) caps_.vendor_id = 0x1AF4, caps_.device_id = 0x1050;
  else if (name_has("Raspberry") || name_has("V3D")) caps_.vendor_id = 0x14E4, caps_.device_id = 0x2711;
  else caps_.vendor_id = 0x10005, caps_.device_id = 2;

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
      const NtStatus s = desc.kind == ShaderKind::Dxbc ? D3d9Program::compile_dxbc(allocator_, desc.code, &program, desc.constant_base) : D3d9Program::compile(allocator_, desc.code, &program);
      if (s != status::kSuccess) return s;
      if (program->constant_bytes() > caps_.max_push_constant_bytes) return status::kNotSupported;
      if (program->attributes_used() > caps_.max_vertex_attributes) return status::kNotSupported;      // (a vertex input the device does not have)
      if (program->texture_unit_mask() >> caps_.max_texture_units) return status::kNotSupported;      // (a texture unit the device does not have)
      if (program->stage() == graphics::ShaderStage::Fragment && program->color_output_count() > caps_.max_color_targets) return status::kNotSupported;
      if (program->required_features() & ~caps_.features) return status::kNotSupported;   // (system values, texel access: the device must have them)   // (more constants than this device is given)
      if (program->required_features2() & ~caps_.features2) return status::kNotSupported;   // (storage images, the layer output)
      // A GPU device (virgl) takes the shader as TGSI text; the others run it through the interpreter's native function. Both are given: each device uses what it understands.
      // A shader the text cannot say (see D3d9Tgsi.cpp) is still fine for the interpreter.
      const graphics::ShaderStage stage = program->stage();
      String text(allocator_);
      const NtStatus t = program->emit_tgsi(&text, caps_.max_varyings, &md.tgsi_bindings);
      if (t == status::kNoMemory) return t;
      if (t == status::kSuccess) {
        md.sampler_constants = program->compare_units() != 0;   // (its comparisons read the bound samplers' from the constants)
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
      for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) {
        shader->texture_kinds_[u] = program->texture_kind(u);
        shader->unit_texture_[u] = static_cast<uint8_t>(program->texture_register(u));
        shader->unit_sampler_[u] = static_cast<uint8_t>(program->sampler_register(u));
      }
      shader->texture_mask_ = program->texture_unit_mask();
      shader->sampled_mask_ = program->sampled_units();
      shader->knows_textures_ = desc.kind == ShaderKind::Dxbc;   // (Direct3D 9 bytecode does not say all of them: its pipelines keep the count they are given)
      for (uint32_t k = 0; k < 3; ++k) shader->thread_group_[k] = program->thread_group(k);
      shader->geometry_input_ = program->geometry_input();
      shader->geometry_streams_ = program->geometry_streams();
      shader->tess_input_points_ = program->tess_input_points();
      shader->tess_output_points_ = program->tess_output_points();
      shader->tess_domain_ = program->tess_domain();
      for (uint32_t u = 0; u < graphics::kMaxStorageBuffers; ++u)
        if (program->storage_kind(u) != D3d9Program::BufferKind::None) shader->storage_mask_.set(u);
      shader->fragment_stores_ = program->uses_fragment_stores();
      shader->vertex_stores_ = program->uses_vertex_stores();
      shader->geometry_stores_ = program->uses_geometry_stores();
      shader->tessellation_stores_ = program->uses_tessellation_stores();
      shader->image_mask_ = program->image_mask();
      for (uint32_t u = 0; u < graphics::kMaxImageSlots; ++u) shader->image_dims_[u] = program->image_dim(u), shader->image_returns_[u] = program->image_return(u);
      shader->writes_layer_ = program->writes_layer();
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
  pipeline->texture_mask_ = shader->texture_mask_;
  pipeline->declared_units_ = shader->texture_mask_;
  for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) {
    pipeline->texture_kinds_[u] = shader->texture_kind(u);
    pipeline->unit_texture_[u] = shader->unit_texture_[u];
    pipeline->unit_sampler_[u] = shader->unit_sampler_[u];
  }
  pipeline->image_mask_ = shader->image_mask_;
  for (uint32_t u = 0; u < graphics::kMaxImageSlots; ++u) pipeline->image_dims_[u] = shader->image_dims_[u], pipeline->image_returns_[u] = shader->image_returns_[u];
  pipeline->vertex_ = shader;   // (kept alive for as long as the pipeline is)
  *out = pipeline;
  return status::kSuccess;
}

NtStatus Adapter::create_pipeline(const PipelineDesc& d, const SharedPointer<ShaderObject>& vertex, const SharedPointer<ShaderObject>& fragment, SharedPointer<PipelineObject>* out,
                                  const SharedPointer<ShaderObject>& geometry, const SharedPointer<ShaderObject>& hull, const SharedPointer<ShaderObject>& domain) {
  if (!vertex || !fragment || vertex->stage() != graphics::ShaderStage::Vertex || fragment->stage() != graphics::ShaderStage::Fragment) return status::kInvalidParameter;
  if (d.vertex_stride > kMaxVertexStride || d.attribute_count > caps_.max_vertex_attributes || d.varying_count > caps_.max_varyings || d.texture_count > caps_.max_texture_units)
    return status::kInvalidParameter;
  for (uint32_t s = 1; s < graphics::kMaxVertexBuffers; ++s)
    if (d.slot_strides[s] > kMaxVertexStride) return status::kInvalidParameter;
  // The pipeline has the texture units its shaders read, when they say which: a description may count more (a root signature with a table of textures
  // for shaders that sample none of them, or fewer), and a device takes no units for a pixel shader that samples nothing.
  uint32_t texture_count = d.texture_count;
  if (vertex->knows_textures_ && fragment->knows_textures_) {
    uint32_t used = 0, paired = 0;
    for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) {
      if (((vertex->texture_mask_ | fragment->texture_mask_) >> u) & 1) used = u + 1;
      // (a unit of a texture and sampler pair of their own is the pipeline's whatever the description counts: the binding fills it from the texture and sampler registers)
      if ((vertex->texture_mask_ >> u & 1) && vertex->unit_texture_[u] != u) paired = u + 1;
      if ((fragment->texture_mask_ >> u & 1) && fragment->unit_texture_[u] != u) paired = u + 1;
    }
    if (used < texture_count) texture_count = used;
    if (paired > texture_count) texture_count = paired;
  }
  uint32_t vertex_slots = 0, instance_slots = 0, slot_rates[graphics::kMaxVertexBuffers] = {};
  for (uint32_t i = 0; i < d.attribute_count; ++i) {
    const graphics::VertexAttribute& a = d.attributes[i];
    if (a.location >= caps_.max_vertex_attributes || a.slot >= caps_.max_vertex_buffers || static_cast<uint32_t>(a.format) >= graphics::kVertexFormatCount) return status::kInvalidParameter;
    if (static_cast<uint32_t>(a.format) > static_cast<uint32_t>(graphics::VertexFormat::Float4) && !(caps_.features & graphics::kFeatureVertexFormats)) return status::kNotSupported;
    const uint32_t stride = a.slot == 0 ? d.vertex_stride : d.slot_strides[a.slot];
    if (stride == 0 || a.offset > stride || graphics::vertex_format_bytes(a.format) > stride - a.offset) return status::kInvalidParameter;
    (a.per_instance ? instance_slots : vertex_slots) |= 1u << a.slot;
    if (a.per_instance && a.step_rate && (slot_rates[a.slot] == 0 || a.step_rate < slot_rates[a.slot])) slot_rates[a.slot] = a.step_rate;
  }
  const graphics::StencilState& st = d.stencil;
  if (static_cast<uint32_t>(d.cull_mode) > 2 || static_cast<uint32_t>(d.front_face) > 1 || static_cast<uint32_t>(st.compare) > 7 || static_cast<uint32_t>(st.fail_op) > 7 ||
      static_cast<uint32_t>(st.depth_fail_op) > 7 || static_cast<uint32_t>(st.pass_op) > 7)
    return status::kInvalidParameter;
  if (st.enable && !caps_.stencil) return status::kNotSupported;
  // The state beyond the base interface, for the devices that have it.
  const graphics::BlendState& bs = d.blend;
  constexpr uint32_t kLastFactor = graphics::kBlendFactorCount - 1;
  if (static_cast<uint32_t>(d.depth_compare) > 7 || static_cast<uint32_t>(bs.src_color) > kLastFactor || static_cast<uint32_t>(bs.dst_color) > kLastFactor ||
      static_cast<uint32_t>(bs.src_alpha) > kLastFactor || static_cast<uint32_t>(bs.dst_alpha) > kLastFactor || static_cast<uint32_t>(bs.color_op) > 4 ||
      static_cast<uint32_t>(bs.alpha_op) > 4 || bs.write_mask > 0xF)
    return status::kInvalidParameter;
  for (const graphics::BlendState& t : d.target_blend)
    if (static_cast<uint32_t>(t.src_color) > kLastFactor || static_cast<uint32_t>(t.dst_color) > kLastFactor || static_cast<uint32_t>(t.src_alpha) > kLastFactor ||
        static_cast<uint32_t>(t.dst_alpha) > kLastFactor || static_cast<uint32_t>(t.color_op) > 4 || static_cast<uint32_t>(t.alpha_op) > 4 || t.write_mask > 0xF)
      return status::kInvalidParameter;
  // (the constant factors: a device that has them, DeviceInfo::blend_constant)
  bool constant_factors = bs.custom && bs.uses_constant();
  if (d.independent_blend)
    for (const graphics::BlendState& t : d.target_blend) constant_factors = constant_factors || (t.custom && t.uses_constant());
  if (constant_factors && !caps_.blend_constant) return status::kNotSupported;
  // (dual-source blending: the Src1 factors of target 0 - a pipeline of one colour target, Direct3D's rule -; logic operations; the rasterizer state of Direct3D 10)
  bool dual_source = bs.custom && bs.uses_dual_source();
  if (d.independent_blend)
    for (const graphics::BlendState& t : d.target_blend) dual_source = dual_source || (t.custom && t.uses_dual_source());
  if (dual_source && !(caps_.features2 & graphics::kFeature2DualSourceBlend)) return status::kNotSupported;
  if (dual_source && d.color_target_count != 1) return status::kInvalidParameter;
  if (static_cast<uint32_t>(d.logic_op) >= graphics::kLogicOpCount || static_cast<uint32_t>(d.fill_mode) > 1) return status::kInvalidParameter;
  if (d.logic_op_enable && !(caps_.features2 & graphics::kFeature2LogicOps)) return status::kNotSupported;
  const graphics::StencilState& back = d.back_stencil;
  if (static_cast<uint32_t>(back.compare) > 7 || static_cast<uint32_t>(back.fail_op) > 7 || static_cast<uint32_t>(back.depth_fail_op) > 7 || static_cast<uint32_t>(back.pass_op) > 7) return status::kInvalidParameter;
  const graphics::DepthBias& bias = d.depth_bias;
  if ((bias.unorm_bits != 0 && bias.unorm_bits != 16 && bias.unorm_bits != 24) || bias.constant != bias.constant || bias.slope != bias.slope || bias.clamp != bias.clamp) return status::kInvalidParameter;
  const bool raster_state = d.fill_mode != graphics::FillMode::Solid || bias.enabled() || !d.depth_clip || d.separate_back_stencil;
  if (raster_state && !(caps_.features2 & graphics::kFeature2RasterizerState)) return status::kNotSupported;
  if (d.independent_blend && !(caps_.features & graphics::kFeatureIndependentBlend)) return status::kNotSupported;
  if ((d.multisample || d.alpha_to_coverage) && !(caps_.features & graphics::kFeatureMultisample)) return status::kNotSupported;
  if (d.alpha_to_coverage && !d.multisample) return status::kInvalidParameter;
  if (d.conservative && !(caps_.features2 & graphics::kFeature2ConservativeRaster)) return status::kNotSupported;
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
  if (d.color_target_count == 0 || d.color_target_count > caps_.max_color_targets) return status::kInvalidParameter;
  if (d.color_target_count > 1 && !(caps_.features & graphics::kFeatureMultipleTargets)) return status::kNotSupported;

  graphics::GraphicsPipelineDesc pd;
  pd.vertex_shader = vertex->module();
  pd.fragment_shader = fragment->module();
  pd.vertex_stride = d.vertex_stride;
  for (uint32_t s = 1; s < graphics::kMaxVertexBuffers; ++s) pd.slot_strides[s] = d.slot_strides[s];
  pd.attribute_count = d.attribute_count;
  for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
  pd.varying_count = d.varying_count;
  pd.texture_count = texture_count;
  pd.fragment_stores = fragment->fragment_stores_ || fragment->image_mask_.any();   // (a pixel shader that writes images runs for every fragment too)
  pd.vertex_stores = vertex->vertex_stores_;
  pd.geometry_stores = geometry && geometry->geometry_stores_;
  pd.tessellation_stores = (hull && hull->tessellation_stores_) || (domain && domain->tessellation_stores_);
  pd.cull_mode = d.cull_mode;
  pd.front_face = d.front_face;
  pd.depth_test = d.depth_test;
  pd.depth_write = d.depth_write;
  pd.alpha_blend = d.alpha_blend;
  pd.stencil = st;
  pd.blend = bs;
  pd.independent_blend = d.independent_blend;
  pd.multisample = d.multisample;
  pd.conservative = d.conservative;
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
  pd.fill_mode = d.fill_mode;
  pd.depth_bias = d.depth_bias;
  pd.depth_clip = d.depth_clip;
  pd.separate_back_stencil = d.separate_back_stencil;
  pd.back_stencil = d.back_stencil;
  pd.logic_op_enable = d.logic_op_enable;
  pd.logic_op = d.logic_op;

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
  pipeline->vertex_slot_mask_ = vertex_slots;
  pipeline->instance_slot_mask_ = instance_slots;
  for (uint32_t s = 0; s < graphics::kMaxVertexBuffers; ++s) pipeline->slot_step_rates_[s] = slot_rates[s];
  for (uint32_t s = 1; s < graphics::kMaxVertexBuffers; ++s) pipeline->slot_strides_[s] = d.slot_strides[s];
  for (uint32_t i = 0; i < d.attribute_count; ++i) pipeline->per_instance_ = pipeline->per_instance_ || d.attributes[i].per_instance;
  pipeline->attribute_count_ = d.attribute_count;
  pipeline->texture_count_ = texture_count;
  // (a unit has the kind the stage that reads it declared, and the texture and sampler pair it reads; a vertex and a pixel shader that read one unit as two kinds, or as two
  // pairs - each takes the units of its own pairs from the last down -, do not go together)
  for (uint32_t u = 0; u < graphics::kMaxTextureUnits; ++u) {
    const bool in_vertex = (vertex->texture_mask_ >> u) & 1, in_fragment = (fragment->texture_mask_ >> u) & 1;
    if (in_vertex && in_fragment && vertex->texture_kind(u) != fragment->texture_kind(u)) return status::kNotSupported;
    if (in_vertex && in_fragment && vertex->unit_texture_[u] != fragment->unit_texture_[u]) return status::kNotSupported;
    const bool vertex_samples = (vertex->sampled_mask_ >> u) & 1, fragment_samples = (fragment->sampled_mask_ >> u) & 1;
    if (vertex_samples && fragment_samples && vertex->unit_sampler_[u] != fragment->unit_sampler_[u]) return status::kNotSupported;
    pipeline->texture_kinds_[u] = in_vertex ? vertex->texture_kind(u) : fragment->texture_kind(u);
    const ShaderObject& reader = in_vertex ? *vertex : *fragment;
    pipeline->unit_texture_[u] = reader.unit_texture_[u];
    pipeline->unit_sampler_[u] = vertex_samples ? vertex->unit_sampler_[u] : fragment_samples ? fragment->unit_sampler_[u] : reader.unit_sampler_[u];
  }
  pipeline->color_target_count_ = d.color_target_count;
  pipeline->declared_units_ = vertex->texture_mask_ | fragment->texture_mask_ | (geometry ? geometry->texture_mask_ : 0u) | (hull ? hull->texture_mask_ : 0u) | (domain ? domain->texture_mask_ : 0u);
  pipeline->fragment_stores_ = fragment->fragment_stores_;
  // (the images of every stage: a slot two stages declare must be the same thing to both)
  for (const ShaderObject* stage : {vertex.get(), fragment.get(), geometry.get(), hull.get(), domain.get()}) {
    if (!stage) continue;
    for (uint32_t u = 0; u < graphics::kMaxImageSlots; ++u) {
      if (!stage->image_mask_.test(u)) continue;
      if (pipeline->image_mask_.test(u) && (pipeline->image_dims_[u] != stage->image_dims_[u] || pipeline->image_returns_[u] != stage->image_returns_[u])) return status::kInvalidParameter;
      pipeline->image_mask_.set(u);
      pipeline->image_dims_[u] = stage->image_dims_[u], pipeline->image_returns_[u] = stage->image_returns_[u];
    }
  }
  pipeline->vertex_stores_ = vertex->vertex_stores_;
  pipeline->geometry_stores_ = geometry && geometry->geometry_stores_;
  pipeline->tessellation_stores_ = (hull && hull->tessellation_stores_) || (domain && domain->tessellation_stores_);
  for (const ShaderObject* stage : {vertex.get(), fragment.get(), geometry.get(), hull.get(), domain.get()})   // (the buffers their shaders read and write)
    if (stage) pipeline->storage_mask_ |= stage->storage_mask_;
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
  runner.collect_decoders(&s->decoders);
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
  runner.replay_decoders(ArrayView<const SharedPointer<VideoDecoderObject>>(s.decoders.view().data(), s.decoders.count()));
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
  // (a copy for the present tap, when it wants this one: the image as the display shows it)
  IPresentTap* tap = __atomic_load_n(&present_tap_, __ATOMIC_ACQUIRE);
  if (st == status::kSuccess && tap && shown.image()) {
    const graphics::Extent2D e = shown.image()->extent();
    ArrayView<uint32_t> into = tap->present_wanted(e.width, e.height);
    if (!into.empty()) {      // (0 x 0: it could not be read)
      const bool read = into.size() >= uint64_t{e.width} * e.height && shown.image()->read_pixels(into.first(uint64_t{e.width} * e.height));
      tap->present_read(read ? e.width : 0, read ? e.height : 0);
    }
  }
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
