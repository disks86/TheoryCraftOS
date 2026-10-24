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
constexpr uint32_t kAvcOffset = kDecodeOffset + kDecode_sizeof;   // the codec's message follows the decode message, whichever codec it is

constexpr uint32_t kMacroblock = 16;
constexpr uint32_t kItTable16x16Offset = 480, kItTable32x32Offset = 864;

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

// H.264 pictures are whole macroblocks (16), H.265 pictures whole minimum coding blocks (8 at the least).
bool size_ok(uint32_t width, uint32_t height) {
  return width >= kMacroblock && height >= kMacroblock && width <= kMaxWidth && height <= kMaxHeight && width % 8 == 0 && height % 8 == 0;
}

// The header, the indexes and the decode message, the same for every codec; `codec_message_id` and `codec_bytes` name the codec's own message that follows.
// `dynamic_dpb`: the message layout of the engines that take the picture buffer's description in a message of its own (a third index, the decode message and the
// dynamic picture buffer message in front of the codec's).
bool build_common_decode(ByteView out, uint32_t total, uint32_t stream_handle, uint32_t feedback_number, uint32_t stream_type, uint32_t codec_message_id, uint32_t codec_bytes,
                         uint32_t width, uint32_t height, const DecodeBuffers& b, bool dynamic_dpb = false) {
  const uint32_t decode_offset = dynamic_dpb ? kDecodeOffset + kIndex_sizeof : kDecodeOffset;
  const uint32_t dpb_message_offset = decode_offset + kDecode_sizeof;
  const uint32_t codec_offset = dynamic_dpb ? dpb_message_offset + kDynamicDpb_sizeof : kAvcOffset;
  if (out.size() < total || width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight) return false;
  if (b.dpb_bytes > UINT32_MAX || b.context_bytes > UINT32_MAX || b.target.bytes > UINT32_MAX || b.bitstream_bytes > UINT32_MAX - 127) return false;
  Image m(out);
  m.zero(0, total);
  m.u32(kHeader_header_size, kHeaderBytes);
  m.u32(kHeader_total_size, codec_offset);   // (Mesa: the size up to the codec's message, which is not counted)
  m.u32(kHeader_num_buffers, dynamic_dpb ? 3 : 2);
  m.u32(kHeader_msg_type, kMsgDecode);
  m.u32(kHeader_stream_handle, stream_handle);
  m.u32(kHeader_status_report_feedback_number, feedback_number);
  m.u32(kHeader_index + kIndex_message_id, kMessageDecode);
  m.u32(kHeader_index + kIndex_offset, decode_offset);
  m.u32(kHeader_index + kIndex_size, kDecode_sizeof);
  m.u32(kHeaderBytes + kIndex_message_id, codec_message_id);
  m.u32(kHeaderBytes + kIndex_offset, codec_offset);
  m.u32(kHeaderBytes + kIndex_size, codec_bytes);
  if (dynamic_dpb) {
    m.u32(kHeaderBytes + kIndex_sizeof + kIndex_message_id, kMessageDynamicDpb);
    m.u32(kHeaderBytes + kIndex_sizeof + kIndex_offset, dpb_message_offset);
    m.u32(kHeaderBytes + kIndex_sizeof + kIndex_size, kDynamicDpb_sizeof);
  }

  const uint32_t d = decode_offset;
  m.u32(d + kDecode_stream_type, stream_type);
  m.u32(d + kDecode_decode_flags, dynamic_dpb ? 1 : 0);
  m.u32(d + kDecode_width_in_samples, width);
  m.u32(d + kDecode_height_in_samples, height);
  m.u32(d + kDecode_bsd_size, static_cast<uint32_t>(align_up(b.bitstream_bytes, 128)));
  m.u32(d + kDecode_dpb_size, static_cast<uint32_t>(b.dpb_bytes));
  m.u32(d + kDecode_dt_size, static_cast<uint32_t>(b.target.bytes));
  m.u32(d + kDecode_sct_size, 0);
  m.u32(d + kDecode_sc_coeff_size, 0);
  m.u32(d + kDecode_hw_ctxt_size, static_cast<uint32_t>(b.context_bytes));
  m.u32(d + kDecode_sw_ctxt_size, kSessionContextBytes);
  m.u32(d + kDecode_db_pitch, static_cast<uint32_t>(align_up(width, 32)));
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

  return true;
}

}  // namespace

uint32_t create_message_bytes() { return kHeaderBytes + kCreate_sizeof; }
uint32_t destroy_message_bytes() { return kHeaderBytes; }
uint32_t decode_message_bytes() { return kAvcOffset + kAvc_sizeof; }
uint32_t hevc_decode_message_bytes() { return kAvcOffset + kHevc_sizeof; }

