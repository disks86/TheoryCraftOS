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

#include "Hardware/VideoCodec/Intel/IntelAv1Session.hpp"
#include "Hardware/VideoCodec/Intel/IntelAv1Cdf.hpp"
#include "Hardware/VideoCodec/Software/Av1Obu.hpp"
#include "Hardware/VideoCodec/Software/Av1Warp.hpp"

namespace hardware {

namespace {
constexpr uint64_t kBitstreamPadding = 64;   // zeros behind the frame, for what the engine reads ahead
constexpr uint32_t kMinimumSide = 16;
constexpr uint32_t kMaxTiles = 128;
constexpr uint32_t kWorkspaceScratch = 0, kIntrabcScratch = 1;
constexpr uint64_t kPage = 4096;
constexpr uint32_t kScaleOne = 1u << 14;     // AV1's reference scale factors are 14 bit fixed point
constexpr uint32_t kMotionFieldStack = 3;    // how many references the motion field projection takes at most

uint32_t align8(uint32_t v) { return (v + 7) & ~7u; }
uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }

// The loop restoration type as the engine numbers it (none, Wiener, self-guided, switchable) from the parser's (none, switchable, Wiener, self-guided).
uint32_t restoration_code(av1::RestorationType t) {
  switch (t) {
    case av1::RESTORATION_WIENER: return 1;
    case av1::RESTORATION_SGRPROJ: return 2;
    case av1::RESTORATION_SWITCHABLE: return 3;
    default: return 0;
  }
}
}  // namespace

IntelAv1Decoder::IntelAv1Decoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), cpu_(allocator), host_bits_(allocator), host_planes_(allocator), host_cdf_(allocator), out_(allocator) {
  forget_references();
}

IntelAv1Decoder::~IntelAv1Decoder() = default;

void IntelAv1Decoder::forget_references() {
  for (int& s : ref_surface_) s = -1;
  for (SlotState& s : state_) s = SlotState();
}

bool IntelAv1Decoder::prepare() {
  if (!buffers_.lease()) return false;
  cpu_.set_offload(this);
  return config_.codec_config.empty() || cpu_.configure(config_.codec_config);
}

void IntelAv1Decoder::reset() {
  cpu_.reset();
  forget_references();
}

bool IntelAv1Decoder::accepts(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr) {
  if (!buffers_.leased()) return false;
  if (!buffers_.engine_ready()) return buffers_.is_open();   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  // Decided by the sequence, not by the frame: a stream whose frames some went to the engine and some to the CPU could not be decoded by either (the CPU decoder keeps no
  // entropy contexts or motion vectors of the frames the engine made).
  if (seq.hbd != 0 || seq.layout != av1::PIXEL_LAYOUT_I420 || seq.film_grain_present || seq.super_res || hdr.super_res.enabled || hdr.spatial_id != 0) return false;
  if (seq.max_width < static_cast<int>(kMinimumSide) || seq.max_height < static_cast<int>(kMinimumSide)) return false;
  if (seq.max_width > static_cast<int>(IntelEngineBuffers::kMaxMbsPerSide * 16) || seq.max_height > static_cast<int>(IntelEngineBuffers::kMaxMbsPerSide * 16)) return false;
  if (hdr.width[0] <= 0 || hdr.height <= 0 || uint64_t{hdr.tiling.cols} * hdr.tiling.rows > kMaxTiles) return false;
  return true;
}

