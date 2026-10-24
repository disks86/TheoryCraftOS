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

#include "Hardware/VideoCodec/Intel/IntelMfxCommands.hpp"

namespace hardware {
namespace intel_mfx {

namespace {
// Emits `count` zero dwords.
void zeros(DwordWriter& w, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) w.emit(0);
}

// Packs bytes into little-endian dwords, zero padded to `dwords` dwords.
void pack_bytes(DwordWriter& w, ConstByteView bytes, uint32_t dwords) {
  for (uint32_t d = 0; d < dwords; ++d) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < 4; ++k) {
      uint8_t b = 0;
      bytes.try_get(uint64_t{d} * 4 + k, &b);
      v |= static_cast<uint32_t>(b) << (8 * k);
    }
    w.emit(v);
  }
}

// An address and its memory-control dword; absent buffers are two zero dwords (the dword after is the caller's).
void address_or_zero(DwordWriter& w, uint64_t address) {
  if (address) w.emit_address(address);
  else zeros(w, 2);
}

uint32_t align(uint32_t v, uint32_t to) { return (v + to - 1) / to * to; }
}  // namespace

bool nv12_tiled_layout(uint32_t width, uint32_t height, Nv12TiledLayout* out) {
  constexpr uint32_t kMax = 16384;
  if (width == 0 || height == 0 || width > kMax || height > kMax) return false;
  out->pitch = align(width, 128);
  out->aligned_height = align(height, 32);
  out->chroma_offset_rows = out->aligned_height;
  const uint32_t chroma_rows = align((height + 1) / 2, 32);
  out->size = uint64_t{out->pitch} * (uint64_t{out->aligned_height} + chroma_rows);
  return true;
}

void emit_mi_flush(DwordWriter& w) {
  w.emit(kMiFlushDw | kMiFlushDwVideoPipelineCacheInvalidate);
  zeros(w, 3);
}

void emit_batch_end(DwordWriter& w) {
  // A batch is a whole number of quadwords: with an even count so far, a no-op goes in front of the end marker (as the reference driver does).
  if ((w.count() & 1) == 0) w.emit(kMiNoop);
  w.emit(kMiBatchBufferEnd);
}

void emit_pipe_mode_select(DwordWriter& w, uint32_t format, bool post_deblocking_output, bool pre_deblocking_output) {
  w.emit(kMfxPipeModeSelect | (5 - 2));
  w.emit((1u << 17) |                                   // long format
         (0u << 15) |                                   // VLD mode
         (uint32_t{post_deblocking_output} << 9) |
         (uint32_t{pre_deblocking_output} << 8) |
         (0u << 4) |                                    // decode
         format);
  zeros(w, 3);
}

void emit_pipe_mode_select(DwordWriter& w, bool post_deblocking_output, bool pre_deblocking_output) {
  emit_pipe_mode_select(w, kFormatAvc, post_deblocking_output, pre_deblocking_output);
}

void emit_surface_state(DwordWriter& w, uint32_t width, uint32_t height, const Nv12TiledLayout& layout) {
  w.emit(kMfxSurfaceState | (6 - 2));
  w.emit(0);
  w.emit(((height - 1) << 18) | ((width - 1) << 4));
  w.emit((kSurfacePlanar420 << 28) | (1u << 27) |       // 4:2:0 planar, interleaved chroma
         ((layout.pitch - 1) << 3) | (1u << 1) | 1u);   // tiled, Y-major walk
  w.emit(layout.chroma_offset_rows);                    // Y offset of Cb (X offset is 0)
  w.emit(0);
}

void emit_pipe_buf_addr_state(DwordWriter& w, const AvcBuffers& b) {
  w.emit(kMfxPipeBufAddrState | (61 - 2));
  // Pre-deblocking output (1-3) and post-deblocking output (4-6): the picture goes out one way or the other.
  address_or_zero(w, b.deblocking_enabled ? 0 : b.current_picture);
  w.emit(b.mocs);
  address_or_zero(w, b.deblocking_enabled ? b.current_picture : 0);
  w.emit(b.mocs);
  zeros(w, 6);                                          // uncompressed video and stream out (7-12)
  address_or_zero(w, b.intra_row_store);                // 13-15
  w.emit(b.mocs);
  address_or_zero(w, b.deblocking_row_store);           // 16-18
  w.emit(b.mocs);
  for (uint32_t i = 0; i < kMaxAvcReferenceFrames; ++i) address_or_zero(w, b.reference_pictures[i]);  // 19-50
  w.emit(b.mocs);                                       // 51
  zeros(w, 6);                                          // macroblock status and ILDB (52-57)
  zeros(w, 3);                                          // the second macroblock status (58-60)
}