bool build_create_message(ByteView out, uint32_t stream_handle, uint32_t width, uint32_t height, uint32_t stream_type) {
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
  m.u32(kHeaderBytes + kCreate_stream_type, stream_type);
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
  if (!size_ok(width, height) || width % kMacroblock != 0 || height % kMacroblock != 0 || max_references > 16) return sizes;
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
  if (!size_ok(p.width, p.height) || !build_common_decode(out, total, stream_handle, feedback_number, kStreamTypeH264Perf, kMessageAvc, kAvc_sizeof, p.width, p.height, b)) return false;
  Image m(out);
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

namespace {
// 6.5.3: raster index (y * n + x) of the i-th position in the up-right diagonal scan of an n x n block.
void diagonal_scan(uint32_t n, uint8_t* raster) {
  uint32_t i = 0;
  int x = 0, y = 0;
  while (i < n * n) {
    while (y >= 0) {
      if (x < static_cast<int>(n) && y < static_cast<int>(n)) raster[i++] = static_cast<uint8_t>(static_cast<uint32_t>(y) * n + static_cast<uint32_t>(x));
      --y;
      ++x;
    }
    y = x;
    x = 0;
  }
}

// The scaling lists in the order the firmware reads them (Mesa converts the raster order of VA-API to the order the bitstream codes them in, which is this one):
// 4x4 lists at 0, 8x8 at 96, 16x16 at 480, 32x32 at 864; the DC values of the 16x16 and 32x32 lists go into the HEVC message.
void put_scaling_lists(Image* m, uint32_t base, const h265::ScalingList& sl) {
  uint8_t scan4[16], scan8[64];
  diagonal_scan(4, scan4);
  diagonal_scan(8, scan8);
  for (uint32_t i = 0; i < 6; ++i)
    for (uint32_t k = 0; k < 16; ++k) m->u8(base + i * 16 + k, sl.list[0][i][scan4[k]]);
  for (uint32_t i = 0; i < 6; ++i)
    for (uint32_t k = 0; k < 64; ++k) {
      m->u8(base + kItTable8x8Offset + i * 64 + k, sl.list[1][i][scan8[k]]);
      m->u8(base + kItTable16x16Offset + i * 64 + k, sl.list[2][i][scan8[k]]);
    }
  for (uint32_t i = 0; i < 2; ++i)   // the two 32x32 lists are the luma ones, matrices 0 and 3
    for (uint32_t k = 0; k < 64; ++k) m->u8(base + kItTable32x32Offset + i * 64 + k, sl.list[3][i * 3][scan8[k]]);
}

const h265::ScalingList& effective_scaling(const h265::Sps& sps, const h265::Pps& pps) { return pps.scaling_list_present ? pps.scaling : sps.scaling; }
}  // namespace

HevcBufferSizes hevc_buffer_sizes(uint32_t width, uint32_t height, uint32_t max_dec_pic_buffering) {
  HevcBufferSizes sizes;
  if (!size_ok(width, height)) return sizes;
  const uint32_t references = uint64_t{width} * height >= uint64_t{4096} * 2000 ? 8 : kMaxHevcReferences;
  if (max_dec_pic_buffering > references) return sizes;
  const uint64_t w16 = align_up(width, 16), h16 = align_up(height, 16);
  sizes.references = references;
  sizes.dpb = align_up(align_up(w16, 32) * h16 * 3 / 2, 256) * references;
  sizes.context = ((w16 + 255) / 16) * ((h16 + 255) / 16) * 16 * references + 52 * 1024;
  return sizes;
}

bool build_hevc_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const h265::OffloadPicture& picture, const DecodeBuffers& b) {
  if (!picture.sps || !picture.pps || !picture.first) return false;
  const h265::Sps& sps = *picture.sps;
  const h265::Pps& pps = *picture.pps;
  if (sps.chroma_format_idc != 1 || sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8 || sps.width <= 0 || sps.height <= 0) return false;
  const uint32_t width = static_cast<uint32_t>(sps.width), height = static_cast<uint32_t>(sps.height);
  const uint32_t total = hevc_decode_message_bytes();
  if (!size_ok(width, height) || !build_common_decode(out, total, stream_handle, feedback_number, kStreamTypeH265, kMessageHevc, kHevc_sizeof, width, height, b)) return false;
  Image m(out);
  const uint32_t h = kAvcOffset;
  auto bit = [](bool v, uint32_t n) { return v ? 1u << n : 0u; };
  m.u32(h + kHevc_sps_info_flags, bit(sps.scaling_list_enabled, 0) | bit(sps.amp, 1) | bit(sps.sao, 2) | bit(sps.pcm, 3) | bit(sps.pcm && sps.pcm_loop_filter_disabled, 4) |
                                      bit(sps.long_term_ref_pics_present, 5) | bit(sps.temporal_mvp, 6) | bit(sps.strong_intra_smoothing, 7) | bit(true, 10));   // 10: the reference lists below are used
  m.u32(h + kHevc_pps_info_flags, bit(pps.dependent_slice_segments, 0) | bit(pps.output_flag_present, 1) | bit(pps.sign_data_hiding, 2) | bit(pps.cabac_init_present, 3) |
                                      bit(pps.constrained_intra_pred, 4) | bit(pps.transform_skip, 5) | bit(pps.cu_qp_delta_enabled, 6) | bit(pps.slice_chroma_qp_offsets_present, 7) |
                                      bit(pps.weighted_pred, 8) | bit(pps.weighted_bipred, 9) | bit(pps.transquant_bypass, 10) | bit(pps.tiles_enabled, 11) |
                                      bit(pps.entropy_coding_sync, 12) | bit(pps.tiles_enabled && pps.uniform_spacing, 13) | bit(pps.tiles_enabled && pps.loop_filter_across_tiles, 14) |
                                      bit(pps.loop_filter_across_slices, 15) | bit(pps.deblocking_override_enabled, 16) | bit(pps.deblocking_disabled, 17) |
                                      bit(pps.lists_modification_present, 18) | bit(pps.slice_header_extension_present, 19));
  m.u8(h + kHevc_chroma_format, 1);
  m.u8(h + kHevc_log2_max_pic_order_cnt_lsb_minus4, static_cast<uint8_t>(sps.log2_max_poc_lsb - 4));
  const int dpb_pictures = sps.max_dec_pic_buffering[sps.max_sub_layers - 1];
  m.u8(h + kHevc_sps_max_dec_pic_buffering_minus1, static_cast<uint8_t>(dpb_pictures > 0 ? dpb_pictures - 1 : 0));
  m.u8(h + kHevc_log2_min_luma_coding_block_size_minus3, static_cast<uint8_t>(sps.log2_min_cb - 3));
  m.u8(h + kHevc_log2_diff_max_min_luma_coding_block_size, static_cast<uint8_t>(sps.log2_ctb - sps.log2_min_cb));
  m.u8(h + kHevc_log2_min_transform_block_size_minus2, static_cast<uint8_t>(sps.log2_min_tb - 2));
  m.u8(h + kHevc_log2_diff_max_min_transform_block_size, static_cast<uint8_t>(sps.log2_max_tb - sps.log2_min_tb));
  m.u8(h + kHevc_max_transform_hierarchy_depth_inter, static_cast<uint8_t>(sps.max_th_depth_inter));
  m.u8(h + kHevc_max_transform_hierarchy_depth_intra, static_cast<uint8_t>(sps.max_th_depth_intra));
  if (sps.pcm) {
    m.u8(h + kHevc_pcm_sample_bit_depth_luma_minus1, static_cast<uint8_t>(sps.pcm_bit_depth_luma - 1));
    m.u8(h + kHevc_pcm_sample_bit_depth_chroma_minus1, static_cast<uint8_t>(sps.pcm_bit_depth_chroma - 1));
    m.u8(h + kHevc_log2_min_pcm_luma_coding_block_size_minus3, static_cast<uint8_t>(sps.log2_min_pcm - 3));
    m.u8(h + kHevc_log2_diff_max_min_pcm_luma_coding_block_size, static_cast<uint8_t>(sps.log2_max_pcm - sps.log2_min_pcm));
  }
  m.u8(h + kHevc_num_extra_slice_header_bits, static_cast<uint8_t>(pps.num_extra_slice_header_bits));
  m.u8(h + kHevc_num_short_term_ref_pic_sets, static_cast<uint8_t>(sps.num_st_rps));
  m.u8(h + kHevc_num_long_term_ref_pic_sps, static_cast<uint8_t>(sps.num_lt_sps));
  m.u8(h + kHevc_num_ref_idx_l0_default_active_minus1, static_cast<uint8_t>(pps.num_ref_idx_default[0] - 1));
  m.u8(h + kHevc_num_ref_idx_l1_default_active_minus1, static_cast<uint8_t>(pps.num_ref_idx_default[1] - 1));
  m.u8(h + kHevc_pps_cb_qp_offset, static_cast<uint8_t>(pps.cb_qp_offset));
  m.u8(h + kHevc_pps_cr_qp_offset, static_cast<uint8_t>(pps.cr_qp_offset));
  m.u8(h + kHevc_pps_beta_offset_div2, static_cast<uint8_t>(pps.beta_offset_div2));
  m.u8(h + kHevc_pps_tc_offset_div2, static_cast<uint8_t>(pps.tc_offset_div2));
  m.u8(h + kHevc_diff_cu_qp_delta_depth, static_cast<uint8_t>(pps.diff_cu_qp_delta_depth));
  m.u8(h + kHevc_log2_parallel_merge_level_minus2, static_cast<uint8_t>(pps.log2_parallel_merge_level - 2));
  if (pps.tiles_enabled) {
    m.u8(h + kHevc_num_tile_columns_minus1, static_cast<uint8_t>(pps.num_tile_cols - 1));
    m.u8(h + kHevc_num_tile_rows_minus1, static_cast<uint8_t>(pps.num_tile_rows - 1));
    // The widths of all columns but the last, in CTBs minus one: given in the bitstream, or divided evenly (6.5.1).
    const int ctb_w = sps.pic_w_ctb, ctb_h = sps.pic_h_ctb;
    for (int i = 0; i < pps.num_tile_cols - 1 && i < 19; ++i) {
      const int w = pps.uniform_spacing ? ((i + 1) * ctb_w) / pps.num_tile_cols - (i * ctb_w) / pps.num_tile_cols : pps.col_width_minus1[i] + 1;
      m.u16(h + kHevc_column_width_minus1 + static_cast<uint32_t>(i) * 2, static_cast<uint16_t>(w - 1));
    }
    for (int i = 0; i < pps.num_tile_rows - 1 && i < 21; ++i) {
      const int r = pps.uniform_spacing ? ((i + 1) * ctb_h) / pps.num_tile_rows - (i * ctb_h) / pps.num_tile_rows : pps.row_height_minus1[i] + 1;
      m.u16(h + kHevc_row_height_minus1 + static_cast<uint32_t>(i) * 2, static_cast<uint16_t>(r - 1));
    }
  }
  m.u8(h + kHevc_init_qp_minus26, static_cast<uint8_t>(pps.init_qp - 26));
  m.u8(h + kHevc_num_delta_pocs_ref_rps_idx, static_cast<uint8_t>(picture.first->num_delta_pocs_ref_rps_idx));
  m.u8(h + kHevc_curr_idx, static_cast<uint8_t>(picture.slot));
  m.i32(h + kHevc_curr_poc, picture.poc);
  for (uint32_t i = 0; i < 16; ++i) {
    // Entries not in use name no picture (Mesa: 0x7F).
    const bool used = i < picture.ref_count;
    m.u8(h + kHevc_ref_pic_list + i, used ? static_cast<uint8_t>(picture.refs[i].slot) : 0x7F);
    m.i32(h + kHevc_poc_list + i * 4, used ? picture.refs[i].poc : 0);
  }
  for (uint32_t i = 0; i < 8; ++i) {
    m.u8(h + kHevc_ref_pic_set_st_curr_before + i, i < picture.st_before_count ? picture.st_before[i] : 0xFF);
    m.u8(h + kHevc_ref_pic_set_st_curr_after + i, i < picture.st_after_count ? picture.st_after[i] : 0xFF);
    m.u8(h + kHevc_ref_pic_set_lt_curr + i, i < picture.lt_count ? picture.lt_curr[i] : 0xFF);
  }
  const h265::ScalingList& sl = effective_scaling(sps, pps);
  for (uint32_t i = 0; i < 6; ++i) m.u8(h + kHevc_ucScalingListDCCoefSizeID2 + i, sl.dc[2][i]);
  for (uint32_t i = 0; i < 2; ++i) m.u8(h + kHevc_ucScalingListDCCoefSizeID3 + i, sl.dc[3][i * 3]);
  for (uint32_t l = 0; l < 2; ++l)
    for (uint32_t i = 0; i < 15; ++i) m.u8(h + kHevc_direct_reflist + l * 15 + i, picture.list[l][i]);
  return true;
}

