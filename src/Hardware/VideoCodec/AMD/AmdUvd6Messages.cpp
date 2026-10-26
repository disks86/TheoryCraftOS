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
  if (out.size() < kUvd_Msg_sizeof || !size_ok(p.width, p.height) || p.chroma_format != 1 || p.reference_count > 16) return false;
  if (b.dpb_bytes > UINT32_MAX || b.context_bytes > UINT32_MAX || b.target.bytes > UINT32_MAX || b.bitstream_bytes > UINT32_MAX - 127 || b.target.luma_pitch == 0) return false;
  Image m(out);
  m.zero(0, kUvd_Msg_sizeof);
  m.u32(kUvd_Msg_size, kUvd_Msg_sizeof);
  m.u32(kUvd_Msg_msg_type, kMsgDecode);
  m.u32(kUvd_Msg_stream_handle, stream_handle);
  m.u32(kUvd_Msg_status_report_feedback_number, feedback_number);

  // ruvd_end_frame: the decode message.
  m.u32(kUvd_Decode_stream_type, kStreamTypeH264Perf);
  m.u32(kUvd_Decode_decode_flags, 0x1);
  m.u32(kUvd_Decode_width_in_samples, p.width);
  m.u32(kUvd_Decode_height_in_samples, p.height);
  m.u32(kUvd_Decode_dpb_size, static_cast<uint32_t>(b.dpb_bytes));
  m.u32(kUvd_Decode_bsd_size, static_cast<uint32_t>(align_up(b.bitstream_bytes, 128)));
  m.u32(kUvd_Decode_db_pitch, static_cast<uint32_t>(align_up(p.width, kDbPitchAlignment)));
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