void emit_ind_obj_base_addr_state(DwordWriter& w, uint64_t bitstream, uint32_t mocs) {
  w.emit(kMfxIndObjBaseAddrState | (26 - 2));
  w.emit_address(bitstream);
  w.emit(mocs);
  zeros(w, 22);                                         // upper bound, indirect MV, IT_COFF, IT_DBLK, PAK_BSE: unused for decoding
}

void emit_bsp_buf_base_addr_state(DwordWriter& w, const AvcBuffers& b) {
  w.emit(kMfxBspBufBaseAddrState | (10 - 2));
  address_or_zero(w, b.bsd_mpc_row_store);
  w.emit(b.mocs);
  address_or_zero(w, b.mpr_row_store);
  w.emit(b.mocs);
  address_or_zero(w, b.bitplane_read);                  // bitplane read buffer: VC-1 only
  w.emit(b.mocs);
}

void emit_qm_state(DwordWriter& w, uint32_t matrix_type, ConstByteView matrix) {
  w.emit(kMfxQmState | (18 - 2));
  w.emit(matrix_type);
  pack_bytes(w, matrix, 16);
}

void emit_avc_picid_state(DwordWriter& w) {
  w.emit(kMfdAvcPicidState | (10 - 2));
  w.emit(0);                                            // picture ID remapping off
  zeros(w, 8);
}

void emit_avc_img_state(DwordWriter& w, const AvcPicture& p) {
  const uint32_t img_struct = !p.field_pic ? 0u : (p.bottom_field ? 3u : 1u);
  w.emit(kMfxAvcImgState | (17 - 2));
  w.emit(p.width_in_mbs * p.height_in_mbs - 1);
  w.emit(((p.height_in_mbs - 1) << 16) | (p.width_in_mbs - 1));
  w.emit((static_cast<uint32_t>(p.second_chroma_qp_index_offset & 0x1F) << 24) |
         (static_cast<uint32_t>(p.chroma_qp_index_offset & 0x1F) << 16) |
         (uint32_t{p.weighted_pred} << 12) | (p.weighted_bipred_idc << 10) | (img_struct << 8));
  w.emit((p.chroma_format_idc << 10) | (uint32_t{p.entropy_coding_cabac} << 7) | (uint32_t{!p.reference_picture} << 6) |
         (uint32_t{p.constrained_intra_pred} << 5) | (uint32_t{p.direct_8x8_inference} << 4) | (uint32_t{p.transform_8x8_mode} << 3) |
         (uint32_t{p.frame_mbs_only} << 2) | (uint32_t{p.mbaff_frame} << 1) | uint32_t{p.field_pic});
  zeros(w, 12);
}

void emit_avc_directmode_state(DwordWriter& w, const AvcBuffers& b) {
  w.emit(kMfxAvcDirectmodeState | (71 - 2));
  for (uint32_t i = 0; i < kMaxAvcReferenceFrames; ++i) address_or_zero(w, b.direct_mv_references[i]);
  w.emit(b.mocs);
  w.emit_address(b.direct_mv_current);
  w.emit(b.mocs);
  for (uint32_t i = 0; i < kMaxAvcReferenceFrames; ++i) {
    w.emit(static_cast<uint32_t>(b.reference_poc[i][0]));
    w.emit(static_cast<uint32_t>(b.reference_poc[i][1]));
  }
  w.emit(static_cast<uint32_t>(b.current_poc[0]));
  w.emit(static_cast<uint32_t>(b.current_poc[1]));
}

void emit_avc_ref_idx_state(DwordWriter& w, uint32_t list, const uint8_t entries[kMaxAvcRefIdx]) {
  w.emit(kMfxAvcRefIdxState | (10 - 2));
  w.emit(list);
  pack_bytes(w, ConstByteView(entries, kMaxAvcRefIdx), 8);
}

