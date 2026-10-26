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

// The values follow Mesa's radeon_uvd.c and si_uvd.c (Copyright Advanced Micro Devices, Inc.; MIT license) for H.264 on a part from Polaris on.
#include "Hardware/VideoCodec/AMD/AmdUvd6Messages.hpp"

namespace hardware {
namespace amd_uvd6 {

namespace {
#include "Hardware/VideoCodec/AMD/AmdUvd6MessageLayout.inc"

constexpr uint32_t kMacroblock = 16;
constexpr uint32_t kDbPitchAlignment = 16;   // get_db_pitch_alignment before Vega 10
constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// A bounds-checked writer over the message buffer.
class Image {
 public:
  explicit Image(ByteView out) : out_(out) {}
  void zero(uint32_t at, uint32_t bytes) { for (uint32_t i = 0; i < bytes; ++i) out_[at + i] = 0; }
  void u8(uint32_t at, uint8_t v) { out_[at] = v; }
  void u32(uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) out_[at + i] = static_cast<uint8_t>(v >> (8 * i)); }
  void u16(uint32_t at, uint16_t v) { out_[at] = static_cast<uint8_t>(v); out_[at + 1] = static_cast<uint8_t>(v >> 8); }
  void i32(uint32_t at, int32_t v) { u32(at, static_cast<uint32_t>(v)); }
 private:
  ByteView out_;
};

uint32_t level_buffer_macroblocks(uint32_t level) {
  switch (level) {
    case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    default: return 184320;   // (51, and every other level)
  }
}

bool size_ok(uint32_t width, uint32_t height) {
  return width >= kMacroblock && height >= kMacroblock && width <= kMaxWidth && height <= kMaxHeight && width % kMacroblock == 0 && height % kMacroblock == 0;
}

// The message header and the decode message every codec shares (ruvd_end_frame); the codec's own structure follows it.
bool build_common_decode(ByteView out, uint32_t stream_type, uint32_t stream_handle, uint32_t feedback_number, uint32_t width, uint32_t height, const DecodeBuffers& b) {
  if (out.size() < kUvd_Msg_sizeof) return false;
  if (b.dpb_bytes > UINT32_MAX || b.context_bytes > UINT32_MAX || b.target.bytes > UINT32_MAX || b.bitstream_bytes > UINT32_MAX - 127 || b.target.luma_pitch == 0) return false;
  Image m(out);
  m.zero(0, kUvd_Msg_sizeof);
  m.u32(kUvd_Msg_size, kUvd_Msg_sizeof);
  m.u32(kUvd_Msg_msg_type, kMsgDecode);
  m.u32(kUvd_Msg_stream_handle, stream_handle);
  m.u32(kUvd_Msg_status_report_feedback_number, feedback_number);

  // ruvd_end_frame: the decode message.
  m.u32(kUvd_Decode_stream_type, stream_type);
  m.u32(kUvd_Decode_decode_flags, 0x1);
  m.u32(kUvd_Decode_width_in_samples, width);
  m.u32(kUvd_Decode_height_in_samples, height);
  m.u32(kUvd_Decode_dpb_size, static_cast<uint32_t>(b.dpb_bytes));
  m.u32(kUvd_Decode_bsd_size, static_cast<uint32_t>(align_up(b.bitstream_bytes, 128)));
  m.u32(kUvd_Decode_db_pitch, static_cast<uint32_t>(align_up(width, kDbPitchAlignment)));
  m.u32(kUvd_Decode_dpb_reserved, static_cast<uint32_t>(b.context_bytes));   // the context buffer's size, for a part from Polaris on
  // si_uvd_set_dt_surfaces (legacy surface, linear aligned): pitch in bytes, linear tiling and array mode, no tile configuration, top and bottom the same for a frame.
  m.u32(kUvd_Decode_dt_pitch, b.target.luma_pitch);
  m.u32(kUvd_Decode_dt_tiling_mode, 0);
  m.u32(kUvd_Decode_dt_array_mode, 0);
  m.u32(kUvd_Decode_dt_field_mode, 0);
  m.u32(kUvd_Decode_dt_luma_top_offset, 0);
  m.u32(kUvd_Decode_dt_luma_bottom_offset, 0);
  m.u32(kUvd_Decode_dt_chroma_top_offset, static_cast<uint32_t>(b.target.chroma_offset));
  m.u32(kUvd_Decode_dt_chroma_bottom_offset, static_cast<uint32_t>(b.target.chroma_offset));
  m.u32(kUvd_Decode_dt_surf_tile_config, 0);
  m.u32(kUvd_Decode_dt_wa_chroma_top_offset, b.target.luma_pitch / 2);   // from Stoney on: the chroma pitch for the interleaved plane
  m.u32(kUvd_Decode_db_surf_tile_config, 0);                                // = dt_surf_tile_config
  m.u8(kUvd_Decode_extension_support, 1);
  return true;
}

}  // namespace

uint32_t message_bytes() { return kUvd_Msg_sizeof; }

bool build_create_message(ByteView out, uint32_t stream_handle, uint32_t width, uint32_t height, uint32_t dpb_bytes, uint32_t stream_type) {
  if (out.size() < kUvd_Msg_sizeof) return false;
  Image m(out);
  m.zero(0, kUvd_Msg_sizeof);
  m.u32(kUvd_Msg_size, kUvd_Msg_sizeof);
  m.u32(kUvd_Msg_msg_type, kMsgCreate);
  m.u32(kUvd_Msg_stream_handle, stream_handle);
  m.u32(kUvd_Create_stream_type, stream_type);
  m.u32(kUvd_Create_width_in_samples, width);
  m.u32(kUvd_Create_height_in_samples, height);
  m.u32(kUvd_Create_dpb_size, dpb_bytes);
  return true;
}

bool build_destroy_message(ByteView out, uint32_t stream_handle) {
  if (out.size() < kUvd_Msg_sizeof) return false;
  Image m(out);
  m.zero(0, kUvd_Msg_sizeof);
  m.u32(kUvd_Msg_size, kUvd_Msg_sizeof);
  m.u32(kUvd_Msg_msg_type, kMsgDestroy);
  m.u32(kUvd_Msg_stream_handle, stream_handle);
  return true;
}

AvcBufferSizes avc_buffer_sizes(uint32_t width, uint32_t height, uint32_t level, uint32_t max_references) {
  AvcBufferSizes sizes;
  if (!size_ok(width, height) || max_references > 16) return sizes;
  // calc_dpb_size: one more for the picture being decoded; the number of pictures the level allows; the size of a picture, aligned.
  const uint64_t width_in_mb = width / kMacroblock, height_in_mb = align_up(height / kMacroblock, 2);
  const uint64_t frame_macroblocks = width_in_mb * height_in_mb;
  uint64_t image = align_up(width, kDbPitchAlignment) * height;
  image += image / 2;
  image = align_up(image, 1024);
  const uint64_t for_level = level_buffer_macroblocks(level) / frame_macroblocks + 1;
  const uint64_t capped = for_level < kMaxH264References ? for_level : kMaxH264References;
  const uint64_t wanted = uint64_t{max_references} + 1;
  const uint64_t refs = capped > wanted ? capped : wanted;
  sizes.dpb = image * refs;   // (no macroblock context or scaling surface in it: the stream type is the performance one on a part from Polaris on)
  sizes.context = refs * align_up(frame_macroblocks * 192, 256);   // calc_ctx_size_h264_perf
  sizes.references = static_cast<uint32_t>(refs);
  return sizes;
}

uint64_t bitstream_buffer_bytes(uint32_t width, uint32_t height) { return uint64_t{width} * height * (512 / (16 * 16)); }

bool build_avc_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const amd_vcn::AvcPictureState& p, const DecodeBuffers& b) {
  if (!size_ok(p.width, p.height) || p.chroma_format != 1 || p.reference_count > 16 || !build_common_decode(out, kStreamTypeH264Perf, stream_handle, feedback_number, p.width, p.height, b)) return false;
  Image m(out);
  // get_h264_msg
  m.u32(kUvd_H264_profile, p.profile);
  m.u32(kUvd_H264_level, p.level);
  m.u32(kUvd_H264_sps_info_flags, p.sps_flags);
  m.u32(kUvd_H264_pps_info_flags, p.pps_flags);
  m.u8(kUvd_H264_chroma_format, p.chroma_format);
  m.u8(kUvd_H264_bit_depth_luma_minus8, 0);
  m.u8(kUvd_H264_bit_depth_chroma_minus8, 0);
  m.u8(kUvd_H264_log2_max_frame_num_minus4, p.log2_max_frame_num_minus4);
  m.u8(kUvd_H264_pic_order_cnt_type, p.poc_type);
  m.u8(kUvd_H264_log2_max_pic_order_cnt_lsb_minus4, p.log2_max_poc_lsb_minus4);
  m.u8(kUvd_H264_num_ref_frames, p.num_ref_frames);
  m.u8(kUvd_H264_pic_init_qp_minus26, static_cast<uint8_t>(p.pic_init_qp_minus26));
  m.u8(kUvd_H264_chroma_qp_index_offset, static_cast<uint8_t>(p.chroma_qp_index_offset));
  m.u8(kUvd_H264_second_chroma_qp_index_offset, static_cast<uint8_t>(p.second_chroma_qp_index_offset));
  m.u8(kUvd_H264_num_slice_groups_minus1, 0);    // (no slice groups: a stream that has them is not taken)
  m.u8(kUvd_H264_slice_group_map_type, 0);
  m.u8(kUvd_H264_num_ref_idx_l0_active_minus1, p.num_ref_idx_l0_active_minus1);
  m.u8(kUvd_H264_num_ref_idx_l1_active_minus1, p.num_ref_idx_l1_active_minus1);
  for (uint32_t list = 0; list < 6; ++list)
    for (uint32_t i = 0; i < 16; ++i) m.u8(kUvd_H264_scaling_list_4x4 + list * 16 + i, p.scaling_4x4[list][i]);
  for (uint32_t list = 0; list < 2; ++list)
    for (uint32_t i = 0; i < 64; ++i) m.u8(kUvd_H264_scaling_list_8x8 + list * 64 + i, p.scaling_8x8[list][i]);
  m.u32(kUvd_H264_frame_num, p.frame_num);
  m.i32(kUvd_H264_curr_field_order_cnt_list, p.top_poc);
  m.i32(kUvd_H264_curr_field_order_cnt_list + 4, p.bottom_poc);
  for (uint32_t i = 0; i < p.reference_count; ++i) {
    m.u32(kUvd_H264_frame_num_list + i * 4, p.references[i].frame_num);
    m.i32(kUvd_H264_field_order_cnt_list + i * 8, p.references[i].top_poc);
    m.i32(kUvd_H264_field_order_cnt_list + i * 8 + 4, p.references[i].bottom_poc);
  }
  m.u32(kUvd_H264_decoded_pic_idx, p.frame_num);
  return true;
}