// The buffers are made for the sequence's biggest picture (a smaller frame is decoded into the top left corner of a surface).
bool IntelAv1Decoder::open_for(const av1::SequenceHeader& seq) {
  const uint32_t width = align8(static_cast<uint32_t>(seq.max_width)), height = align8(static_cast<uint32_t>(seq.max_height));
  const bool sb128 = seq.sb128 != 0;
  if (buffers_.is_open() && open_width_ == width && open_height_ == height && open_sb128_ == sb128) return true;
  forget_references();
  open_width_ = open_height_ = 0;
  if (!buffers_.open(width, height)) return false;
  intel_avp::Av1Dimensions dims;
  const uint32_t sb = sb128 ? 128 : 64;
  dims.sb_wide = (width + sb - 1) / sb;
  dims.sb_high = (height + sb - 1) / sb;
  dims.sb128 = sb128;
  intel_avp::Av1BufferBytes bytes;
  if (!intel_avp::av1_buffer_bytes(dims, &bytes)) return false;
  aux_ = AuxLayout();
  aux_.cdf_init = 0;
  aux_.cdf_backward = round_up(intel_av1::kCdfContextBytes, kPage);
  aux_.segment_id = aux_.cdf_backward * 2;
  aux_.segment_id_bytes = round_up(bytes.segment_id, kPage);
  aux_.motion_vector = aux_.segment_id + aux_.segment_id_bytes;
  aux_.bytes = aux_.motion_vector + round_up(bytes.motion_vector, kPage);
  open_width_ = width;
  open_height_ = height;
  open_sb128_ = sb128;
  return true;
}

// A surface no reference slot holds.
int IntelAv1Decoder::pick_surface() const {
  for (int s = 0; s < static_cast<int>(IntelEngineBuffers::kMaxSurfaces); ++s) {
    bool used = false;
    for (int r : ref_surface_) used = used || r == s;
    if (!used) return s;
  }
  return -1;
}

// Which references take part in the motion field projection (the reference driver's SetupMotionFieldProjection): at most three, taken in an order of its own, and only
// those that are inter frames of the size of this one.
uint32_t IntelAv1Decoder::motion_field_mask(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr, uint32_t mi_cols, uint32_t mi_rows) const {
  int surface[7];
  uint32_t hint[7] = {};
  for (int i = 0; i < 7; ++i) {
    const int slot = hdr.refidx[i];
    surface[i] = slot >= 0 && slot < 8 ? ref_surface_[slot] : -1;
    if (surface[i] >= 0) hint[i] = state_[surface[i]].order_hint;
  }
  const int bits = seq.order_hint_n_bits;
  const uint32_t current = hdr.frame_offset;
  uint32_t mask = 0;
  auto usable = [&](int r) {
    if (surface[r] < 0) return false;
    const SlotState& s = state_[surface[r]];
    return s.valid && s.frame_type != av1::FRAME_TYPE_KEY && s.frame_type != av1::FRAME_TYPE_INTRA && s.mi_cols == mi_cols && s.mi_rows == mi_rows;
  };
  auto project = [&](int r) {
    if (!usable(r)) return false;
    mask |= 1u << r;
    return true;
  };
  constexpr int kLast = 0, kLast2 = 1, kGolden = 3, kBwd = 4, kAlt2 = 5, kAlt = 6;
  uint32_t stamp = kMotionFieldStack - 1;
  if (surface[kLast] >= 0) {
    if (state_[surface[kLast]].ref_order_hint[kAlt] != hint[kGolden]) project(kLast);
    --stamp;
  }
  if (av1::get_poc_diff(bits, static_cast<int>(hint[kBwd]), static_cast<int>(current)) > 0 && project(kBwd)) --stamp;
  if (av1::get_poc_diff(bits, static_cast<int>(hint[kAlt2]), static_cast<int>(current)) > 0 && project(kAlt2)) --stamp;
  if (av1::get_poc_diff(bits, static_cast<int>(hint[kAlt]), static_cast<int>(current)) > 0 && static_cast<int32_t>(stamp) >= 0 && project(kAlt)) --stamp;
  if (static_cast<int32_t>(stamp) >= 0 && surface[kLast2] >= 0 && project(kLast2)) --stamp;
  return mask;
}