void emit_avc_weightoffset_state(DwordWriter& w, uint32_t list, const int16_t table[kMaxAvcRefIdx * 6]) {
  w.emit(kMfxAvcWeightoffsetState | (98 - 2));
  w.emit(list);
  for (uint32_t i = 0; i < kMaxAvcRefIdx * 6; i += 2)
    w.emit(static_cast<uint32_t>(static_cast<uint16_t>(table[i])) | (static_cast<uint32_t>(static_cast<uint16_t>(table[i + 1])) << 16));
}

void emit_avc_slice_state(DwordWriter& w, const AvcPicture& p, const AvcSlice& s) {
  const uint32_t mbs_w = p.width_in_mbs;
  const uint32_t shift = p.mbaff_frame ? 1 : 0;
  const uint32_t hor = s.first_mb % mbs_w, ver = (s.first_mb / mbs_w) << shift;
  uint32_t next_hor = 0, next_ver = p.height_in_mbs / (p.field_pic ? 2u : 1u);
  if (!s.last_slice) {
    next_hor = s.next_first_mb % mbs_w;
    next_ver = (s.next_first_mb / mbs_w) << shift;
  }
  w.emit(kMfxAvcSliceState | (11 - 2));
  w.emit(s.slice_type);
  w.emit((s.num_ref_idx_l1 << 24) | (s.num_ref_idx_l0 << 16) | (s.chroma_log2_weight_denom << 8) | s.luma_log2_weight_denom);
  w.emit((uint32_t{s.direct_spatial_mv_pred} << 29) | (s.disable_deblocking_filter_idc << 27) | (s.cabac_init_idc << 24) |
         ((static_cast<uint32_t>(s.slice_qp) & 0xFF) << 16) | ((static_cast<uint32_t>(s.slice_beta_offset_div2) & 0xF) << 8) |
         (static_cast<uint32_t>(s.slice_alpha_c0_offset_div2) & 0xF));
  w.emit((ver << 24) | (hor << 16) | s.first_mb);
  w.emit((next_ver << 16) | next_hor);
  w.emit(uint32_t{s.last_slice} << 19);
  zeros(w, 4);
}

void emit_avc_bsd_object(DwordWriter& w, const AvcSlice& s) {
  w.emit(kMfdAvcBsdObject | (6 - 2));
  w.emit(s.data_size);
  w.emit(s.data_offset);
  w.emit(0);
  w.emit(((s.first_mb_bit_offset >> 3) << 16) | (1u << 7) | (uint32_t{s.last_slice} << 3) | (s.first_mb_bit_offset & 7));
  w.emit(0);
}

void emit_avc_phantom_slice(DwordWriter& w, const AvcPicture& p, bool has_next, uint32_t next_first_mb) {
  const uint32_t shift = p.mbaff_frame ? 1 : 0;
  uint32_t hor = 0, ver = 0, start_mb = 0, next_hor = 0, next_ver = 0;
  if (has_next) {
    const uint32_t next = next_first_mb << shift;
    next_hor = next % p.width_in_mbs;
    next_ver = next / p.width_in_mbs;
  } else {
    ver = p.height_in_mbs;
    start_mb = p.width_in_mbs * p.height_in_mbs / (p.field_pic ? 2u : 1u);
  }
  w.emit(kMfxAvcSliceState | (11 - 2));
  zeros(w, 3);
  w.emit((ver << 24) | (hor << 16) | start_mb);
  w.emit((next_ver << 16) | next_hor);
  zeros(w, 5);
  w.emit(kMfdAvcBsdObject | (6 - 2));
  zeros(w, 5);
}

void emit_mpeg2_pic_state(DwordWriter& w, const Mpeg2Picture& p) {
  w.emit(kMfxMpeg2PicState | (13 - 2));
  w.emit((p.f_code[1][1] & 0xF) << 28 | (p.f_code[1][0] & 0xF) << 24 | (p.f_code[0][1] & 0xF) << 20 | (p.f_code[0][0] & 0xF) << 16 | (p.intra_dc_precision & 3) << 14 |
         (p.picture_structure & 3) << 12 | uint32_t{p.top_field_first} << 11 | uint32_t{p.frame_pred_frame_dct} << 10 | uint32_t{p.concealment_motion_vectors} << 9 |
         uint32_t{p.q_scale_type} << 8 | uint32_t{p.intra_vlc_format} << 7 | uint32_t{p.alternate_scan} << 6);
  w.emit((p.picture_coding_type & 7) << 9);
  w.emit((1u << 31) |                                   // slice concealment off
         ((p.height_in_mbs - 1) << 16) | (p.width_in_mbs - 1));
  zeros(w, 9);
}