bool prepare_avc_feedback_and_tables(ByteView message_buffer, const amd_vcn::AvcPictureState& picture) {
  if (message_buffer.size() < kMessageFeedbackItBytes) return false;
  Image m(message_buffer);
  m.zero(kFeedbackOffset, kFeedbackBytes + kItTableBytes);
  m.u32(kFeedbackOffset, kFeedbackBytes);   // "set at least the feedback buffer size"
  for (uint32_t list = 0; list < 6; ++list)
    for (uint32_t i = 0; i < 16; ++i) m.u8(kItTableOffset + list * 16 + i, picture.scaling_4x4[list][i]);
  for (uint32_t list = 0; list < 2; ++list)
    for (uint32_t i = 0; i < 64; ++i) m.u8(kItTableOffset + kItTable8x8Offset + list * 64 + i, picture.scaling_8x8[list][i]);
  return true;
}

HevcBufferSizes hevc_buffer_sizes(uint32_t width, uint32_t height, uint32_t max_dec_pic_buffering) {
  HevcBufferSizes sizes;
  if (width < 16 || height < 16 || width > kMaxWidth || height > kMaxHeight || width % 8 != 0 || height % 8 != 0) return sizes;
  // calc_dpb_size and calc_ctx_size_h265_main: slots for 17 pictures (8 from 4096 x 2000 up), a picture's slot aligned to 256.
  const uint32_t references = uint64_t{width} * height >= uint64_t{4096} * 2000 ? 8 : 17;
  if (max_dec_pic_buffering > references) return sizes;
  const uint64_t w16 = align_up(width, 16), h16 = align_up(height, 16);
  sizes.references = references;
  sizes.dpb = align_up(align_up(w16, kDbPitchAlignment) * h16 * 3 / 2, 256) * references;
  sizes.context = ((w16 + 255) / 16) * ((h16 + 255) / 16) * 16 * references + 52 * 1024;
  return sizes;
}