bool prepare_hevc_feedback_and_tables(ByteView buffer, const h265::Sps& sps, const h265::Pps& pps) {
  if (buffer.size() < kMessageFeedbackItBytes) return false;
  Image m(buffer);
  m.zero(kFeedbackOffset, kFeedbackBytes);
  m.u32(kFeedbackOffset + kFeedback_header_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_total_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_num_buffers, 0);
  m.zero(kItTableOffset, kItTableBytes);
  if (sps.scaling_list_enabled) put_scaling_lists(&m, kItTableOffset, effective_scaling(sps, pps));
  return true;
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnAv1Context.inc"

uint32_t restoration_type_for_engine(av1::RestorationType type) {
  switch (type) {
    case av1::RESTORATION_WIENER: return 1;
    case av1::RESTORATION_SGRPROJ: return 2;
    case av1::RESTORATION_SWITCHABLE: return 3;
    default: return 0;
  }
}

// frame_header_flags (RDECODE_FRAME_HDR_INFO_AV1_*): one bit per syntax element, at the positions Mesa defines.
uint32_t av1_flags(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr) {
  uint32_t f = 0;
  auto set = [&f](bool v, uint32_t bit) { if (v) f |= 1u << bit; };
  set(hdr.show_frame, 0);
  set(hdr.disable_cdf_update, 1);
  set(hdr.refresh_context, 2);
  set(hdr.frame_type == av1::FRAME_TYPE_INTRA, 3);
  set(hdr.allow_intrabc, 4);
  set(hdr.hp, 5);
  set(seq.monochrome, 6);
  set(hdr.skip_mode_enabled, 7);
  set(hdr.quant.qm, 8);
  set(seq.filter_intra, 9);
  set(seq.intra_edge_filter, 10);
  set(seq.inter_intra, 11);
  set(seq.masked_compound, 12);
  set(hdr.warp_motion, 13);
  set(seq.dual_filter, 14);
  set(seq.order_hint, 15);
  set(seq.jnt_comp, 16);
  set(hdr.use_ref_frame_mvs, 17);
  set(hdr.allow_screen_content_tools, 18);
  set(hdr.force_integer_mv, 19);
  set(hdr.loopfilter.mode_ref_delta_enabled, 20);
  set(hdr.loopfilter.mode_ref_delta_update, 21);
  set(hdr.delta.q.present, 22);
  set(hdr.delta.lf.present, 23);
  set(hdr.reduced_txtp_set, 24);
  set(hdr.segmentation.enabled, 25);
  set(hdr.segmentation.update_map, 26);
  set(hdr.segmentation.temporal, 27);
  set(hdr.delta.lf.multi, 28);
  set(hdr.switchable_motion_mode, 29);   // (what Mesa calls is_motion_mode_switchable)
  set(hdr.refresh_frame_flags == 0, 30);
  set(!seq.ref_frame_mvs, 31);
  return f;
}

// The engine's segmentation feature data: eight features per segment, in the order of the syntax, and a mask of the ones in use. (The CPU decoder's header keeps the
// values, not which features were switched on: a value that is not zero, a reference, a skip or a global motion flag count as on.)
void av1_segment_features(const av1::FrameHeader& hdr, int16_t data[8][8], uint8_t mask[8]) {
  for (int i = 0; i < 8; ++i) {
    const av1::SegmentationData& d = hdr.segmentation.seg_data.d[i];
    const int16_t values[8] = {d.delta_q, d.delta_lf_y_v, d.delta_lf_y_h, d.delta_lf_u, d.delta_lf_v, static_cast<int16_t>(d.ref < 0 ? 0 : d.ref), d.skip, d.globalmv};
    mask[i] = 0;
    for (int j = 0; j < 8; ++j) {
      data[i][j] = values[j];
      const bool on = j < 5 ? values[j] != 0 : j == 5 ? d.ref >= 0 : values[j] != 0;
      if (on) mask[i] = static_cast<uint8_t>(mask[i] | (1u << j));
    }
  }
}
}  // namespace

