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


// --- HEVC

HevcBufferBytes hevc_buffer_bytes(uint32_t width, uint32_t height, uint32_t log2_ctb) {
  const uint64_t ctb = uint64_t{1} << log2_ctb;
  const uint64_t w_ctbs = (width + ctb - 1) / ctb, h_ctbs = (height + ctb - 1) / ctb;
  HevcBufferBytes b;
  b.deblocking_line = (uint64_t{align(width, 32)} >> 3) << 6;
  b.deblocking_tile_line = b.deblocking_line;
  b.deblocking_tile_column = (uint64_t{align(static_cast<uint32_t>(height + 6 * h_ctbs), 32)} >> 3) << 6;
  b.metadata_line = (((uint64_t{width} + 15) / 16 * 188 + 9 * w_ctbs + 1023) >> 9) << 6;
  b.metadata_tile_line = (((uint64_t{width} + 15) / 16 * 172 + 9 * w_ctbs + 1023) >> 9) << 6;
  b.metadata_tile_column = (((uint64_t{height} + 15) / 16 * 176 + 89 * h_ctbs + 1023) >> 9) << 6;
  b.sao_line = (uint64_t{align(static_cast<uint32_t>(width / 2 + 3 * w_ctbs), 16)} >> 3) << 6;
  b.sao_tile_line = (uint64_t{align(static_cast<uint32_t>(width / 2 + 6 * w_ctbs), 16)} >> 3) << 6;
  b.sao_tile_column = (uint64_t{align(static_cast<uint32_t>(height / 2 + 6 * h_ctbs), 16)} >> 3) << 6;
  const uint64_t units = log2_ctb == 4 ? ((uint64_t{width} + 63) >> 6) * ((uint64_t{height} + 15) >> 4) : ((uint64_t{width} + 31) >> 5) * ((uint64_t{height} + 31) >> 5);
  b.motion_vector = units << 6;
  return b;
}

void emit_hcp_pipe_mode_select(DwordWriter& w, uint32_t codec) {
  w.emit(kHcpPipeModeSelect | (6 - 2));                 // (the Kaby Lake layout: two more dwords than Skylake)
  w.emit((codec << 5) | 0u);                            // decode, no status report
  zeros(w, 2);
  w.emit(codec == kHcpCodecVp9 ? 1u << 6 : 0u);
  w.emit(0);
}

void emit_hcp_surface_state(DwordWriter& w, const Nv12TiledLayout& layout) {
  w.emit(kHcpSurfaceState | (3 - 2));
  w.emit(layout.pitch - 1);
  w.emit((kSurfacePlanar420 << 28) | layout.chroma_offset_rows);
}

namespace {
// A buffer with its memory-control dword (three dwords); an absent one is zeros and the dword.
void hcp_buffer(DwordWriter& w, uint64_t address, uint32_t mocs) {
  address_or_zero(w, address);
  w.emit(mocs);
}
}  // namespace

void emit_hevc_pipe_buf_addr_state(DwordWriter& w, const HevcBuffers& b) {
  w.emit(kHcpPipeBufAddrState | (95 - 2));
  hcp_buffer(w, b.current_picture, b.mocs);             // 1-3
  hcp_buffer(w, b.deblocking_line, b.mocs);             // 4-6
  hcp_buffer(w, b.deblocking_tile_line, b.mocs);
  hcp_buffer(w, b.deblocking_tile_column, b.mocs);
  hcp_buffer(w, b.metadata_line, b.mocs);               // 13-15
  hcp_buffer(w, b.metadata_tile_line, b.mocs);
  hcp_buffer(w, b.metadata_tile_column, b.mocs);
  hcp_buffer(w, b.sao_line, b.mocs);                    // 22-24
  hcp_buffer(w, b.sao_tile_line, b.mocs);
  hcp_buffer(w, b.sao_tile_column, b.mocs);
  hcp_buffer(w, b.motion_vector_current, b.mocs);       // 31-33
  hcp_buffer(w, 0, b.mocs);                             // 34-36, reserved
  for (uint32_t i = 0; i < kMaxHevcReferenceFrames; ++i) address_or_zero(w, b.reference_pictures[i]);   // 37-52
  w.emit(0);                                            // 53
  for (uint32_t i = 0; i < 4; ++i) hcp_buffer(w, 0, b.mocs);   // 54-65, not used for decoding
  for (uint32_t i = 0; i < kMaxHevcReferenceFrames; ++i) address_or_zero(w, b.motion_vector_references[i]);   // 66-81
  w.emit(0);                                            // 82
  for (uint32_t i = 0; i < 4; ++i) hcp_buffer(w, 0, b.mocs);   // 83-94, not used for HEVC
}

