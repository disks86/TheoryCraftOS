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


#include "Hardware/VideoCodec/Intel/IntelHevcHeaders.hpp"

namespace hardware {
namespace intel_enc {

namespace {

// Bits, most significant first, into a ByteBuffer; the last byte is padded with zeros by finish().
class Bits {
 public:
  explicit Bits(ByteBuffer* out) : out_(out) {}
  bool ok() const { return ok_; }
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
  // rbsp_trailing_bits() and byte_alignment() are the same bits: a one, then zeros to the byte boundary.
  void trailing() {
    put_bit(1);
    while (fill_ != 0) put_bit(0);
  }

 private:
  void put_bit(uint32_t b) {
    cur_ = static_cast<uint8_t>((cur_ << 1) | (b & 1u));
    ++fill_;
    if (fill_ == 8) {
      ok_ = ok_ && out_->put8(cur_);
      cur_ = 0;
      fill_ = 0;
    }
  }
  ByteBuffer* out_;
  uint8_t cur_ = 0;
  uint32_t fill_ = 0;
  bool ok_ = true;
};

constexpr uint32_t kNalTrailR = 1, kNalIdrWRadl = 19, kNalVps = 32, kNalSps = 33, kNalPps = 34;

bool valid(const HevcStreamHeaders& s) {
  return s.width >= 16 && s.height >= 16 && s.width <= 8192 && s.height <= 8192 && (s.width & 1) == 0 && (s.height & 1) == 0 && s.level_idc <= 255 &&
         s.log2_max_poc_lsb >= 4 && s.log2_max_poc_lsb <= 16 && s.cb_qp_offset >= -12 && s.cb_qp_offset <= 12 && s.cr_qp_offset >= -12 && s.cr_qp_offset <= 12;
}

// Start code and the two-byte NAL unit header (nuh_layer_id 0, nuh_temporal_id_plus1 1).
bool append_nal_header(ByteBuffer* out, uint32_t nal_type) {
  static const uint8_t kStart[4] = {0, 0, 0, 1};
  return out->append(ConstByteView(kStart, 4)) && out->put8(static_cast<uint8_t>(nal_type << 1)) && out->put8(1);
}

// Start code, NAL unit header and the RBSP bytes with emulation prevention bytes put in. On failure `out` is restored.
bool append_nal(ByteBuffer* out, uint32_t nal_type, ConstByteView rbsp, uint32_t* bits) {
  const uint64_t before = out->size();
  bool ok = append_nal_header(out, nal_type);
  uint32_t zeros = 0;
  for (uint64_t i = 0; ok && i < rbsp.size(); ++i) {
    const uint8_t b = rbsp[i];
    if (zeros >= 2 && b <= 3) {
      ok = out->put8(3);
      zeros = 0;
    }
    ok = ok && out->put8(b);
    zeros = b == 0 ? zeros + 1 : 0;
  }
  if (!ok) {
    out->truncate(before);
    return false;
  }
  *bits = static_cast<uint32_t>((out->size() - before) * 8);
  return true;
}

// profile_tier_level(1, 0): Main profile, main tier, progressive frames only, 8-bit 4:2:0 (7.3.3).
void profile_tier_level(Bits& b, const HevcStreamHeaders& s) {
  b.put(0, 2);            // general_profile_space
  b.put(0, 1);            // general_tier_flag: main
  b.put(1, 5);            // general_profile_idc: Main
  b.put(0x60000000u, 32); // general_profile_compatibility_flag[1] (Main) and [2] (Main 10)
  b.put(1, 1);            // general_progressive_source_flag
  b.put(0, 1);            // general_interlaced_source_flag
  b.put(0, 1);            // general_non_packed_constraint_flag
  b.put(1, 1);            // general_frame_only_constraint_flag
  b.put(0, 32);           // 43 reserved bits and general_inbld_flag/reserved: 44 bits of zero
  b.put(0, 12);
  b.put(s.level_idc, 8);  // general_level_idc (no sub-layers: max_sub_layers_minus1 is 0)
}

void sub_layer_ordering(Bits& b) {
  b.ue(1);                // max_dec_pic_buffering_minus1: the current picture and one reference
  b.ue(0);                // max_num_reorder_pics
  b.ue(0);                // max_latency_increase_plus1
}

uint32_t coded_size(uint32_t v) { return (v + 7) & ~7u; }

}  // namespace

bool write_hevc_vps(const HevcStreamHeaders& s, ByteBuffer* out, uint32_t* bits) {
  if (!valid(s)) return false;
  ByteBuffer rbsp(out->allocator());
  Bits b(&rbsp);
  b.put(0, 4);            // vps_video_parameter_set_id
  b.put(1, 1);            // vps_base_layer_internal_flag
  b.put(1, 1);            // vps_base_layer_available_flag
  b.put(0, 6);            // vps_max_layers_minus1
  b.put(0, 3);            // vps_max_sub_layers_minus1
  b.put(1, 1);            // vps_temporal_id_nesting_flag
  b.put(0xFFFF, 16);      // vps_reserved_0xffff_16bits
  profile_tier_level(b, s);
  b.put(1, 1);            // vps_sub_layer_ordering_info_present_flag
  sub_layer_ordering(b);
  b.put(0, 6);            // vps_max_layer_id
  b.ue(0);                // vps_num_layer_sets_minus1
  b.put(0, 1);            // vps_timing_info_present_flag
  b.put(0, 1);            // vps_extension_flag
  b.trailing();
  return b.ok() && append_nal(out, kNalVps, rbsp.view(), bits);
}

bool write_hevc_sps(const HevcStreamHeaders& s, ByteBuffer* out, uint32_t* bits) {
  if (!valid(s)) return false;
  const uint32_t cw = coded_size(s.width), ch = coded_size(s.height);
  ByteBuffer rbsp(out->allocator());
  Bits b(&rbsp);
  b.put(0, 4);            // sps_video_parameter_set_id
  b.put(0, 3);            // sps_max_sub_layers_minus1
  b.put(1, 1);            // sps_temporal_id_nesting_flag
  profile_tier_level(b, s);
  b.ue(0);                // sps_seq_parameter_set_id
  b.ue(1);                // chroma_format_idc: 4:2:0 (no separate_colour_plane_flag)
  b.ue(cw);               // pic_width_in_luma_samples
  b.ue(ch);               // pic_height_in_luma_samples
  if (cw != s.width || ch != s.height) {
    b.put(1, 1);          // conformance_window_flag (offsets are in chroma units: two luma samples)
    b.ue(0);
    b.ue((cw - s.width) / 2);
    b.ue(0);
    b.ue((ch - s.height) / 2);
  } else {
    b.put(0, 1);
  }
  b.ue(0);                // bit_depth_luma_minus8
  b.ue(0);                // bit_depth_chroma_minus8
  b.ue(s.log2_max_poc_lsb - 4);
  b.put(1, 1);            // sps_sub_layer_ordering_info_present_flag
  sub_layer_ordering(b);
  b.ue(kHevcLog2MinCb - 3);                       // log2_min_luma_coding_block_size_minus3
  b.ue(kHevcLog2Ctb - kHevcLog2MinCb);            // log2_diff_max_min_luma_coding_block_size
  b.ue(kHevcLog2MinTb - 2);                       // log2_min_luma_transform_block_size_minus2
  b.ue(kHevcLog2MaxTb - kHevcLog2MinTb);          // log2_diff_max_min_luma_transform_block_size
  b.ue(kHevcMaxTransformDepthInter);              // max_transform_hierarchy_depth_inter
  b.ue(kHevcMaxTransformDepthIntra);              // max_transform_hierarchy_depth_intra
  b.put(0, 1);            // scaling_list_enabled_flag
  b.put(0, 1);            // amp_enabled_flag
  b.put(0, 1);            // sample_adaptive_offset_enabled_flag
  b.put(0, 1);            // pcm_enabled_flag
  b.ue(0);                // num_short_term_ref_pic_sets (the slice headers carry theirs)
  b.put(0, 1);            // long_term_ref_pics_present_flag
  b.put(0, 1);            // sps_temporal_mvp_enabled_flag
  b.put(1, 1);            // strong_intra_smoothing_enabled_flag
  b.put(0, 1);            // vui_parameters_present_flag
  b.put(0, 1);            // sps_extension_present_flag
  b.trailing();
  return b.ok() && append_nal(out, kNalSps, rbsp.view(), bits);
}

bool write_hevc_pps(const HevcStreamHeaders& s, ByteBuffer* out, uint32_t* bits) {
  if (!valid(s)) return false;
  ByteBuffer rbsp(out->allocator());
  Bits b(&rbsp);
  b.ue(0);                // pps_pic_parameter_set_id
  b.ue(0);                // pps_seq_parameter_set_id
  b.put(0, 1);            // dependent_slice_segments_enabled_flag
  b.put(0, 1);            // output_flag_present_flag
  b.put(0, 3);            // num_extra_slice_header_bits
  b.put(0, 1);            // sign_data_hiding_enabled_flag
  b.put(0, 1);            // cabac_init_present_flag
  b.ue(0);                // num_ref_idx_l0_default_active_minus1
  b.ue(0);                // num_ref_idx_l1_default_active_minus1
  b.se(0);                // init_qp_minus26
  b.put(0, 1);            // constrained_intra_pred_flag
  b.put(0, 1);            // transform_skip_enabled_flag
  b.put(1, 1);            // cu_qp_delta_enabled_flag
  b.ue(kHevcDiffCuQpDeltaDepth);
  b.se(s.cb_qp_offset);   // pps_cb_qp_offset
  b.se(s.cr_qp_offset);   // pps_cr_qp_offset
  b.put(0, 1);            // pps_slice_chroma_qp_offsets_present_flag
  b.put(0, 1);            // weighted_pred_flag
  b.put(0, 1);            // weighted_bipred_flag
  b.put(0, 1);            // transquant_bypass_enabled_flag
  b.put(0, 1);            // tiles_enabled_flag
  b.put(0, 1);            // entropy_coding_sync_enabled_flag
  b.put(1, 1);            // pps_loop_filter_across_slices_enabled_flag
  b.put(1, 1);            // deblocking_filter_control_present_flag
  b.put(0, 1);            // deblocking_filter_override_enabled_flag
  b.put(0, 1);            // pps_deblocking_filter_disabled_flag
  b.se(0);                // pps_beta_offset_div2
  b.se(0);                // pps_tc_offset_div2
  b.put(0, 1);            // pps_scaling_list_data_present_flag
  b.put(0, 1);            // lists_modification_present_flag
  b.ue(kHevcLog2ParMrgLevel - 2);
  b.put(0, 1);            // slice_segment_header_extension_present_flag
  b.put(0, 1);            // pps_extension_present_flag
  b.trailing();
  return b.ok() && append_nal(out, kNalPps, rbsp.view(), bits);
}

bool write_hevc_slice_header(const HevcStreamHeaders& s, const HevcSliceHeader& h, ByteBuffer* out, uint32_t* bits) {
  if (!valid(s) || h.qp < 0 || h.qp > 51 || !h.deblocking || h.poc_lsb >= (1u << s.log2_max_poc_lsb)) return false;
  const uint64_t before = out->size();
  if (!append_nal_header(out, h.idr ? kNalIdrWRadl : kNalTrailR)) {
    out->truncate(before);
    return false;
  }
  Bits b(out);
  b.put(1, 1);                              // first_slice_segment_in_pic_flag
  if (h.idr) b.put(0, 1);                   // no_output_of_prior_pics_flag
  b.ue(0);                                  // slice_pic_parameter_set_id
  const bool p = h.p_slice && !h.idr;
  b.ue(p ? 1 : 2);                          // slice_type: P or I
  if (!h.idr) {
    b.put(h.poc_lsb, s.log2_max_poc_lsb);   // slice_pic_order_cnt_lsb
    b.put(0, 1);                            // short_term_ref_pic_set_sps_flag: the set follows (st_ref_pic_set(0))
    b.ue(1);                                // num_negative_pics (no inter_ref_pic_set_prediction_flag for set 0)
    b.ue(0);                                // num_positive_pics
    b.ue(0);                                // delta_poc_s0_minus1: the previous picture
    b.put(1, 1);                            // used_by_curr_pic_s0_flag
  }
  if (p) {
    b.put(0, 1);                            // num_ref_idx_active_override_flag
    b.ue(0);                                // five_minus_max_num_merge_cand
  }
  b.se(h.qp - 26);                          // slice_qp_delta
  b.put(1, 1);                              // slice_loop_filter_across_slices_enabled_flag
  b.trailing();                             // byte_alignment()
  if (!b.ok()) {
    out->truncate(before);
    return false;
  }
  *bits = static_cast<uint32_t>((out->size() - before) * 8);
  return true;
}

}  // namespace intel_enc
}  // namespace hardware