uint32_t av1_decode_message_bytes() { return kAvcOffset + kAv1_sizeof; }
uint32_t av1_message_buffer_bytes() { return kItTableOffset + kAv1SegmentFg_sizeof; }
uint32_t av1_context_run_count() { return kAv1ContextRunCount; }
uint64_t av1_context_bytes() { return uint64_t{4} * kAv1ContextStride; }

bool av1_context_run(uint32_t index, uint32_t* offset, ConstByteView* bytes) {
  if (index >= kAv1ContextRunCount) return false;
  uint64_t start = 0;
  for (uint32_t i = 0; i < index; ++i) start += kAv1ContextRuns[i][1];
  *offset = kAv1ContextRuns[index][0];
  *bytes = ConstByteView(kAv1ContextData).subview(start, kAv1ContextRuns[index][1]);
  return true;
}

Av1BufferSizes av1_buffer_sizes(uint32_t max_width, uint32_t max_height) {
  Av1BufferSizes sizes;
  if (max_width == 0 || max_height == 0 || max_width > kMaxWidth || max_height > kMaxHeight) return sizes;
  // Mesa's context buffer: the entropy contexts (nine for the reference slots, four initial ones), the segment and loop-filter line buffers of the engine for 8k wide
  // pictures, with 64x64 or 128x128 superblocks (the larger of each kind), doubled; none of it depends on the picture's size.
  const uint64_t hw_context = align_up(kAv1HwFrameContextBytes, 2048);
  const uint64_t sdb_pitch_64 = align_up(32 * 68, 256), sdb_pitch_128 = align_up(32 * 34, 256);
  auto bigger = [](uint64_t a, uint64_t b) { return a > b ? a : b; };
  uint64_t context = (9 + 4) * hw_context + uint64_t{9} * 64 * 34 * 512 + uint64_t{9} * 64 * 34 * 256 * 5;
  context += (bigger(sdb_pitch_64 * (1728 / 32), sdb_pitch_128 * (3008 / 32)) + bigger(sdb_pitch_64 * (3232 / 32), sdb_pitch_128 * (6208 / 32)) +
              bigger(sdb_pitch_64 * (1312 / 32), sdb_pitch_128 * (2336 / 32)) + bigger(sdb_pitch_64 * (384 / 32), sdb_pitch_128 * (640 / 32))) * 2 + 68 * 512;
  sizes.context = context;
  sizes.references = kAv1References;
  // (Mesa: 8192 * 4320 * 3 / 2 * references * 3 / 2, the last factor for 10-bit pictures.)
  sizes.dpb = align_up(max_width, 64) * align_up(max_height, 64) * 3 / 2 * kAv1References * 3 / 2;
  return sizes;
}