bool build_hevc_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const h265::OffloadPicture& picture, const DecodeBuffers& b) {
  if (!picture.sps || !picture.pps || !picture.first) return false;
  const h265::Sps& sps = *picture.sps;
  const h265::Pps& pps = *picture.pps;
  if (sps.chroma_format_idc != 1 || sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8 || sps.width <= 0 || sps.height <= 0) return false;
  const uint32_t width = static_cast<uint32_t>(sps.width), height = static_cast<uint32_t>(sps.height);
  if (hevc_buffer_sizes(width, height, 0).dpb == 0 || !build_common_decode(out, kStreamTypeH265, stream_handle, feedback_number, width, height, b)) return false;
  Image m(out);
  auto bit = [](bool v, uint32_t n) { return v ? 1u << n : 0u; };
  m.u32(kUvd_H265_sps_info_flags, bit(sps.scaling_list_enabled, 0) | bit(sps.amp, 1) | bit(sps.sao, 2) | bit(sps.pcm, 3) | bit(sps.pcm && sps.pcm_loop_filter_disabled, 4) |
                                      bit(sps.long_term_ref_pics_present, 5) | bit(sps.temporal_mvp, 6) | bit(sps.strong_intra_smoothing, 7) | bit(true, 10));   // 10: the reference lists below are used
  m.u32(kUvd_H265_pps_info_flags, bit(pps.dependent_slice_segments, 0) | bit(pps.output_flag_present, 1) | bit(pps.sign_data_hiding, 2) | bit(pps.cabac_init_present, 3) |
                                      bit(pps.constrained_intra_pred, 4) | bit(pps.transform_skip, 5) | bit(pps.cu_qp_delta_enabled, 6) | bit(pps.slice_chroma_qp_offsets_present, 7) |
                                      bit(pps.weighted_pred, 8) | bit(pps.weighted_bipred, 9) | bit(pps.transquant_bypass, 10) | bit(pps.tiles_enabled, 11) |
                                      bit(pps.entropy_coding_sync, 12) | bit(pps.tiles_enabled && pps.uniform_spacing, 13) | bit(pps.tiles_enabled && pps.loop_filter_across_tiles, 14) |
                                      bit(pps.loop_filter_across_slices, 15) | bit(pps.deblocking_override_enabled, 16) | bit(pps.deblocking_disabled, 17) |
                                      bit(pps.lists_modification_present, 18) | bit(pps.slice_header_extension_present, 19));
  m.u8(kUvd_H265_chroma_format, 1);
  m.u8(kUvd_H265_log2_max_pic_order_cnt_lsb_minus4, static_cast<uint8_t>(sps.log2_max_poc_lsb - 4));
  const int dpb_pictures = sps.max_dec_pic_buffering[sps.max_sub_layers - 1];
  m.u8(kUvd_H265_sps_max_dec_pic_buffering_minus1, static_cast<uint8_t>(dpb_pictures > 0 ? dpb_pictures - 1 : 0));
  m.u8(kUvd_H265_log2_min_luma_coding_block_size_minus3, static_cast<uint8_t>(sps.log2_min_cb - 3));
  m.u8(kUvd_H265_log2_diff_max_min_luma_coding_block_size, static_cast<uint8_t>(sps.log2_ctb - sps.log2_min_cb));
  m.u8(kUvd_H265_log2_min_transform_block_size_minus2, static_cast<uint8_t>(sps.log2_min_tb - 2));
  m.u8(kUvd_H265_log2_diff_max_min_transform_block_size, static_cast<uint8_t>(sps.log2_max_tb - sps.log2_min_tb));
  m.u8(kUvd_H265_max_transform_hierarchy_depth_inter, static_cast<uint8_t>(sps.max_th_depth_inter));
  m.u8(kUvd_H265_max_transform_hierarchy_depth_intra, static_cast<uint8_t>(sps.max_th_depth_intra));
  if (sps.pcm) {
    m.u8(kUvd_H265_pcm_sample_bit_depth_luma_minus1, static_cast<uint8_t>(sps.pcm_bit_depth_luma - 1));
    m.u8(kUvd_H265_pcm_sample_bit_depth_chroma_minus1, static_cast<uint8_t>(sps.pcm_bit_depth_chroma - 1));
    m.u8(kUvd_H265_log2_min_pcm_luma_coding_block_size_minus3, static_cast<uint8_t>(sps.log2_min_pcm - 3));
    m.u8(kUvd_H265_log2_diff_max_min_pcm_luma_coding_block_size, static_cast<uint8_t>(sps.log2_max_pcm - sps.log2_min_pcm));
  }
  m.u8(kUvd_H265_num_extra_slice_header_bits, static_cast<uint8_t>(pps.num_extra_slice_header_bits));
  m.u8(kUvd_H265_num_short_term_ref_pic_sets, static_cast<uint8_t>(sps.num_st_rps));
  m.u8(kUvd_H265_num_long_term_ref_pic_sps, static_cast<uint8_t>(sps.num_lt_sps));
  m.u8(kUvd_H265_num_ref_idx_l0_default_active_minus1, static_cast<uint8_t>(pps.num_ref_idx_default[0] - 1));
  m.u8(kUvd_H265_num_ref_idx_l1_default_active_minus1, static_cast<uint8_t>(pps.num_ref_idx_default[1] - 1));
  m.u8(kUvd_H265_pps_cb_qp_offset, static_cast<uint8_t>(pps.cb_qp_offset));
  m.u8(kUvd_H265_pps_cr_qp_offset, static_cast<uint8_t>(pps.cr_qp_offset));
  m.u8(kUvd_H265_pps_beta_offset_div2, static_cast<uint8_t>(pps.beta_offset_div2));
  m.u8(kUvd_H265_pps_tc_offset_div2, static_cast<uint8_t>(pps.tc_offset_div2));
  m.u8(kUvd_H265_diff_cu_qp_delta_depth, static_cast<uint8_t>(pps.diff_cu_qp_delta_depth));
  m.u8(kUvd_H265_log2_parallel_merge_level_minus2, static_cast<uint8_t>(pps.log2_parallel_merge_level - 2));
  if (pps.tiles_enabled) {
    m.u8(kUvd_H265_num_tile_columns_minus1, static_cast<uint8_t>(pps.num_tile_cols - 1));
    m.u8(kUvd_H265_num_tile_rows_minus1, static_cast<uint8_t>(pps.num_tile_rows - 1));
    // The widths of all columns but the last, in CTBs minus one: given in the bitstream, or divided evenly (6.5.1).
    const int ctb_w = sps.pic_w_ctb, ctb_h = sps.pic_h_ctb;
    for (int i = 0; i < pps.num_tile_cols - 1 && i < 19; ++i) {
      const int w = pps.uniform_spacing ? ((i + 1) * ctb_w) / pps.num_tile_cols - (i * ctb_w) / pps.num_tile_cols : pps.col_width_minus1[i] + 1;
      m.u16(kUvd_H265_column_width_minus1 + static_cast<uint32_t>(i) * 2, static_cast<uint16_t>(w - 1));
    }
    for (int i = 0; i < pps.num_tile_rows - 1 && i < 21; ++i) {
      const int r = pps.uniform_spacing ? ((i + 1) * ctb_h) / pps.num_tile_rows - (i * ctb_h) / pps.num_tile_rows : pps.row_height_minus1[i] + 1;
      m.u16(kUvd_H265_row_height_minus1 + static_cast<uint32_t>(i) * 2, static_cast<uint16_t>(r - 1));
    }
  }
  m.u8(kUvd_H265_init_qp_minus26, static_cast<uint8_t>(pps.init_qp - 26));
  m.u8(kUvd_H265_num_delta_pocs_ref_rps_idx, static_cast<uint8_t>(picture.first->num_delta_pocs_ref_rps_idx));
  m.u8(kUvd_H265_curr_idx, static_cast<uint8_t>(picture.slot));
  m.i32(kUvd_H265_curr_poc, picture.poc);
  for (uint32_t i = 0; i < 16; ++i) {
    // Entries not in use name no picture (Mesa: 0x7F).
    const bool used = i < picture.ref_count;
    m.u8(kUvd_H265_ref_pic_list + i, used ? static_cast<uint8_t>(picture.refs[i].slot) : 0x7F);
    m.i32(kUvd_H265_poc_list + i * 4, used ? picture.refs[i].poc : 0);
  }
  for (uint32_t i = 0; i < 8; ++i) {
    m.u8(kUvd_H265_ref_pic_set_st_curr_before + i, i < picture.st_before_count ? picture.st_before[i] : 0xFF);
    m.u8(kUvd_H265_ref_pic_set_st_curr_after + i, i < picture.st_after_count ? picture.st_after[i] : 0xFF);
    m.u8(kUvd_H265_ref_pic_set_lt_curr + i, i < picture.lt_count ? picture.lt_curr[i] : 0xFF);
  }
  const h265::ScalingList& sl = pps.scaling_list_present ? pps.scaling : sps.scaling;
  for (uint32_t i = 0; i < 6; ++i) m.u8(kUvd_H265_ucScalingListDCCoefSizeID2 + i, sl.dc[2][i]);
  for (uint32_t i = 0; i < 2; ++i) m.u8(kUvd_H265_ucScalingListDCCoefSizeID3 + i, sl.dc[3][i * 3]);
  for (uint32_t l = 0; l < 2; ++l)
    for (uint32_t i = 0; i < 15; ++i) m.u8(kUvd_H265_direct_reflist + l * 15 + i, picture.list[l][i]);
  return true;
}

