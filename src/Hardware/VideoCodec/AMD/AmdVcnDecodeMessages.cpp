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


#include "Hardware/VideoCodec/AMD/AmdVcnDecodeMessages.hpp"

namespace hardware {
namespace amd_vcn {

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnMessageLayout.inc"

// A message under construction: every field is written at the byte offset the layout gives, as little-endian 32-bit words (or bytes), inside the caller's buffer.
class Image {
 public:
  explicit Image(ByteView out) : out_(out) {}
  void u32(uint32_t offset, uint32_t value) {
    ByteView word = out_.subview(offset, 4);
    for (uint32_t i = 0; i < 4; ++i) word[i] = static_cast<uint8_t>(value >> (8 * i));
  }
  void i32(uint32_t offset, int32_t value) { u32(offset, static_cast<uint32_t>(value)); }
  void u8(uint32_t offset, uint8_t value) { out_[offset] = value; }
  void u16(uint32_t offset, uint16_t value) {
    out_[offset] = static_cast<uint8_t>(value);
    out_[offset + 1] = static_cast<uint8_t>(value >> 8);
  }
  void zero(uint32_t offset, uint32_t bytes) { out_.subview(offset, bytes).fill(0); }

 private:
  ByteView out_;
};

constexpr uint32_t kHeaderBytes = kHeader_sizeof;   // the header carries the first index
constexpr uint32_t kDecodeOffset = kHeaderBytes + kIndex_sizeof;   // after the codec's index
constexpr uint32_t kAvcOffset = kDecodeOffset + kDecode_sizeof;

constexpr uint32_t kMacroblock = 16;

// level_idc -> macroblocks the level allows in the decoded picture buffer (Mesa's table; anything else gets level 5.1's)
uint32_t level_buffer_macroblocks(uint32_t level) {
  switch (level) {
    case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    default: return 184320;
  }
}

uint32_t frames_for_level(uint32_t width, uint32_t height, uint32_t level, uint32_t max_references) {
  const uint32_t width_in_mb = width / kMacroblock;
  const uint32_t height_in_mb = static_cast<uint32_t>(align_up(height / kMacroblock, 2));
  const uint32_t frame_macroblocks = width_in_mb * height_in_mb;
  const uint32_t for_level = level_buffer_macroblocks(level) / frame_macroblocks + 1;
  const uint32_t wanted = max_references + 1;   // one more for the picture being decoded
  const uint32_t capped = for_level < kMaxH264References ? for_level : kMaxH264References;
  return capped > wanted ? capped : wanted;
}

bool size_ok(uint32_t width, uint32_t height) {
  return width >= kMacroblock && height >= kMacroblock && width <= kMaxWidth && height <= kMaxHeight && width % kMacroblock == 0 && height % kMacroblock == 0;
}
}  // namespace

uint32_t create_message_bytes() { return kHeaderBytes + kCreate_sizeof; }
uint32_t destroy_message_bytes() { return kHeaderBytes; }
uint32_t decode_message_bytes() { return kAvcOffset + kAvc_sizeof; }

bool build_create_message(ByteView out, uint32_t stream_handle, uint32_t width, uint32_t height) {
  const uint32_t total = create_message_bytes();
  if (out.size() < total) return false;
  Image m(out);
  m.zero(0, total);
  m.u32(kHeader_header_size, kHeaderBytes);
  m.u32(kHeader_total_size, total);
  m.u32(kHeader_num_buffers, 1);
  m.u32(kHeader_msg_type, kMsgCreate);
  m.u32(kHeader_stream_handle, stream_handle);
  m.u32(kHeader_status_report_feedback_number, 0);
  m.u32(kHeader_index + kIndex_message_id, kMessageCreate);
  m.u32(kHeader_index + kIndex_offset, kHeaderBytes);
  m.u32(kHeader_index + kIndex_size, kCreate_sizeof);
  m.u32(kHeader_index + kIndex_filled, 0);
  m.u32(kHeaderBytes + kCreate_stream_type, kStreamTypeH264Perf);
  m.u32(kHeaderBytes + kCreate_session_flags, 0);
  m.u32(kHeaderBytes + kCreate_width_in_samples, width);
  m.u32(kHeaderBytes + kCreate_height_in_samples, height);
  return true;
}

bool build_destroy_message(ByteView out, uint32_t stream_handle) {
  if (out.size() < kHeaderBytes) return false;
  Image m(out);
  m.zero(0, kHeaderBytes);
  m.u32(kHeader_header_size, kHeaderBytes);
  m.u32(kHeader_total_size, kHeaderBytes - kIndex_sizeof);   // (Mesa: the header without its one index)
  m.u32(kHeader_num_buffers, 0);
  m.u32(kHeader_msg_type, kMsgDestroy);
  m.u32(kHeader_stream_handle, stream_handle);
  return true;
}

bool make_avc_picture_state(const h264::ParameterSets& sets, const h264::Sps& sps, const h264::Pps& pps, const h264::SliceHeader& first_slice, int32_t top_poc, int32_t bottom_poc,
                            ArrayView<const AvcReference> references, AvcPictureState* state) {
  if (!state) return false;
  const uint64_t width = uint64_t{static_cast<uint32_t>(sps.mb_w)} * kMacroblock, height = uint64_t{static_cast<uint32_t>(sps.mb_h)} * kMacroblock;
  if (sps.mb_w <= 0 || sps.mb_h <= 0 || width > kMaxWidth || height > kMaxHeight) return false;
  if (sps.chroma_format_idc != 1 || !sps.frame_mbs_only) return false;   // 4:2:0 progressive, what the parser accepts
  AvcPictureState s;
  s.width = static_cast<uint32_t>(width);
  s.height = static_cast<uint32_t>(height);
  s.level = static_cast<uint32_t>(sps.level_idc);
  switch (sps.profile_idc) {
    case 66: s.profile = kProfileBaseline; break;
    case 77: case 88: s.profile = kProfileMain; break;
    case 100: s.profile = kProfileHigh; break;
    default: return false;
  }
  s.sps_flags = (sps.direct_8x8_inference ? 1u << 0 : 0) | (sps.mbaff ? 1u << 1 : 0) | (sps.frame_mbs_only ? 1u << 2 : 0) | (sps.delta_pic_order_always_zero ? 1u << 3 : 0) | (1u << 7);
  s.pps_flags = (pps.transform_8x8 ? 1u << 0 : 0) | (pps.redundant_pic_cnt_present ? 1u << 1 : 0) | (pps.constrained_intra_pred ? 1u << 2 : 0) | (pps.deblocking_control ? 1u << 3 : 0) |
                (static_cast<uint32_t>(pps.weighted_bipred_idc & 3) << 4) | (pps.weighted_pred ? 1u << 6 : 0) | (pps.bottom_field_pic_order ? 1u << 7 : 0) | (pps.cabac ? 1u << 8 : 0);
  s.chroma_format = static_cast<uint8_t>(sps.chroma_format_idc);
  s.log2_max_frame_num_minus4 = static_cast<uint8_t>(sps.log2_max_frame_num - 4);
  s.poc_type = static_cast<uint8_t>(sps.poc_type);
  s.log2_max_poc_lsb_minus4 = static_cast<uint8_t>(sps.log2_max_poc_lsb > 4 ? sps.log2_max_poc_lsb - 4 : 0);
  s.num_ref_frames = static_cast<uint8_t>(sps.max_num_ref_frames);
  s.pic_init_qp_minus26 = static_cast<int8_t>(pps.pic_init_qp - 26);
  s.chroma_qp_index_offset = static_cast<int8_t>(pps.chroma_qp_offset[0]);
  s.second_chroma_qp_index_offset = static_cast<int8_t>(pps.chroma_qp_offset[1]);
  const int l0 = first_slice.num_ref_override ? first_slice.num_ref_idx[0] : pps.num_ref_idx_default[0];
  const int l1 = first_slice.num_ref_override ? first_slice.num_ref_idx[1] : pps.num_ref_idx_default[1];
  s.num_ref_idx_l0_active_minus1 = static_cast<uint8_t>(l0 > 0 ? l0 - 1 : 0);
  s.num_ref_idx_l1_active_minus1 = static_cast<uint8_t>(l1 > 0 ? l1 - 1 : 0);
  uint8_t w4[6][16], w8[2][64];
  sets.effective_scaling(sps, pps, w4, w8);
  h264::ParameterSets::scaling_to_bitstream_order(w4, w8, s.scaling_4x4, s.scaling_8x8);
  s.frame_num = static_cast<uint32_t>(first_slice.frame_num);
  s.top_poc = top_poc;
  s.bottom_poc = bottom_poc;
  s.reference_count = static_cast<uint32_t>(references.size() < 16 ? references.size() : 16);
  for (uint32_t i = 0; i < s.reference_count; ++i) s.references[i] = references[i];
  *state = s;
  return true;
}

TargetLayout target_layout(uint32_t width, uint32_t height) {
  TargetLayout layout;
  if (!size_ok(width, height)) return layout;
  layout.luma_pitch = static_cast<uint32_t>(align_up(width, 256));
  layout.chroma_pitch = layout.luma_pitch;
  layout.uv_pitch_texels = layout.chroma_pitch / 2;
  const uint64_t luma_bytes = uint64_t{layout.luma_pitch} * align_up(height, 32);
  layout.chroma_offset = luma_bytes;
  layout.bytes = align_up(luma_bytes + uint64_t{layout.chroma_pitch} * align_up(height / 2, 16), 4096);
  return layout;
}

AvcBufferSizes avc_buffer_sizes(uint32_t width, uint32_t height, uint32_t level, uint32_t max_references) {
  AvcBufferSizes sizes;
  if (!size_ok(width, height) || max_references > 16) return sizes;
  const uint32_t frames = frames_for_level(width, height, level, max_references);
  const uint32_t width_in_mb = width / kMacroblock;
  const uint32_t height_in_mb = static_cast<uint32_t>(align_up(height / kMacroblock, 2));
  uint64_t image = align_up(width, 32) * uint64_t{height};
  image += image / 2;
  image = align_up(image, 1024);
  sizes.references = frames;
  sizes.dpb = image * frames;
  sizes.context = uint64_t{frames} * align_up(uint64_t{width_in_mb} * height_in_mb * 192, 256);
  return sizes;
}

uint64_t bitstream_buffer_bytes(uint32_t width, uint32_t height) {
  if (!size_ok(width, height)) return 0;
  return align_up(uint64_t{width} * height * (512 / (kMacroblock * kMacroblock)), 4096);
}

bool build_avc_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const AvcPictureState& p, const DecodeBuffers& b) {
  const uint32_t total = decode_message_bytes();
  if (out.size() < total || !size_ok(p.width, p.height)) return false;
  if (b.dpb_bytes > UINT32_MAX || b.context_bytes > UINT32_MAX || b.target.bytes > UINT32_MAX || b.bitstream_bytes > UINT32_MAX - 127) return false;
  Image m(out);
  m.zero(0, total);
  m.u32(kHeader_header_size, kHeaderBytes);
  m.u32(kHeader_total_size, kAvcOffset);   // (Mesa: the size up to the codec's message, which is not counted)
  m.u32(kHeader_num_buffers, 2);
  m.u32(kHeader_msg_type, kMsgDecode);
  m.u32(kHeader_stream_handle, stream_handle);
  m.u32(kHeader_status_report_feedback_number, feedback_number);
  m.u32(kHeader_index + kIndex_message_id, kMessageDecode);
  m.u32(kHeader_index + kIndex_offset, kDecodeOffset);
  m.u32(kHeader_index + kIndex_size, kDecode_sizeof);
  m.u32(kHeaderBytes + kIndex_message_id, kMessageAvc);
  m.u32(kHeaderBytes + kIndex_offset, kAvcOffset);
  m.u32(kHeaderBytes + kIndex_size, kAvc_sizeof);

  const uint32_t d = kDecodeOffset;
  m.u32(d + kDecode_stream_type, kStreamTypeH264Perf);
  m.u32(d + kDecode_decode_flags, 0);
  m.u32(d + kDecode_width_in_samples, p.width);
  m.u32(d + kDecode_height_in_samples, p.height);
  m.u32(d + kDecode_bsd_size, static_cast<uint32_t>(align_up(b.bitstream_bytes, 128)));
  m.u32(d + kDecode_dpb_size, static_cast<uint32_t>(b.dpb_bytes));
  m.u32(d + kDecode_dt_size, static_cast<uint32_t>(b.target.bytes));
  m.u32(d + kDecode_sct_size, 0);
  m.u32(d + kDecode_sc_coeff_size, 0);
  m.u32(d + kDecode_hw_ctxt_size, static_cast<uint32_t>(b.context_bytes));
  m.u32(d + kDecode_sw_ctxt_size, kSessionContextBytes);
  m.u32(d + kDecode_db_pitch, static_cast<uint32_t>(align_up(p.width, 32)));
  m.u32(d + kDecode_db_surf_tile_config, 0);
  m.u32(d + kDecode_dt_pitch, b.target.luma_pitch);
  m.u32(d + kDecode_dt_uv_pitch, b.target.uv_pitch_texels);
  m.u32(d + kDecode_dt_tiling_mode, 0);
  m.u32(d + kDecode_dt_swizzle_mode, 0);
  m.u32(d + kDecode_dt_array_mode, kArrayModeLinear);
  m.u32(d + kDecode_dt_field_mode, 0);
  m.u32(d + kDecode_dt_surf_tile_config, 0);
  m.u32(d + kDecode_dt_uv_surf_tile_config, 0);
  m.u32(d + kDecode_dt_luma_top_offset, 0);
  m.u32(d + kDecode_dt_luma_bottom_offset, 0);
  m.u32(d + kDecode_dt_chroma_top_offset, static_cast<uint32_t>(b.target.chroma_offset));
  m.u32(d + kDecode_dt_chroma_bottom_offset, static_cast<uint32_t>(b.target.chroma_offset));

  const uint32_t a = kAvcOffset;
  m.u32(a + kAvc_profile, p.profile);
  m.u32(a + kAvc_level, p.level);
  m.u32(a + kAvc_sps_info_flags, p.sps_flags);
  m.u32(a + kAvc_pps_info_flags, p.pps_flags);
  m.u8(a + kAvc_chroma_format, p.chroma_format);
  m.u8(a + kAvc_log2_max_frame_num_minus4, p.log2_max_frame_num_minus4);
  m.u8(a + kAvc_pic_order_cnt_type, p.poc_type);
  m.u8(a + kAvc_log2_max_pic_order_cnt_lsb_minus4, p.log2_max_poc_lsb_minus4);
  m.u8(a + kAvc_num_ref_frames, p.num_ref_frames);
  m.u8(a + kAvc_pic_init_qp_minus26, static_cast<uint8_t>(p.pic_init_qp_minus26));
  m.u8(a + kAvc_chroma_qp_index_offset, static_cast<uint8_t>(p.chroma_qp_index_offset));
  m.u8(a + kAvc_second_chroma_qp_index_offset, static_cast<uint8_t>(p.second_chroma_qp_index_offset));
  m.u8(a + kAvc_num_ref_idx_l0_active_minus1, p.num_ref_idx_l0_active_minus1);
  m.u8(a + kAvc_num_ref_idx_l1_active_minus1, p.num_ref_idx_l1_active_minus1);
  for (uint32_t i = 0; i < 6; ++i)
    for (uint32_t k = 0; k < 16; ++k) m.u8(a + kAvc_scaling_list_4x4 + i * 16 + k, p.scaling_4x4[i][k]);
  for (uint32_t i = 0; i < 2; ++i)
    for (uint32_t k = 0; k < 64; ++k) m.u8(a + kAvc_scaling_list_8x8 + i * 64 + k, p.scaling_8x8[i][k]);
  m.u32(a + kAvc_frame_num, p.frame_num);
  for (uint32_t i = 0; i < p.reference_count && i < 16; ++i) {
    m.u32(a + kAvc_frame_num_list + i * 4, p.references[i].frame_num);
    m.i32(a + kAvc_field_order_cnt_list + i * 8, p.references[i].top_poc);
    m.i32(a + kAvc_field_order_cnt_list + i * 8 + 4, p.references[i].bottom_poc);
  }
  m.i32(a + kAvc_curr_field_order_cnt_list, p.top_poc);
  m.i32(a + kAvc_curr_field_order_cnt_list + 4, p.bottom_poc);
  m.u32(a + kAvc_decoded_pic_idx, p.frame_num);
  return true;
}