bool build_av1_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const av1::OffloadFrame& frame, const Av1Slots& slots, const DecodeBuffers& b) {
  if (!frame.seq || !frame.hdr) return false;
  const av1::SequenceHeader& seq = *frame.seq;
  const av1::FrameHeader& hdr = *frame.hdr;
  if (seq.hbd != 0 || (seq.layout != av1::PIXEL_LAYOUT_I420 && seq.layout != av1::PIXEL_LAYOUT_I400)) return false;
  if (hdr.film_grain.present || hdr.super_res.enabled || hdr.width[0] != hdr.width[1] || hdr.width[0] <= 0 || hdr.height <= 0) return false;
  if (frame.tiles.size() > 256 || frame.tiles.size() < uint64_t{hdr.tiling.cols} * hdr.tiling.rows) return false;
  const uint32_t width = static_cast<uint32_t>(hdr.width[0]), height = static_cast<uint32_t>(hdr.height);
  const uint32_t total = av1_decode_message_bytes();
  if (!build_common_decode(out, total, stream_handle, feedback_number, kStreamTypeAv1, kMessageAv1, kAv1_sizeof, width, height, b)) return false;
  Image m(out);
  // The decode message's picture fields that differ from H.264/H.265: the picture buffer is 64 aligned and the chroma plane has its own pitch.
  m.u32(kDecodeOffset + kDecode_db_pitch, static_cast<uint32_t>(align_up(width, 64)));
  m.u32(kDecodeOffset + kDecode_db_aligned_height, static_cast<uint32_t>(align_up(height, 64)));
  m.u32(kDecodeOffset + kDecode_db_pitch_uv, b.target.chroma_pitch);
  const uint32_t a = kAvcOffset;
  m.u32(a + kAv1_frame_header_flags, av1_flags(seq, hdr));
  m.u32(a + kAv1_current_frame_id, hdr.frame_id);
  m.u32(a + kAv1_frame_offset, hdr.frame_offset);
  m.u8(a + kAv1_profile, seq.profile);
  m.u8(a + kAv1_is_annexb, 0);
  m.u8(a + kAv1_frame_type, static_cast<uint8_t>(hdr.frame_type));
  m.u8(a + kAv1_primary_ref_frame, hdr.primary_ref_frame);
  m.u8(a + kAv1_curr_pic_idx, slots.current);
  m.u8(a + kAv1_sb_size, seq.sb128);
  m.u8(a + kAv1_interp_filter, static_cast<uint8_t>(hdr.subpel_filter_mode));
  for (uint32_t i = 0; i < 2; ++i) m.u8(a + kAv1_filter_level + i, hdr.loopfilter.level_y[i]);
  m.u8(a + kAv1_filter_level_u, hdr.loopfilter.level_u);
  m.u8(a + kAv1_filter_level_v, hdr.loopfilter.level_v);
  m.u8(a + kAv1_sharpness_level, hdr.loopfilter.sharpness);
  for (uint32_t i = 0; i < 8; ++i) m.u8(a + kAv1_ref_deltas + i, static_cast<uint8_t>(hdr.loopfilter.mode_ref_deltas.ref_delta[i]));
  for (uint32_t i = 0; i < 2; ++i) m.u8(a + kAv1_mode_deltas + i, static_cast<uint8_t>(hdr.loopfilter.mode_ref_deltas.mode_delta[i]));
  m.u8(a + kAv1_base_qindex, hdr.quant.yac);
  m.u8(a + kAv1_y_dc_delta_q, static_cast<uint8_t>(hdr.quant.ydc_delta));
  m.u8(a + kAv1_u_dc_delta_q, static_cast<uint8_t>(hdr.quant.udc_delta));
  m.u8(a + kAv1_v_dc_delta_q, static_cast<uint8_t>(hdr.quant.vdc_delta));
  m.u8(a + kAv1_u_ac_delta_q, static_cast<uint8_t>(hdr.quant.uac_delta));
  m.u8(a + kAv1_v_ac_delta_q, static_cast<uint8_t>(hdr.quant.vac_delta));
  // The quantizer matrix levels carry 0xF0 in their upper half (Mesa); 15 means "none".
  m.u8(a + kAv1_qm_y, static_cast<uint8_t>((hdr.quant.qm ? hdr.quant.qm_y : 15) | 0xF0));
  m.u8(a + kAv1_qm_u, static_cast<uint8_t>((hdr.quant.qm ? hdr.quant.qm_u : 15) | 0xF0));
  m.u8(a + kAv1_qm_v, static_cast<uint8_t>((hdr.quant.qm ? hdr.quant.qm_v : 15) | 0xF0));
  m.u8(a + kAv1_delta_q_res, static_cast<uint8_t>(1u << hdr.delta.q.res_log2));
  m.u8(a + kAv1_delta_lf_res, static_cast<uint8_t>(1u << hdr.delta.lf.res_log2));
  m.u8(a + kAv1_tile_cols, hdr.tiling.cols);
  m.u8(a + kAv1_tile_rows, hdr.tiling.rows);
  m.u8(a + kAv1_tx_mode, static_cast<uint8_t>(hdr.txfm_mode));
  m.u8(a + kAv1_reference_mode, hdr.switchable_comp_refs ? 2 : 0);
  m.u8(a + kAv1_chroma_format, seq.monochrome ? 0 : 1);
  m.u32(a + kAv1_tile_size_bytes, 0xFF);
  m.u32(a + kAv1_context_update_tile_id, hdr.tiling.update);
  for (uint32_t i = 0; i < 65; ++i) {
    m.u32(a + kAv1_tile_col_start_sb + i * 4, i <= hdr.tiling.cols ? hdr.tiling.col_start_sb[i] : 0);
    m.u32(a + kAv1_tile_row_start_sb + i * 4, i <= hdr.tiling.rows ? hdr.tiling.row_start_sb[i] : 0);
  }
  m.u32(a + kAv1_max_width, static_cast<uint32_t>(seq.max_width));
  m.u32(a + kAv1_max_height, static_cast<uint32_t>(seq.max_height));
  m.u32(a + kAv1_width, width);
  m.u32(a + kAv1_height, height);
  m.u32(a + kAv1_superres_upscaled_width, static_cast<uint32_t>(hdr.width[1]));
  m.u8(a + kAv1_superres_scale_denominator, 8);
  m.u8(a + kAv1_order_hint_bits, seq.order_hint_n_bits);
  for (uint32_t i = 0; i < 8; ++i) m.u8(a + kAv1_ref_frame_map + i, slots.ref_frame_map[i]);
  for (uint32_t i = 0; i < 7; ++i) {
    const int8_t idx = hdr.refidx[i];
    m.u8(a + kAv1_frame_refs + i, idx >= 0 && idx < 8 ? slots.ref_frame_map[idx] : kAv1NoPicture);
  }
  int16_t feature_data[8][8];
  uint8_t feature_mask[8];
  av1_segment_features(hdr, feature_data, feature_mask);
  for (uint32_t i = 0; i < 8; ++i) {
    for (uint32_t j = 0; j < 8; ++j) m.i32(a + kAv1_feature_data + (i * 8 + j) * 4, feature_data[i][j]);
    m.u8(a + kAv1_feature_mask + i, feature_mask[i]);
  }
  m.u8(a + kAv1_cdef_damping, hdr.cdef.damping);
  m.u8(a + kAv1_cdef_bits, hdr.cdef.n_bits);
  for (uint32_t i = 0; i < 8; ++i) {
    m.u16(a + kAv1_cdef_strengths + i * 2, hdr.cdef.y_strength[i]);
    m.u16(a + kAv1_cdef_uv_strengths + i * 2, hdr.cdef.uv_strength[i]);
  }
  for (uint32_t i = 0; i < 3; ++i) {
    const av1::RestorationType type = hdr.restoration.type[i];
    m.u8(a + kAv1_frame_restoration_type + i, static_cast<uint8_t>(restoration_type_for_engine(type)));
    const uint32_t log2 = hdr.restoration.unit_size[i != 0];
    m.u8(a + kAv1_log2_restoration_unit_size_minus5 + i, type != av1::RESTORATION_NONE && log2 >= 5 ? static_cast<uint8_t>(log2 - 5) : 0);
  }
  m.u8(a + kAv1_preskip_segid, hdr.segmentation.seg_data.preskip);
  m.u8(a + kAv1_last_active_segid, static_cast<uint8_t>(hdr.segmentation.seg_data.last_active_segid < 0 ? 0 : hdr.segmentation.seg_data.last_active_segid));
  uint8_t lossless = 0;
  for (uint32_t i = 0; i < 8; ++i)
    if (hdr.segmentation.lossless[i]) lossless = static_cast<uint8_t>(lossless | (1u << i));
  m.u8(a + kAv1_seg_lossless_flag, lossless);
  for (uint32_t i = 0; i < 7; ++i) {
    const uint32_t g = a + kAv1_global_motion + (i + 1) * kAv1Warp_sizeof;
    m.u32(g + kAv1Warp_wmtype, static_cast<uint32_t>(hdr.gmv[i].type));
    for (uint32_t k = 0; k < 6; ++k) m.i32(g + kAv1Warp_wmmat + k * 4, hdr.gmv[i].matrix[k]);
  }
  uint64_t tile_offset = 0;
  for (uint64_t i = 0; i < frame.tiles.size(); ++i) {
    const uint64_t size = frame.tiles[i].data.size();
    if (tile_offset > UINT32_MAX || size > UINT32_MAX - tile_offset) return false;
    m.u32(a + kAv1_tile_info + static_cast<uint32_t>(i) * kAv1TileInfo_sizeof + kAv1TileInfo_offset, static_cast<uint32_t>(tile_offset));
    m.u32(a + kAv1_tile_info + static_cast<uint32_t>(i) * kAv1TileInfo_sizeof + kAv1TileInfo_size, static_cast<uint32_t>(size));
    tile_offset += size;
  }
  return true;
}

