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
  dpb_.reset();
  have_slice_ = false;
  cur_ = nullptr;
  waiting_for_idr_ = true;
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
    if (s.chroma_format_idc > 1 || bdl != 0 || bdc != 0 || s.qpprime_bypass) return false;  // 8-bit 4:2:0 and monochrome only
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
  s.mb_w = w;
  s.mb_h = hmap * (s.frame_mbs_only ? 1 : 2);
  if (w < 1 || s.mb_h < 1 || static_cast<int64_t>(w) * s.mb_h > kMaxMbs || w > 1024 || s.mb_h > 1024) return false;
  if (br.u(1)) {
    const int sub = s.chroma_format_idc == 0 ? 1 : 2;   // CropUnitX / CropUnitY (7-19 .. 7-22)
    const int unit_x = sub, unit_y = sub * (s.frame_mbs_only ? 1 : 2);
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

void ParameterSets::scaling_to_bitstream_order(const uint8_t w4[6][16], const uint8_t w8[2][64], uint8_t z4[6][16], uint8_t z8[2][64]) {
  for (int i = 0; i < 6; ++i)
    for (int k = 0; k < 16; ++k) z4[i][k] = w4[i][kZigzag4x4[k]];
  for (int i = 0; i < 2; ++i)
    for (int k = 0; k < 64; ++k) z8[i][k] = w8[i][kZigzag8x8[k]];
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
  dpb_.release_returned();
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
  if (waiting_for_idr_ && !sh.idr() && !ext_) {
    // A stream can also start with a recovery point (an I picture); accept a plain I slice.
    if (sh.type != kSliceI) return false;
  }
  if (!have_slice_ || new_picture(prev_sh_, sh)) {
    if (ext_ && ext_started_) return false;   // decode_external() takes one picture: the slices of another are not part of it
    if (have_slice_) finish_picture();
    if (!start_picture(sh, pts)) return false;
  }
  prev_sh_ = sh;
  have_slice_ = true;
  sh_ = sh;
  build_ref_lists(sh);
  if (observer_) {
    H264Decoder::SliceRefs refs;
    refs.slice_type = sh.type;
    refs.first_mb = sh.first_mb;
    refs.current_poc = pic_poc_;
    for (int l = 0; l < 2; ++l) {
      refs.count[l] = ref_n_[0][l] < 32 ? ref_n_[0][l] : 32;
      for (int i = 0; i < refs.count[l]; ++i) {
        const RefPic& r = refs_[0][l][i];
        refs.poc[l][i] = r.parity == 1 ? r.pic->top_poc : r.parity == 2 ? r.pic->bottom_poc : r.pic->poc;
        refs.long_term[l][i] = r.parity ? r.pic->field_ref[r.parity - 1] == 2 : r.pic->is_long;
      }
    }
    observer_(observer_context_, refs);
  }
  if (!decode_slice(br, sh)) {
    // A damaged slice: keep what was decoded so far.
    return true;
  }
  return true;
}

// 7.4.1.2.4
bool ParameterSets::new_picture(const Sps& sps, const SliceHeader& a, const SliceHeader& b) {
  if (a.frame_num != b.frame_num || a.pps_id != b.pps_id) return true;
  if (a.field_pic != b.field_pic || (a.field_pic && a.bottom_field != b.bottom_field)) return true;
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
    if (sh->field_pic) sh->bottom_field = br.u(1) != 0;
  }
  if (sh->idr()) sh->idr_pic_id = static_cast<int>(br.ue());
  if (s.poc_type == 0) {
    sh->poc_lsb = static_cast<int>(br.u(s.log2_max_poc_lsb));
    if (p.bottom_field_pic_order && !sh->field_pic) sh->delta_poc_bottom = br.se();
  } else if (s.poc_type == 1 && !s.delta_pic_order_always_zero) {
    sh->delta_poc[0] = br.se();
    if (p.bottom_field_pic_order && !sh->field_pic) sh->delta_poc[1] = br.se();
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
        sh->chroma_weight_flag[l][i] = s.chroma_format_idc != 0 && br.u(1) != 0;   // (no chroma weights without chroma)
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
  // first_mb_in_slice counts macroblock pairs in an MBAFF frame (CurrMbAddr = first_mb_in_slice * ( 1 + MbaffFrameFlag )) and the macroblocks of the field in a field picture.
  const int pic_mbs = s.mb_w * (sh->field_pic ? s.mb_h / 2 : s.mb_h);
  const int first_addr = (s.mbaff && !sh->field_pic) ? 2 * sh->first_mb : sh->first_mb;
  return !br.error() && first_addr < pic_mbs;
}

// ---------------------------------------------------------------------------------------------------------------
// Pictures

// A new sequence size: the pictures are released and get their memory again as they come into use.
bool Core::allocate_pictures() {
  const Sps& s = *sp_;
  dpb_.clear_pictures();
  for (Picture& p : pics_) {
    p.has_memory = false;
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
  cur_ = nullptr;
  return true;
}

bool Core::alloc_picture(Picture* p) {
  if (p->y.size() != 0) {
    p->has_memory = true;
    return true;
  }
  const uint64_t w = static_cast<uint64_t>(mb_w_) * 16, h = static_cast<uint64_t>(mb_h_) * 16;
  p->has_memory = p->y.alloc(allocator_, w * h) && p->u.alloc(allocator_, w / 2 * (h / 2)) && p->v.alloc(allocator_, w / 2 * (h / 2)) &&
                  p->motion.alloc(allocator_, static_cast<uint64_t>(mb_w_) * mb_h_);
  return p->has_memory;
}

// A "non-existing" frame of a gap in frame_num gets memory and, as ffmpeg gives it, the samples of the short-term reference before it (mid-grey if there is none).
// A stream that keeps to the standard never predicts from it; one that lost pictures is concealed with the last reference.
bool Core::prepare_non_existing(void* context, PictureInfo* frame, const PictureInfo* previous) {
  Core& core = *static_cast<Core*>(context);
  Picture* p = static_cast<Picture*>(frame);   // every slot of the buffer is one of our pictures
  p->alias = false;
  p->has_key = false;
  if (!core.alloc_picture(p)) return false;
  const Picture* prev = static_cast<const Picture*>(previous);
  if (prev && prev != p && prev->has_memory) return core.copy_picture(p, *prev);
  if (prev != p) core.make_grey(p);
  return true;
}

// The samples and motion of `from` (the same size: every picture has the sequence's size) into `to`. (Direct prediction reads a "non-existing" frame as intra
// whatever its motion says.)
bool Core::copy_picture(Picture* to, const Picture& from) {
  if (!to->y.view().copy_from(from.y.view()) || !to->u.view().copy_from(from.u.view()) || !to->v.view().copy_from(from.v.view())) return false;
  if (!to->motion.view().copy_from(from.motion.view())) return false;
  to->keyframe = from.keyframe;
  return true;
}

// Mid-grey samples and intra motion (no motion vectors for direct prediction), for a reference that holds no decoded picture.
void Core::make_grey(Picture* p) {
  p->y.view().fill(128);
  p->u.view().fill(128);
  p->v.view().fill(128);
  for (uint64_t k = 0; k < p->motion.size(); ++k) {
    MotionMb& m = p->motion[k];
    for (int l = 0; l < 2; ++l)
      for (int b = 0; b < 4; ++b) {
        m.ref[l][b] = -1;
        m.refid[l][b] = -1;
      }
    for (int l = 0; l < 2; ++l)
      for (int b = 0; b < 16; ++b) m.mv[l][b][0] = m.mv[l][b][1] = 0;
    m.intra = 1;
    m.field = 0;
  }
}

bool Core::start_picture(const SliceHeader& sh, int64_t pts) {
  const Pps& p = ps_.pps[sh.pps_id];
  const Sps& s = ps_.sps[p.sps_id];
  if (sp_ == nullptr || mb_w_ != s.mb_w || mb_h_ != s.mb_h) {
    if (sp_ == nullptr && !sh.idr() && sh.type != kSliceI && !ext_) return false;
    dpb_.bump_output(true);
    sp_ = &s;
    dpb_.set_sps(sp_);
    if (!allocate_pictures()) {
      sp_ = nullptr;
      return false;
    }
  }
  if (sh.field_pic && (s.mb_h & 1)) return false;   // (a field needs whole macroblock pairs: frame_mbs_only_flag 0 makes the height even)
  sp_ = &s;
  dpb_.set_sps(sp_);
  pp_ = &p;
  active_sps_id_ = p.sps_id;
  if (ext_ && !apply_external_references(sh)) {
    ext_no_memory_ = true;
    return false;
  }
  // A gap in frame_num (8.2.5.2; the program does this itself when decoding for a DXVA driver).
  if (!ext_ && !dpb_.fill_frame_num_gap(sh, &Core::prepare_non_existing, this)) return false;
  // Decoding for a DXVA driver, a field: the surface's frame (the picture the key held) is where the field goes. When it holds the other field only, the
  // field is decoded into it (the second field of a pair); otherwise into a new picture that takes the frame's other lines.
  Picture* surface = nullptr;
  if (ext_ && sh.field_pic) {
    for (Picture& q : pics_)
      if (q.has_key && q.key == ext_->key) surface = &q;
  }
  const uint8_t own = static_cast<uint8_t>(sh.bottom_field ? 2 : 1);
  Picture* into = (surface && surface->decoded && !(surface->decoded & own) && surface->has_memory) ? surface : nullptr;
  if (ext_) dpb_.forget_pending();
  PictureInfo* info = dpb_.begin_picture(sh, pts, into);
  if (!info) return false;
  Picture* pic = static_cast<Picture*>(info);  // every slot of the buffer is one of our pictures
  pic->alias = false;
  const bool second = dpb_.second_field();
  if (!second && !alloc_picture(pic)) {
    dpb_.abort_picture();
    if (ext_) ext_no_memory_ = true;
    return false;
  }
  if (ext_) {
    dpb_.set_current_poc(ext_->top_poc, ext_->bottom_poc);
    // The surface's earlier picture (which this one may predict from) keeps its key until this one is finished.
    if (!second && pic != surface) pic->has_key = false;
    ext_started_ = true;
  }
  structure_ = dpb_.structure();
  field_pic_ = structure_ != 3;
  mbaff_ = s.mbaff && !field_pic_;
  mono_ = s.chroma_format_idc == 0;
  pic_mb_h_ = field_pic_ ? mb_h_ / 2 : mb_h_;
  pic_mbs_ = mb_w_ * pic_mb_h_;
  motion_base_ = structure_ == 2 ? pic_mbs_ : 0;
  pic_ls_ = field_pic_ ? 2 * pic->stride : pic->stride;
  pic_cs_ = field_pic_ ? 2 * pic->cstride : pic->cstride;
  pic_y_ = pic->y.data() + (structure_ == 2 ? pic->stride : 0);
  pic_u_ = pic->u.data() + (structure_ == 2 ? pic->cstride : 0);
  pic_v_ = pic->v.data() + (structure_ == 2 ? pic->cstride : 0);
  cur_ = pic;
  pic_poc_ = dpb_.pic_poc();
  pic_pts_ = pts;
  slice_no_ = 0;
  for (int i = 0; i < pic_mbs_; ++i) mbs_[static_cast<uint64_t>(i)].slice = -1;
  if (!second) {
    if (surface && surface != pic) {
      // (a field into a surface that held a frame or the same field: the other lines are the surface's)
      for (uint64_t i = 0; i < pic->y.size(); ++i) pic->y[i] = surface->y[i];
      for (uint64_t i = 0; i < pic->u.size(); ++i) {
        pic->u[i] = surface->u[i];
        pic->v[i] = surface->v[i];
      }
    } else if (surface != pic) {
      // Samples of macroblocks never decoded stay mid-grey (and so do a field's other lines until its pair comes).
      for (uint64_t i = 0; i < pic->y.size(); ++i) pic->y[i] = 128;
      for (uint64_t i = 0; i < pic->u.size(); ++i) pic->u[i] = pic->v[i] = 128;
    }
    if (field_pic_) {
      // The other field's macroblocks have no motion until it is decoded (a direct prediction from it sees intra macroblocks).
      const int other = structure_ == 2 ? 0 : pic_mbs_;
      for (int i = 0; i < pic_mbs_; ++i) {
        MotionMb& m = pic->motion[static_cast<uint64_t>(other + i)];
        m.intra = 1;
        m.field = 1;
        for (int l = 0; l < 2; ++l)
          for (int b = 0; b < 4; ++b) {
            m.ref[l][b] = -1;
            m.refid[l][b] = -1;
          }
      }
    }
  }
  pic->mbaff = mbaff_;
  compute_scaling();
  waiting_for_idr_ = false;
  return true;
}

void Core::finish_picture() {
  if (!cur_ || !have_slice_) {
    have_slice_ = false;
    return;
  }
  have_slice_ = false;
  deblock_picture();
  if (ext_) ext_decoded_ = cur_;
  dpb_.finish_picture(prev_sh_);
  cur_ = nullptr;
}

// Decoding for a DXVA driver: the slots hold the pictures of the program's surfaces, by key. Before a picture starts, exactly the pictures the program names are
// references, with its frame numbers, order counts and the fields it says are used; a reference whose surface holds nothing gets a mid-grey stand-in (intra, so
// direct prediction from it has no motion), and so does an entry NonExistingFrameFlags marks (a frame inferred for a gap in frame_num: its surface is not read). A
// surface named a second time with another frame number or long-term status - ffmpeg names the surface of the reference before an inferred frame for it - is a
// second reference: a copy of the picture, kept while both stay named. Every other slot is free and forgets its key: a picture no longer referenced is never
// referenced again - except, for a field picture, the frame of the output surface, which the field goes into.
bool Core::apply_external_references(const SliceHeader& sh) {
  const H264Decoder::ExternalPicture& ext = *ext_;
  bool named[kMaxPictures] = {};
  bool kept[kMaxPictures] = {};
  for (int i = 0; i < kMaxPictures; ++i) {
    Picture& p = pics_[i];
    p.field_ref[0] = p.field_ref[1] = 0;
    p.is_ref = p.is_long = false;
    if (!p.has_key) continue;
    for (const H264Decoder::ExternalReference& r : ext.references)
      if (r.key == p.key && !r.non_existing) named[i] = true;
    kept[i] = named[i] || (sh.field_pic && p.key == ext.key);
    if (!kept[i]) p.has_key = false;
  }
  // A copy stays while the picture it copies is kept under the same key.
  for (int i = 0; i < kMaxPictures; ++i) {
    Picture& p = pics_[i];
    if (!p.alias) continue;
    bool held = false;
    for (int j = 0; j < kMaxPictures; ++j)
      if (named[j] && pics_[j].has_key && pics_[j].key == p.key && pics_[j].id == p.alias_of) held = true;
    if (held) named[i] = kept[i] = true;
    else p.alias = false;
  }
  auto free_slot = [&]() -> Picture* {
    for (int i = 0; i < kMaxPictures; ++i)
      if (!kept[i] && !pics_[i].is_ref && pics_[i].has_memory) return &pics_[i];
    for (int i = 0; i < kMaxPictures; ++i)
      if (!kept[i] && !pics_[i].is_ref) return &pics_[i];
    return nullptr;
  };
  for (const H264Decoder::ExternalReference& r : ext.references) {
    const uint8_t fields = r.fields & 3;
    if (!fields) continue;
    Picture* found = nullptr;
    Picture* held = nullptr;
    for (int i = 0; i < kMaxPictures && !held; ++i)
      if (named[i] && pics_[i].has_key && pics_[i].key == r.key && !r.non_existing) held = &pics_[i];
    if (held && !held->is_ref) {
      found = held;
    } else if (held) {
      // The surface is named again. With the same frame number and status as an earlier entry it is the same reference (the first entry stands).
      bool same = false;
      for (const Picture& q : pics_)
        if (q.is_ref && (&q == held || (q.alias && q.key == r.key && q.alias_of == held->id)) && q.is_long == r.long_term &&
            (r.long_term ? q.long_term_idx : q.frame_num) == r.frame_num)
          same = true;
      if (same) continue;
      for (int i = 0; i < kMaxPictures && !found; ++i) {
        Picture& q = pics_[i];
        if (named[i] && q.alias && !q.is_ref && q.key == r.key && q.alias_of == held->id) found = &q;
      }
      if (!found) {
        found = free_slot();
        if (!found || !alloc_picture(found) || !copy_picture(found, *held)) return false;
        found->id = dpb_.new_id();
        found->has_key = false;
        found->alias = true;
        found->alias_of = held->id;
        found->key = r.key;
        found->non_existing = false;
        found->decoded = held->decoded;
        found->field_coded = held->field_coded;
        found->mbaff = held->mbaff;
        named[found->slot] = kept[found->slot] = true;
      }
    } else {
      if (!r.non_existing) ext_missing_ = true;
      found = free_slot();
      if (!found || !alloc_picture(found)) return false;
      make_grey(found);
      found->id = dpb_.new_id();
      found->has_key = false;
      found->alias = false;
      found->non_existing = r.non_existing;
      found->keyframe = false;
      found->decoded = 3;
      found->field_coded = false;
      found->mbaff = false;
    }
    found->in_use = true;
    for (int f = 0; f < 2; ++f) found->field_ref[f] = (fields >> f) & 1 ? (r.long_term ? 2 : 1) : 0;
    found->is_ref = true;
    found->is_long = r.long_term;
    found->needed_for_output = false;
    if (r.long_term) found->long_term_idx = r.frame_num;
    else found->frame_num = r.frame_num;
    // (an unused field's count is not given: it takes the used field's, which makes PicOrderCnt( ) of the entry that field's)
    found->top_poc = (fields & 1) ? r.top_poc : r.bottom_poc;
    found->bottom_poc = (fields & 2) ? r.bottom_poc : r.top_poc;
    found->poc = found->top_poc < found->bottom_poc ? found->top_poc : found->bottom_poc;
  }
  for (int i = 0; i < kMaxPictures; ++i)
    if (!pics_[i].is_ref && !kept[i]) pics_[i].in_use = false;
  return true;
}

H264Decoder::ExternalStatus Core::decode_external(ConstByteView data, const H264Decoder::ExternalPicture& picture, VideoFrame* decoded, int* structure) {
  dpb_.release_returned();
  build_vlc();
  // Whatever an earlier call left half-done is dropped: this call is one whole picture.
  if (have_slice_) finish_picture();
  ext_ = &picture;
  ext_started_ = ext_missing_ = ext_no_memory_ = false;
  ext_decoded_ = nullptr;
  dpb_.set_decode_order(true);
  for_each_nal(data, 0, [&](const uint8_t* nal, uint64_t size) { (void)handle_nal(nal, size, 0); });
  const int pic_structure = structure_;
  finish_picture();
  dpb_.set_decode_order(false);
  const bool no_memory = ext_no_memory_, missing = ext_missing_;
  Picture* p = ext_decoded_;
  ext_ = nullptr;
  ext_decoded_ = nullptr;
  if (!p) return no_memory ? H264Decoder::ExternalStatus::NoMemory : H264Decoder::ExternalStatus::NotDecoded;
  // The picture now is the surface's: an earlier one kept under the same key is gone.
  for (Picture& q : pics_)
    if (&q != p && q.has_key && q.key == picture.key) {
      q.has_key = false;
      if (!q.is_ref) q.in_use = false;
    }
  p->has_key = true;
  p->key = picture.key;
  decoded->format.width = static_cast<uint32_t>(mb_w_ * 16);
  decoded->format.height = static_cast<uint32_t>(mb_h_ * 16);
  decoded->format.pixel_format = PixelFormat::I420;
  decoded->plane[0] = p->y.data();
  decoded->plane[1] = p->u.data();
  decoded->plane[2] = p->v.data();
  decoded->stride[0] = static_cast<uint32_t>(p->stride);
  decoded->stride[1] = decoded->stride[2] = static_cast<uint32_t>(p->cstride);
  decoded->pts_us = 0;
  decoded->keyframe = p->keyframe;
  decoded->limited_range = !sp_->full_range;
  if (structure) *structure = pic_structure;
  return missing ? H264Decoder::ExternalStatus::MissingReference : H264Decoder::ExternalStatus::Decoded;
}

void Core::flush() {
  finish_picture();
  dpb_.bump_output(true);
}

bool Core::receive(VideoFrame* frame) {
  PictureInfo* info = dpb_.take_output();
  if (!info) return false;
  const Picture* p = static_cast<const Picture*>(info);
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

namespace {

int poc_of(const RefPic& r) { return r.parity == 1 ? r.pic->top_poc : r.parity == 2 ? r.pic->bottom_poc : r.pic->poc; }
bool long_of(const RefPic& r) { return r.parity ? r.pic->field_ref[r.parity - 1] == 2 : r.pic->is_long; }

}  // namespace

// Implicit bi-prediction weights (8.4.2.3.1) for every pair of references of the list set `set`, the current picture (or field) counting `cur_poc`.
void Core::implicit_weights(int set, int cur_poc) {
  for (int i = 0; i < ref_n_[set][0]; ++i) {
    for (int j = 0; j < ref_n_[set][1]; ++j) {
      const RefPic& r0 = refs_[set][0][i];
      const RefPic& r1 = refs_[set][1][j];
      int w1 = 32;
      const int poc0 = poc_of(r0), poc1 = poc_of(r1);
      const int td = clip3(-128, 127, poc1 - poc0);
      if (poc1 != poc0 && !long_of(r0) && !long_of(r1)) {
        const int tb = clip3(-128, 127, cur_poc - poc0);
        const int tx = (16384 + iabs(td / 2)) / td;
        const int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
        if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128) w1 = dsf >> 2;
      }
      implicit_w_[set][i][j] = w1;
    }
  }
}

// The reference lists of a slice (the Dpb builds them), as pictures with samples; in an MBAFF frame also the field lists of its field macroblocks (8.4.2.1); and
// the implicit bi-prediction weights of each.
void Core::build_ref_lists(const SliceHeader& sh) {
  dpb_.build_ref_lists(sh);
  for (int l = 0; l < 2; ++l) {
    ref_n_[0][l] = dpb_.ref_count(l);
    for (int i = 0; i <= kMaxRefs; ++i) {
      refs_[0][l][i].pic = static_cast<Picture*>(dpb_.ref(l, i));
      refs_[0][l][i].parity = static_cast<uint8_t>(dpb_.ref_parity(l, i));
    }
    for (int set = 1; set < 3; ++set) {
      ref_n_[set][l] = 0;
      if (!mbaff_) continue;
      // set 1: top field macroblocks, set 2: bottom field macroblocks
      const uint8_t same = static_cast<uint8_t>(set), other = static_cast<uint8_t>(3 - set);
      for (int i = 0; i < ref_n_[0][l]; ++i) {
        refs_[set][l][2 * i] = RefPic{refs_[0][l][i].pic, same};
        refs_[set][l][2 * i + 1] = RefPic{refs_[0][l][i].pic, other};
      }
      ref_n_[set][l] = 2 * ref_n_[0][l];
      refs_[set][l][ref_n_[set][l]] = RefPic{};
    }
  }
  if (sh.type == kSliceB) {
    implicit_weights(0, pic_poc_);
    if (mbaff_) {
      implicit_weights(1, dpb_.top_poc());
      implicit_weights(2, dpb_.bottom_poc());
    }
  }
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

H264Decoder::ExternalStatus H264Decoder::decode_external(ConstByteView access_unit, const ExternalPicture& picture, VideoFrame* decoded, int* structure) {
  if (!core_) core_ = UniquePointer<h264::Core>::make(allocator_, allocator_);
  if (!core_) return ExternalStatus::NoMemory;
  return core_->decode_external(access_unit, picture, decoded, structure);
}

void H264Decoder::flush() {
  if (core_) core_->flush();
}

void H264Decoder::reset() {
  if (core_) core_->reset();
}

VideoFormat H264Decoder::output_format() const { return core_ ? core_->output_format() : VideoFormat{}; }

bool H264Decoder::ready() const { return core_ && core_->ready(); }

void H264Decoder::set_slice_observer(SliceObserver observer, void* context) {
  if (!core_) core_ = UniquePointer<h264::Core>::make(allocator_, allocator_);
  if (core_) core_->set_slice_observer(observer, context);
}

}  // namespace hardware