bool IntelAv1Decoder::run_frame(const av1::OffloadFrame& frame, int current) {
  using namespace intel_avp;
  if (!frame.seq || !frame.hdr || !buffers_.engine_ready()) return false;
  const av1::SequenceHeader& seq = *frame.seq;
  const av1::FrameHeader& hdr = *frame.hdr;
  if (!open_for(seq)) return false;
  if (frame.width <= 0 || frame.height <= 0 || static_cast<uint32_t>(frame.width) > open_width_ || static_cast<uint32_t>(frame.height) > open_height_) return false;
  const uint32_t width = static_cast<uint32_t>(frame.width), height = static_cast<uint32_t>(frame.height);
  const uint32_t cols = hdr.tiling.cols, rows = hdr.tiling.rows, tiles = cols * rows;
  const bool sb128 = seq.sb128 != 0;
  const uint32_t sb = sb128 ? 128 : 64;
  const uint32_t sb_wide = (width + sb - 1) / sb, sb_high = (height + sb - 1) / sb;
  if (cols == 0 || rows == 0 || tiles > kMaxTiles || frame.tiles.size() != tiles || hdr.tiling.col_start_sb[cols] != sb_wide || hdr.tiling.row_start_sb[rows] != sb_high) return false;
  const bool key = hdr.frame_type == av1::FRAME_TYPE_KEY, intra = key || hdr.frame_type == av1::FRAME_TYPE_INTRA;
  const uint32_t mi_cols = align8(width) / 4, mi_rows = align8(height) / 4;
  const bool order_hints = seq.order_hint != 0;
  const int hint_bits = seq.order_hint_n_bits;
  const uint32_t current_hint = order_hints ? hdr.frame_offset : 0;

  AvpBuffers b;
  b.mocs = 0;
  uint32_t address = 0;
  if (!buffers_.surface(current, &address)) return false;
  b.current = address;

  // The pictures the frame predicts from, and what the engine kept for them.
  int surface[7];
  uint32_t aux_address[7] = {};
  AvpPicture p;
  p.reference_scale_vertical[0] = p.reference_scale_horizontal[0] = intra ? 0 : kScaleOne;
  uint32_t cur_aux = 0;
  if (!buffers_.picture_buffer(current, aux_.bytes, &cur_aux)) return false;
  if (!intra) {
    b.reference[0] = b.current;
    for (int i = 0; i < 7; ++i) {
      const int slot = hdr.refidx[i];
      if (slot < 0 || slot > 7 || ref_surface_[slot] < 0) return false;
      surface[i] = ref_surface_[slot];
      const SlotState& r = state_[surface[i]];
      if (r.width == 0 || r.height == 0) return false;
      if (!buffers_.surface(surface[i], &address)) return false;
      b.reference[i + 1] = address;
      if (!buffers_.picture_buffer(surface[i], aux_.bytes, &aux_address[i])) return false;
      // How the reference is scaled to this frame (14 bit fixed point, between 1/16 and 2).
      const uint32_t sh = (r.width * kScaleOne + (width >> 1)) / width, sv = (r.height * kScaleOne + (height >> 1)) / height;
      if (sh > 2 * kScaleOne || sv > 2 * kScaleOne || sh < kScaleOne / 16 || sv < kScaleOne / 16) return false;
      p.reference_scale_horizontal[i + 1] = sh;
      p.reference_scale_vertical[i + 1] = sv;
      p.reference_width[i + 1] = r.width;
      p.reference_height[i + 1] = r.height;
      const uint32_t hint = order_hints ? r.order_hint : 0;
      p.reference_order_hint[i + 1] = hint;
      if (order_hints) {
        const int dist = av1::get_poc_diff(hint_bits, static_cast<int>(hint), static_cast<int>(current_hint));
        if (dist > 0) p.sign_bias_mask |= 1u << (i + 1);
        if (dist > 0 || hint == current_hint) p.ref_frame_side |= 1u << (i + 1);
      }
    }
    p.reference_width[0] = width;
    p.reference_height[0] = height;
  } else {
    if (!key)
      for (uint64_t& r : b.reference) r = b.current;
    for (uint32_t i = 1; i < 8; ++i) p.reference_order_hint[i] = current_hint;
  }
  p.reference_order_hint[0] = current_hint;

  // The engine's line buffers, in one workspace.
  Av1Dimensions dims;
  dims.sb_wide = sb_wide;
  dims.sb_high = sb_high;
  dims.sb128 = sb128;
  dims.tile_columns = cols;
  dims.tiles = tiles;
  Av1BufferBytes sizes;
  if (!av1_buffer_bytes(dims, &sizes)) return false;
  uint64_t offset[kLineCount], total = 0;
  for (uint32_t i = 0; i < kLineCount; ++i) {
    offset[i] = total;
    total += round_up(sizes.line[i], kPage);
  }
  uint32_t workspace = 0;
  if (!buffers_.scratch(kWorkspaceScratch, total, &workspace)) return false;
  for (uint32_t i = 0; i < kLineCount; ++i) b.line[i] = uint64_t{workspace} + offset[i];
  if (hdr.allow_intrabc) {
    if (!buffers_.scratch(kIntrabcScratch, buffers_.layout().size, &address)) return false;
    b.intrabc = address;
  }

  // The entropy contexts the frame starts from: the defaults for its quantizer, or the tables the primary reference ended with. The engine leaves the adapted ones in the
  // backward adaptation buffer (when the frame updates them at its end).
  b.cdf_init = uint64_t{cur_aux} + aux_.cdf_init;
  b.cdf_backward = uint64_t{cur_aux} + aux_.cdf_backward;
  int primary = -1;
  if (hdr.primary_ref_frame != av1::PRIMARY_REF_NONE && !intra) {
    const int slot = hdr.refidx[hdr.primary_ref_frame];
    if (slot >= 0 && slot < 8) primary = ref_surface_[slot];
  }
  bool copied = false;
  if (primary >= 0 && state_[primary].cdf_valid) {
    const uint64_t from = state_[primary].frame_end_update ? aux_.cdf_backward : aux_.cdf_init;
    copied = buffers_.copy_picture_buffer(primary, from, current, aux_.cdf_init, intel_av1::kCdfContextBytes);
  }
  if (!copied) {
    host_cdf_.clear();
    if (!host_cdf_.put_zeros(intel_av1::kCdfContextBytes) || !intel_av1::build_default_cdf_context(intel_av1::default_context_for(hdr.quant.yac), host_cdf_.writable_view())) return false;
    if (!buffers_.write_picture_buffer(current, aux_.cdf_init, host_cdf_.view())) return false;
  }

  // The segmentation map: read from the primary reference's when the frame does not code its own, written when it does.
  const bool seg = hdr.segmentation.enabled != 0;
  const bool seg_reads = seg && (hdr.segmentation.temporal || !hdr.segmentation.update_map);
  const bool seg_writes = seg && hdr.segmentation.update_map;
  const bool seg_available = seg_reads && primary >= 0 && state_[primary].seg_valid && state_[primary].mi_cols == mi_cols && state_[primary].mi_rows == mi_rows;
  if (seg_available) {
    uint32_t primary_aux = 0;
    if (!buffers_.picture_buffer(primary, aux_.bytes, &primary_aux)) return false;
    b.segment_read = uint64_t{primary_aux} + aux_.segment_id;
  }
  if (seg_writes) b.segment_write = uint64_t{cur_aux} + aux_.segment_id;
  // (a map the frame does not code is the primary reference's, which later frames then find in this frame's own buffer)
  if (seg && !hdr.segmentation.update_map && seg_available && !buffers_.copy_picture_buffer(primary, aux_.segment_id, current, aux_.segment_id, aux_.segment_id_bytes)) return false;

  // The collocated motion vectors: this frame's, and those of the references.
  b.motion_vector_current = uint64_t{cur_aux} + aux_.motion_vector;
  if (!key) {
    b.motion_vector_collocated[0] = b.motion_vector_current;
    for (uint32_t i = 1; i < 8; ++i) b.motion_vector_collocated[i] = intra ? b.motion_vector_current : uint64_t{aux_address[i - 1]} + aux_.motion_vector;
  }

  // The tiles, back to back.
  host_bits_.clear();
  uint32_t tile_offset[kMaxTiles], tile_size[kMaxTiles];
  for (uint32_t t = 0; t < tiles; ++t) {
    const ConstByteView data = frame.tiles[t].data;
    if (data.size() == 0 || data.size() > UINT32_MAX / 2 || host_bits_.size() > UINT32_MAX / 2) return false;
    tile_offset[t] = static_cast<uint32_t>(host_bits_.size());
    tile_size[t] = static_cast<uint32_t>(data.size());
    if (!host_bits_.append(data)) return false;
  }
  if (!host_bits_.put_zeros(kBitstreamPadding) || !buffers_.upload_bitstream(host_bits_.view(), &address)) return false;
  const uint64_t bitstream = address, bitstream_bytes = host_bits_.size();

  // The frame state.
  p.width = width;
  p.height = height;
  p.sb128 = sb128;
  p.order_hint_enabled = order_hints;
  p.order_hint_bits = static_cast<uint32_t>(hint_bits);
  p.full_range = seq.color_range != 0;
  p.filter_intra = seq.filter_intra;
  p.intra_edge_filter = seq.intra_edge_filter;
  p.dual_filter = seq.dual_filter;
  p.inter_intra = seq.inter_intra;
  p.masked_compound = seq.masked_compound;
  p.joint_compound = seq.jnt_comp;
  p.screen_content_tools = hdr.allow_screen_content_tools;
  p.force_integer_mv = hdr.force_integer_mv;
  p.warped_motion = hdr.warp_motion;
  p.cdef = !(hdr.all_lossless || hdr.allow_intrabc || !seq.cdef);
  p.restoration = hdr.restoration.type[0] != av1::RESTORATION_NONE || hdr.restoration.type[1] != av1::RESTORATION_NONE || hdr.restoration.type[2] != av1::RESTORATION_NONE;
  p.frame_type = static_cast<uint32_t>(hdr.frame_type);
  p.error_resilient = hdr.error_resilient_mode;
  p.intrabc = hdr.allow_intrabc;
  p.primary_ref_frame = hdr.primary_ref_frame;
  p.segmentation_enabled = seg;
  p.segmentation_update_map = hdr.segmentation.update_map;
  p.segmentation_temporal_update = hdr.segmentation.temporal;
  p.segmentation_preskip = hdr.segmentation.seg_data.preskip;
  p.segmentation_last_active = static_cast<uint32_t>(hdr.segmentation.seg_data.last_active_segid) & 7;
  p.delta_q_present = hdr.delta.q.present;
  p.delta_q_res = hdr.delta.q.res_log2;
  p.coded_lossless = hdr.all_lossless;
  p.segment_id_in = seg_available;
  p.segment_id_out = seg_writes;
  p.segment_map_is_zero = seg_reads && !seg_available;
  p.base_qindex = hdr.quant.yac;
  p.y_dc_delta_q = hdr.quant.ydc_delta;
  p.u_dc_delta_q = hdr.quant.udc_delta;
  p.u_ac_delta_q = hdr.quant.uac_delta;
  p.v_dc_delta_q = hdr.quant.vdc_delta;
  p.v_ac_delta_q = hdr.quant.vac_delta;
  p.allow_high_precision_mv = hdr.hp;
  p.reference_select = hdr.switchable_comp_refs;
  p.interp_filter = static_cast<uint32_t>(hdr.subpel_filter_mode);
  p.motion_mode_switchable = hdr.switchable_motion_mode;
  p.use_ref_frame_mvs = hdr.use_ref_frame_mvs;
  p.order_hint = current_hint;
  p.reduced_tx_set = hdr.reduced_txtp_set;
  p.tx_mode = static_cast<uint32_t>(hdr.txfm_mode);
  p.skip_mode_present = hdr.skip_mode_enabled;
  if (!intra && order_hints && hdr.switchable_comp_refs && hdr.skip_mode_allowed) {
    p.skip_mode_frame[0] = static_cast<uint32_t>(hdr.skip_mode_refs[0]) + kLastFrame;
    p.skip_mode_frame[1] = static_cast<uint32_t>(hdr.skip_mode_refs[1]) + kLastFrame;
  }
  for (uint32_t i = 0; i < 7; ++i) {
    const av1::WarpedMotionParams& g = hdr.gmv[i];
    p.global_motion_type[i] = static_cast<uint32_t>(g.type);
    if (g.type > av1::WM_TYPE_TRANSLATION) {
      av1::WarpedMotionParams copy = g;
      p.global_motion_invalid[i] = av1::get_shear_params(&copy) != 0;
    }
    const AvpGlobalMotion words = av1_global_motion(static_cast<uint32_t>(g.type), g.matrix, hdr.hp != 0);
    for (uint32_t k = 0; k < 3; ++k) p.global_motion_words[3 * i + k] = words.words[k];
  }

  AvpInloop loop;
  loop.level_y_vertical = hdr.loopfilter.level_y[0];
  loop.level_y_horizontal = hdr.loopfilter.level_y[1];
  loop.level_u = hdr.loopfilter.level_u;
  loop.level_v = hdr.loopfilter.level_v;
  loop.sharpness = hdr.loopfilter.sharpness;
  loop.mode_ref_delta_enabled = hdr.loopfilter.mode_ref_delta_enabled;
  loop.delta_lf_res = hdr.delta.lf.res_log2;
  loop.delta_lf_multi = hdr.delta.lf.multi;
  loop.delta_lf_present = hdr.delta.lf.present;
  for (uint32_t i = 0; i < 8; ++i) loop.ref_delta[i] = hdr.loopfilter.mode_ref_deltas.ref_delta[i];
  for (uint32_t i = 0; i < 2; ++i) loop.mode_delta[i] = hdr.loopfilter.mode_ref_deltas.mode_delta[i];
  loop.cdef_damping_minus3 = hdr.cdef.damping >= 3 ? hdr.cdef.damping - 3u : 0;
  loop.cdef_bits = hdr.cdef.n_bits;
  for (uint32_t i = 0; i < 8; ++i) {
    loop.cdef_y_strength[i] = hdr.cdef.y_strength[i];
    loop.cdef_uv_strength[i] = hdr.cdef.uv_strength[i];
  }
  loop.upscaled_width = width;
  for (uint32_t i = 0; i < 3; ++i) loop.restoration_type[i] = restoration_code(hdr.restoration.type[i]);
  if (p.restoration) {
    loop.restoration_unit_size_luma = hdr.restoration.unit_size[0] >= 5 ? hdr.restoration.unit_size[0] - 5u : 0;
    loop.restoration_same_unit_size_chroma = (hdr.restoration.type[1] != av1::RESTORATION_NONE || hdr.restoration.type[2] != av1::RESTORATION_NONE) && hdr.restoration.unit_size[1] == hdr.restoration.unit_size[0];
  }

  // The batch.
  DwordWriter w(buffers_.batch_words());
  intel_mfx::emit_mi_flush(w);
  emit_mfx_wait(w);
  emit_avp_pipe_mode_select(w);
  emit_mfx_wait(w);
  emit_avp_surface_state(w, buffers_.layout(), kSurfaceRecon);
  if (!intra) {
    emit_avp_surface_state(w, buffers_.layout(), kSurfaceIntra);
    for (uint32_t i = 0; i < 7; ++i) emit_avp_surface_state(w, buffers_.layout(), kSurfaceLast + i);
  }
  if (hdr.allow_intrabc) emit_avp_surface_state(w, buffers_.layout(), kSurfaceIntrabc);
  emit_avp_pipe_buf_addr_state(w, b);
  emit_avp_ind_obj_base_addr_state(w, bitstream, bitstream_bytes, b.mocs);
  const uint32_t segments = seg ? 8 : 1;
  for (uint32_t s = 0; s < segments; ++s) {
    const av1::SegmentationData& d = hdr.segmentation.seg_data.d[s];
    AvpSegment sd;
    sd.id = s;
    // (the parser keeps the values of the features, not which of them are on: a feature that is on with the value zero does the same as one that is off)
    sd.feature_mask = (d.delta_q != 0 ? 1u : 0u) | (d.delta_lf_y_v != 0 ? 2u : 0u) | (d.delta_lf_y_h != 0 ? 4u : 0u) | (d.delta_lf_u != 0 ? 8u : 0u) | (d.delta_lf_v != 0 ? 16u : 0u) |
                      (d.ref >= 0 ? 32u : 0u) | (d.skip ? 64u : 0u) | (d.globalmv ? 128u : 0u);
    sd.delta_q = d.delta_q;
    sd.skip = d.skip != 0;
    sd.global_mv = d.globalmv != 0;
    sd.lossless = hdr.segmentation.lossless[s] != 0;
    sd.qm_level_y = hdr.quant.qm && !sd.lossless ? hdr.quant.qm_y : 15;
    sd.qm_level_u = hdr.quant.qm && !sd.lossless ? hdr.quant.qm_u : 15;
    sd.qm_level_v = hdr.quant.qm && !sd.lossless ? hdr.quant.qm_v : 15;
    sd.delta_lf_y_vertical = d.delta_lf_y_v;
    sd.delta_lf_y_horizontal = d.delta_lf_y_h;
    sd.delta_lf_u = d.delta_lf_u;
    sd.delta_lf_v = d.delta_lf_v;
    sd.reference_frame = d.ref < 0 ? 0u : static_cast<uint32_t>(d.ref) & 7u;
    emit_avp_segment_state(w, sd);
  }
  emit_avp_pic_state(w, p);
  emit_avp_inloop_filter_state(w, loop);
  if (!intra && order_hints) {
    AvpInterPred ip;
    for (uint32_t r = 0; r < 7; ++r)
      for (uint32_t j = 0; j < 7; ++j) ip.saved_order_hint[r][j] = state_[surface[r]].ref_order_hint[j];
    ip.motion_field_mask = motion_field_mask(seq, hdr, mi_cols, mi_rows);
    emit_avp_inter_pred_state(w, ip);
  }
  for (uint32_t t = 0; t < tiles; ++t) {
    const uint32_t col = t % cols, row = t / cols;
    AvpTile tile;
    tile.id = t;
    tile.group_num = t;
    tile.group_id = 0;
    tile.column_sb = hdr.tiling.col_start_sb[col];
    tile.row_sb = hdr.tiling.row_start_sb[row];
    tile.width_sb = hdr.tiling.col_start_sb[col + 1] - hdr.tiling.col_start_sb[col];
    tile.height_sb = hdr.tiling.row_start_sb[row + 1] - hdr.tiling.row_start_sb[row];
    tile.last_of_column = row == rows - 1;
    tile.last_of_row = col == cols - 1;
    tile.first_of_group = t == 0;
    tile.last_of_group = t == tiles - 1;
    tile.last_of_frame = t == tiles - 1;
    tile.disable_cdf_update = hdr.disable_cdf_update != 0;
    tile.disable_frame_context_update = !hdr.refresh_context || t != hdr.tiling.update;
    tile.columns = cols;
    tile.rows = rows;
    emit_avp_tile_coding(w, tile);
    emit_avp_bsd_object(w, tile_size[t], tile_offset[t]);
  }
  emit_vd_pipeline_flush(w);
  intel_mfx::emit_mi_flush(w);
  intel_mfx::emit_batch_end(w);
  if (!w.ok() || !buffers_.run(ArrayView<const uint32_t>(buffers_.batch_words().data(), w.count()))) return false;

  // What the next frames find.
  SlotState& s = state_[current];
  s.valid = true;
  s.cdf_valid = true;
  s.frame_end_update = hdr.refresh_context != 0;
  s.seg_valid = seg && (seg_writes || seg_available);
  s.mv_valid = !key;
  return true;
}