bool prepare_av1_feedback_and_tables(ByteView buffer, const av1::FrameHeader& hdr) {
  if (buffer.size() < av1_message_buffer_bytes()) return false;
  Image m(buffer);
  m.zero(kFeedbackOffset, kFeedbackBytes);
  m.u32(kFeedbackOffset + kFeedback_header_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_total_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_num_buffers, 0);
  m.zero(kItTableOffset, kAv1SegmentFg_sizeof);
  int16_t data[8][8];
  uint8_t mask[8];
  av1_segment_features(hdr, data, mask);
  for (uint32_t i = 0; i < 8; ++i)
    for (uint32_t j = 0; j < 8; ++j) m.u16(kItTableOffset + (i * 8 + j) * 2, static_cast<uint16_t>(data[i][j]));
  for (uint32_t i = 0; i < 8; ++i) m.u8(kItTableOffset + 128 + i, mask[i]);
  return true;
}

namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnVp9Probs.inc"

// Where the segmentation data sits in the table behind the message (measured with the compiler from rvcn_dec_vp9_probs_segment_t): after the 2304 bytes the
// probabilities take (2072 are used), 8 words of feature data, the tree and prediction probabilities, the delta mode, the 8 feature masks.
constexpr uint32_t kVp9TableProbsBytes = 2304, kVp9TableFeatureData = 2304, kVp9TableTreeProbs = 2336, kVp9TablePredProbs = 2343, kVp9TableAbsDelta = 2346,
                   kVp9TableFeatureMask = 2347, kVp9TableBytes = 2560;
static_assert(sizeof(kVp9DefaultProbs) == kVp9Probs_sizeof, "the probability table is a rvcn_dec_vp9_probs_t");

// frame_header_flags of the VP9 message (RDECODE_FRAME_HDR_INFO_VP9_*).
uint32_t vp9_flags(const vp9::OffloadFrame& f) {
  uint32_t flags = 0;
  auto set = [&flags](bool v, uint32_t bit) { if (v) flags |= 1u << bit; };
  set(!f.info.key_frame, 1);   // (frame_type: 0 for a key frame)
  set(f.error_resilient, 2);
  set(f.info.intra_only, 3);
  set(f.hp_mv, 4);
  set(f.refresh_frame_context, 5);
  set(f.frame_parallel, 6);
  set(f.seg.enabled, 7);
  set(f.seg.update_map, 8);
  set(f.seg.temporal_update, 9);
  set(f.mode_ref_delta_enabled, 11);
  set(f.mode_ref_delta_update, 12);
  set(f.use_prev_frame_mvs, 13);
  return flags;
}
}  // namespace

uint32_t vp9_decode_message_bytes() { return kDecodeOffset + kIndex_sizeof + kDecode_sizeof + kDynamicDpb_sizeof + kVp9_sizeof; }
uint32_t vp9_message_buffer_bytes() { return kItTableOffset + kVp9TableBytes; }
ConstByteView vp9_default_probabilities() { return ConstByteView(kVp9DefaultProbs); }

