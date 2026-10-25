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

#include "Hardware/VideoCodec/Intel/IntelAvpCommands.hpp"

namespace hardware {
namespace intel_avp {

namespace {
constexpr uint64_t kCacheLine = 64, kPage = 4096;
constexpr uint32_t kMaxTileWidthPixels = 4096;

// A command being put together, field by field: dword and bit range as the register description gives them. A field that does not fit is remembered (the command is then
// not emitted).
class Fields {
 public:
  explicit Fields(ArrayView<uint32_t> dwords) : d_(dwords) { d_.fill(0); }
  void set(uint32_t dw, uint32_t lo, uint32_t hi, uint64_t value) {
    if (dw >= d_.size() || lo > hi || hi > 31) { bad_ = true; return; }
    const uint32_t bits = hi - lo + 1;
    const uint32_t mask = bits == 32 ? ~0u : ((1u << bits) - 1);
    d_[dw] = (d_[dw] & ~(mask << lo)) | ((static_cast<uint32_t>(value) & mask) << lo);
  }
  void flag(uint32_t dw, uint32_t bit, bool value) { set(dw, bit, bit, value ? 1 : 0); }
  void whole(uint32_t dw, uint32_t value) { set(dw, 0, 31, value); }
  // A 64-bit address in dwords `dw` and `dw + 1`, with the memory attributes in dword `attribute_dw`. An absent buffer leaves the address zero (the attributes are given anyway).
  void buffer(uint32_t dw, uint32_t attribute_dw, uint64_t address, uint32_t attributes) {
    whole(dw, static_cast<uint32_t>(address));
    whole(dw + 1, static_cast<uint32_t>(address >> 32));
    whole(attribute_dw, attributes);
  }
  bool ok() const { return !bad_; }
  ArrayView<const uint32_t> words() const { return d_; }

