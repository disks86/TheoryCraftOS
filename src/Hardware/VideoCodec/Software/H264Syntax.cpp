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

// H.264 decoding, part 1: NAL units, sequence/picture parameter sets, slice headers, picture order counts, the
// decoded picture buffer (reference marking, reference lists, output order) and the public H264Decoder class.

#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h264 {

#include "Hardware/VideoCodec/Software/H264Tables.inc"

namespace {

inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }

// Table A-1: MaxDpbMbs by level_idc.
int max_dpb_mbs(int level) {
  switch (level) {
    case 9: case 10: return 396;
    case 11: return 900;
    case 12: case 13: case 20: return 2376;
    case 21: return 4752;
    case 22: case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 40: case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    case 51: case 52: return 184320;
    default: return level > 52 ? 696320 : 396;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Bit reading and VLC trees

void BitReader::init(const uint8_t* data, uint64_t bytes) {
  data_ = data;
  size_ = bytes * 8;
  pos_ = 0;
  error_ = false;
  stop_ = 0;
  // The rbsp_stop_one_bit is the last 1 bit of the data.
  uint64_t i = bytes;
  while (i > 0 && data[i - 1] == 0) --i;
  if (i == 0) return;
  const uint8_t last = data[i - 1];
  int tz = 0;
  while (!((last >> tz) & 1)) ++tz;
  stop_ = (i - 1) * 8 + static_cast<uint64_t>(7 - tz);
}

uint32_t BitReader::ue() {
  int zeros = 0;
  while (zeros < 32) {
    if (pos_ >= size_) {
      error_ = true;
      return 0;
    }
    if (bit()) break;
    ++zeros;
  }
  if (zeros >= 32) {
    error_ = true;
    return 0;
  }
  if (zeros == 0) return 0;
  return ((1u << zeros) - 1) + u(zeros);
}

void VlcTree::build(const VlcEntry* entries, int n) {
  count = 1;
  node[0][0] = node[0][1] = 0;  // 0 = no child (the root is never a child)
  for (int e = 0; e < n; ++e) {
    int cur = 0;
    for (int b = entries[e].len - 1; b >= 0; --b) {
      const int bit = (entries[e].code >> b) & 1;
      if (b == 0) {
        node[cur][bit] = static_cast<int16_t>(~static_cast<int>(entries[e].sym));
      } else {
        if (node[cur][bit] <= 0) {
          node[count][0] = node[count][1] = 0;
          node[cur][bit] = static_cast<int16_t>(count++);
        }
        cur = node[cur][bit];
      }
    }
  }
}

int VlcTree::decode(BitReader& br) const {
  int cur = 0;
  for (int depth = 0; depth < 20; ++depth) {
    const int next = node[cur][br.bit()];
    if (br.error() || next == 0) return -1;
    if (next < 0) return ~next;
    cur = next;
  }
  return -1;
}

void Core::build_vlc() {
  if (vlc_ready_) return;
  coeff_token_[0].build(kCoeffToken0, 62);
  coeff_token_[1].build(kCoeffToken1, 62);
  coeff_token_[2].build(kCoeffToken2, 62);
  coeff_token_[3].build(kCoeffToken3, 62);
  coeff_token_chroma_dc_.build(kCoeffTokenChromaDc, 14);
  const VlcEntry* tz[15] = {kTotalZeros1, kTotalZeros2, kTotalZeros3, kTotalZeros4, kTotalZeros5,
                            kTotalZeros6, kTotalZeros7, kTotalZeros8, kTotalZeros9, kTotalZeros10,
                            kTotalZeros11, kTotalZeros12, kTotalZeros13, kTotalZeros14, kTotalZeros15};
  const int tzn[15] = {sizeof(kTotalZeros1) / sizeof(VlcEntry), sizeof(kTotalZeros2) / sizeof(VlcEntry), sizeof(kTotalZeros3) / sizeof(VlcEntry), sizeof(kTotalZeros4) / sizeof(VlcEntry),
                       sizeof(kTotalZeros5) / sizeof(VlcEntry), sizeof(kTotalZeros6) / sizeof(VlcEntry), sizeof(kTotalZeros7) / sizeof(VlcEntry), sizeof(kTotalZeros8) / sizeof(VlcEntry),
                       sizeof(kTotalZeros9) / sizeof(VlcEntry), sizeof(kTotalZeros10) / sizeof(VlcEntry), sizeof(kTotalZeros11) / sizeof(VlcEntry), sizeof(kTotalZeros12) / sizeof(VlcEntry),
                       sizeof(kTotalZeros13) / sizeof(VlcEntry), sizeof(kTotalZeros14) / sizeof(VlcEntry), sizeof(kTotalZeros15) / sizeof(VlcEntry)};
  for (int i = 0; i < 15; ++i) total_zeros_[i].build(tz[i], tzn[i]);
  total_zeros_chroma_dc_[0].build(kTotalZerosChromaDc1, sizeof(kTotalZerosChromaDc1) / sizeof(VlcEntry));
  total_zeros_chroma_dc_[1].build(kTotalZerosChromaDc2, sizeof(kTotalZerosChromaDc2) / sizeof(VlcEntry));
  total_zeros_chroma_dc_[2].build(kTotalZerosChromaDc3, sizeof(kTotalZerosChromaDc3) / sizeof(VlcEntry));
  const VlcEntry* rb[7] = {kRunBefore1, kRunBefore2, kRunBefore3, kRunBefore4, kRunBefore5, kRunBefore6, kRunBefore7};
  const int rbn[7] = {sizeof(kRunBefore1) / sizeof(VlcEntry), sizeof(kRunBefore2) / sizeof(VlcEntry), sizeof(kRunBefore3) / sizeof(VlcEntry), sizeof(kRunBefore4) / sizeof(VlcEntry),
                      sizeof(kRunBefore5) / sizeof(VlcEntry), sizeof(kRunBefore6) / sizeof(VlcEntry), sizeof(kRunBefore7) / sizeof(VlcEntry)};
  for (int i = 0; i < 7; ++i) run_before_[i].build(rb[i], rbn[i]);
  vlc_ready_ = true;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifetime

Core::~Core() {}

VideoFormat Core::output_format() const {
  VideoFormat f;
  if (sp_) {
    f.width = static_cast<uint32_t>(sp_->mb_w * 16 - sp_->crop_l - sp_->crop_r);
    f.height = static_cast<uint32_t>(sp_->mb_h * 16 - sp_->crop_t - sp_->crop_b);
    f.pixel_format = PixelFormat::I420;
  }
  return f;
}

void Core::reset() {
  flush_dpb();
  have_slice_ = false;
  cur_ = nullptr;
  waiting_for_idr_ = true;
  prev_poc_msb_ = prev_poc_lsb_ = 0;
  prev_frame_num_ = prev_frame_num_offset_ = 0;
  returned_ = nullptr;
}

void Core::flush_dpb() {
  for (Picture& p : pics_) {
    p.is_ref = false;
    p.is_long = false;
    p.needed_for_output = false;
  }
  out_count_ = 0;
  out_wait_count_ = 0;
  cur_ = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Parameter sets

namespace {

void unzigzag_list(const int* zz_values, int size, uint8_t* raster) {
  for (int j = 0; j < size; ++j) raster[size == 16 ? kZigzag4x4[j] : kZigzag8x8[j]] = static_cast<uint8_t>(zz_values[j]);
}

// 7.3.2.1.1.1: reads one scaling list; *use_default is set when the list says "use the default".
bool read_scaling_list(BitReader& br, int size, uint8_t* raster, bool* use_default) {
  int last = 8, next = 8;
  int vals[64];
  *use_default = false;
  for (int j = 0; j < size; ++j) {
    if (next != 0) {
      const int delta = br.se();
      next = (last + delta + 256) & 255;
      *use_default = (j == 0 && next == 0);
    }
    vals[j] = next == 0 ? last : next;
    last = vals[j];
  }
  if (br.error()) return false;
  unzigzag_list(vals, size, raster);
  return true;
}

struct ScalingRaw {
  bool present[8];
  bool use_default[8];
  uint8_t l4[6][16];
  uint8_t l8[2][64];
};

// 8.5.6 fall-back rules A (defaults) and B (the sequence-level lists).
void resolve_scaling(const ScalingRaw& raw, bool rule_a, const uint8_t (*seq4)[16], const uint8_t (*seq8)[64], uint8_t (*out4)[16],
                     uint8_t (*out8)[64]) {
  for (int i = 0; i < 6; ++i) {
    const uint8_t* def = kDefault4x4 + (i < 3 ? 0 : 16);
    if (raw.present[i]) {
      const uint8_t* src = raw.use_default[i] ? def : raw.l4[i];
      for (int k = 0; k < 16; ++k) out4[i][k] = src[k];
    } else {
      const uint8_t* src = (i == 0 || i == 3) ? (rule_a ? def : seq4[i]) : out4[i - 1];
      for (int k = 0; k < 16; ++k) out4[i][k] = src[k];
    }
  }
  for (int i = 0; i < 2; ++i) {
    const uint8_t* def = kDefault8x8 + i * 64;
    const uint8_t* src;
    if (raw.present[6 + i]) src = raw.use_default[6 + i] ? def : raw.l8[i];
    else src = rule_a ? def : seq8[i];
    for (int k = 0; k < 64; ++k) out8[i][k] = src[k];
  }
}

}  // namespace

bool ParameterSets::parse_sps(BitReader& br) {
  Sps s;
  s.profile_idc = static_cast<int>(br.u(8));
  br.u(8);
  s.level_idc = static_cast<int>(br.u(8));
  s.id = static_cast<int>(br.ue());
  if (s.id > 31) return false;
  for (int i = 0; i < 6; ++i)
    for (int k = 0; k < 16; ++k) s.list4[i][k] = 16;
  for (int i = 0; i < 2; ++i)
    for (int k = 0; k < 64; ++k) s.list8[i][k] = 16;
  const int p = s.profile_idc;
  if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 ||
      p == 139 || p == 134 || p == 135) {
    s.chroma_format_idc = static_cast<int>(br.ue());
    if (s.chroma_format_idc == 3) br.u(1);
    const int bdl = static_cast<int>(br.ue()), bdc = static_cast<int>(br.ue());
    s.qpprime_bypass = br.u(1) != 0;
    if (s.chroma_format_idc != 1 || bdl != 0 || bdc != 0 || s.qpprime_bypass) return false;  // 8-bit 4:2:0 only
    s.scaling_matrix_present = br.u(1) != 0;
    if (s.scaling_matrix_present) {
      ScalingRaw raw = {};
      for (int i = 0; i < 8; ++i) {
        raw.present[i] = br.u(1) != 0;
        if (!raw.present[i]) continue;
        bool ok = i < 6 ? read_scaling_list(br, 16, raw.l4[i], &raw.use_default[i]) : read_scaling_list(br, 64, raw.l8[i - 6], &raw.use_default[i]);
        if (!ok) return false;
      }
      resolve_scaling(raw, true, nullptr, nullptr, s.list4, s.list8);
    }
  }
  s.log2_max_frame_num = static_cast<int>(br.ue()) + 4;
  s.poc_type = static_cast<int>(br.ue());
  if (s.log2_max_frame_num > 16 || s.poc_type > 2) return false;
  if (s.poc_type == 0) {
    s.log2_max_poc_lsb = static_cast<int>(br.ue()) + 4;
    if (s.log2_max_poc_lsb > 16) return false;
  } else if (s.poc_type == 1) {
    s.delta_pic_order_always_zero = br.u(1) != 0;
    s.offset_for_non_ref_pic = br.se();
    s.offset_for_top_to_bottom = br.se();
    s.num_ref_in_cycle = static_cast<int>(br.ue());
    if (s.num_ref_in_cycle > 255) return false;
    for (int i = 0; i < s.num_ref_in_cycle; ++i) s.offset_for_ref_frame[i] = br.se();
  }
  s.max_num_ref_frames = static_cast<int>(br.ue());
  s.gaps_allowed = br.u(1) != 0;
  const uint32_t w_minus1 = br.ue(), h_minus1 = br.ue();
  if (w_minus1 >= 1024 || h_minus1 >= 1024) return false;  // checked before the arithmetic below: the values come from the stream
  const int w = static_cast<int>(w_minus1) + 1;
  const int hmap = static_cast<int>(h_minus1) + 1;
  s.frame_mbs_only = br.u(1) != 0;
  if (!s.frame_mbs_only) s.mbaff = br.u(1) != 0;
  s.direct_8x8_inference = br.u(1) != 0;
  if (br.error() || s.max_num_ref_frames > 16) return false;
  if (s.mbaff) return false;  // interlaced coding is not supported
  s.mb_w = w;
  s.mb_h = hmap * (s.frame_mbs_only ? 1 : 2);
  if (w < 1 || s.mb_h < 1 || static_cast<int64_t>(w) * s.mb_h > kMaxMbs || w > 1024 || s.mb_h > 1024) return false;
  if (br.u(1)) {
    const int unit_x = 2, unit_y = 2 * (s.frame_mbs_only ? 1 : 2);
    uint32_t crop[4];
    for (uint32_t& c : crop) {
      c = br.ue();
      if (c > 16384) return false;  // no larger than the picture can be
    }
    s.crop_l = static_cast<int>(crop[0]) * unit_x;
    s.crop_r = static_cast<int>(crop[1]) * unit_x;
    s.crop_t = static_cast<int>(crop[2]) * unit_y;
    s.crop_b = static_cast<int>(crop[3]) * unit_y;
    if (s.crop_l + s.crop_r >= w * 16 || s.crop_t + s.crop_b >= s.mb_h * 16) return false;
  }
  if (br.u(1)) {  // VUI
    if (br.u(1)) {
      if (br.u(8) == 255) br.u(32);
    }
    if (br.u(1)) br.u(1);
    if (br.u(1)) {
      br.u(3);
      s.full_range = br.u(1) != 0;
      if (br.u(1)) br.u(24);
    }
    if (br.u(1)) {
      br.ue();
      br.ue();
    }
    if (br.u(1)) {
      br.u(32);
      br.u(32);
      br.u(1);
    }
    const bool nal_hrd = br.u(1) != 0;
    auto hrd = [&br]() {
      const uint32_t cnt = br.ue() + 1;
      br.u(8);
      for (uint32_t i = 0; i < cnt && i < 32 && !br.error(); ++i) {
        br.ue();
        br.ue();
        br.u(1);
      }
      br.u(20);
    };
    if (nal_hrd) hrd();
    const bool vcl_hrd = br.u(1) != 0;
    if (vcl_hrd) hrd();
    if (nal_hrd || vcl_hrd) br.u(1);
    br.u(1);
    if (br.u(1)) {
      br.u(1);
      br.ue();
      br.ue();
      br.ue();
      br.ue();
      const uint32_t reorder = br.ue();
      const uint32_t dpb = br.ue();
      if (!br.error() && reorder <= 16 && dpb <= 16) {
        s.num_reorder_frames = static_cast<int>(reorder);
        s.max_dec_frame_buffering = static_cast<int>(dpb);
      }
    }
    if (br.error()) {  // damaged VUI: ignore what was read of it
      s.num_reorder_frames = s.max_dec_frame_buffering = -1;
      s.full_range = false;
    }
  }
  int frames = max_dpb_mbs(s.level_idc) / (s.mb_w * s.mb_h);
  frames = clip3(1, 16, frames);
  if (s.max_dec_frame_buffering >= 0) frames = s.max_dec_frame_buffering;
  if (frames < s.max_num_ref_frames) frames = s.max_num_ref_frames;
  if (frames < 1) frames = 1;
  s.dpb_frames = frames;
  s.valid = true;
  sps[s.id] = s;
  return true;
}

bool ParameterSets::parse_pps(BitReader& br) {
  Pps p;
  p.id = static_cast<int>(br.ue());
  p.sps_id = static_cast<int>(br.ue());
  if (p.id > 255 || p.sps_id > 31) return false;
  p.cabac = br.u(1) != 0;
  p.bottom_field_pic_order = br.u(1) != 0;
  p.num_slice_groups = static_cast<int>(br.ue()) + 1;
  if (p.num_slice_groups > 1) return false;  // flexible macroblock ordering is not supported
  p.num_ref_idx_default[0] = static_cast<int>(br.ue()) + 1;
  p.num_ref_idx_default[1] = static_cast<int>(br.ue()) + 1;
  p.weighted_pred = br.u(1) != 0;
  p.weighted_bipred_idc = static_cast<int>(br.u(2));
  p.pic_init_qp = 26 + br.se();
  p.pic_init_qs = 26 + br.se();
  p.chroma_qp_offset[0] = br.se();
  p.chroma_qp_offset[1] = p.chroma_qp_offset[0];
  p.deblocking_control = br.u(1) != 0;
  p.constrained_intra_pred = br.u(1) != 0;
  p.redundant_pic_cnt_present = br.u(1) != 0;
  if (br.error() || p.num_ref_idx_default[0] > kMaxRefs || p.num_ref_idx_default[1] > kMaxRefs) return false;
  ScalingRaw raw = {};
  if (br.more_rbsp_data()) {
    p.transform_8x8 = br.u(1) != 0;
    p.scaling_matrix_present = br.u(1) != 0;
    if (p.scaling_matrix_present) {
      const int n = 6 + (p.transform_8x8 ? 2 : 0);
      for (int i = 0; i < n; ++i) {
        raw.present[i] = br.u(1) != 0;
        if (!raw.present[i]) continue;
        bool ok = i < 6 ? read_scaling_list(br, 16, raw.l4[i], &raw.use_default[i]) : read_scaling_list(br, 64, raw.l8[i - 6], &raw.use_default[i]);
        if (!ok) return false;
      }
    }
    p.chroma_qp_offset[1] = br.se();
  }
  if (br.error()) return false;
  // The final lists depend on the sequence parameter set; keep the raw ones until a picture starts.
  for (int i = 0; i < 6; ++i)
    for (int k = 0; k < 16; ++k) p.list4[i][k] = raw.use_default[i] ? 0 : raw.l4[i][k];
  for (int i = 0; i < 2; ++i)
    for (int k = 0; k < 64; ++k) p.list8[i][k] = raw.use_default[6 + i] ? 0 : raw.l8[i][k];
  // Remember presence and "use default" in the otherwise unused first bytes of a side table.
  raw_present[p.id] = 0;
  raw_default[p.id] = 0;
  for (int i = 0; i < 8; ++i) {
    if (raw.present[i]) raw_present[p.id] |= static_cast<uint8_t>(1u << i);
    if (raw.use_default[i]) raw_default[p.id] |= static_cast<uint8_t>(1u << i);
  }
  p.valid = true;
  pps[p.id] = p;
  return true;
}

// The scaling lists in force for a picture using `pps`, after the fall-back rules (8.5.6): flat 16 where the stream sends none.
void ParameterSets::effective_scaling(const Sps& s, const Pps& p, uint8_t w4[6][16], uint8_t w8[2][64]) const {
  if (p.scaling_matrix_present) {
    ScalingRaw raw = {};
    const uint8_t pr = raw_present[p.id], pd = raw_default[p.id];
    for (int i = 0; i < 8; ++i) {
      raw.present[i] = (pr >> i) & 1;
      raw.use_default[i] = (pd >> i) & 1;
    }
    for (int i = 0; i < 6; ++i)
      for (int k = 0; k < 16; ++k) raw.l4[i][k] = p.list4[i][k];
    for (int i = 0; i < 2; ++i)
      for (int k = 0; k < 64; ++k) raw.l8[i][k] = p.list8[i][k];
    resolve_scaling(raw, !s.scaling_matrix_present, s.list4, s.list8, w4, w8);
  } else {
    for (int i = 0; i < 6; ++i)
      for (int k = 0; k < 16; ++k) w4[i][k] = s.list4[i][k];
    for (int i = 0; i < 2; ++i)
      for (int k = 0; k < 64; ++k) w8[i][k] = s.list8[i][k];
  }
}

// Builds LevelScale for the active parameter sets (8.5.9).
void Core::compute_scaling() {
  uint8_t w4[6][16], w8[2][64];
  ps_.effective_scaling(*sp_, *pp_, w4, w8);
  static const int v4[6][3] = {{10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23}};
  static const int v8[6][6] = {{20, 18, 32, 19, 25, 24}, {22, 19, 35, 21, 28, 26}, {26, 23, 42, 24, 33, 31},
                               {28, 25, 45, 26, 35, 33}, {32, 28, 51, 30, 40, 38}, {36, 32, 58, 34, 46, 43}};
  for (int m = 0; m < 6; ++m) {
    for (int l = 0; l < 6; ++l) {
      for (int pos = 0; pos < 16; ++pos) {
        const int i = pos >> 2, j = pos & 3;
        const int cls = (i % 2 == 0 && j % 2 == 0) ? 0 : (i % 2 == 1 && j % 2 == 1) ? 1 : 2;
        level_scale4_[l][m][pos] = w4[l][pos] * v4[m][cls];
      }
    }
    for (int l = 0; l < 2; ++l) {
      for (int pos = 0; pos < 64; ++pos) {
        const int i = pos >> 3, j = pos & 7;
        int cls;
        if (i % 4 == 0 && j % 4 == 0) cls = 0;
        else if (i % 2 == 1 && j % 2 == 1) cls = 1;
        else if (i % 4 == 2 && j % 4 == 2) cls = 2;
        else if ((i % 4 == 0 && j % 2 == 1) || (i % 2 == 1 && j % 4 == 0)) cls = 3;
        else if ((i % 4 == 0 && j % 4 == 2) || (i % 4 == 2 && j % 4 == 0)) cls = 4;
        else cls = 5;
        level_scale8_[l][m][pos] = w8[l][pos] * v8[m][cls];
      }
    }
  }
}

bool Core::configure(ConstByteView avcc) {
  if (avcc.size() < 7 || avcc[0] != 1) return false;
  nal_length_size_ = (avcc[4] & 3) + 1;
  uint64_t pos = 5;
  const int n_sps = avcc[pos++] & 31;
  for (int pass = 0; pass < 2; ++pass) {
    int count = pass == 0 ? n_sps : (pos < avcc.size() ? avcc[pos++] : 0);
    for (int i = 0; i < count; ++i) {
      if (pos + 2 > avcc.size()) return false;
      const uint64_t len = (static_cast<uint64_t>(avcc[pos]) << 8) | avcc[pos + 1];
      pos += 2;
      if (pos + len > avcc.size()) return false;
      if (!handle_nal(avcc.data() + pos, len, 0)) return false;
      pos += len;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// NAL units

bool Core::decode(ConstByteView data, int64_t pts_us) {
  returned_ = nullptr;
  build_vlc();
  bool any = false;
  for_each_nal(data, nal_length_size_, [&](const uint8_t* nal, uint64_t size) {
    if (handle_nal(nal, size, pts_us)) any = true;
  });
  finish_picture();
  return any;
}

bool Core::handle_nal(const uint8_t* nal, uint64_t size, int64_t pts) {
  if (size < 1 || (nal[0] & 0x80)) return false;
  const int ref_idc = (nal[0] >> 5) & 3;
  const int type = nal[0] & 31;
  if (type != 1 && type != 5 && type != 7 && type != 8) return false;
  if (rbsp_.size() < size && !rbsp_.alloc(allocator_, size + 64)) return false;
  uint8_t* out = rbsp_.data();
  const uint64_t n = remove_emulation_prevention(nal, size, out);
  BitReader br;
  br.init(out, n);
  if (type == 7) return ps_.parse_sps(br);
  if (type == 8) return ps_.parse_pps(br);
  SliceHeader sh;
  sh.nal_unit_type = type;
  sh.nal_ref_idc = ref_idc;
  if (!ps_.parse_slice_header(br, &sh)) return false;
  if (sh.redundant_pic_cnt > 0) return false;
  if (waiting_for_idr_ && !sh.idr()) {
    // A stream can also start with a recovery point (an I picture); accept a plain I slice.
    if (sh.type != kSliceI) return false;
  }
  if (!have_slice_ || new_picture(prev_sh_, sh)) {
    if (have_slice_) finish_picture();
    if (!start_picture(sh, pts)) return false;
  }
  prev_sh_ = sh;
  have_slice_ = true;
  sh_ = sh;
  build_ref_lists(sh);
  if (!decode_slice(br, sh)) {
    // A damaged slice: keep what was decoded so far.
    return true;
  }
  return true;
}

// 7.4.1.2.4
bool ParameterSets::new_picture(const Sps& sps, const SliceHeader& a, const SliceHeader& b) {
  if (a.frame_num != b.frame_num || a.pps_id != b.pps_id) return true;
  if ((a.nal_ref_idc == 0) != (b.nal_ref_idc == 0)) return true;
  if (a.idr() != b.idr()) return true;
  if (b.idr() && a.idr_pic_id != b.idr_pic_id) return true;
  if (sps.poc_type == 0) {
    if (a.poc_lsb != b.poc_lsb || a.delta_poc_bottom != b.delta_poc_bottom) return true;
  } else if (sps.poc_type == 1) {
    if (a.delta_poc[0] != b.delta_poc[0] || a.delta_poc[1] != b.delta_poc[1]) return true;
  }
  return false;
}

bool Core::new_picture(const SliceHeader& a, const SliceHeader& b) const { return ParameterSets::new_picture(*sp_, a, b); }

uint64_t remove_emulation_prevention(const uint8_t* nal, uint64_t size, uint8_t* out) {
  uint64_t n = 0;
  int zeros = 0;
  for (uint64_t i = 1; i < size; ++i) {
    const uint8_t b = nal[i];
    if (zeros >= 2 && b == 3) {
      zeros = 0;
      continue;
    }
    out[n++] = b;
    zeros = b == 0 ? zeros + 1 : 0;
  }
  return n;
}

bool ParameterSets::parse_slice_header(BitReader& br, SliceHeader* sh) const {
  sh->first_mb = static_cast<int>(br.ue());
  uint32_t t = br.ue();
  if (t > 9) return false;
  t %= 5;
  if (t > 2) return false;  // SP and SI slices
  sh->type = t == 0 ? kSliceP : t == 1 ? kSliceB : kSliceI;
  sh->pps_id = static_cast<int>(br.ue());
  if (sh->pps_id > 255 || !pps[sh->pps_id].valid) return false;
  const Pps& p = pps[sh->pps_id];
  if (!sps[p.sps_id].valid) return false;
  const Sps& s = sps[p.sps_id];
  sh->frame_num = static_cast<int>(br.u(s.log2_max_frame_num));
  if (!s.frame_mbs_only) {
    sh->field_pic = br.u(1) != 0;
    if (sh->field_pic) return false;  // field pictures are not supported
  }
  if (sh->idr()) sh->idr_pic_id = static_cast<int>(br.ue());
  if (s.poc_type == 0) {
    sh->poc_lsb = static_cast<int>(br.u(s.log2_max_poc_lsb));
    if (p.bottom_field_pic_order) sh->delta_poc_bottom = br.se();
  } else if (s.poc_type == 1 && !s.delta_pic_order_always_zero) {
    sh->delta_poc[0] = br.se();
    if (p.bottom_field_pic_order) sh->delta_poc[1] = br.se();
  }
  if (p.redundant_pic_cnt_present) sh->redundant_pic_cnt = static_cast<int>(br.ue());
  if (sh->type == kSliceB) sh->direct_spatial_mv_pred = br.u(1) != 0;
  sh->num_ref_idx[0] = p.num_ref_idx_default[0];
  sh->num_ref_idx[1] = p.num_ref_idx_default[1];
  if (sh->type != kSliceI) {
    if (br.u(1)) {
      sh->num_ref_idx[0] = static_cast<int>(br.ue()) + 1;
      if (sh->type == kSliceB) sh->num_ref_idx[1] = static_cast<int>(br.ue()) + 1;
    }
    if (sh->num_ref_idx[0] > kMaxRefs || sh->num_ref_idx[1] > kMaxRefs) return false;
  }
  if (sh->type != kSliceB) sh->num_ref_idx[1] = 0;
  if (sh->type == kSliceI) sh->num_ref_idx[0] = 0;
  // ref_pic_list_modification
  for (int l = 0; l < (sh->type == kSliceB ? 2 : sh->type == kSliceP ? 1 : 0); ++l) {
    if (!br.u(1)) continue;
    for (;;) {
      const uint32_t idc = br.ue();
      if (idc == 3) break;
      if (idc > 2 || sh->num_mod[l] > kMaxRefs || br.error()) return false;
      sh->mods[l][sh->num_mod[l]].idc = static_cast<int>(idc);
      sh->mods[l][sh->num_mod[l]].value = br.ue();
      ++sh->num_mod[l];
    }
  }
  // Prediction weights; the defaults are "no weighting".
  sh->luma_log2_denom = sh->chroma_log2_denom = 0;
  if ((p.weighted_pred && sh->type == kSliceP) || (p.weighted_bipred_idc == 1 && sh->type == kSliceB)) {
    sh->luma_log2_denom = static_cast<int>(br.ue());
    sh->chroma_log2_denom = static_cast<int>(br.ue());
    if (sh->luma_log2_denom > 7 || sh->chroma_log2_denom > 7) return false;
    for (int l = 0; l < (sh->type == kSliceB ? 2 : 1); ++l) {
      for (int i = 0; i < sh->num_ref_idx[l]; ++i) {
        sh->luma_weight_flag[l][i] = br.u(1) != 0;
        sh->luma_weight[l][i] = 1 << sh->luma_log2_denom;
        sh->luma_offset[l][i] = 0;
        if (sh->luma_weight_flag[l][i]) {
          sh->luma_weight[l][i] = br.se();
          sh->luma_offset[l][i] = br.se();
        }
        sh->chroma_weight_flag[l][i] = br.u(1) != 0;
        for (int c = 0; c < 2; ++c) {
          sh->chroma_weight[l][i][c] = 1 << sh->chroma_log2_denom;
          sh->chroma_offset[l][i][c] = 0;
          if (sh->chroma_weight_flag[l][i]) {
            sh->chroma_weight[l][i][c] = br.se();
            sh->chroma_offset[l][i][c] = br.se();
          }
        }
      }
    }
  }
  if (sh->nal_ref_idc != 0) {
    if (sh->idr()) {
      sh->no_output_of_prior_pics = br.u(1) != 0;
      sh->long_term_reference = br.u(1) != 0;
    } else {
      sh->adaptive_marking = br.u(1) != 0;
      if (sh->adaptive_marking) {
        for (;;) {
          const uint32_t op = br.ue();
          if (op == 0) break;
          if (op > 6 || sh->num_mmco >= 66 || br.error()) return false;
          Mmco& m = sh->mmco[sh->num_mmco++];
          m.op = static_cast<int>(op);
          m.a = m.b = 0;
          if (op == 1 || op == 3) m.a = br.ue();
          if (op == 2) m.a = br.ue();
          if (op == 3) m.b = br.ue();
          if (op == 6) m.b = br.ue();
          if (op == 4) m.a = br.ue();
        }
      }
    }
  }
  if (p.cabac && sh->type != kSliceI) {
    sh->cabac_init_idc = static_cast<int>(br.ue());
    if (sh->cabac_init_idc > 2) return false;
  }
  sh->slice_qp = p.pic_init_qp + br.se();
  if (sh->slice_qp < 0 || sh->slice_qp > 51) return false;
  if (p.deblocking_control) {
    sh->disable_deblocking = static_cast<int>(br.ue());
    if (sh->disable_deblocking > 2) return false;
    if (sh->disable_deblocking != 1) {
      sh->alpha_off = br.se() * 2;
      sh->beta_off = br.se() * 2;
      if (sh->alpha_off < -12 || sh->alpha_off > 12 || sh->beta_off < -12 || sh->beta_off > 12) return false;
    }
  }
  return !br.error() && sh->first_mb < s.mb_w * s.mb_h;
}

// ---------------------------------------------------------------------------------------------------------------
// Pictures

// A new sequence size: the pictures are released and get their memory again as they come into use.
bool Core::allocate_pictures() {
  const Sps& s = *sp_;
  for (Picture& p : pics_) {
    p.in_use = false;
    p.is_ref = p.is_long = p.needed_for_output = false;
    p.y.release();
    p.u.release();
    p.v.release();
    p.motion.release();
    p.stride = s.mb_w * 16;
    p.cstride = s.mb_w * 8;
  }
  if (!mbs_.alloc(allocator_, static_cast<uint64_t>(s.mb_w) * s.mb_h)) return false;
  mb_w_ = s.mb_w;
  mb_h_ = s.mb_h;
  out_count_ = 0;
  out_wait_count_ = 0;
  returned_ = nullptr;
  cur_ = nullptr;
  return true;
}

bool Core::alloc_picture(Picture* p) {
  if (p->y.size() != 0) return true;
  const uint64_t w = static_cast<uint64_t>(mb_w_) * 16, h = static_cast<uint64_t>(mb_h_) * 16;
  return p->y.alloc(allocator_, w * h) && p->u.alloc(allocator_, w / 2 * (h / 2)) && p->v.alloc(allocator_, w / 2 * (h / 2)) &&
         p->motion.alloc(allocator_, static_cast<uint64_t>(mb_w_) * mb_h_);
}

Picture* Core::find_free_slot() {
  Picture* empty = nullptr;
  for (Picture& p : pics_) {
    if (&p == cur_ || &p == returned_ || p.is_ref || p.needed_for_output) continue;
    if (p.y.size() != 0) return &p;  // memory already there
    if (!empty) empty = &p;
  }
  return empty;
}

bool Core::start_picture(const SliceHeader& sh, int64_t pts) {
  const Pps& p = ps_.pps[sh.pps_id];
  const Sps& s = ps_.sps[p.sps_id];
  if (sp_ == nullptr || mb_w_ != s.mb_w || mb_h_ != s.mb_h) {
    if (sp_ == nullptr && !sh.idr() && sh.type != kSliceI) return false;
    bump_output(true);
    sp_ = &s;
    if (!allocate_pictures()) {
      sp_ = nullptr;
      return false;
    }
  }
  sp_ = &s;
  pp_ = &p;
  active_sps_id_ = p.sps_id;
  if (sh.idr()) {
    // Everything before an IDR picture is output first.
    bump_output(true);
    for (Picture& q : pics_) {
      q.is_ref = false;
      q.is_long = false;
    }
    max_long_term_idx_ = -1;
  }
  compute_poc(sh);
  Picture* pic = find_free_slot();
  while (!pic) {
    // The buffer is full: push the earliest picture out, and if that frees nothing drop the oldest reference.
    if (out_count_ > 0) {
      bump_output(false);
      pic = find_free_slot();
      if (pic) break;
    }
    Picture* oldest = nullptr;
    for (Picture& q : pics_)
      if (q.is_ref && &q != returned_ && (!oldest || q.frame_num_wrap < oldest->frame_num_wrap)) oldest = &q;
    if (!oldest) return false;
    oldest->is_ref = false;
    oldest->is_long = false;
    pic = find_free_slot();
  }
  if (!alloc_picture(pic)) return false;
  cur_ = pic;
  cur_->in_use = true;
  cur_->id = next_pic_id_++;
  cur_->poc = pic_poc_;
  cur_->frame_num = sh.frame_num;
  cur_->is_ref = false;
  cur_->is_long = false;
  cur_->needed_for_output = false;
  cur_->non_existing = false;
  cur_->keyframe = sh.idr() || sh.type == kSliceI;
  cur_->pts = pts;
  pic_pts_ = pts;
  slice_no_ = 0;
  for (uint64_t i = 0; i < mbs_.size(); ++i) mbs_[i].slice = -1;
  // Samples of macroblocks never decoded stay mid-grey.
  for (uint64_t i = 0; i < cur_->y.size(); ++i) cur_->y[i] = 128;
  for (uint64_t i = 0; i < cur_->u.size(); ++i) cur_->u[i] = cur_->v[i] = 128;
  compute_scaling();
  waiting_for_idr_ = false;
  return true;
}

void Core::compute_poc(const SliceHeader& sh) {
  const Sps& s = *sp_;
  const int max_frame_num = 1 << s.log2_max_frame_num;
  if (s.poc_type == 0) {
    int prev_msb, prev_lsb;
    if (sh.idr()) {
      prev_msb = prev_lsb = 0;
    } else {
      prev_msb = prev_poc_msb_;
      prev_lsb = prev_poc_lsb_;
    }
    const int max_lsb = 1 << s.log2_max_poc_lsb;
    int msb;
    if (sh.poc_lsb < prev_lsb && prev_lsb - sh.poc_lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
    else if (sh.poc_lsb > prev_lsb && sh.poc_lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
    else msb = prev_msb;
    top_poc_ = msb + sh.poc_lsb;
    bottom_poc_ = top_poc_ + sh.delta_poc_bottom;
    cur_poc_msb_ = msb;
  } else {
    int offset;
    if (sh.idr()) offset = 0;
    else if (prev_frame_num_ > sh.frame_num) offset = prev_frame_num_offset_ + max_frame_num;
    else offset = prev_frame_num_offset_;
    frame_num_offset_ = offset;
    if (s.poc_type == 1) {
      int abs_frame_num = s.num_ref_in_cycle != 0 ? offset + sh.frame_num : 0;
      if (sh.nal_ref_idc == 0 && abs_frame_num > 0) --abs_frame_num;
      int expected = 0;
      if (abs_frame_num > 0) {
        int per_cycle = 0;
        for (int i = 0; i < s.num_ref_in_cycle; ++i) per_cycle += s.offset_for_ref_frame[i];
        const int cycle = (abs_frame_num - 1) / s.num_ref_in_cycle;
        const int in_cycle = (abs_frame_num - 1) % s.num_ref_in_cycle;
        expected = cycle * per_cycle;
        for (int i = 0; i <= in_cycle; ++i) expected += s.offset_for_ref_frame[i];
      }
      if (sh.nal_ref_idc == 0) expected += s.offset_for_non_ref_pic;
      top_poc_ = expected + sh.delta_poc[0];
      bottom_poc_ = top_poc_ + s.offset_for_top_to_bottom + sh.delta_poc[1];
    } else {
      int temp;
      if (sh.idr()) temp = 0;
      else if (sh.nal_ref_idc == 0) temp = 2 * (offset + sh.frame_num) - 1;
      else temp = 2 * (offset + sh.frame_num);
      top_poc_ = bottom_poc_ = temp;
    }
  }
  pic_poc_ = top_poc_ < bottom_poc_ ? top_poc_ : bottom_poc_;
}

void Core::finish_picture() {
  if (!cur_ || !have_slice_) {
    have_slice_ = false;
    return;
  }
  have_slice_ = false;
  deblock_picture();
  const SliceHeader& sh = prev_sh_;
  bool mmco5 = false;
  if (sh.nal_ref_idc != 0) {
    for (int i = 0; i < sh.num_mmco; ++i)
      if (sh.mmco[i].op == 5) mmco5 = true;
    mark_references(sh);
  }
  if (mmco5) {
    // 8.2.1: the picture is treated as having frame_num 0 and the order counts restart from it.
    bump_output(true);
    const int temp = cur_->poc;
    top_poc_ -= temp;
    bottom_poc_ -= temp;
    cur_->poc = top_poc_ < bottom_poc_ ? top_poc_ : bottom_poc_;
    cur_->frame_num = 0;
  }
  // State for the next picture's order count.
  const Sps& s = *sp_;
  if (sh.nal_ref_idc != 0) {
    if (mmco5) {
      prev_poc_msb_ = 0;
      prev_poc_lsb_ = top_poc_;  // tempPicOrderCnt-adjusted TopFieldOrderCnt (frame)
    } else {
      prev_poc_msb_ = cur_poc_msb_;
      prev_poc_lsb_ = sh.poc_lsb;
    }
  }
  if (s.poc_type != 0) {
    prev_frame_num_ = mmco5 ? 0 : sh.frame_num;
    prev_frame_num_offset_ = mmco5 ? 0 : frame_num_offset_;
  } else {
    prev_frame_num_ = sh.frame_num;
  }
  prev_ref_frame_num_ = sh.nal_ref_idc != 0 ? (mmco5 ? 0 : sh.frame_num) : prev_ref_frame_num_;
  cur_->needed_for_output = true;
  Picture* done = cur_;
  cur_ = nullptr;
  queue_output(done);
}

// Pictures wait here until the display order allows them out.
void Core::queue_output(Picture* p) {
  out_wait_[out_wait_count_++] = p;
  const int depth = sp_->num_reorder_frames >= 0 ? sp_->num_reorder_frames : (sp_->poc_type == 2 ? 0 : sp_->dpb_frames);
  while (out_wait_count_ > depth) bump_output(false);
}

// Moves the earliest waiting picture (or all of them) to the queue the user receives from.
void Core::bump_output(bool all) {
  do {
    if (out_wait_count_ == 0) return;
    int best = 0;
    for (int i = 1; i < out_wait_count_; ++i)
      if (out_wait_[i]->poc < out_wait_[best]->poc) best = i;
    Picture* p = out_wait_[best];
    for (int i = best; i + 1 < out_wait_count_; ++i) out_wait_[i] = out_wait_[i + 1];
    --out_wait_count_;
    if (out_count_ < kMaxPictures) {
      out_queue_[out_count_++] = p;
    } else {
      p->needed_for_output = false;
    }
  } while (all);
}

void Core::flush() {
  finish_picture();
  bump_output(true);
}

bool Core::receive(VideoFrame* frame) {
  returned_ = nullptr;
  if (out_count_ == 0) return false;
  Picture* p = out_queue_[0];
  for (int i = 0; i + 1 < out_count_; ++i) out_queue_[i] = out_queue_[i + 1];
  --out_count_;
  p->needed_for_output = false;
  returned_ = p;
  const Sps& s = *sp_;
  frame->format.width = static_cast<uint32_t>(s.mb_w * 16 - s.crop_l - s.crop_r);
  frame->format.height = static_cast<uint32_t>(s.mb_h * 16 - s.crop_t - s.crop_b);
  frame->format.pixel_format = PixelFormat::I420;
  frame->plane[0] = p->y.data() + static_cast<int64_t>(s.crop_t) * p->stride + s.crop_l;
  frame->plane[1] = p->u.data() + static_cast<int64_t>(s.crop_t / 2) * p->cstride + s.crop_l / 2;
  frame->plane[2] = p->v.data() + static_cast<int64_t>(s.crop_t / 2) * p->cstride + s.crop_l / 2;
  frame->stride[0] = static_cast<uint32_t>(p->stride);
  frame->stride[1] = frame->stride[2] = static_cast<uint32_t>(p->cstride);
  frame->pts_us = p->pts;
  frame->keyframe = p->keyframe;
  frame->limited_range = !s.full_range;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Reference picture marking (8.2.5)

void Core::sliding_window() {
  int refs = 0;
  for (Picture& p : pics_)
    if (&p != cur_ && p.is_ref) ++refs;
  const int max_refs = sp_->max_num_ref_frames > 0 ? sp_->max_num_ref_frames : 1;
  while (refs >= max_refs) {
    Picture* oldest = nullptr;
    for (Picture& p : pics_)
      if (&p != cur_ && p.is_ref && !p.is_long && (!oldest || p.frame_num_wrap < oldest->frame_num_wrap)) oldest = &p;
    if (!oldest) break;
    oldest->is_ref = false;
    --refs;
  }
}

void Core::mark_references(const SliceHeader& sh) {
  const int max_frame_num = 1 << sp_->log2_max_frame_num;
  for (Picture& p : pics_)
    if (p.is_ref && !p.is_long) p.frame_num_wrap = p.frame_num > sh.frame_num ? p.frame_num - max_frame_num : p.frame_num;
  cur_->is_ref = true;
  cur_->is_long = false;
  if (sh.idr()) {
    if (sh.long_term_reference) {
      cur_->is_long = true;
      cur_->long_term_idx = 0;
      max_long_term_idx_ = 0;
    } else {
      max_long_term_idx_ = -1;
    }
    cur_->frame_num_wrap = sh.frame_num;
    return;
  }
  cur_->frame_num_wrap = sh.frame_num;
  if (sh.adaptive_marking) {
    apply_mmco(sh);
  } else {
    cur_->is_ref = false;  // the current picture does not count in the window
    sliding_window();
    cur_->is_ref = true;
  }
}

void Core::apply_mmco(const SliceHeader& sh) {
  const int max_frame_num = 1 << sp_->log2_max_frame_num;
  (void)max_frame_num;
  for (int i = 0; i < sh.num_mmco; ++i) {
    const Mmco& m = sh.mmco[i];
    switch (m.op) {
      case 1: {
        const int pic_num = sh.frame_num - (static_cast<int>(m.a) + 1);
        for (Picture& p : pics_)
          if (&p != cur_ && p.is_ref && !p.is_long && p.frame_num_wrap == pic_num) p.is_ref = false;
        break;
      }
      case 2:
        for (Picture& p : pics_)
          if (&p != cur_ && p.is_ref && p.is_long && p.long_term_idx == static_cast<int>(m.a)) {
            p.is_ref = false;
            p.is_long = false;
          }
        break;
      case 3: {
        const int pic_num = sh.frame_num - (static_cast<int>(m.a) + 1);
        for (Picture& p : pics_)
          if (&p != cur_ && p.is_ref && p.is_long && p.long_term_idx == static_cast<int>(m.b)) {
            p.is_ref = false;
            p.is_long = false;
          }
        for (Picture& p : pics_)
          if (&p != cur_ && p.is_ref && !p.is_long && p.frame_num_wrap == pic_num) {
            p.is_long = true;
            p.long_term_idx = static_cast<int>(m.b);
          }
        break;
      }
      case 4:
        max_long_term_idx_ = static_cast<int>(m.a) - 1;
        for (Picture& p : pics_)
          if (&p != cur_ && p.is_ref && p.is_long && p.long_term_idx > max_long_term_idx_) {
            p.is_ref = false;
            p.is_long = false;
          }
        break;
      case 5:
        for (Picture& p : pics_)
          if (&p != cur_) {
            p.is_ref = false;
            p.is_long = false;
          }
        max_long_term_idx_ = -1;
        break;
      case 6:
        for (Picture& p : pics_)
          if (&p != cur_ && p.is_ref && p.is_long && p.long_term_idx == static_cast<int>(m.b)) {
            p.is_ref = false;
            p.is_long = false;
          }
        cur_->is_long = true;
        cur_->long_term_idx = static_cast<int>(m.b);
        break;
      default:
        break;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Reference lists (8.2.4)

void Core::build_ref_lists(const SliceHeader& sh) {
  ref_count_[0] = ref_count_[1] = 0;
  for (int l = 0; l < 2; ++l)
    for (int i = 0; i <= kMaxRefs; ++i) ref_list_[l][i] = nullptr;
  if (sh.type == kSliceI) return;
  const int max_frame_num = 1 << sp_->log2_max_frame_num;
  Picture* shorts[kMaxPictures];
  Picture* longs[kMaxPictures];
  int ns = 0, nl = 0;
  for (Picture& p : pics_) {
    if (&p == cur_ || !p.is_ref) continue;
    if (p.is_long) {
      longs[nl++] = &p;
    } else {
      p.frame_num_wrap = p.frame_num > sh.frame_num ? p.frame_num - max_frame_num : p.frame_num;
      shorts[ns++] = &p;
    }
  }
  // sort long-term by index ascending
  for (int i = 1; i < nl; ++i)
    for (int j = i; j > 0 && longs[j]->long_term_idx < longs[j - 1]->long_term_idx; --j) {
      Picture* t = longs[j];
      longs[j] = longs[j - 1];
      longs[j - 1] = t;
    }
  Picture* init[2][kMaxPictures + 1];
  int init_n[2] = {0, 0};
  if (sh.type == kSliceP) {
    for (int i = 1; i < ns; ++i)
      for (int j = i; j > 0 && shorts[j]->frame_num_wrap > shorts[j - 1]->frame_num_wrap; --j) {
        Picture* t = shorts[j];
        shorts[j] = shorts[j - 1];
        shorts[j - 1] = t;
      }
    for (int i = 0; i < ns; ++i) init[0][init_n[0]++] = shorts[i];
    for (int i = 0; i < nl; ++i) init[0][init_n[0]++] = longs[i];
  } else {
    // B: by picture order count around the current picture.
    Picture* before[kMaxPictures];
    Picture* after[kMaxPictures];
    int nb = 0, na = 0;
    for (int i = 0; i < ns; ++i) (shorts[i]->poc < pic_poc_ ? before[nb++] : after[na++]) = shorts[i];
    for (int i = 1; i < nb; ++i)
      for (int j = i; j > 0 && before[j]->poc > before[j - 1]->poc; --j) {
        Picture* t = before[j];
        before[j] = before[j - 1];
        before[j - 1] = t;
      }
    for (int i = 1; i < na; ++i)
      for (int j = i; j > 0 && after[j]->poc < after[j - 1]->poc; --j) {
        Picture* t = after[j];
        after[j] = after[j - 1];
        after[j - 1] = t;
      }
    for (int i = 0; i < nb; ++i) init[0][init_n[0]++] = before[i];
    for (int i = 0; i < na; ++i) init[0][init_n[0]++] = after[i];
    for (int i = 0; i < na; ++i) init[1][init_n[1]++] = after[i];
    for (int i = 0; i < nb; ++i) init[1][init_n[1]++] = before[i];
    for (int i = 0; i < nl; ++i) {
      init[0][init_n[0]++] = longs[i];
      init[1][init_n[1]++] = longs[i];
    }
    if (init_n[1] > 1) {
      bool same = init_n[0] == init_n[1];
      for (int i = 0; same && i < init_n[0]; ++i) same = init[0][i] == init[1][i];
      if (same) {
        Picture* t = init[1][0];
        init[1][0] = init[1][1];
        init[1][1] = t;
      }
    }
  }
  const int max_frame = 1 << sp_->log2_max_frame_num;
  for (int l = 0; l < (sh.type == kSliceB ? 2 : 1); ++l) {
    const int n = sh.num_ref_idx[l];
    Picture* list[kMaxRefs + 2];
    for (int i = 0; i < kMaxRefs + 2; ++i) list[i] = nullptr;
    for (int i = 0; i < n && i < init_n[l]; ++i) list[i] = init[l][i];
    // modification
    int pred = sh.frame_num;
    int idx = 0;
    for (int m = 0; m < sh.num_mod[l] && idx < n; ++m) {
      const RefMod& rm = sh.mods[l][m];
      Picture* target = nullptr;
      if (rm.idc < 2) {
        const int diff = static_cast<int>(rm.value) + 1;
        int no_wrap;
        if (rm.idc == 0) {
          no_wrap = pred - diff;
          if (no_wrap < 0) no_wrap += max_frame;
        } else {
          no_wrap = pred + diff;
          if (no_wrap >= max_frame) no_wrap -= max_frame;
        }
        pred = no_wrap;
        const int pic_num = no_wrap > sh.frame_num ? no_wrap - max_frame : no_wrap;
        for (int i = 0; i < ns; ++i)
          if (shorts[i]->frame_num_wrap == pic_num) target = shorts[i];
      } else {
        for (int i = 0; i < nl; ++i)
          if (longs[i]->long_term_idx == static_cast<int>(rm.value)) target = longs[i];
      }
      if (!target) continue;
      for (int c = n; c > idx; --c) list[c] = list[c - 1];
      list[idx++] = target;
      int w = idx;
      for (int c = idx; c <= n; ++c)
        if (list[c] != target) list[w++] = list[c];
    }
    // Entries without a picture repeat the first one so a damaged stream never reads a missing reference.
    Picture* fallback = nullptr;
    for (int i = 0; i < n; ++i)
      if (list[i]) {
        fallback = list[i];
        break;
      }
    if (!fallback) fallback = shorts_any_or_cur();
    for (int i = 0; i < n; ++i) ref_list_[l][i] = list[i] ? list[i] : fallback;
    ref_count_[l] = n;
  }
  if (sh.type == kSliceB) {
    // Implicit bi-prediction weights (8.4.2.3.1) for every pair of references.
    for (int i = 0; i < ref_count_[0]; ++i) {
      for (int j = 0; j < ref_count_[1]; ++j) {
        const Picture* p0 = ref_list_[0][i];
        const Picture* p1 = ref_list_[1][j];
        int w1 = 32;
        const int td = clip3(-128, 127, p1->poc - p0->poc);
        if (td != 0 && !p0->is_long && !p1->is_long) {
          const int tb = clip3(-128, 127, pic_poc_ - p0->poc);
          const int tx = (16384 + iabs(td / 2)) / td;
          const int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
          if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128) w1 = dsf >> 2;
        }
        implicit_w_[i][j] = w1;
      }
    }
  }
}

Picture* Core::shorts_any_or_cur() {
  for (Picture& p : pics_)
    if (&p != cur_ && p.is_ref) return &p;
  return cur_;
}

}  // namespace h264

// ---------------------------------------------------------------------------------------------------------------
// The public class

H264Decoder::~H264Decoder() {}

bool H264Decoder::configure(ConstByteView avcc) {
  if (!core_) core_ = UniquePointer<h264::Core>::make(allocator_, allocator_);
  return core_ && core_->configure(avcc);
}

bool H264Decoder::decode(ConstByteView access_unit, int64_t pts_us) {
  if (!core_) core_ = UniquePointer<h264::Core>::make(allocator_, allocator_);
  return core_ && core_->decode(access_unit, pts_us);
}

bool H264Decoder::receive(VideoFrame* frame) { return core_ && core_->receive(frame); }

void H264Decoder::flush() {
  if (core_) core_->flush();
}

void H264Decoder::reset() {
  if (core_) core_->reset();
}

VideoFormat H264Decoder::output_format() const { return core_ ? core_->output_format() : VideoFormat{}; }

bool H264Decoder::ready() const { return core_ && core_->ready(); }

}  // namespace hardware
