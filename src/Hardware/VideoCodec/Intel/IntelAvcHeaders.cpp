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


#include "Hardware/VideoCodec/Intel/IntelAvcHeaders.hpp"

namespace hardware {
namespace intel_enc {

namespace {

// Bits, most significant first, into a ByteBuffer; the last byte is padded with zeros by finish().
class Bits {
 public:
  explicit Bits(ByteBuffer* out) : out_(out) {}
  bool ok() const { return ok_; }
  uint64_t bits() const { return count_; }
  void put(uint32_t value, uint32_t n) {
    for (uint32_t i = n; i > 0; --i) put_bit((value >> (i - 1)) & 1u);
  }
  void ue(uint32_t v) {
    const uint32_t x = v + 1;
    uint32_t len = 0;
    while ((x >> len) > 1) ++len;
    put(0, len);
    put(x, len + 1);
  }
  void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v)); }
  // rbsp_trailing_bits: a one, then zeros to the byte boundary.
  void trailing() {
    put_bit(1);
    while (fill_ != 0) put_bit(0);
  }
  void flush() {
    if (fill_ != 0) {
      ok_ = ok_ && out_->put8(static_cast<uint8_t>(cur_ << (8 - fill_)));
      cur_ = 0;
      fill_ = 0;
    }
  }

 private:
  void put_bit(uint32_t b) {
    cur_ = static_cast<uint8_t>((cur_ << 1) | (b & 1u));
    ++fill_;
    ++count_;
    if (fill_ == 8) {
      ok_ = ok_ && out_->put8(cur_);
      cur_ = 0;
      fill_ = 0;
    }
  }
  ByteBuffer* out_;
  uint8_t cur_ = 0;
  uint32_t fill_ = 0;
  uint64_t count_ = 0;
  bool ok_ = true;
};

constexpr uint32_t kNalSps = 7, kNalPps = 8, kNalSliceIdr = 5, kNalSlice = 1;

bool valid(const AvcStreamHeaders& s) {
  return s.width >= 1 && s.height >= 1 && s.width <= 4096 && s.height <= 4096 && s.log2_max_frame_num >= 4 && s.log2_max_frame_num <= 16 && s.chroma_qp_offset >= -12 &&
         s.chroma_qp_offset <= 12;
}

// Start code, NAL unit header and the RBSP bytes with emulation prevention bytes put in.
bool append_nal(ByteBuffer* out, uint32_t nal_ref_idc, uint32_t nal_type, ConstByteView rbsp) {
  static const uint8_t kStart[4] = {0, 0, 0, 1};
  if (!out->append(ConstByteView(kStart, 4)) || !out->put8(static_cast<uint8_t>((nal_ref_idc << 5) | nal_type))) return false;
  uint32_t zeros = 0;
  for (uint64_t i = 0; i < rbsp.size(); ++i) {
    const uint8_t b = rbsp[i];
    if (zeros >= 2 && b <= 3) {
      if (!out->put8(3)) return false;
      zeros = 0;
    }
    if (!out->put8(b)) return false;
    zeros = b == 0 ? zeros + 1 : 0;
  }
  return true;
}

}  // namespace

bool write_avc_sps(const AvcStreamHeaders& s, ByteBuffer* out, uint32_t* bits) {
  if (!valid(s)) return false;
  const uint32_t mb_w = (s.width + 15) / 16, mb_h = (s.height + 15) / 16;
  ByteBuffer rbsp(out->allocator());
  Bits b(&rbsp);
  b.put(66, 8);        // profile_idc: Baseline
  b.put(0x40, 8);      // constraint_set1_flag (Constrained Baseline), the rest zero
  b.put(s.level_idc, 8);
  b.ue(0);             // seq_parameter_set_id
  b.ue(s.log2_max_frame_num - 4);
  b.ue(2);             // pic_order_cnt_type: output order is decoding order
  b.ue(1);             // max_num_ref_frames
  b.put(0, 1);         // gaps_in_frame_num_value_allowed_flag
  b.ue(mb_w - 1);
  b.ue(mb_h - 1);
  b.put(1, 1);         // frame_mbs_only_flag
  b.put(1, 1);         // direct_8x8_inference_flag
  const uint32_t crop_r = (mb_w * 16 - s.width) / 2, crop_b = (mb_h * 16 - s.height) / 2;   // (4:2:0 crop units are two samples)
  if (crop_r || crop_b) {
    b.put(1, 1);
    b.ue(0);
    b.ue(crop_r);
    b.ue(0);
    b.ue(crop_b);
  } else {
    b.put(0, 1);
  }
  b.put(0, 1);         // vui_parameters_present_flag
  b.trailing();
  if (!b.ok()) return false;
  const uint64_t before = out->size();
  if (!append_nal(out, 3, kNalSps, rbsp.view())) return false;
  *bits = static_cast<uint32_t>((out->size() - before) * 8);
  return true;
}