// The decoded surface is tiled NV12: read it into the planes of 8-bit samples, then the cropped frame into the CPU decoder's 16-bit planes.
void IntelAv1Decoder::read_back(const av1::OffloadFrame& frame, int surface) {
  const uint32_t w = buffers_.width(), h = buffers_.height(), cw = w / 2, ch = h / 2;
  host_planes_.clear();
  if (!host_planes_.put_zeros(uint64_t{w} * h + 2 * uint64_t{cw} * ch)) return;
  ByteView all = host_planes_.writable_view();
  ByteView y = all.first(uint64_t{w} * h), u = all.subview(uint64_t{w} * h, uint64_t{cw} * ch), v = all.subview(uint64_t{w} * h + uint64_t{cw} * ch, uint64_t{cw} * ch);
  if (!buffers_.read_back(surface, y, w, u, cw, v, cw, 0, 1)) return;
  ByteView planes[3] = {y, u, v};
  const uint32_t strides[3] = {w, cw, cw};
  const int widths[3] = {frame.width, (frame.width + 1) / 2, (frame.width + 1) / 2};
  const int heights[3] = {frame.height, (frame.height + 1) / 2, (frame.height + 1) / 2};
  for (int p = 0; p < 3; ++p)
    for (int row = 0; row < heights[p]; ++row)
      for (int x = 0; x < widths[p]; ++x) frame.plane[p].at(x, row) = planes[p][uint64_t{static_cast<uint32_t>(row)} * strides[p] + static_cast<uint32_t>(x)];
}