Vp9BufferSizes vp9_buffer_sizes(uint32_t width, uint32_t height) {
  Vp9BufferSizes sizes;
  if (width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight) return sizes;
  sizes.references = kVp9References;
  // Mesa for the engines before Renoir: pictures of the (32 aligned) size, nine of them; the context holds the default and the saved probabilities (five times 2304)
  // and the engine's collocated motion data and left-tile pixels, whose sizes do not depend on the picture's.
  sizes.dpb = align_up(width, 32) * align_up(height, 32) * 3 / 2 * kVp9References;
  sizes.context = uint64_t{2304} * 5 + uint64_t{32} * 2 * 64 * 64 + uint64_t{9} * 64 * 2 * 64 * 64 + uint64_t{8} * 2 * 4096;
  return sizes;
}

bool build_vp9_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const vp9::OffloadFrame& frame, const Vp9Slots& slots, const DecodeBuffers& b) {
  const vp9::FrameHeaderInfo& info = frame.info;
  if (info.bit_depth != 8 || info.width <= 0 || info.height <= 0) return false;
  const uint32_t width = static_cast<uint32_t>(info.width), height = static_cast<uint32_t>(info.height);
  const uint32_t total = vp9_decode_message_bytes();
  if (!build_common_decode(out, total, stream_handle, feedback_number, kStreamTypeVp9, kMessageVp9, kVp9_sizeof, width, height, b, /*dynamic_dpb=*/true)) return false;
  Image m(out);
  const uint32_t decode_offset = kDecodeOffset + kIndex_sizeof;
  const uint32_t dpb_offset = decode_offset + kDecode_sizeof;
  const uint32_t v = dpb_offset + kDynamicDpb_sizeof;
  // The picture buffer, described for nine pictures of the frame's size (Mesa: dpbArraySize = NUM_VP9_REFS + 1, 32 aligned, chroma at half).
  const uint32_t luma_pitch = static_cast<uint32_t>(align_up(width, 32)), luma_height = static_cast<uint32_t>(align_up(height, 32));
  m.u32(dpb_offset + kDynamicDpb_dpbLumaPitch, luma_pitch);
  m.u32(dpb_offset + kDynamicDpb_dpbLumaAlignedHeight, luma_height);
  m.u32(dpb_offset + kDynamicDpb_dpbLumaAlignedSize, luma_pitch * luma_height);
  m.u32(dpb_offset + kDynamicDpb_dpbChromaPitch, luma_pitch >> 1);
  m.u32(dpb_offset + kDynamicDpb_dpbChromaAlignedHeight, luma_height >> 1);
  m.u32(dpb_offset + kDynamicDpb_dpbChromaAlignedSize, (luma_pitch >> 1) * (luma_height >> 1) * 2);
  m.u8(dpb_offset + kDynamicDpb_dpbArraySize, static_cast<uint8_t>(kVp9References));
  m.u8(dpb_offset + kDynamicDpb_dpbReserved0, 32);
  m.u32(v + kVp9_frame_header_flags, vp9_flags(frame));
  m.u8(v + kVp9_frame_context_idx, static_cast<uint8_t>(frame.frame_context_idx));
  m.u8(v + kVp9_reset_frame_context, static_cast<uint8_t>(frame.reset_frame_context));
  m.u8(v + kVp9_curr_pic_idx, slots.current);
  m.u8(v + kVp9_interp_filter, static_cast<uint8_t>(frame.interp_filter));
  m.u8(v + kVp9_filter_level, static_cast<uint8_t>(frame.filter_level));
  m.u8(v + kVp9_sharpness_level, static_cast<uint8_t>(frame.sharpness_level));
  for (uint32_t seg = 0; seg < 8; ++seg)
    for (uint32_t ref = 0; ref < 4; ++ref)
      for (uint32_t mode = 0; mode < 2; ++mode) m.u8(v + kVp9_lf_adj_level + seg * 8 + ref * 2 + mode, frame.lf_adj_level[seg][ref][mode]);
  m.u8(v + kVp9_base_qindex, static_cast<uint8_t>(frame.base_qindex));
  m.u8(v + kVp9_y_dc_delta_q, static_cast<uint8_t>(frame.y_dc_delta_q));
  m.u8(v + kVp9_uv_ac_delta_q, static_cast<uint8_t>(frame.uv_ac_delta_q));
  m.u8(v + kVp9_uv_dc_delta_q, static_cast<uint8_t>(frame.uv_dc_delta_q));
  m.u8(v + kVp9_log2_tile_cols, static_cast<uint8_t>(frame.log2_tile_cols));
  m.u8(v + kVp9_log2_tile_rows, static_cast<uint8_t>(frame.log2_tile_rows));
  m.u8(v + kVp9_chroma_format, 1);
  for (uint32_t i = 0; i < 8; ++i) m.u8(v + kVp9_ref_frame_map + i, slots.ref_frame_map[i]);
  for (uint32_t i = 0; i < 3; ++i) {
    m.u8(v + kVp9_frame_refs + i, frame.ref_slot[i] >= 0 && frame.ref_slot[i] < 8 ? slots.ref_frame_map[frame.ref_slot[i]] : kVp9NoPicture);
    m.u8(v + kVp9_ref_frame_sign_bias + i, static_cast<uint8_t>(frame.sign_bias[i]));
  }
  m.u8(v + kVp9_bit_depth_luma_minus8, 0);
  m.u8(v + kVp9_bit_depth_chroma_minus8, 0);
  m.u32(v + kVp9_vp9_frame_size, static_cast<uint32_t>(align_up(b.bitstream_bytes, 128)));
  m.u32(v + kVp9_compressed_header_size, frame.compressed_header_bytes);
  m.u32(v + kVp9_uncompressed_header_size, frame.uncompressed_header_bytes);
  return true;
}

