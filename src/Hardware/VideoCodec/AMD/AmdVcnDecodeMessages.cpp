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
bool build_common_decode(ByteView out, uint32_t total, uint32_t stream_handle, uint32_t feedback_number, uint32_t stream_type, uint32_t codec_message_id, uint32_t codec_bytes,
                         uint32_t width, uint32_t height, const DecodeBuffers& b) {
  if (out.size() < total || !size_ok(width, height)) return false;
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
  m.u32(kHeaderBytes + kIndex_message_id, codec_message_id);
  m.u32(kHeaderBytes + kIndex_offset, kAvcOffset);
  m.u32(kHeaderBytes + kIndex_size, codec_bytes);

  const uint32_t d = kDecodeOffset;
  m.u32(d + kDecode_stream_type, stream_type);
  m.u32(d + kDecode_decode_flags, 0);
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
  if (!build_common_decode(out, total, stream_handle, feedback_number, kStreamTypeH264Perf, kMessageAvc, kAvc_sizeof, p.width, p.height, b)) return false;
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
  if (!build_common_decode(out, total, stream_handle, feedback_number, kStreamTypeH265, kMessageHevc, kHevc_sizeof, width, height, b)) return false;
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

}  // namespace amd_vcn
}  // namespace hardware