bool IntelAv1Decoder::decode_frame(const av1::OffloadFrame& frame) {
  // (a new sequence's buffers first: opening them forgets the references)
  const bool opened = frame.seq && buffers_.engine_ready() && open_for(*frame.seq);
  const int current = pick_surface();
  if (current >= 0 && frame.hdr) {
    // (what later frames need of this one, whether or not the engine decodes it)
    const av1::FrameHeader& hdr = *frame.hdr;
    SlotState s;
    s.frame_type = static_cast<uint32_t>(hdr.frame_type);
    s.order_hint = frame.seq && frame.seq->order_hint ? hdr.frame_offset : 0;
    s.width = static_cast<uint32_t>(frame.width);
    s.height = static_cast<uint32_t>(frame.height);
    s.mi_cols = align8(s.width) / 4;
    s.mi_rows = align8(s.height) / 4;
    const bool inter = hdr.frame_type == av1::FRAME_TYPE_INTER || hdr.frame_type == av1::FRAME_TYPE_SWITCH;
    if (inter)
      for (int i = 0; i < 7; ++i) {
        const int slot = hdr.refidx[i];
        if (slot >= 0 && slot < 8 && ref_surface_[slot] >= 0 && frame.seq && frame.seq->order_hint) s.ref_order_hint[i] = state_[ref_surface_[slot]].order_hint;
      }
    state_[current] = s;
  }
  bool ok = opened && current >= 0 && frame.hdr && run_frame(frame, current);
  if (ok) {
    read_back(frame, current);
    ++decoded_;
  } else {
    ++failed_;
    if (current >= 0) {
      buffers_.grey(current);
      state_[current].valid = false;
    }
  }
  // The reference slots the frame refreshes hold it from now on, decoded or not (a damaged frame is what the stream's later frames would refer to).
  if (current >= 0 && frame.hdr)
    for (unsigned i = 0; i < 8; ++i)
      if (frame.hdr->refresh_frame_flags & (1u << i)) ref_surface_[i] = current;
  return ok;
}

void IntelAv1Decoder::show_existing(unsigned idx, bool refresh_all) {
  if (!refresh_all || idx >= 8) return;
  const int surface = ref_surface_[idx];
  for (int& s : ref_surface_) s = surface;
}

SubmitResult IntelAv1Decoder::submit(const VideoPacket& packet) {
  if (!buffers_.leased()) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (cpu_.ready()) return SubmitResult::Full;
  if (!cpu_.decode(packet.data, packet.pts_us)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  ++stats_.packets;
  stats_.bytes += packet.data.size();
  return SubmitResult::Accepted;
}

bool IntelAv1Decoder::receive(VideoFrame* frame) {
  VideoFrame in;
  if (!cpu_.receive(&in)) return false;
  if (!out_.allocate({in.format.width, in.format.height, config_.output}) || !convert_frame(in, out_)) {
    ++stats_.errors;
    return false;
  }
  *frame = out_.frame(in.pts_us, in.keyframe);
  frame->limited_range = in.limited_range && config_.output != PixelFormat::Bgra8888;
  ++stats_.frames;
  return true;
}

VideoFormat IntelAv1Decoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics IntelAv1Decoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
