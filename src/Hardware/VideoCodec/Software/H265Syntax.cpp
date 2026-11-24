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


// H.265 decoding, part 1: NAL units, parameter sets and slice segment headers (clauses 7.3 and 7.4 of ITU-T H.265).
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/Software/H265Trace.hpp"

namespace hardware {
namespace h265 {

namespace {
constexpr int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }

int ceil_log2(uint32_t v) {
  int n = 0;
  while ((uint64_t{1} << n) < v) ++n;
  return n;
}

// 6.5.3: the up-right diagonal scan of an N x N block: position i of the scan is (x, y).
void diagonal_scan(int n, uint8_t* sx, uint8_t* sy) {
  int i = 0, x = 0, y = 0;
  bool stop = false;
  while (!stop) {
    while (y >= 0) {
      if (x < n && y < n) {
        sx[i] = static_cast<uint8_t>(x);
        sy[i] = static_cast<uint8_t>(y);
        ++i;
      }
      --y;
      ++x;
    }
    y = x;
    x = 0;
    if (i >= n * n) stop = true;
  }
}

// Table 7-6: the default 8x8 lists, raster order.
const uint8_t kDefaultIntra8x8[64] = {
    16, 16, 16, 16, 17, 18, 21, 24, 16, 16, 16, 16, 17, 19, 22, 25, 16, 16, 17, 18, 20, 22, 25, 29, 16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47, 18, 19, 22, 27, 35, 44, 54, 65, 21, 22, 25, 31, 41, 54, 70, 88, 24, 25, 29, 36, 47, 65, 88, 115};
const uint8_t kDefaultInter8x8[64] = {
    16, 16, 16, 16, 17, 18, 20, 24, 16, 16, 16, 17, 18, 20, 24, 25, 16, 16, 17, 18, 20, 24, 25, 28, 16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41, 18, 20, 24, 25, 28, 33, 41, 54, 20, 24, 25, 28, 33, 41, 54, 71, 24, 25, 28, 33, 41, 54, 71, 91};

void set_default_list(ScalingList* s, int size_id, int matrix_id) {
  uint8_t* l = s->list[size_id][matrix_id];
  if (size_id == 0) {
    for (int i = 0; i < 16; ++i) l[i] = 16;
  } else {
    const uint8_t* src = matrix_id < 3 ? kDefaultIntra8x8 : kDefaultInter8x8;
    for (int i = 0; i < 64; ++i) l[i] = src[i];
  }
  s->dc[size_id][matrix_id] = 16;
}
}  // namespace

void ScalingList::set_default() {
  for (int size_id = 0; size_id < 4; ++size_id)
    for (int matrix_id = 0; matrix_id < 6; ++matrix_id) set_default_list(this, size_id, matrix_id);
}

// 7.3.4
bool Core::parse_scaling_list(BitReader& br, ScalingList* s) {
  s->set_default();
  uint8_t sx[64], sy[64];
  for (int size_id = 0; size_id < 4; ++size_id) {
    const int n = size_id == 0 ? 4 : 8;
    diagonal_scan(n, sx, sy);
    for (int matrix_id = 0; matrix_id < 6; matrix_id += size_id == 3 ? 3 : 1) {
      const bool pred_mode_flag = br.u(1) != 0;
      if (!pred_mode_flag) {
        const uint32_t delta = br.ue();
        if (delta == 0) {
          set_default_list(s, size_id, matrix_id);
        } else {
          const int step = size_id == 3 ? 3 : 1;
          if (delta > static_cast<uint32_t>(matrix_id / step)) return H265_FALSE;
          const int ref = matrix_id - static_cast<int>(delta) * step;
          const int count = size_id == 0 ? 16 : 64;
          for (int i = 0; i < count; ++i) s->list[size_id][matrix_id][i] = s->list[size_id][ref][i];
          s->dc[size_id][matrix_id] = s->dc[size_id][ref];
        }
      } else {
        int next = 8;
        const int count = n * n;
        if (size_id > 1) {
          const int dc = br.se() + 8;
          if (dc < 1 || dc > 255) return H265_FALSE;
          next = dc;
          s->dc[size_id][matrix_id] = static_cast<uint8_t>(dc);
        }
        for (int i = 0; i < count; ++i) {
          const int delta = br.se();
          if (delta < -128 || delta > 127) return H265_FALSE;
          next = (next + delta + 256) & 255;
          s->list[size_id][matrix_id][sy[i] * n + sx[i]] = static_cast<uint8_t>(next);
        }
      }
    }
  }
  // The 32x32 chroma matrices (only used with 4:4:4) follow the luma ones.
  const int chroma_matrices[4] = {1, 2, 4, 5};
  for (int m : chroma_matrices) {
    for (int i = 0; i < 64; ++i) s->list[3][m][i] = s->list[2][m][i];
    s->dc[3][m] = s->dc[2][m];
  }
  return !br.error();
}

// 7.3.7 / 7.4.8
bool Core::parse_st_rps(BitReader& br, int idx, int num_sets, const StRps* sets, StRps* out) {
  *out = StRps();
  bool inter = false;
  if (idx != 0) inter = br.u(1) != 0;
  if (inter) {
    int delta_idx = 1;
    if (idx == num_sets) delta_idx = static_cast<int>(br.ue()) + 1;
    if (delta_idx > idx) return H265_FALSE;
    const StRps& ref = sets[idx - delta_idx];
    if (idx == num_sets) rps_ref_delta_pocs_ = ref.num_delta_pocs();   // (the slice header's own set: a hardware decoder needs the size of the one it predicts from)
    const int sign = static_cast<int>(br.u(1));
    const int abs_delta = static_cast<int>(br.ue()) + 1;
    if (abs_delta > 32768) return H265_FALSE;
    const int delta_rps = (1 - 2 * sign) * abs_delta;
    bool used[33], use_delta[33];
    const int n = ref.num_delta_pocs();
    for (int j = 0; j <= n; ++j) {
      used[j] = br.u(1) != 0;
      use_delta[j] = true;
      if (!used[j]) use_delta[j] = br.u(1) != 0;
    }
    int i = 0;
    for (int j = ref.num_positive - 1; j >= 0; --j) {
      const int d = ref.delta_poc_s1[j] + delta_rps;
      if (d < 0 && use_delta[ref.num_negative + j]) {
        if (i >= 16) return H265_FALSE;
        out->delta_poc_s0[i] = d;
        out->used_s0[i++] = used[ref.num_negative + j];
      }
    }
    if (delta_rps < 0 && use_delta[n]) {
      if (i >= 16) return H265_FALSE;
      out->delta_poc_s0[i] = delta_rps;
      out->used_s0[i++] = used[n];
    }
    for (int j = 0; j < ref.num_negative; ++j) {
      const int d = ref.delta_poc_s0[j] + delta_rps;
      if (d < 0 && use_delta[j]) {
        if (i >= 16) return H265_FALSE;
        out->delta_poc_s0[i] = d;
        out->used_s0[i++] = used[j];
      }
    }
    out->num_negative = i;
    i = 0;
    for (int j = ref.num_negative - 1; j >= 0; --j) {
      const int d = ref.delta_poc_s0[j] + delta_rps;
      if (d > 0 && use_delta[j]) {
        if (i >= 16) return H265_FALSE;
        out->delta_poc_s1[i] = d;
        out->used_s1[i++] = used[j];
      }
    }
    if (delta_rps > 0 && use_delta[n]) {
      if (i >= 16) return H265_FALSE;
      out->delta_poc_s1[i] = delta_rps;
      out->used_s1[i++] = used[n];
    }
    for (int j = 0; j < ref.num_positive; ++j) {
      const int d = ref.delta_poc_s1[j] + delta_rps;
      if (d > 0 && use_delta[ref.num_negative + j]) {
        if (i >= 16) return H265_FALSE;
        out->delta_poc_s1[i] = d;
        out->used_s1[i++] = used[ref.num_negative + j];
      }
    }
    out->num_positive = i;
  } else {
    const uint32_t neg = br.ue(), pos = br.ue();
    if (neg > 16 || pos > 16 || neg + pos > 16) return H265_FALSE;
    out->num_negative = static_cast<int>(neg);
    out->num_positive = static_cast<int>(pos);
    int prev = 0;
    for (int i = 0; i < out->num_negative; ++i) {
      prev -= static_cast<int>(br.ue()) + 1;
      out->delta_poc_s0[i] = prev;
      out->used_s0[i] = br.u(1) != 0;
    }
    prev = 0;
    for (int i = 0; i < out->num_positive; ++i) {
      prev += static_cast<int>(br.ue()) + 1;
      out->delta_poc_s1[i] = prev;
      out->used_s1[i] = br.u(1) != 0;
    }
  }
  return !br.error();
}

namespace {
// 7.3.3 profile_tier_level(1, max_sub_layers_minus1)
bool skip_profile_tier_level(BitReader& br, int max_sub_layers_minus1) {
  br.u(8);          // profile space, tier, profile idc
  br.u(32);         // compatibility flags
  br.u(16);         // progressive, interlaced, non-packed, frame-only flags and constraint / reserved bits: 4 + 43 + 1 = 48 bits
  br.u(32);
  br.u(8);          // general_level_idc
  bool profile_present[8] = {}, level_present[8] = {};
  for (int i = 0; i < max_sub_layers_minus1; ++i) {
    profile_present[i] = br.u(1) != 0;
    level_present[i] = br.u(1) != 0;
  }
  if (max_sub_layers_minus1 > 0)
    for (int i = max_sub_layers_minus1; i < 8; ++i) br.u(2);
  for (int i = 0; i < max_sub_layers_minus1; ++i) {
    if (profile_present[i]) {
      br.u(8);
      br.u(32);
      br.u(16);
      br.u(32);
    }
    if (level_present[i]) br.u(8);
  }
  return !br.error();
}

// E.2.3: sub_layer_hrd_parameters(), read past
void skip_sub_layer_hrd(BitReader& br, uint32_t cpb_count, bool sub_pic) {
  for (uint32_t i = 0; i < cpb_count && !br.error(); ++i) {
    br.ue();
    br.ue();
    if (sub_pic) {
      br.ue();
      br.ue();
    }
    br.u(1);
  }
}

// E.2.2: hrd_parameters(commonInfPresentFlag 1, maxNumSubLayersMinus1), read past
bool skip_hrd_parameters(BitReader& br, int max_sub_layers_minus1) {
  const bool nal = br.u(1) != 0, vcl = br.u(1) != 0;
  bool sub_pic = false;
  if (nal || vcl) {
    sub_pic = br.u(1) != 0;
    if (sub_pic) br.u(8 + 5 + 1 + 5);   // tick_divisor_minus2, du_cpb_removal_delay_increment_length_minus1, sub_pic_cpb_params_in_pic_timing_sei_flag, dpb_output_delay_du_length_minus1
    br.u(4 + 4);                        // bit_rate_scale, cpb_size_scale
    if (sub_pic) br.u(4);               // cpb_size_du_scale
    br.u(5 + 5 + 5);                    // the three delay lengths
  }
  for (int i = 0; i <= max_sub_layers_minus1 && !br.error(); ++i) {
    const bool fixed_general = br.u(1) != 0;
    bool fixed_within_cvs = true;
    if (!fixed_general) fixed_within_cvs = br.u(1) != 0;
    bool low_delay = false;
    if (fixed_within_cvs) br.ue();      // elemental_duration_in_tc_minus1
    else low_delay = br.u(1) != 0;
    uint32_t cpb_count = 1;
    if (!low_delay) {
      cpb_count = br.ue() + 1;
      if (cpb_count > 32) return H265_FALSE;
    }
    if (nal) skip_sub_layer_hrd(br, cpb_count, sub_pic);
    if (vcl) skip_sub_layer_hrd(br, cpb_count, sub_pic);
  }
  return !br.error();
}

// E.2.1: vui_parameters(); the decoder keeps video_full_range_flag, and reads the rest to get to the SPS extensions after it.
bool parse_vui(BitReader& br, int max_sub_layers_minus1, Sps* s) {
  if (br.u(1)) {                       // aspect_ratio_info_present_flag
    if (br.u(8) == 255) br.u(32);      // EXTENDED_SAR: sar_width, sar_height
  }
  if (br.u(1)) br.u(1);                // overscan_info_present_flag, overscan_appropriate_flag
  if (br.u(1)) {                       // video_signal_type_present_flag
    br.u(3);                           // video_format
    s->full_range = br.u(1) != 0;
    if (br.u(1)) br.u(24);             // colour_primaries, transfer_characteristics, matrix_coeffs
  }
  if (br.u(1)) {                       // chroma_loc_info_present_flag
    br.ue();
    br.ue();
  }
  br.u(3);                             // neutral_chroma_indication_flag, field_seq_flag, frame_field_info_present_flag
  if (br.u(1))                         // default_display_window_flag
    for (int i = 0; i < 4; ++i) br.ue();
  if (br.u(1)) {                       // vui_timing_info_present_flag
    br.u(32);
    br.u(32);
    if (br.u(1)) br.ue();              // vui_poc_proportional_to_timing_flag, vui_num_ticks_poc_diff_one_minus1
    if (br.u(1) && !skip_hrd_parameters(br, max_sub_layers_minus1)) return H265_FALSE;
  }
  if (br.u(1)) {                       // bitstream_restriction_flag
    br.u(3);
    for (int i = 0; i < 5; ++i) br.ue();
  }
  return !br.error();
}
}  // namespace

// 7.3.2.2
bool Core::parse_sps(BitReader& br) {
  Sps s;
  br.u(4);   // video parameter set id
  const int max_sub_layers = static_cast<int>(br.u(3)) + 1;
  br.u(1);   // temporal id nesting
  if (max_sub_layers > 7) return H265_FALSE;
  s.max_sub_layers = max_sub_layers;
  if (!skip_profile_tier_level(br, max_sub_layers - 1)) return H265_FALSE;
  const uint32_t id = br.ue();
  if (id > 15) return H265_FALSE;
  s.id = static_cast<int>(id);
  const uint32_t chroma_format = br.ue();
  if (chroma_format > 3) return H265_FALSE;
  s.chroma_format_idc = static_cast<int>(chroma_format);
  // separate_colour_plane_flag: the three colour planes coded as three monochrome pictures (colour_plane_id). No profile of the standard allows it (A.3: the
  // profiles' SPSs have it 0), and no other decoder takes it (FFmpeg, libde265 and the HM refuse it as well): it is refused.
  if (s.chroma_format_idc == 3 && br.u(1)) return H265_FALSE;
  const uint32_t w = br.ue(), h = br.ue();
  if (w == 0 || h == 0 || w > 8192 || h > 8192) return H265_FALSE;
  s.width = static_cast<int>(w);
  s.height = static_cast<int>(h);
  if (br.u(1)) {   // conformance window, in units of chroma samples (SubWidthC x SubHeightC luma samples; one for 4:0:0 and 4:4:4)
    const uint32_t l = br.ue(), r = br.ue(), t = br.ue(), b = br.ue();
    if (l > 8192 || r > 8192 || t > 8192 || b > 8192) return H265_FALSE;
    const int unit_x = 1 << s.sub_w_shift(), unit_y = 1 << s.sub_h_shift();
    s.crop_l = static_cast<int>(l) * unit_x;
    s.crop_r = static_cast<int>(r) * unit_x;
    s.crop_t = static_cast<int>(t) * unit_y;
    s.crop_b = static_cast<int>(b) * unit_y;
    if (s.crop_l + s.crop_r >= s.width || s.crop_t + s.crop_b >= s.height) return H265_FALSE;
  }
  // 8 to 16 bits a sample, each component on its own (bit_depth_luma_minus8 0..8, 7.4.3.2.1).
  const uint32_t depth_luma = br.ue(), depth_chroma = br.ue();
  if (depth_luma > 8 || depth_chroma > 8) return H265_FALSE;
  s.bit_depth_luma = static_cast<int>(depth_luma) + 8;
  s.bit_depth_chroma = static_cast<int>(depth_chroma) + 8;
  const uint32_t poc_bits = br.ue() + 4;
  if (poc_bits > 16) return H265_FALSE;
  s.log2_max_poc_lsb = static_cast<int>(poc_bits);
  const bool ordering_present = br.u(1) != 0;
  for (int i = ordering_present ? 0 : max_sub_layers - 1; i < max_sub_layers; ++i) {
    s.max_dec_pic_buffering[i] = static_cast<int>(br.ue()) + 1;
    s.num_reorder[i] = static_cast<int>(br.ue());
    s.max_latency_increase_plus1[i] = static_cast<int>(br.ue());
  }
  if (!ordering_present)
    for (int i = 0; i < max_sub_layers - 1; ++i) {
      s.max_dec_pic_buffering[i] = s.max_dec_pic_buffering[max_sub_layers - 1];
      s.num_reorder[i] = s.num_reorder[max_sub_layers - 1];
      s.max_latency_increase_plus1[i] = s.max_latency_increase_plus1[max_sub_layers - 1];
    }
  s.log2_min_cb = static_cast<int>(br.ue()) + 3;
  s.log2_ctb = s.log2_min_cb + static_cast<int>(br.ue());
  s.log2_min_tb = static_cast<int>(br.ue()) + 2;
  s.log2_max_tb = s.log2_min_tb + static_cast<int>(br.ue());
  s.max_th_depth_inter = static_cast<int>(br.ue());
  s.max_th_depth_intra = static_cast<int>(br.ue());
  if (s.log2_ctb < 4 || s.log2_ctb > 6 || s.log2_min_cb > s.log2_ctb || s.log2_min_tb >= s.log2_min_cb || s.log2_max_tb > 5 || s.log2_max_tb > s.log2_ctb) return H265_FALSE;
  if (s.max_th_depth_inter > s.log2_ctb - s.log2_min_tb || s.max_th_depth_intra > s.log2_ctb - s.log2_min_tb) return H265_FALSE;
  const int min_cb = 1 << s.log2_min_cb;
  if (s.width % min_cb || s.height % min_cb) return H265_FALSE;
  s.scaling.set_default();
  s.scaling_list_enabled = br.u(1) != 0;
  if (s.scaling_list_enabled && br.u(1)) {
    if (!parse_scaling_list(br, &s.scaling)) return H265_FALSE;
  }
  s.amp = br.u(1) != 0;
  s.sao = br.u(1) != 0;
  s.pcm = br.u(1) != 0;
  if (s.pcm) {
    s.pcm_bit_depth_luma = static_cast<int>(br.u(4)) + 1;
    s.pcm_bit_depth_chroma = static_cast<int>(br.u(4)) + 1;
    s.log2_min_pcm = static_cast<int>(br.ue()) + 3;
    s.log2_max_pcm = s.log2_min_pcm + static_cast<int>(br.ue());
    s.pcm_loop_filter_disabled = br.u(1) != 0;
    if (s.pcm_bit_depth_luma > s.bit_depth_luma || s.pcm_bit_depth_chroma > s.bit_depth_chroma || s.log2_max_pcm > 5) return H265_FALSE;
  }
  const uint32_t num_rps = br.ue();
  if (num_rps > 64) return H265_FALSE;
  s.num_st_rps = static_cast<int>(num_rps);
  for (int i = 0; i < s.num_st_rps; ++i)
    if (!parse_st_rps(br, i, s.num_st_rps, s.st_rps, &s.st_rps[i])) return H265_FALSE;
  s.long_term_ref_pics_present = br.u(1) != 0;
  if (s.long_term_ref_pics_present) {
    const uint32_t n = br.ue();
    if (n > 32) return H265_FALSE;
    s.num_lt_sps = static_cast<int>(n);
    for (int i = 0; i < s.num_lt_sps; ++i) {
      s.lt_ref_pic_poc_lsb_sps[i] = static_cast<int>(br.u(s.log2_max_poc_lsb));
      s.used_by_curr_pic_lt_sps[i] = br.u(1) != 0;
    }
  }
  s.temporal_mvp = br.u(1) != 0;
  s.strong_intra_smoothing = br.u(1) != 0;
  if (br.u(1) && !parse_vui(br, s.max_sub_layers - 1, &s)) return H265_FALSE;
  if (br.u(1)) {   // sps_extension_present_flag
    const bool range = br.u(1) != 0, multilayer = br.u(1) != 0;
    br.u(1);       // sps_3d_extension_flag: the depth tools of 3D-HEVC layers, which a base layer does not use
    const bool scc = br.u(1) != 0;
    br.u(4);       // sps_extension_4bits (then sps_extension_data_flag, ignored)
    if (range) {   // 7.3.2.2.2
      s.transform_skip_rotation = br.u(1) != 0;
      s.transform_skip_context = br.u(1) != 0;
      s.implicit_rdpcm = br.u(1) != 0;
      s.explicit_rdpcm = br.u(1) != 0;
      s.extended_precision = br.u(1) != 0;
      s.intra_smoothing_disabled = br.u(1) != 0;
      s.high_precision_offsets = br.u(1) != 0;
      s.persistent_rice = br.u(1) != 0;
      s.cabac_bypass_alignment = br.u(1) != 0;
    }
    if (multilayer) br.u(1);   // inter_view_mv_vert_constraint_flag (other layers only)
    // The screen content coding extension (palettes, intra block copy, adaptive colour transform) changes the decoding of the base layer: not supported.
    if (scc) return H265_FALSE;
  }
  if (br.error()) return H265_FALSE;
  s.ctb_size = 1 << s.log2_ctb;
  s.pic_w_ctb = (s.width + s.ctb_size - 1) >> s.log2_ctb;
  s.pic_h_ctb = (s.height + s.ctb_size - 1) >> s.log2_ctb;
  if (static_cast<int64_t>(s.pic_w_ctb) * s.pic_h_ctb > kMaxCtbs) return H265_FALSE;
  s.valid = true;
  sps_[s.id] = s;
  have_sps_ = true;
  return true;
}

// 7.3.2.3
bool Core::parse_pps(BitReader& br) {
  Pps p;
  const uint32_t id = br.ue(), sps_id = br.ue();
  if (id > 63 || sps_id > 15) return H265_FALSE;
  p.id = static_cast<int>(id);
  p.sps_id = static_cast<int>(sps_id);
  p.dependent_slice_segments = br.u(1) != 0;
  p.output_flag_present = br.u(1) != 0;
  p.num_extra_slice_header_bits = static_cast<int>(br.u(3));
  p.sign_data_hiding = br.u(1) != 0;
  p.cabac_init_present = br.u(1) != 0;
  const uint32_t l0 = br.ue(), l1 = br.ue();
  if (l0 > 14 || l1 > 14) return H265_FALSE;
  p.num_ref_idx_default[0] = static_cast<int>(l0) + 1;
  p.num_ref_idx_default[1] = static_cast<int>(l1) + 1;
  p.init_qp = 26 + br.se();
  p.constrained_intra_pred = br.u(1) != 0;
  p.transform_skip = br.u(1) != 0;
  p.cu_qp_delta_enabled = br.u(1) != 0;
  if (p.cu_qp_delta_enabled) p.diff_cu_qp_delta_depth = static_cast<int>(br.ue());
  p.cb_qp_offset = br.se();
  p.cr_qp_offset = br.se();
  if (p.cb_qp_offset < -12 || p.cb_qp_offset > 12 || p.cr_qp_offset < -12 || p.cr_qp_offset > 12) return H265_FALSE;
  p.slice_chroma_qp_offsets_present = br.u(1) != 0;
  p.weighted_pred = br.u(1) != 0;
  p.weighted_bipred = br.u(1) != 0;
  p.transquant_bypass = br.u(1) != 0;
  p.tiles_enabled = br.u(1) != 0;
  p.entropy_coding_sync = br.u(1) != 0;
  if (p.tiles_enabled) {
    const uint32_t cols = br.ue() + 1, rows = br.ue() + 1;
    if (cols > 20 || rows > 22) return H265_FALSE;
    p.num_tile_cols = static_cast<int>(cols);
    p.num_tile_rows = static_cast<int>(rows);
    p.uniform_spacing = br.u(1) != 0;
    if (!p.uniform_spacing) {
      for (int i = 0; i < p.num_tile_cols - 1; ++i) p.col_width_minus1[i] = static_cast<int>(br.ue());
      for (int i = 0; i < p.num_tile_rows - 1; ++i) p.row_height_minus1[i] = static_cast<int>(br.ue());
    }
    p.loop_filter_across_tiles = br.u(1) != 0;
  }
  p.loop_filter_across_slices = br.u(1) != 0;
  p.deblocking_control_present = br.u(1) != 0;
  if (p.deblocking_control_present) {
    p.deblocking_override_enabled = br.u(1) != 0;
    p.deblocking_disabled = br.u(1) != 0;
    if (!p.deblocking_disabled) {
      p.beta_offset_div2 = br.se();
      p.tc_offset_div2 = br.se();
      if (p.beta_offset_div2 < -6 || p.beta_offset_div2 > 6 || p.tc_offset_div2 < -6 || p.tc_offset_div2 > 6) return H265_FALSE;
    }
  }
  p.scaling.set_default();
  p.scaling_list_present = br.u(1) != 0;
  if (p.scaling_list_present && !parse_scaling_list(br, &p.scaling)) return H265_FALSE;
  p.lists_modification_present = br.u(1) != 0;
  p.log2_parallel_merge_level = static_cast<int>(br.ue()) + 2;
  if (p.log2_parallel_merge_level > 6) return H265_FALSE;
  p.slice_header_extension_present = br.u(1) != 0;
  if (br.u(1)) {   // pps_extension_present_flag
    const bool range = br.u(1) != 0;
    br.u(1);       // pps_multilayer_extension_flag and pps_3d_extension_flag: what layers other than the base layer use (read after the range extension, so not needed)
    br.u(1);
    const bool scc = br.u(1) != 0;
    br.u(4);
    if (range) {   // 7.3.2.3.2; the limits that depend on the SPS (bit depths, ChromaArrayType, the coding block sizes) are checked when a picture starts (rext_ok)
      if (p.transform_skip) {
        const uint32_t ts = br.ue();
        if (ts > 3) return H265_FALSE;
        p.log2_max_transform_skip = static_cast<int>(ts) + 2;
      }
      p.cross_component_prediction = br.u(1) != 0;
      p.chroma_qp_offset_list = br.u(1) != 0;
      if (p.chroma_qp_offset_list) {
        const uint32_t depth = br.ue(), len = br.ue() + 1;
        if (depth > 3 || len > 6) return H265_FALSE;
        p.diff_cu_chroma_qp_offset_depth = static_cast<int>(depth);
        p.chroma_qp_offset_list_len = static_cast<int>(len);
        for (int i = 0; i < p.chroma_qp_offset_list_len; ++i) {
          p.cb_qp_offset_list[i] = br.se();
          p.cr_qp_offset_list[i] = br.se();
          if (p.cb_qp_offset_list[i] < -12 || p.cb_qp_offset_list[i] > 12 || p.cr_qp_offset_list[i] < -12 || p.cr_qp_offset_list[i] > 12) return H265_FALSE;
        }
      }
      const uint32_t sao_luma = br.ue(), sao_chroma = br.ue();
      if (sao_luma > 6 || sao_chroma > 6) return H265_FALSE;
      p.log2_sao_offset_scale_luma = static_cast<int>(sao_luma);
      p.log2_sao_offset_scale_chroma = static_cast<int>(sao_chroma);
    }
    if (scc) return H265_FALSE;   // (the screen content coding extension: not supported)
  }
  if (br.error()) return H265_FALSE;
  p.valid = true;
  pps_[p.id] = p;
  return true;
}

// 7.3.6
bool Core::parse_slice_header(BitReader& br, int nal_type, int temporal_id, SliceHeader* out) {
  SliceHeader sh;
  sh.nal_type = nal_type;
  sh.temporal_id = temporal_id;
  sh.first_slice_segment_in_pic = br.u(1) != 0;
  if (nal_type >= kBlaWLp && nal_type <= 23) sh.no_output_of_prior_pics = br.u(1) != 0;
  const uint32_t pps_id = br.ue();
  if (pps_id > 63) return H265_FALSE;
  if (external_) {
    // The parameter sets are the caller's, whatever number the slice gives (DXVA carries no parameter set ids).
    if (!picture_open_) return H265_FALSE;
    sh.pps_id = active_pps_.id;
  } else {
    if (!pps_[pps_id].valid) return H265_FALSE;
    sh.pps_id = static_cast<int>(pps_id);
  }
  // The slices after the first of a picture are read with the parameter sets the picture started with, whatever has been received since.
  const bool continuing = external_ || (picture_open_ && !sh.first_slice_segment_in_pic);
  if (continuing && !external_ && static_cast<int>(pps_id) != active_pps_.id) return H265_FALSE;
  const Pps& pps = continuing ? active_pps_ : pps_[pps_id];
  if (!continuing && !sps_[pps.sps_id].valid) return H265_FALSE;
  const Sps& sps = continuing ? active_sps_ : sps_[pps.sps_id];
  const int pic_size_in_ctbs = sps.pic_w_ctb * sps.pic_h_ctb;
  if (!sh.first_slice_segment_in_pic) {
    if (pps.dependent_slice_segments) sh.dependent_slice_segment = br.u(1) != 0;
    const uint32_t addr = br.u(ceil_log2(static_cast<uint32_t>(pic_size_in_ctbs)));
    if (addr >= static_cast<uint32_t>(pic_size_in_ctbs)) return H265_FALSE;
    sh.segment_address = static_cast<int>(addr);
  }
  if (sh.dependent_slice_segment) {
    // Everything but the address and the entry points is that of the preceding independent slice segment.
    if (!picture_open_) return H265_FALSE;
    const int saved_address = sh.segment_address;
    const bool first = sh.first_slice_segment_in_pic;
    sh = first_header_;
    sh.dependent_slice_segment = true;
    sh.segment_address = saved_address;
    sh.first_slice_segment_in_pic = first;
    sh.nal_type = nal_type;
    sh.temporal_id = temporal_id;
  } else {
    for (int i = 0; i < pps.num_extra_slice_header_bits; ++i) br.u(1);
    const uint32_t type = br.ue();
    if (type > 2) return H265_FALSE;
    sh.type = static_cast<int>(type);
    if (pps.output_flag_present) sh.pic_output_flag = br.u(1) != 0;
    if (!is_idr(nal_type)) {
      sh.poc_lsb = static_cast<int>(br.u(sps.log2_max_poc_lsb));
      const bool st_sps_flag = br.u(1) != 0;
      sh.st_rps_sps_flag = st_sps_flag;
      if (!st_sps_flag) {
        const uint64_t start = br.position();
        if (external_) {
          // The set itself comes with the picture; only its size is needed to find what follows.
          for (int left = external_st_rps_bits_; left > 0; left -= 16) br.u(left < 16 ? left : 16);
          if (br.error()) return H265_FALSE;
        } else {
          rps_ref_delta_pocs_ = 0;
          if (!parse_st_rps(br, sps.num_st_rps, sps.num_st_rps, sps.st_rps, &sh.st_rps)) return H265_FALSE;
          sh.num_delta_pocs_ref_rps_idx = rps_ref_delta_pocs_;
        }
        sh.st_rps_bits = static_cast<int>(br.position() - start);
      } else {
        int idx = 0;
        if (sps.num_st_rps > 1) idx = static_cast<int>(br.u(ceil_log2(static_cast<uint32_t>(sps.num_st_rps))));
        if (idx >= sps.num_st_rps) return H265_FALSE;
        sh.st_rps = sps.st_rps[idx];
      }
      if (sps.long_term_ref_pics_present) {
        int num_lt_sps = 0;
        if (sps.num_lt_sps > 0) num_lt_sps = static_cast<int>(br.ue());
        const int num_lt_pics = static_cast<int>(br.ue());
        if (num_lt_sps > sps.num_lt_sps || num_lt_pics < 0 || num_lt_sps + num_lt_pics > 32) return H265_FALSE;
        sh.num_lt = num_lt_sps + num_lt_pics;
        for (int i = 0; i < sh.num_lt; ++i) {
          if (i < num_lt_sps) {
            int idx = 0;
            if (sps.num_lt_sps > 1) idx = static_cast<int>(br.u(ceil_log2(static_cast<uint32_t>(sps.num_lt_sps))));
            if (idx >= sps.num_lt_sps) return H265_FALSE;
            sh.lt_poc_lsb[i] = sps.lt_ref_pic_poc_lsb_sps[idx];
            sh.lt_used_by_curr[i] = sps.used_by_curr_pic_lt_sps[idx];
          } else {
            sh.lt_poc_lsb[i] = static_cast<int>(br.u(sps.log2_max_poc_lsb));
            sh.lt_used_by_curr[i] = br.u(1) != 0;
          }
          sh.lt_msb_present[i] = br.u(1) != 0;
          int cycle = 0;
          if (sh.lt_msb_present[i]) cycle = static_cast<int>(br.ue());
          if (i == 0 || i == num_lt_sps) sh.lt_delta_poc_msb_cycle[i] = cycle;
          else sh.lt_delta_poc_msb_cycle[i] = cycle + sh.lt_delta_poc_msb_cycle[i - 1];
        }
      }
      if (sps.temporal_mvp) sh.temporal_mvp = br.u(1) != 0;
    }
    if (sps.sao) {
      sh.sao_luma = br.u(1) != 0;
      if (sps.chroma_array_type() != 0) sh.sao_chroma = br.u(1) != 0;
    }
    // NumPicTotalCurr (7-55)
    int total = 0;
    for (int i = 0; i < sh.st_rps.num_negative; ++i) total += sh.st_rps.used_s0[i];
    for (int i = 0; i < sh.st_rps.num_positive; ++i) total += sh.st_rps.used_s1[i];
    for (int i = 0; i < sh.num_lt; ++i) total += sh.lt_used_by_curr[i];
    if (external_) total = external_total_curr_;
    sh.num_pic_total_curr = total;
    if (sh.type != kSliceI) {
      sh.num_ref_idx[0] = pps.num_ref_idx_default[0];
      sh.num_ref_idx[1] = sh.type == kSliceB ? pps.num_ref_idx_default[1] : 0;
      if (br.u(1)) {
        sh.num_ref_idx[0] = static_cast<int>(br.ue()) + 1;
        if (sh.type == kSliceB) sh.num_ref_idx[1] = static_cast<int>(br.ue()) + 1;
      }
      if (sh.num_ref_idx[0] > kMaxRefs || sh.num_ref_idx[1] > kMaxRefs) return H265_FALSE;
      if (pps.lists_modification_present && total > 1) {
        const int bits = ceil_log2(static_cast<uint32_t>(total));
        for (int l = 0; l < (sh.type == kSliceB ? 2 : 1); ++l) {
          sh.list_modification[l] = br.u(1) != 0;
          if (sh.list_modification[l])
            for (int i = 0; i < sh.num_ref_idx[l]; ++i) {
              sh.list_entry[l][i] = static_cast<int>(br.u(bits));
              if (sh.list_entry[l][i] >= total) return H265_FALSE;
            }
        }
      }
      if (sh.type == kSliceB) sh.mvd_l1_zero = br.u(1) != 0;
      if (pps.cabac_init_present) sh.cabac_init_flag = br.u(1) != 0;
      if (sh.temporal_mvp) {
        if (sh.type == kSliceB) sh.collocated_from_l0 = br.u(1) != 0;
        if ((sh.collocated_from_l0 && sh.num_ref_idx[0] > 1) || (!sh.collocated_from_l0 && sh.num_ref_idx[1] > 1)) {
          sh.collocated_ref_idx = static_cast<int>(br.ue());
          if (sh.collocated_ref_idx >= sh.num_ref_idx[sh.collocated_from_l0 ? 0 : 1]) return H265_FALSE;
        }
      }
      if ((pps.weighted_pred && sh.type == kSliceP) || (pps.weighted_bipred && sh.type == kSliceB)) {
        // 7.3.6.3 pred_weight_table(); the offsets are in units of 8-bit samples unless high_precision_offsets_enabled_flag (WpOffsetHalfRangeY / C, 7-56)
        const bool chroma = sps.chroma_array_type() != 0;
        const int half_y = 1 << (sps.high_precision_offsets ? sps.bit_depth_luma - 1 : 7);
        const int half_c = 1 << (sps.high_precision_offsets ? sps.bit_depth_chroma - 1 : 7);
        sh.luma_log2_denom = static_cast<int>(br.ue());
        if (sh.luma_log2_denom > 7) return H265_FALSE;
        sh.chroma_log2_denom = sh.luma_log2_denom;
        if (chroma) sh.chroma_log2_denom += br.se();
        if (sh.chroma_log2_denom < 0 || sh.chroma_log2_denom > 7) return H265_FALSE;
        for (int l = 0; l < (sh.type == kSliceB ? 2 : 1); ++l) {
          for (int i = 0; i < sh.num_ref_idx[l]; ++i) sh.luma_weight_flag[l][i] = br.u(1) != 0;
          for (int i = 0; i < sh.num_ref_idx[l]; ++i) sh.chroma_weight_flag[l][i] = chroma && br.u(1) != 0;
          for (int i = 0; i < sh.num_ref_idx[l]; ++i) {
            sh.luma_weight[l][i] = 1 << sh.luma_log2_denom;
            sh.luma_offset[l][i] = 0;
            if (sh.luma_weight_flag[l][i]) {
              const int dw = br.se(), off = br.se();
              if (dw < -128 || dw > 127 || off < -half_y || off >= half_y) return H265_FALSE;
              sh.luma_weight[l][i] += dw;
              sh.luma_offset[l][i] = off;
            }
            for (int j = 0; j < 2; ++j) {
              sh.chroma_weight[l][i][j] = 1 << sh.chroma_log2_denom;
              sh.chroma_offset[l][i][j] = 0;
            }
            if (sh.chroma_weight_flag[l][i])
              for (int j = 0; j < 2; ++j) {
                const int dw = br.se(), doff = br.se();
                if (dw < -128 || dw > 127 || doff < -4 * half_c || doff >= 4 * half_c) return H265_FALSE;
                const int w = (1 << sh.chroma_log2_denom) + dw;
                sh.chroma_weight[l][i][j] = w;
                sh.chroma_offset[l][i][j] = clip3(-half_c, half_c - 1, (half_c + doff) - ((half_c * w) >> sh.chroma_log2_denom));   // (7-56)
              }
          }
        }
      }
      const int merge = 5 - static_cast<int>(br.ue());
      if (merge < 1 || merge > 5) return H265_FALSE;
      sh.max_num_merge_cand = merge;
    }
    sh.slice_qp = pps.init_qp + br.se();
    if (sh.slice_qp < -6 * (sps.bit_depth_luma - 8) || sh.slice_qp > 51) return H265_FALSE;   // (-QpBdOffsetY .. 51)
    if (pps.slice_chroma_qp_offsets_present) {
      sh.cb_qp_offset = br.se();
      sh.cr_qp_offset = br.se();
      if (pps.cb_qp_offset + sh.cb_qp_offset < -12 || pps.cb_qp_offset + sh.cb_qp_offset > 12 || pps.cr_qp_offset + sh.cr_qp_offset < -12 ||
          pps.cr_qp_offset + sh.cr_qp_offset > 12)
        return H265_FALSE;
    }
    if (pps.chroma_qp_offset_list) sh.cu_chroma_qp_offset_enabled = br.u(1) != 0;
    bool deblocking_override = false;
    if (pps.deblocking_override_enabled) deblocking_override = br.u(1) != 0;
    sh.deblocking_disabled = pps.deblocking_disabled;
    sh.beta_offset_div2 = pps.beta_offset_div2;
    sh.tc_offset_div2 = pps.tc_offset_div2;
    if (deblocking_override) {
      sh.deblocking_disabled = br.u(1) != 0;
      if (!sh.deblocking_disabled) {
        sh.beta_offset_div2 = br.se();
        sh.tc_offset_div2 = br.se();
        if (sh.beta_offset_div2 < -6 || sh.beta_offset_div2 > 6 || sh.tc_offset_div2 < -6 || sh.tc_offset_div2 > 6) return H265_FALSE;
      }
    }
    sh.loop_filter_across_slices = pps.loop_filter_across_slices;
    if (pps.loop_filter_across_slices && (sh.sao_luma || sh.sao_chroma || !sh.deblocking_disabled)) sh.loop_filter_across_slices = br.u(1) != 0;
  }
  if (pps.tiles_enabled || pps.entropy_coding_sync) {
    const uint32_t n = br.ue();
    if (n > static_cast<uint32_t>(pic_size_in_ctbs)) return H265_FALSE;
    sh.num_entry_points = static_cast<int>(n);
    if (n > 0) {
      const uint32_t len = br.ue() + 1;
      if (len > 32) return H265_FALSE;
      for (uint32_t i = 0; i < n; ++i) br.u(static_cast<int>(len));   // (the substreams are found by position, not by these offsets)
    }
  }
  if (pps.slice_header_extension_present) {
    const uint32_t len = br.ue();
    for (uint32_t i = 0; i < len && !br.error(); ++i) br.u(8);
  }
  // byte_alignment(): a one bit and zeros up to the next byte.
  if (br.u(1) != 1) return H265_FALSE;
  br.align();
  if (br.error()) return H265_FALSE;
  sh.data_bit_offset = br.position();
  *out = sh;
  return true;
}

// D.2.1: the messages of a suffix SEI NAL unit; only the decoded picture hash (payload type 132) is used.
void Core::parse_sei(BitReader& br) {
  while (br.bits_left() > 16 && !br.error()) {
    int type = 0, size = 0;
    for (uint32_t b = br.u(8); ; b = br.u(8)) { type += static_cast<int>(b); if (b != 0xFF || br.error()) break; }
    for (uint32_t b = br.u(8); ; b = br.u(8)) { size += static_cast<int>(b); if (b != 0xFF || br.error()) break; }
    if (br.error() || static_cast<uint64_t>(size) * 8 > br.bits_left()) return;
    const uint64_t end = br.position() + static_cast<uint64_t>(size) * 8;
    if (type == 132) {
      PendingHash h;
      h.type = static_cast<int>(br.u(8));
      bool ok = h.type <= 2;
      // one hash for each colour component: only luma for 4:0:0 (the active SPS's, which a suffix SEI follows)
      h.components = sp_ && sp_->chroma_format_idc == 0 ? 1 : 3;
      for (int c = 0; c < h.components && ok; ++c) {
        if (h.type == 0) for (int i = 0; i < 16; ++i) h.md5[c][i] = static_cast<uint8_t>(br.u(8));
        else if (h.type == 1) h.value[c] = br.u(16);
        else h.value[c] = br.u(32);
      }
      h.present = ok && !br.error();
      pending_hash_ = h;
    }
    br.seek(end);
  }
}

// D.3.19: compares the finished picture with the hash that came with it
void Core::verify_picture_hash() {
  if (!pending_hash_.present || !cur_) return;
  const PendingHash h = pending_hash_;
  pending_hash_.present = false;
  bool match = true;
  // (a monochrome picture's hash has luma alone; its chroma planes are not the picture's)
  const int components = cur_->chroma_format == 0 ? 1 : h.components;
  for (int c = 0; c < components; ++c) {
    // A picture deeper than eight bits holds 16-bit little-endian words: the hashes (8.? / D.3.19) run over those words as two bytes each, the low byte first, which is
    // what hashing the plane's bytes does; `w` counts bytes in the MD5 and CRC, samples in the checksum.
    const int bytes = cur_->deep ? 2 : 1;
    const int w = c ? cur_->chroma_width() : cur_->width, hh = c ? cur_->chroma_height() : cur_->height;
    const uint8_t* plane = cur_->plane[c].data();
    const int stride = cur_->stride[c];
    if (h.type == 0) {
      if (!hasher_) return;   // nothing to check it with
      uint8_t digest[16];
      hasher_->md5(plane, static_cast<uint32_t>(w * bytes), static_cast<uint32_t>(hh), static_cast<uint32_t>(stride), digest);
      for (int i = 0; i < 16; ++i) match = match && digest[i] == h.md5[c][i];
    } else if (h.type == 1) {
      uint32_t crc = 0xFFFF;
      for (int y = 0; y < hh; ++y)
        for (int x = 0; x < w * bytes; ++x) {
          const uint32_t byte = plane[static_cast<uint64_t>(y) * stride + x];
          for (int bit = 0; bit < 8; ++bit) {
            const uint32_t msb = (crc >> 15) & 1, v = (byte >> (7 - bit)) & 1;
            crc = (((crc << 1) + v) & 0xFFFF) ^ (msb * 0x1021);
          }
        }
      for (int bit = 0; bit < 16; ++bit) {
        const uint32_t msb = (crc >> 15) & 1;
        crc = ((crc << 1) & 0xFFFF) ^ (msb * 0x1021);
      }
      match = match && crc == h.value[c];
    } else {
      uint32_t sum = 0;
      for (int y = 0; y < hh; ++y)
        for (int x = 0; x < w; ++x) {
          const uint32_t mask = (static_cast<uint32_t>(x) & 0xFF) ^ (static_cast<uint32_t>(y) & 0xFF) ^ (static_cast<uint32_t>(x) >> 8) ^ (static_cast<uint32_t>(y) >> 8);
          const uint8_t* sample = plane + static_cast<uint64_t>(y) * stride + static_cast<uint64_t>(x) * bytes;
          sum += (sample[0] & 0xFFu) ^ mask;
          if (cur_->deep) sum += (sample[1] & 0xFFu) ^ mask;
        }
      match = match && sum == h.value[c];
    }
  }
  ++hash_stats_.checked;
  if (!match) ++hash_stats_.mismatched;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------

bool Core::configure(ConstByteView hvcc) {
  // ISO 14496-15 HEVCDecoderConfigurationRecord: 22 bytes, then arrays of parameter sets.
  if (hvcc.size() < 23 || hvcc[0] != 1) return H265_FALSE;
  nal_length_size_ = (hvcc[21] & 3) + 1;
  const int num_arrays = hvcc[22];
  uint64_t pos = 23;
  for (int a = 0; a < num_arrays; ++a) {
    if (pos + 3 > hvcc.size()) return H265_FALSE;
    pos += 1;   // array_completeness, reserved, NAL_unit_type
    const int count = (hvcc[pos] << 8) | hvcc[pos + 1];
    pos += 2;
    for (int i = 0; i < count; ++i) {
      if (pos + 2 > hvcc.size()) return H265_FALSE;
      const uint64_t len = (static_cast<uint64_t>(hvcc[pos]) << 8) | hvcc[pos + 1];
      pos += 2;
      if (len > hvcc.size() - pos) return H265_FALSE;
      if (len >= 2) handle_nal(hvcc.data() + pos, len, 0);
      pos += len;
    }
  }
  return have_sps_;
}

bool Core::decode(ConstByteView data, int64_t pts) {
  bool any = false;
  h264::for_each_nal(data, nal_length_size_, [&](const uint8_t* nal, uint64_t size) { any |= handle_nal(nal, size, pts); });
  // A call holds whole pictures: the last one is complete now.
  if (picture_open_) finish_picture();
  return any;
}

bool Core::handle_nal(const uint8_t* nal, uint64_t size, int64_t pts) {
  if (size < 3 || (nal[0] & 0x80)) return H265_FALSE;
  const int type = (nal[0] >> 1) & 0x3F;
  const int layer = ((nal[0] & 1) << 5) | (nal[1] >> 3);
  const int tid = (nal[1] & 7) - 1;
  if (layer != 0 || tid < 0) return H265_FALSE;
  if (type == kEos || type == kEob) {
    if (picture_open_) finish_picture();
    after_eos_ = true;
    return true;
  }
  const bool slice = type <= kRaslR || (type >= kBlaWLp && type <= kCra);
  if (type != kSpsNal && type != kPpsNal && type != kSeiSuffix && !slice) return H265_FALSE;
  if (rbsp_.size() < size && !rbsp_.alloc(allocator_, size + 64)) return H265_FALSE;
  const uint64_t n = h264::remove_emulation_prevention(nal + 1, size - 1, rbsp_.data());
  BitReader br;
  br.init(rbsp_.data(), n);
  if (type == kSpsNal) return parse_sps(br);
  if (type == kPpsNal) return parse_pps(br);
  if (type == kSeiSuffix) {
    if (picture_open_) parse_sei(br);
    return true;
  }
  SliceHeader sh;
  current_nal_ = ConstByteView(nal, size);
  if (!parse_slice_header(br, type, tid, &sh)) return H265_FALSE;
  if (sh.first_slice_segment_in_pic) {
    if (picture_open_) finish_picture();
    if (!start_picture(sh, pts)) return H265_FALSE;
  } else if (!picture_open_) {
    return H265_FALSE;   // the rest of a picture whose first slice was lost or skipped
  }
  return decode_slice_data(sh, br);
}

}  // namespace h265
}  // namespace hardware