bool prepare_hevc_feedback_and_tables(ByteView message_buffer, const h265::Sps& sps, const h265::Pps& pps) {
  static_assert(kMessageFeedbackItBytes == amd_vcn::kMessageFeedbackItBytes && kItTableOffset == amd_vcn::kItTableOffset, "the message pages are laid out alike");
  if (message_buffer.size() < kMessageFeedbackItBytes || !amd_vcn::prepare_hevc_feedback_and_tables(message_buffer, sps, pps)) return false;
  Image m(message_buffer);
  m.zero(kFeedbackOffset, kFeedbackBytes);   // (the VCN feedback header is not this engine's)
  m.u32(kFeedbackOffset, kFeedbackBytes);    // "set at least the feedback buffer size"
  return true;
}

Mpeg2BufferSizes mpeg2_buffer_sizes(uint32_t width, uint32_t height) {
  Mpeg2BufferSizes sizes;
  if (!size_ok(width, height)) return sizes;
  uint64_t image = align_up(width, kDbPitchAlignment) * height;
  image += image / 2;
  image = align_up(image, 1024);
  sizes.references = kMpeg2References;
  sizes.dpb = image * kMpeg2References;   // calc_dpb_size: "must be big enough for all frames"
  return sizes;
}

bool build_mpeg2_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const mpeg2::OffloadPicture& picture, const Mpeg2Pictures& p, const DecodeBuffers& b) {
  if (!picture.seq || !picture.pic || picture.seq->chroma_format != 1) return false;
  const mpeg2::SequenceHeader& seq = *picture.seq;
  const mpeg2::PictureHeader& pic = *picture.pic;
  if (seq.mb_width <= 0 || seq.mb_height <= 0 || pic.type < mpeg2::kPictureI || pic.type > mpeg2::kPictureB || pic.structure < mpeg2::kTopField || pic.structure > mpeg2::kFramePicture) return false;
  // get_ref_pic_idx limits a reference to the last six jobs; a picture further back is no longer in the engine's buffer.
  const uint32_t refs[2] = {p.forward, p.backward};
  for (uint32_t r : refs)
    if (p.current == 0 || r >= p.current || p.current - r > kMpeg2References) return false;
  const uint32_t width = static_cast<uint32_t>(seq.mb_width) * kMacroblock, height = static_cast<uint32_t>(seq.mb_height) * kMacroblock;
  if (!size_ok(width, height) || !build_common_decode(out, kStreamTypeMpeg2, stream_handle, feedback_number, width, height, b)) return false;
  Image m(out);
  m.u32(kUvd_Mpeg2_decoded_pic_idx, p.current);
  m.u32(kUvd_Mpeg2_ref_pic_idx, p.forward);
  m.u32(kUvd_Mpeg2_ref_pic_idx + 4, p.backward);
  m.u8(kUvd_Mpeg2_load_intra_quantiser_matrix, 1);
  m.u8(kUvd_Mpeg2_load_nonintra_quantiser_matrix, 1);
  for (uint32_t i = 0; i < 64; ++i) {
    m.u8(kUvd_Mpeg2_intra_quantiser_matrix + i, picture.intra_matrix_scan[i]);
    m.u8(kUvd_Mpeg2_nonintra_quantiser_matrix + i, picture.non_intra_matrix_scan[i]);
  }
  m.u8(kUvd_Mpeg2_profile_and_level_indication, 0);
  m.u8(kUvd_Mpeg2_chroma_format, 1);
  m.u8(kUvd_Mpeg2_picture_coding_type, static_cast<uint8_t>(pic.type));
  for (uint32_t d = 0; d < 2; ++d)
    for (uint32_t c = 0; c < 2; ++c) m.u8(kUvd_Mpeg2_f_code + d * 2 + c, static_cast<uint8_t>(pic.f_code[d][c]));   // (the codes as they are in the stream: Mesa's 0-based ones plus one)
  m.u8(kUvd_Mpeg2_intra_dc_precision, static_cast<uint8_t>(pic.intra_dc_precision));
  m.u8(kUvd_Mpeg2_pic_structure, static_cast<uint8_t>(pic.structure));
  m.u8(kUvd_Mpeg2_top_field_first, pic.top_field_first ? 1 : 0);
  m.u8(kUvd_Mpeg2_frame_pred_frame_dct, pic.frame_pred_frame_dct ? 1 : 0);
  m.u8(kUvd_Mpeg2_concealment_motion_vectors, pic.concealment_motion_vectors ? 1 : 0);
  m.u8(kUvd_Mpeg2_q_scale_type, pic.q_scale_type ? 1 : 0);
  m.u8(kUvd_Mpeg2_intra_vlc_format, pic.intra_vlc_format ? 1 : 0);
  m.u8(kUvd_Mpeg2_alternate_scan, pic.alternate_scan ? 1 : 0);
  return true;
}