bool prepare_vp9_feedback_and_tables(ByteView buffer, const vp9::OffloadFrame& frame) {
  if (buffer.size() < vp9_message_buffer_bytes()) return false;
  Image m(buffer);
  m.zero(kFeedbackOffset, kFeedbackBytes);
  m.u32(kFeedbackOffset + kFeedback_header_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_total_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_num_buffers, 0);
  m.zero(kItTableOffset, kVp9TableBytes);
  for (uint32_t i = 0; i < sizeof(kVp9DefaultProbs); ++i) m.u8(kItTableOffset + i, kVp9DefaultProbs[i]);
  const vp9::Segmentation& seg = frame.seg;
  if (seg.enabled) {
    for (uint32_t i = 0; i < 8; ++i) {
      // alt_quant in the low 16 bits, alt_lf in the next 8, the reference in 4 more (Mesa's packing of the segment features), and which of the four are on.
      const uint32_t quant = static_cast<uint32_t>(seg.feature_data[i][0]) & 0xFFFF, lf = static_cast<uint32_t>(seg.feature_data[i][1]) & 0xFF, ref = static_cast<uint32_t>(seg.feature_data[i][2]) & 0xF;
      m.u32(kItTableOffset + kVp9TableFeatureData + i * 4, quant | (lf << 16) | (ref << 24));
      m.u8(kItTableOffset + kVp9TableFeatureMask + i, static_cast<uint8_t>(seg.feature_mask[i] & 0xF));
    }
    for (uint32_t i = 0; i < 7; ++i) m.u8(kItTableOffset + kVp9TableTreeProbs + i, seg.tree_probs[i]);
    for (uint32_t i = 0; i < 3; ++i) m.u8(kItTableOffset + kVp9TablePredProbs + i, seg.pred_probs[i]);
    m.u8(kItTableOffset + kVp9TableAbsDelta, seg.abs_delta ? 1 : 0);
  }
  return true;
}

// MPEG-2 ------------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t mpeg2_decode_message_bytes() { return kAvcOffset + kMpeg2_sizeof; }

Mpeg2BufferSizes mpeg2_buffer_sizes(uint32_t width, uint32_t height) {
  Mpeg2BufferSizes sizes;
  if (!size_ok(width, height)) return sizes;
  // Mesa's calc_dpb_size: the picture aligned to a macroblock, its width then to 32, with half again for the chroma, a multiple of 1024 bytes, six of them.
  const uint64_t luma = align_up(align_up(width, 16), 32) * align_up(height, 16);
  const uint64_t image = align_up(luma + luma / 2, 1024);
  sizes.references = kMpeg2References;
  sizes.dpb = image * kMpeg2References;
  return sizes;
}

bool build_mpeg2_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const mpeg2::OffloadPicture& picture, const Mpeg2Slots& slots, const DecodeBuffers& b) {
  if (!picture.seq || !picture.pic || picture.seq->chroma_format != 1) return false;
  const mpeg2::SequenceHeader& seq = *picture.seq;
  const mpeg2::PictureHeader& pic = *picture.pic;
  if (seq.mb_width <= 0 || seq.mb_height <= 0 || pic.type < mpeg2::kPictureI || pic.type > mpeg2::kPictureB || pic.structure < mpeg2::kTopField || pic.structure > mpeg2::kFramePicture) return false;
  if (slots.current >= kMpeg2References || slots.forward >= kMpeg2References || slots.backward >= kMpeg2References) return false;
  const uint32_t width = static_cast<uint32_t>(seq.mb_width) * kMacroblock, height = static_cast<uint32_t>(seq.mb_height) * kMacroblock;
  if (!size_ok(width, height) || !build_common_decode(out, mpeg2_decode_message_bytes(), stream_handle, feedback_number, kStreamTypeMpeg2, kMessageMpeg2, kMpeg2_sizeof, width, height, b)) return false;
  Image m(out);
  const uint32_t h = kAvcOffset;
  m.u32(h + kMpeg2_decoded_pic_idx, slots.current);
  m.u32(h + kMpeg2_forward_ref_pic_idx, slots.forward);
  m.u32(h + kMpeg2_backward_ref_pic_idx, slots.backward);
  // The matrices as the picture's scan orders them: the i-th byte is the weight of the i-th coefficient of the scan.
  m.u8(h + kMpeg2_load_intra_quantiser_matrix, 1);
  m.u8(h + kMpeg2_load_nonintra_quantiser_matrix, 1);
  for (uint32_t i = 0; i < 64; ++i) {
    m.u8(h + kMpeg2_intra_quantiser_matrix + i, picture.intra_matrix_scan[i]);
    m.u8(h + kMpeg2_nonintra_quantiser_matrix + i, picture.non_intra_matrix_scan[i]);
  }
  m.u8(h + kMpeg2_profile_and_level_indication, 0);
  m.u8(h + kMpeg2_chroma_format, 1);
  m.u8(h + kMpeg2_picture_coding_type, static_cast<uint8_t>(pic.type));
  for (uint32_t d = 0; d < 2; ++d)
    for (uint32_t c = 0; c < 2; ++c) m.u8(h + kMpeg2_f_code + d * 2 + c, static_cast<uint8_t>(pic.f_code[d][c]));   // (the codes as they are in the stream: Mesa's 0-based ones plus one)
  m.u8(h + kMpeg2_intra_dc_precision, static_cast<uint8_t>(pic.intra_dc_precision));
  m.u8(h + kMpeg2_pic_structure, static_cast<uint8_t>(pic.structure));
  m.u8(h + kMpeg2_top_field_first, pic.top_field_first ? 1 : 0);
  m.u8(h + kMpeg2_frame_pred_frame_dct, pic.frame_pred_frame_dct ? 1 : 0);
  m.u8(h + kMpeg2_concealment_motion_vectors, pic.concealment_motion_vectors ? 1 : 0);
  m.u8(h + kMpeg2_q_scale_type, pic.q_scale_type ? 1 : 0);
  m.u8(h + kMpeg2_intra_vlc_format, pic.intra_vlc_format ? 1 : 0);
  m.u8(h + kMpeg2_alternate_scan, pic.alternate_scan ? 1 : 0);
  return true;
}

bool prepare_mpeg2_feedback(ByteView buffer) {
  if (buffer.size() < kMessageFeedbackItBytes) return false;
  Image m(buffer);
  m.zero(kFeedbackOffset, kFeedbackBytes);
  m.u32(kFeedbackOffset + kFeedback_header_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_total_size, kFeedback_sizeof);
  m.u32(kFeedbackOffset + kFeedback_num_buffers, 0);
  return true;
}

}  // namespace amd_vcn
}  // namespace hardware