bool write_avc_pps(const AvcStreamHeaders& s, ByteBuffer* out, uint32_t* bits) {
  if (!valid(s)) return false;
  ByteBuffer rbsp(out->allocator());
  Bits b(&rbsp);
  b.ue(0);             // pic_parameter_set_id
  b.ue(0);             // seq_parameter_set_id
  b.put(0, 1);         // entropy_coding_mode_flag: CAVLC
  b.put(0, 1);         // bottom_field_pic_order_in_frame_present_flag
  b.ue(0);             // num_slice_groups_minus1
  b.ue(0);             // num_ref_idx_l0_default_active_minus1
  b.ue(0);             // num_ref_idx_l1_default_active_minus1
  b.put(0, 1);         // weighted_pred_flag
  b.put(0, 2);         // weighted_bipred_idc
  b.se(0);             // pic_init_qp_minus26
  b.se(0);             // pic_init_qs_minus26
  b.se(s.chroma_qp_offset);
  b.put(1, 1);         // deblocking_filter_control_present_flag
  b.put(0, 1);         // constrained_intra_pred_flag
  b.put(0, 1);         // redundant_pic_cnt_present_flag
  b.trailing();
  if (!b.ok()) return false;
  const uint64_t before = out->size();
  if (!append_nal(out, 3, kNalPps, rbsp.view())) return false;
  *bits = static_cast<uint32_t>((out->size() - before) * 8);
  return true;
}

bool write_avc_slice_header(const AvcStreamHeaders& s, const AvcSliceHeader& h, ByteBuffer* out, uint32_t* bits) {
  if (!valid(s) || h.qp < 0 || h.qp > 51 || h.frame_num >= (1u << s.log2_max_frame_num)) return false;
  static const uint8_t kStart[4] = {0, 0, 0, 1};
  const uint64_t before = out->size();
  if (!out->append(ConstByteView(kStart, 4)) || !out->put8(static_cast<uint8_t>((3u << 5) | (h.idr ? kNalSliceIdr : kNalSlice)))) return false;
  Bits b(out);
  b.ue(0);                                  // first_mb_in_slice
  b.ue(h.p_slice ? 5 : 7);                  // slice_type (all slices of the picture have this type)
  b.ue(0);                                  // pic_parameter_set_id
  b.put(h.frame_num, s.log2_max_frame_num);
  if (h.idr) b.ue(h.idr_pic_id);
  if (h.p_slice) {
    b.put(0, 1);                            // num_ref_idx_active_override_flag
    b.put(0, 1);                            // ref_pic_list_modification_flag_l0
  }
  if (h.idr) {
    b.put(0, 1);                            // no_output_of_prior_pics_flag
    b.put(0, 1);                            // long_term_reference_flag
  } else {
    b.put(0, 1);                            // adaptive_ref_pic_marking_mode_flag
  }
  b.se(h.qp - 26);                          // slice_qp_delta
  b.ue(h.deblocking ? 0 : 1);               // disable_deblocking_filter_idc
  if (h.deblocking) {
    b.se(h.alpha_c0_offset_div2);
    b.se(h.beta_offset_div2);
  }
  const uint64_t header_bits = b.bits();
  b.flush();
  if (!b.ok()) return false;
  *bits = static_cast<uint32_t>((out->size() - before - (header_bits + 7) / 8) * 8 + header_bits);
  return true;
}

}  // namespace intel_enc
}  // namespace hardware