bool prepare_feedback_and_tables(ByteView buffer, const AvcPictureState& picture) {
  if (buffer.size() < kMessageFeedbackItBytes) return false;
  Image m(buffer);
  m.zero(kFeedbackOffset, kFeedbackBytes);
  m.u32(kFeedbackOffset + kFeedback_header_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_total_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_num_buffers, 0);
  m.zero(kItTableOffset, kItTableBytes);
  for (uint32_t i = 0; i < 6; ++i)
    for (uint32_t k = 0; k < 16; ++k) m.u8(kItTableOffset + i * 16 + k, picture.scaling_4x4[i][k]);
  for (uint32_t i = 0; i < 2; ++i)
    for (uint32_t k = 0; k < 64; ++k) m.u8(kItTableOffset + kItTable8x8Offset + i * 64 + k, picture.scaling_8x8[i][k]);
  return true;
}

Feedback read_feedback(ConstByteView buffer) {
  Feedback f;
  if (buffer.size() < kMessageFeedbackItBytes) return f;
  auto word = [&](uint32_t offset) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; ++i) v |= uint32_t{buffer[kFeedbackOffset + offset + i]} << (8 * i);
    return v;
  };
  f.valid = word(kFeedback_header_size) == kFeedback_sizeof;
  f.feedback_number = word(kFeedback_status_report_feedback_number);
  f.status = word(kFeedback_status);
  f.error_bits = word(kFeedback_errorBits);
  return f;
}

}  // namespace amd_vcn
}  // namespace hardware