 private:
  ArrayView<uint32_t> d_;
  bool bad_ = false;
};

void emit_words(DwordWriter& w, const Fields& f) {
  if (!f.ok()) {
    // Never reached with the callers' values; if it ever is, the batch is made unusable (a stray dword in front of the end marker is refused by the driver).
    w.emit(0xFFFFFFFFu);
    return;
  }
  for (uint32_t v : f.words()) w.emit(v);
}

uint32_t attributes_of(uint32_t mocs) { return mocs << 1; }

enum class Dim { One, TileWidth, FrameWidth, FrameHeight, TileColumns, Tiles };
struct Entry {
  Av1Line line;
  uint8_t lines[2];       // cache lines (per unit of the dimension) for 64 and 128 superblocks
  uint8_t extra[2];       // cache lines added once
  Dim dim;
  bool page;              // plus a page (it fixes a page fault the reference driver saw)
};
// iHD's CodecAv1BufferSize and CodecAv1BufferSizeExt for 8-bit streams.
constexpr Entry kEntries[] = {
    {kBsdLine, {2, 4}, {0, 0}, Dim::TileWidth, false},
    {kBsdTileLine, {2, 4}, {0, 0}, Dim::FrameWidth, false},
    {kIntraPredLine, {2, 4}, {0, 0}, Dim::TileWidth, false},
    {kIntraPredTileLine, {2, 4}, {0, 0}, Dim::FrameWidth, false},
    {kSpatialMvLine, {4, 8}, {0, 0}, Dim::TileWidth, false},
    {kSpatialMvTileLine, {4, 8}, {0, 0}, Dim::FrameWidth, false},
    {kLrMetaTileColumn, {1, 1}, {1, 1}, Dim::FrameHeight, true},
    {kLrTileLineY, {7, 7}, {0, 0}, Dim::TileColumns, false},
    {kLrTileLineU, {5, 5}, {0, 0}, Dim::TileColumns, false},
    {kLrTileLineV, {5, 5}, {0, 0}, Dim::TileColumns, false},
    {kDeblockLineY, {9, 17}, {0, 0}, Dim::TileWidth, false},
    {kDeblockLineU, {3, 4}, {0, 0}, Dim::TileWidth, false},
    {kDeblockLineV, {3, 4}, {0, 0}, Dim::TileWidth, false},
    {kDeblockTileLineY, {9, 17}, {0, 0}, Dim::FrameWidth, false},
    {kDeblockTileLineV, {3, 4}, {0, 0}, Dim::FrameWidth, false},
    {kDeblockTileLineU, {3, 4}, {0, 0}, Dim::FrameWidth, false},
    {kDeblockTileColumnY, {8, 16}, {0, 0}, Dim::FrameHeight, false},
    {kDeblockTileColumnU, {2, 4}, {0, 0}, Dim::FrameHeight, false},
    {kDeblockTileColumnV, {2, 4}, {0, 0}, Dim::FrameHeight, false},
    {kCdefLine, {8, 16}, {1, 1}, Dim::TileWidth, false},
    {kCdefTileLine, {8, 16}, {1, 1}, Dim::FrameWidth, false},
    {kCdefTileColumn, {8, 16}, {1, 1}, Dim::FrameHeight, true},
    {kCdefMetaTileLine, {1, 1}, {0, 0}, Dim::TileColumns, false},
    {kCdefMetaTileColumn, {1, 1}, {1, 1}, Dim::FrameHeight, true},
    {kCdefTopLeft, {1, 1}, {0, 0}, Dim::Tiles, false},
    {kSuperResTileColumnY, {22, 44}, {22, 44}, Dim::FrameHeight, true},
    {kSuperResTileColumnU, {8, 16}, {8, 16}, Dim::FrameHeight, true},
    {kSuperResTileColumnV, {8, 16}, {8, 16}, Dim::FrameHeight, true},
    {kLrTileColumnY, {9, 17}, {2, 2}, Dim::FrameHeight, true},
    {kLrTileColumnU, {5, 9}, {1, 1}, Dim::FrameHeight, true},
    {kLrTileColumnV, {5, 9}, {1, 1}, Dim::FrameHeight, true},
    {kFrameStatus, {1, 1}, {0, 0}, Dim::One, false},
    {kBlockStreamout, {1, 1}, {0, 0}, Dim::One, false},
};
static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == kLineCount, "every buffer has a size");

// The dword of AVP_PIPE_BUF_ADDR_STATE each line buffer's address starts at.
constexpr uint8_t kLineDword[kLineCount] = {
    62, 65, 68, 71, 74, 77, 80, 83, 86, 89, 92, 95, 98, 101, 104, 107, 110, 113, 116, 119, 128, 137, 140, 143, 146, 149, 152, 155, 158, 161, 164, 176, 179,
};

uint32_t signed_field(int32_t v, uint32_t bits) { return static_cast<uint32_t>(v) & ((1u << bits) - 1); }
}  // namespace

bool av1_buffer_bytes(const Av1Dimensions& d, Av1BufferBytes* out) {
  if (d.sb_wide == 0 || d.sb_high == 0 || d.tile_columns == 0 || d.tiles == 0 || d.sb_wide > 1024 || d.sb_high > 1024 || d.tiles > 4096 || d.tile_columns > 64) return false;
  const uint32_t sb_pixels = d.sb128 ? 128 : 64;
  const uint32_t tile_width = (kMaxTileWidthPixels + sb_pixels - 1) / sb_pixels;
  const uint64_t total = uint64_t{d.sb_wide} * d.sb_high;
  const uint32_t s = d.sb128 ? 1 : 0;
  *out = Av1BufferBytes();
  for (const Entry& e : kEntries) {
    uint64_t units = 1;
    switch (e.dim) {
      case Dim::One: units = 1; break;
      case Dim::TileWidth: units = tile_width; break;
      case Dim::FrameWidth: units = d.sb_wide; break;
      case Dim::FrameHeight: units = d.sb_high; break;
      case Dim::TileColumns: units = d.tile_columns; break;
      case Dim::Tiles: units = d.tiles; break;
    }
    uint64_t bytes = (units * e.lines[s] + e.extra[s]) * kCacheLine;
    // (the frame status and the block streout buffers are a single cache line; the ones with extra space add a page)
    if (e.page) bytes += kPage;
    out->line[e.line] = bytes;
  }
  out->segment_id = (d.sb128 ? 8 : 2) * total * kCacheLine;
  out->motion_vector = (d.sb128 ? 16 : 4) * total * kCacheLine;
  return true;
}

void emit_mfx_wait(DwordWriter& w) { w.emit(kMfxWait); }

void emit_vd_pipeline_flush(DwordWriter& w) {
  w.emit(kVdPipelineFlush | (2 - 2));
  // The AV1 pipeline and the command parser are waited for, and the AV1 pipeline's commands flushed.
  w.emit((1u << 5) | (1u << 4) | (1u << 20));
}

void emit_avp_pipe_mode_select(DwordWriter& w) {
  uint32_t d[kPipeModeSelectDwords];
  Fields f{ArrayView<uint32_t>(d)};
  f.set(0, 0, 31, kAvpPipeModeSelect | (kPipeModeSelectDwords - 2));
  f.set(1, 0, 0, 0);          // decode
  f.set(1, 5, 7, 2);          // AV1
  f.set(1, 13, 14, 0);        // a single engine
  f.set(1, 15, 16, 0);        // legacy single-pipe mode
  emit_words(w, f);
}

void emit_avp_surface_state(DwordWriter& w, const intel_mfx::Nv12TiledLayout& layout, uint32_t id) {
  uint32_t d[kSurfaceStateDwords];
  Fields f{ArrayView<uint32_t>(d)};
  f.set(0, 0, 31, kAvpSurfaceState | (kSurfaceStateDwords - 2));
  f.set(1, 0, 16, layout.pitch - 1);
  f.set(1, 28, 31, id);
  f.set(2, 0, 14, layout.chroma_offset_rows);
  f.set(2, 27, 31, 4);        // planar 4:2:0, 8 bits
  emit_words(w, f);
}

void emit_avp_pipe_buf_addr_state(DwordWriter& w, const AvpBuffers& b) {
  uint32_t d[kPipeBufAddrDwords];
  Fields f{ArrayView<uint32_t>(d)};
  const uint32_t attr = attributes_of(b.mocs);
  f.set(0, 0, 31, kAvpPipeBufAddrState | (kPipeBufAddrDwords - 2));
  for (uint32_t i = 0; i < 8; ++i) f.buffer(1 + 2 * i, 17, b.reference[i], attr);
  f.buffer(18, 20, b.current, attr);
  f.buffer(24, 26, b.intrabc, attr);
  f.buffer(27, 29, b.cdf_init, attr);
  f.buffer(30, 32, b.cdf_backward, attr);
  f.buffer(33, 35, b.segment_read, attr);
  f.buffer(36, 38, b.segment_write, attr);
  for (uint32_t i = 0; i < 8; ++i) f.buffer(39 + 2 * i, 55, b.motion_vector_collocated[i], attr);
  f.buffer(56, 58, b.motion_vector_current, attr);
  for (uint32_t i = 0; i < kLineCount; ++i) f.buffer(kLineDword[i], kLineDword[i] + 2, b.line[i], attr);
  emit_words(w, f);
}

void emit_avp_ind_obj_base_addr_state(DwordWriter& w, uint64_t bitstream, uint64_t bitstream_bytes, uint32_t mocs) {
  uint32_t d[kIndObjDwords];
  Fields f{ArrayView<uint32_t>(d)};
  f.set(0, 0, 31, kAvpIndObjBaseAddrState | (kIndObjDwords - 2));
  f.buffer(1, 3, bitstream, attributes_of(mocs));
  const uint64_t upper = bitstream + ((bitstream_bytes + 4095) & ~uint64_t{4095});
  f.whole(4, static_cast<uint32_t>(upper));
  f.whole(5, static_cast<uint32_t>(upper >> 32));
  emit_words(w, f);
}

AvpGlobalMotion av1_global_motion(uint32_t type, const int32_t matrix[6], bool allow_high_precision_mv) {
  // iHD's CalculateGlobalMotionParams: the rotation/zoom terms lose the 1.0 and two bits of precision, the translation terms what the type does not use.
  constexpr int32_t kWarpedModelPrecBits = 16, kAlphaPrecDiff = 1, kTransOnlyPrecDiff = 13, kTransPrecDiff = 10;
  constexpr uint32_t kTranslation = 1, kRotZoom = 2, kAffine = 3;
  int32_t m[6];
  for (uint32_t i = 0; i < 6; ++i) m[i] = matrix[i];
  if (type >= kRotZoom) {
    m[2] = (m[2] - (1 << kWarpedModelPrecBits)) >> kAlphaPrecDiff;
    m[3] >>= kAlphaPrecDiff;
  }
  if (type == kAffine) {
    m[4] >>= kAlphaPrecDiff;
    m[5] = (m[5] - (1 << kWarpedModelPrecBits)) >> kAlphaPrecDiff;
  } else {
    m[4] = -m[3];
    m[5] = m[2];
  }
  if (type >= kTranslation) {
    const int32_t shift = type == kTranslation ? kTransOnlyPrecDiff + (allow_high_precision_mv ? 0 : 1) : kTransPrecDiff;
    m[0] >>= shift;
    m[1] >>= shift;
  }
  AvpGlobalMotion g;
  for (uint32_t i = 0; i < 3; ++i) g.words[i] = (static_cast<uint32_t>(m[2 * i]) & 0xFFFF) | (static_cast<uint32_t>(m[2 * i + 1]) << 16);
  return g;
}

void emit_avp_pic_state(DwordWriter& w, const AvpPicture& p) {
  uint32_t d[kPicStateDwords];
  Fields f{ArrayView<uint32_t>(d)};
  f.set(0, 0, 31, kAvpPicState | (kPicStateDwords - 2));
  f.set(1, 0, 15, p.width - 1);
  f.set(1, 16, 31, p.height - 1);
  f.set(2, 0, 1, 1);                                   // 4:2:0
  f.set(2, 3, 4, 0);                                   // 8 bits
  f.flag(2, 6, p.full_range);
  f.set(2, 7, 8, p.sb128 ? 1 : 0);
  f.flag(2, 9, p.order_hint_enabled);
  f.set(2, 10, 12, p.order_hint_enabled && p.order_hint_bits > 0 ? p.order_hint_bits - 1 : 0);
  f.flag(2, 16, p.filter_intra);
  f.flag(2, 17, p.intra_edge_filter);
  f.flag(2, 18, p.dual_filter);
  f.flag(2, 19, p.inter_intra);
  f.flag(2, 20, p.masked_compound);
  f.flag(2, 21, p.joint_compound);
  f.flag(3, 0, p.screen_content_tools);
  f.flag(3, 1, p.force_integer_mv);
  f.flag(3, 2, p.warped_motion);
  f.flag(3, 4, p.cdef);
  f.flag(3, 5, false);                                 // no super-resolution
  f.flag(3, 6, p.restoration);
  f.flag(3, 8, false);                                 // no large scale tile
  f.set(3, 16, 17, p.frame_type);
  f.flag(3, 19, p.frame_type == 0 || p.frame_type == 2);
  f.flag(3, 22, p.error_resilient);
  f.flag(3, 23, p.intrabc);
  f.set(3, 28, 30, p.primary_ref_frame);
  f.flag(4, 0, p.segmentation_enabled);
  f.flag(4, 1, p.segmentation_update_map);
  f.flag(4, 2, p.segmentation_temporal_update && p.frame_type != 0 && p.frame_type != 2);   // (an intra frame never predicts its map)
  f.flag(4, 3, p.segmentation_preskip);
  f.set(4, 4, 6, p.segmentation_last_active);
  f.flag(4, 7, p.delta_q_present);
  f.set(4, 8, 9, p.delta_q_res);
  f.flag(4, 10, p.coded_lossless);
  f.flag(4, 11, p.segment_map_is_zero);
  f.flag(4, 12, p.segment_id_in);
  f.flag(4, 13, p.segment_id_out);
  f.set(4, 16, 23, p.base_qindex);
  f.set(4, 24, 30, signed_field(p.y_dc_delta_q, 7));
  f.set(5, 0, 6, signed_field(p.u_dc_delta_q, 7));
  f.set(5, 8, 14, signed_field(p.u_ac_delta_q, 7));
  f.set(5, 16, 22, signed_field(p.v_dc_delta_q, 7));
  f.set(5, 24, 30, signed_field(p.v_ac_delta_q, 7));
  f.flag(6, 0, p.allow_high_precision_mv);
  f.flag(6, 1, p.reference_select);
  f.set(6, 2, 4, p.interp_filter);
  f.flag(6, 6, p.motion_mode_switchable);
  f.flag(6, 7, p.use_ref_frame_mvs);
  f.set(6, 8, 15, p.sign_bias_mask);
  f.set(6, 16, 23, p.order_hint);
  f.flag(7, 0, p.reduced_tx_set);
  f.set(7, 1, 2, p.tx_mode);
  f.flag(7, 4, p.skip_mode_present);
  f.set(7, 5, 7, p.skip_mode_frame[0]);
  f.set(7, 9, 11, p.skip_mode_frame[1]);
  f.set(7, 24, 31, p.ref_frame_side);
  uint32_t invalid = 0;
  for (uint32_t i = 0; i < 7; ++i) {
    f.set(8, 3 + 3 * i, 4 + 3 * i, p.global_motion_type[i]);
    if (p.global_motion_invalid[i]) invalid |= 1u << (i + 1);
  }
  f.set(8, 24, 31, invalid);
  for (uint32_t i = 0; i < 21; ++i) f.whole(9 + i, p.global_motion_words[i]);
  for (uint32_t i = 0; i < 8; ++i) f.set(30, 4 * i, 4 * i + 2, i);   // the reference names, in order
  for (uint32_t i = 0; i < 8; ++i) {
    f.set(31 + i, 0, 15, p.reference_width[i] ? p.reference_width[i] - 1 : 0);
    f.set(31 + i, 16, 31, p.reference_height[i] ? p.reference_height[i] - 1 : 0);
    f.set(39 + i, 0, 15, p.reference_scale_vertical[i]);
    f.set(39 + i, 16, 31, p.reference_scale_horizontal[i]);
  }
  for (uint32_t i = 0; i < 8; ++i) f.set(47 + i / 4, 8 * (i % 4), 8 * (i % 4) + 7, p.reference_order_hint[i]);
  emit_words(w, f);
}

void emit_avp_segment_state(DwordWriter& w, const AvpSegment& s) {
  uint32_t d[kSegmentStateDwords];
  Fields f{ArrayView<uint32_t>(d)};
  f.set(0, 0, 31, kAvpSegmentState | (kSegmentStateDwords - 2));
  f.set(1, 0, 2, s.id);
  f.set(2, 0, 7, s.feature_mask);
  f.set(2, 8, 16, signed_field(s.delta_q, 9));
  f.flag(2, 17, s.skip);
  f.flag(2, 18, s.global_mv);
  f.flag(2, 19, s.lossless);
  f.set(2, 20, 23, s.qm_level_y);
  f.set(2, 24, 27, s.qm_level_u);
  f.set(2, 28, 31, s.qm_level_v);
  f.set(3, 0, 6, signed_field(s.delta_lf_y_vertical, 7));
  f.set(3, 7, 13, signed_field(s.delta_lf_y_horizontal, 7));
  f.set(3, 14, 20, signed_field(s.delta_lf_u, 7));
  f.set(3, 21, 27, signed_field(s.delta_lf_v, 7));
  f.set(3, 28, 30, s.reference_frame);
  emit_words(w, f);
}

void emit_avp_inloop_filter_state(DwordWriter& w, const AvpInloop& l) {
  uint32_t d[kInloopFilterDwords];
  Fields f{ArrayView<uint32_t>(d)};
  f.set(0, 0, 31, kAvpInloopFilterState | (kInloopFilterDwords - 2));
  f.set(1, 0, 5, l.level_y_vertical);
  f.set(1, 6, 11, l.level_y_horizontal);
  f.set(1, 12, 17, l.level_u);
  f.set(1, 18, 23, l.level_v);
  f.set(1, 24, 26, l.sharpness);
  f.flag(1, 27, l.mode_ref_delta_enabled);
  f.set(1, 28, 29, l.delta_lf_res);
  f.flag(1, 30, l.delta_lf_multi);
  f.flag(1, 31, l.delta_lf_present);
  for (uint32_t i = 0; i < 8; ++i) f.set(2 + i / 4, 8 * (i % 4), 8 * (i % 4) + 6, signed_field(l.ref_delta[i], 7));
  for (uint32_t i = 0; i < 2; ++i) f.set(4, 8 * i, 8 * i + 6, signed_field(l.mode_delta[i], 7));
  for (uint32_t i = 0; i < 4; ++i) f.set(5, 6 * i, 6 * i + 5, l.cdef_y_strength[i]);
  f.set(5, 28, 29, l.cdef_bits);
  f.set(5, 30, 31, l.cdef_damping_minus3);
  for (uint32_t i = 0; i < 4; ++i) f.set(6, 6 * i, 6 * i + 5, l.cdef_y_strength[4 + i]);
  for (uint32_t i = 0; i < 4; ++i) f.set(7, 6 * i, 6 * i + 5, l.cdef_uv_strength[i]);
  for (uint32_t i = 0; i < 4; ++i) f.set(8, 6 * i, 6 * i + 5, l.cdef_uv_strength[4 + i]);
  f.set(9, 0, 15, l.upscaled_width ? l.upscaled_width - 1 : 0);
  f.set(9, 16, 20, 8);                                 // no super-resolution: the denominator is 8
  f.flag(9, 21, false);
  f.set(10, 0, 1, l.restoration_type[0]);
  f.set(10, 2, 3, l.restoration_type[1]);
  f.set(10, 4, 5, l.restoration_type[2]);
  f.set(10, 8, 9, l.restoration_unit_size_luma);
  f.flag(10, 10, l.restoration_same_unit_size_chroma);
  emit_words(w, f);
}

void emit_avp_inter_pred_state(DwordWriter& w, const AvpInterPred& p) {
  uint32_t d[kInterPredDwords];
  Fields f{ArrayView<uint32_t>(d)};
  f.set(0, 0, 31, kAvpInterPredState | (kInterPredDwords - 2));
  // Seven order hints for each of the seven references, three bytes then four in the dwords of the next, the first dword also taking the motion field mask.
  for (uint32_t r = 0; r < 7; ++r) {
    for (uint32_t j = 0; j < 7; ++j) {
      const uint32_t group = r * 2 + (j >= 4 ? 1 : 0);   // dword 1 + group
      const uint32_t slot = j >= 4 ? j - 4 : j;
      f.set(1 + group, 8 * slot, 8 * slot + 7, p.saved_order_hint[r][j]);
    }
  }
  f.set(2, 24, 31, p.motion_field_mask);
  emit_words(w, f);
}

void emit_avp_tile_coding(DwordWriter& w, const AvpTile& t) {
  uint32_t d[kTileCodingDwords];
  Fields f{ArrayView<uint32_t>(d)};
  f.set(0, 0, 31, kAvpTileCoding | (kTileCodingDwords - 2));
  f.set(1, 0, 11, t.id);
  f.set(1, 12, 23, t.group_num);
  f.set(1, 24, 31, t.group_id);
  f.set(2, 0, 9, t.column_sb);
  f.set(2, 16, 25, t.row_sb);
  f.set(3, 0, 5, t.width_sb - 1);
  f.set(3, 16, 25, t.height_sb - 1);
  f.flag(4, 25, t.last_of_column);
  f.flag(4, 26, t.last_of_row);
  f.flag(4, 27, t.first_of_group);
  f.flag(4, 28, t.last_of_group);
  f.flag(4, 29, t.last_of_frame);
  f.flag(4, 30, t.disable_cdf_update);
  f.flag(4, 31, t.disable_frame_context_update);
  f.set(5, 0, 7, 1);                                   // one back-end pipe
  f.set(5, 12, 21, t.columns - 1);
  f.set(5, 22, 31, t.rows - 1);
  emit_words(w, f);
}

void emit_avp_bsd_object(DwordWriter& w, uint32_t length, uint32_t offset) {
  w.emit(kAvpBsdObject | (kBsdObjectDwords - 2));
  w.emit(length);
  w.emit(offset);
}

}  // namespace intel_avp
}  // namespace hardware