void emit_hevc_ind_obj_base_addr_state(DwordWriter& w, const HevcBuffers& b) {
  w.emit(kHcpIndObjBaseAddrState | (14 - 2));
  hcp_buffer(w, b.bitstream, b.mocs);                   // 1-3
  w.emit_address(b.bitstream + ((b.bitstream_bytes + 4095) & ~uint64_t{4095}));   // 4-5: the upper bound
  hcp_buffer(w, 0, b.mocs);                             // 6-8
  hcp_buffer(w, 0, b.mocs);                             // 9-11
  zeros(w, 2);                                          // 12-13
}

namespace {
void hevc_qm_state(DwordWriter& w, uint32_t size_id, uint32_t component, uint32_t pred_type, uint32_t dc, const uint8_t* matrix, uint32_t length) {
  w.emit(kHcpQmState | (18 - 2));
  w.emit((dc << 5) | (component << 3) | (size_id << 1) | pred_type);
  pack_bytes(w, ConstByteView(matrix, length), 16);
}
}  // namespace

void emit_hevc_qm_states(DwordWriter& w, const HevcScaling& s) {
  for (uint32_t i = 0; i < 6; ++i) hevc_qm_state(w, 0, i % 3, i / 3, 0, s.list4x4[i], 16);
  for (uint32_t i = 0; i < 6; ++i) hevc_qm_state(w, 1, i % 3, i / 3, 0, s.list8x8[i], 64);
  for (uint32_t i = 0; i < 6; ++i) hevc_qm_state(w, 2, i % 3, i / 3, s.dc16x16[i], s.list16x16[i], 64);
  for (uint32_t i = 0; i < 2; ++i) hevc_qm_state(w, 3, 0, i, s.dc32x32[i], s.list32x32[i], 64);
}

void emit_hevc_pic_state(DwordWriter& w, const HevcPicture& p) {
  const uint32_t min_cb = 1u << p.log2_min_cb;
  uint32_t max_pcm_minus3, min_pcm_minus3, pcm_luma_minus1 = 7, pcm_chroma_minus1 = 7;
  if (p.pcm) {
    max_pcm_minus3 = p.log2_max_pcm - 3;
    min_pcm_minus3 = p.log2_min_pcm - 3;
    pcm_luma_minus1 = (p.pcm_bit_depth_luma - 1) & 0xF;
    pcm_chroma_minus1 = (p.pcm_bit_depth_chroma - 1) & 0xF;
  } else {
    min_pcm_minus3 = 0;
    max_pcm_minus3 = p.log2_ctb - 3 < 2 ? p.log2_ctb - 3 : 2;
  }
  w.emit(kHcpPicState | (19 - 2));
  w.emit(((p.height / min_cb - 1) << 16) | (p.width / min_cb - 1));
  w.emit((max_pcm_minus3 << 10) | (min_pcm_minus3 << 8) | (p.log2_max_tb - 2) << 6 | (p.log2_min_tb - 2) << 4 | (p.log2_ctb - 3) << 2 | (p.log2_min_cb - 3));
  w.emit(0);
  w.emit((uint32_t{p.strong_intra_smoothing} << 26) | (uint32_t{p.transquant_bypass} << 25) | (uint32_t{p.amp} << 23) | (uint32_t{p.transform_skip} << 22) |
         (1u << 21) |                                   // (a frame: the bottom field flag is inverted)
         (uint32_t{p.weighted_pred} << 19) | (uint32_t{p.weighted_bipred} << 18) | (uint32_t{p.tiles} << 17) | (uint32_t{p.entropy_coding_sync} << 16) |
         (uint32_t{p.tiles ? p.loop_filter_across_tiles : true} << 15) | (uint32_t{p.sign_data_hiding} << 13) | ((p.log2_parallel_merge_level - 2) << 10) |
         (uint32_t{p.constrained_intra_pred} << 9) | (uint32_t{p.pcm_loop_filter_disabled} << 8) | ((p.diff_cu_qp_delta_depth & 3u) << 6) | (uint32_t{p.cu_qp_delta} << 5) |
         (uint32_t{p.pcm} << 4) | (uint32_t{p.sao} << 3));
  w.emit((pcm_luma_minus1 << 20) | (pcm_chroma_minus1 << 16) | (p.max_transform_hierarchy_depth_inter << 13) | (p.max_transform_hierarchy_depth_intra << 10) |
         ((static_cast<uint32_t>(p.cr_qp_offset) & 0x1F) << 5) | (static_cast<uint32_t>(p.cb_qp_offset) & 0x1F));
  zeros(w, 13);
}