bool prepare_mpeg2_feedback(ByteView message_buffer) {
  if (message_buffer.size() < kMessageFeedbackItBytes) return false;
  Image m(message_buffer);
  m.zero(kFeedbackOffset, kFeedbackBytes);
  m.u32(kFeedbackOffset, kFeedbackBytes);   // "set at least the feedback buffer size"
  return true;
}

uint32_t mpeg2_job_buffers(const JobAddresses& a, ArrayView<AmdUvd6Ring::Buffer> out) {
  if (out.size() < kMpeg2JobBuffers) return 0;
  out[0] = {amd_vcn::kCmdSessionContextBuffer, a.session_context};
  out[1] = {amd_vcn::kCmdMsgBuffer, a.message};
  out[2] = {amd_vcn::kCmdDpbBuffer, a.dpb};
  out[3] = {amd_vcn::kCmdBitstreamBuffer, a.bitstream};
  out[4] = {amd_vcn::kCmdDecodingTargetBuffer, a.target};
  out[5] = {amd_vcn::kCmdFeedbackBuffer, a.message + kFeedbackOffset};
  return kMpeg2JobBuffers;
}

uint32_t session_job_buffers(const JobAddresses& a, ArrayView<AmdUvd6Ring::Buffer> out) {
  if (out.size() < kSessionJobBuffers) return 0;
  out[0] = {amd_vcn::kCmdSessionContextBuffer, a.session_context};
  out[1] = {amd_vcn::kCmdMsgBuffer, a.message};
  return kSessionJobBuffers;
}

uint32_t decode_job_buffers(const JobAddresses& a, ArrayView<AmdUvd6Ring::Buffer> out) {
  if (out.size() < kDecodeJobBuffers) return 0;
  // radeon_uvd.c: send_msg_buf (session context, message), then ruvd_end_frame.
  out[0] = {amd_vcn::kCmdSessionContextBuffer, a.session_context};
  out[1] = {amd_vcn::kCmdMsgBuffer, a.message};
  out[2] = {amd_vcn::kCmdDpbBuffer, a.dpb};
  out[3] = {amd_vcn::kCmdContextBuffer, a.context};
  out[4] = {amd_vcn::kCmdBitstreamBuffer, a.bitstream};
  out[5] = {amd_vcn::kCmdDecodingTargetBuffer, a.target};
  out[6] = {amd_vcn::kCmdFeedbackBuffer, a.message + kFeedbackOffset};
  out[7] = {amd_vcn::kCmdItScalingTableBuffer, a.message + kItTableOffset};
  return kDecodeJobBuffers;
}

}  // namespace amd_uvd6
}  // namespace hardware