void emit_mpeg2_bsd_object(DwordWriter& w, const Mpeg2SliceJob& s) {
  w.emit(kMfdMpeg2BsdObject | (5 - 2));
  w.emit(s.data_size);
  w.emit(s.data_offset);
  w.emit((s.hpos & 0xFF) << 24 | (s.vpos & 0xFF) << 16 | (s.mb_count & 0xFF) << 8 | uint32_t{s.last} << 5 | uint32_t{s.last} << 3 | (s.bit_offset & 7));
  w.emit((s.quantiser_scale_code & 0xFF) << 24 | (s.next_vpos & 0xFFFF) << 8 | (s.next_hpos & 0xFF));
}

void emit_vc1_pic_state(DwordWriter& w, const Vc1Picture& p) {
  w.emit(kMfdVc1LongPicState | (6 - 2));
  w.emit(((p.height_in_mbs - 1) << 16) | (p.width_in_mbs - 1));
  w.emit((((p.width_in_mbs + 1) / 2 - 1) << 24) | uint32_t{p.direct_mv_valid} << 15 | uint32_t{p.implicit_quantizer} << 14 | (p.rounding_control & 1) << 13 | uint32_t{p.sync_marker} << 12 |
         (p.interpolation_mode & 15) << 8 | uint32_t{p.range_reduction_scale} << 7 | uint32_t{p.range_reduction} << 6 | uint32_t{p.loop_filter} << 5 | uint32_t{p.overlap} << 4 |
         (0u << 3) |                                    // (the first field: pictures are frames)
         uint32_t{p.advanced});
  w.emit((p.conditional_overlap & 3) << 29 | (p.picture_type & 7) << 26 | (0u << 24) | (p.alt_pq & 31) << 16 | (p.pic_quantizer_scale & 31) << 8 | (p.scale_factor & 0xFF));
  w.emit((p.unified_mv_mode & 3) << 28 | (0u << 27) | uint32_t{p.fast_uvmc} << 26 | (p.extended_mv_range & 3) << 8 | (p.alt_pquant_edge_mask & 15) << 4 | (p.alt_pquant_config & 3) << 2 |
         uint32_t{p.half_qp} << 1 | uint32_t{p.pic_quantizer_type});
  w.emit(uint32_t{p.bitplane_present} << 31 | uint32_t{p.raw_mv_type} << 29 | uint32_t{p.raw_skip} << 28 | uint32_t{p.raw_direct} << 27 | uint32_t{p.raw_overflags} << 26 |
         uint32_t{p.raw_ac_pred} << 25 | (p.mv_table & 7) << 20 | (p.frame_level_transform_type & 3) << 12 | uint32_t{p.mb_level_transform_type} << 11 | (p.trans_ac_y & 3) << 6 |
         (p.transform_ac_codingset_idx1 & 3) << 4 | (p.intra_transform_dc_table & 1) << 3 | (p.cbp_table & 7));
}

void emit_vc1_pred_pipe_state(DwordWriter& w, bool compensate_forward, uint32_t luma_scale, uint32_t luma_shift) {
  w.emit(kMfxVc1PredPipeState | (6 - 2));
  w.emit(uint32_t{compensate_forward} << 10);       // single forward compensation; no replication (a progressive reference)
  w.emit(compensate_forward ? ((luma_shift & 0xFF) << 16 | (luma_scale & 0xFF)) : 0);
  zeros(w, 3);
}

void emit_vc1_directmode_state(DwordWriter& w, uint64_t write_buffer, uint64_t read_buffer, uint32_t mocs) {
  w.emit(kMfxVc1DirectmodeState | (7 - 2));
  address_or_zero(w, write_buffer);
  w.emit(mocs);
  address_or_zero(w, read_buffer);
  w.emit(mocs);
}

void emit_vc1_bsd_object(DwordWriter& w, const Vc1BsdJob& job) {
  w.emit(kMfdVc1BsdObject | (5 - 2));
  w.emit(job.data_size);
  w.emit(job.data_offset);
  w.emit((job.first_row & 0xFFFF) << 16 | (job.next_row & 0xFFFF));
  w.emit(job.bit_offset & 7);
}

}  // namespace intel_mfx
}  // namespace hardware