void emit_hevc_tile_state(DwordWriter& w, const HevcPicture& p) {
  uint8_t cols[20] = {}, rows[24] = {};
  for (uint32_t i = 0; i < p.num_tile_columns && i < 19; ++i) cols[i + 1] = static_cast<uint8_t>(cols[i] + p.column_width[i]);
  for (uint32_t i = 0; i < p.num_tile_rows && i < 21; ++i) rows[i + 1] = static_cast<uint8_t>(rows[i] + p.row_height[i]);
  w.emit(kHcpTileState | (13 - 2));
  w.emit(((p.num_tile_columns - 1) << 5) | (p.num_tile_rows - 1));
  pack_bytes(w, ConstByteView(cols, 20), 5);
  pack_bytes(w, ConstByteView(rows, 24), 6);
}

void emit_hevc_ref_idx_state(DwordWriter& w, uint32_t list, uint32_t count, const uint32_t entries[kMaxHevcRefIdx]) {
  const uint32_t n = count < kMaxHevcRefIdx ? count : kMaxHevcRefIdx;
  w.emit(kHcpRefIdxState | (18 - 2));
  w.emit(((count - 1) << 1) | list);
  for (uint32_t i = 0; i < 16; ++i) w.emit(i < n ? entries[i] : 0);
}

void emit_hevc_weightoffset_state(DwordWriter& w, uint32_t list, uint32_t count, const HevcWeights& t) {
  const uint32_t n = count < kMaxHevcRefIdx ? count : kMaxHevcRefIdx;
  auto b = [](int8_t v) { return static_cast<uint32_t>(static_cast<uint8_t>(v)); };
  w.emit(kHcpWeightoffset | (34 - 2));
  w.emit(list);
  for (uint32_t i = 0; i < 16; ++i) w.emit(i < n ? (b(t.luma_offset[i]) << 8) | b(t.luma_weight[i]) : 0);
  for (uint32_t i = 0; i < 16; ++i)
    w.emit(i < n ? (b(t.chroma_offset[i][1]) << 24) | (b(t.chroma_weight[i][1]) << 16) | (b(t.chroma_offset[i][0]) << 8) | b(t.chroma_weight[i][0]) : 0);
}

void emit_hevc_slice_state(DwordWriter& w, const HevcSlice& s) {
  w.emit(kHcpSliceState | (9 - 2));
  w.emit((s.ver_pos << 16) | s.hor_pos);
  w.emit((s.next_ver_pos << 16) | s.next_hor_pos);
  w.emit(((static_cast<uint32_t>(s.cr_qp_offset) & 0x1F) << 17) | ((static_cast<uint32_t>(s.cb_qp_offset) & 0x1F) << 12) | (static_cast<uint32_t>(s.qp) << 6) |
         (uint32_t{s.temporal_mvp} << 5) | (uint32_t{s.dependent} << 4) | (uint32_t{s.last} << 2) | s.slice_type);
  w.emit((s.collocated_frame_store << 26) | ((s.max_merge_candidates - 1) << 23) | (uint32_t{s.cabac_init} << 22) | (s.luma_log2_weight_denom << 19) |
         ((s.chroma_log2_weight_denom & 7u) << 16) | (uint32_t{s.collocated_from_l0} << 15) | (uint32_t{s.low_delay} << 14) | (uint32_t{s.mvd_l1_zero} << 13) |
         (uint32_t{s.sao_luma} << 12) | (uint32_t{s.sao_chroma} << 11) | (uint32_t{s.loop_filter_across_slices} << 10) |
         ((static_cast<uint32_t>(s.beta_offset_div2) & 0xF) << 5) | ((static_cast<uint32_t>(s.tc_offset_div2) & 0xF) << 1) | uint32_t{s.deblocking_disabled});
  w.emit(s.header_bytes);
  zeros(w, 3);
}

void emit_hevc_bsd_object(DwordWriter& w, const HevcSlice& s) {
  w.emit(kHcpBsdObject | (3 - 2));
  w.emit(s.data_size);
  w.emit(s.data_offset);
}

}  // namespace intel_mfx
}  // namespace hardware
