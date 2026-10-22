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

// H.264 encoding: parameter sets, slice headers, CAVLC, Intra16x16 / P16x16 macroblock coding and the encoder's
// own reconstruction (kept bit-exact with the decoder: the reference picture of the next P picture).

#include "Hardware/VideoCodec/Software/H264Encoder.hpp"
#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h264 {

#include "Hardware/VideoCodec/Software/H264Tables.inc"

namespace {

inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline uint8_t clip_pixel(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// Writes bits MSB first into a byte buffer (an RBSP; emulation prevention is added when the NAL unit is built).
class BitWriter {
 public:
  explicit BitWriter(allocators::IAllocator& allocator) : buf_(allocator) {}
  void reset() {
    buf_.clear();
    acc_ = 0;
    count_ = 0;
    ok_ = true;
  }
  void bit(uint32_t b) {
    acc_ = static_cast<uint8_t>((acc_ << 1) | (b & 1));
    if (++count_ == 8) {
      ok_ = buf_.put8(acc_) && ok_;
      acc_ = 0;
      count_ = 0;
    }
  }
  void put(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) bit(value >> i);
  }
  void ue(uint32_t v) {
    const uint64_t x = uint64_t{v} + 1;
    int len = 0;
    while ((x >> (len + 1)) != 0) ++len;
    put(0, len);
    put(static_cast<uint32_t>(x), len + 1);
  }
  void se(int v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v)); }
  void trailing() {
    bit(1);
    while (count_ != 0) bit(0);
  }
  void fail() { ok_ = false; }
  bool ok() const { return ok_; }
  ConstByteView bytes() const { return buf_.view(); }

 private:
  ByteBuffer buf_;
  uint8_t acc_ = 0;
  int count_ = 0;
  bool ok_ = true;
};

struct VlcRef {
  const VlcEntry* entries;
  int count;
};
template <int N>
constexpr VlcRef vlc(const VlcEntry (&a)[N]) {
  return {a, N};
}

const VlcRef kCoeffTokenTables[4] = {vlc(kCoeffToken0), vlc(kCoeffToken1), vlc(kCoeffToken2), vlc(kCoeffToken3)};
const VlcRef kCoeffTokenDc = vlc(kCoeffTokenChromaDc);
const VlcRef kTotalZerosTables[15] = {vlc(kTotalZeros1),  vlc(kTotalZeros2),  vlc(kTotalZeros3),  vlc(kTotalZeros4),  vlc(kTotalZeros5),
                                      vlc(kTotalZeros6),  vlc(kTotalZeros7),  vlc(kTotalZeros8),  vlc(kTotalZeros9),  vlc(kTotalZeros10),
                                      vlc(kTotalZeros11), vlc(kTotalZeros12), vlc(kTotalZeros13), vlc(kTotalZeros14), vlc(kTotalZeros15)};
const VlcRef kTotalZerosDcTables[3] = {vlc(kTotalZerosChromaDc1), vlc(kTotalZerosChromaDc2), vlc(kTotalZerosChromaDc3)};
const VlcRef kRunBeforeTables[7] = {vlc(kRunBefore1), vlc(kRunBefore2), vlc(kRunBefore3), vlc(kRunBefore4),
                                    vlc(kRunBefore5), vlc(kRunBefore6), vlc(kRunBefore7)};

void put_vlc(BitWriter& w, const VlcRef& table, int symbol) {
  for (int i = 0; i < table.count; ++i) {
    if (table.entries[i].sym == symbol) {
      w.put(table.entries[i].code, table.entries[i].len);
      return;
    }
  }
  w.fail();
}

// 9.2 in reverse: one residual block. `lvl` holds max_coeff levels in scan order; nc < 0 is the chroma DC block.
// Returns the number of non-zero levels.
int write_block(BitWriter& w, int nc, const int* lvl, int max_coeff) {
  int vals[16], pos[16];
  int total = 0;
  for (int i = max_coeff - 1; i >= 0; --i) {
    if (lvl[i] != 0) {
      vals[total] = lvl[i];
      pos[total] = i;
      ++total;
    }
  }
  int t1 = 0;
  while (t1 < total && t1 < 3 && iabs(vals[t1]) == 1) ++t1;
  put_vlc(w, nc < 0 ? kCoeffTokenDc : kCoeffTokenTables[nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3], total * 4 + t1);
  if (total == 0) return 0;
  for (int i = 0; i < t1; ++i) w.bit(vals[i] < 0);
  int suffix_len = (total > 10 && t1 < 3) ? 1 : 0;
  for (int i = t1; i < total; ++i) {
    const int v = vals[i];
    int code = v > 0 ? 2 * v - 2 : -2 * v - 1;
    if (i == t1 && t1 < 3) code -= 2;
    int prefix, suffix = 0, suffix_bits = 0;
    if (suffix_len == 0) {
      if (code < 14) {
        prefix = code;
      } else if (code < 30) {
        prefix = 14;
        suffix = code - 14;
        suffix_bits = 4;
      } else {
        prefix = 15;
        suffix = code - 30;
        suffix_bits = 12;
      }
    } else if (code < (15 << suffix_len)) {
      prefix = code >> suffix_len;
      suffix = code & ((1 << suffix_len) - 1);
      suffix_bits = suffix_len;
    } else {
      prefix = 15;
      suffix = code - (15 << suffix_len);
      suffix_bits = 12;
    }
    if (suffix >= (1 << suffix_bits) && suffix_bits > 0) w.fail();  // levels are clamped so this cannot happen
    w.put(1, prefix + 1);
    if (suffix_bits > 0) w.put(static_cast<uint32_t>(suffix), suffix_bits);
    if (suffix_len == 0) suffix_len = 1;
    if (iabs(v) > (3 << (suffix_len - 1)) && suffix_len < 6) ++suffix_len;
  }
  int zeros_left = 0;
  if (total < max_coeff) {
    zeros_left = pos[0] + 1 - total;
    put_vlc(w, nc < 0 ? kTotalZerosDcTables[total - 1] : kTotalZerosTables[total - 1], zeros_left);
  }
  for (int i = 0; i < total - 1 && zeros_left > 0; ++i) {
    const int run = pos[i] - pos[i + 1] - 1;
    put_vlc(w, kRunBeforeTables[imin(zeros_left, 7) - 1], run);
    zeros_left -= run;
  }
  return total;
}

// Forward quantiser and dequantiser constants (flat scaling matrices).
const int kMf[6][3] = {{13107, 5243, 8066}, {11916, 4660, 7490}, {10082, 4194, 6554}, {9362, 3647, 5825}, {8192, 3355, 5243}, {7282, 2893, 4559}};
const int kNorm[6][3] = {{10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23}};
const int kMaxLevel = 2047;  // keeps level_prefix within 15, as Baseline requires

// Position class of a coefficient in a 4x4 block (raster index): 0 both even, 1 both odd, 2 otherwise.
inline int pos_class(int p) {
  const int i = p >> 2, j = p & 3;
  return ((i & 1) == 0 && (j & 1) == 0) ? 0 : ((i & 1) && (j & 1)) ? 1 : 2;
}

inline int quantize(int w, int mf, int shift, int round) {
  const int64_t q = (static_cast<int64_t>(iabs(w)) * mf + round) >> shift;
  const int level = static_cast<int>(q > kMaxLevel ? kMaxLevel : q);
  return w < 0 ? -level : level;
}

void forward4x4(const int* r, int* w) {
  int t[16];
  for (int i = 0; i < 4; ++i) {
    const int a = r[i * 4], b = r[i * 4 + 1], c = r[i * 4 + 2], d = r[i * 4 + 3];
    const int s0 = a + d, s1 = b + c, d0 = a - d, d1 = b - c;
    t[i * 4 + 0] = s0 + s1;
    t[i * 4 + 1] = 2 * d0 + d1;
    t[i * 4 + 2] = s0 - s1;
    t[i * 4 + 3] = d0 - 2 * d1;
  }
  for (int j = 0; j < 4; ++j) {
    const int a = t[j], b = t[4 + j], c = t[8 + j], d = t[12 + j];
    const int s0 = a + d, s1 = b + c, d0 = a - d, d1 = b - c;
    w[j] = s0 + s1;
    w[4 + j] = 2 * d0 + d1;
    w[8 + j] = s0 - s1;
    w[12 + j] = d0 - 2 * d1;
  }
}

// 8.5.12: the inverse transform of dequantised coefficients (raster order), added to the prediction at `dst`.
void inverse4x4_add(const int* d, ByteView dst, uint64_t offset, uint32_t stride) {
  int t[16];
  for (int i = 0; i < 4; ++i) {
    const int e0 = d[i * 4] + d[i * 4 + 2], e1 = d[i * 4] - d[i * 4 + 2];
    const int e2 = (d[i * 4 + 1] >> 1) - d[i * 4 + 3], e3 = d[i * 4 + 1] + (d[i * 4 + 3] >> 1);
    t[i * 4 + 0] = e0 + e3;
    t[i * 4 + 1] = e1 + e2;
    t[i * 4 + 2] = e1 - e2;
    t[i * 4 + 3] = e0 - e3;
  }
  for (int j = 0; j < 4; ++j) {
    const int e0 = t[j] + t[8 + j], e1 = t[j] - t[8 + j];
    const int e2 = (t[4 + j] >> 1) - t[12 + j], e3 = t[4 + j] + (t[12 + j] >> 1);
    const int r[4] = {e0 + e3, e1 + e2, e1 - e2, e0 - e3};
    for (int i = 0; i < 4; ++i) {
      const uint64_t at = offset + uint64_t{static_cast<uint32_t>(i)} * stride + static_cast<uint32_t>(j);
      dst[at] = clip_pixel(dst[at] + ((r[i] + 32) >> 6));
    }
  }
}

// Dequantised coefficient from a level at raster position `pos` (8.5.12.1, flat matrices: LevelScale = 16 * normAdjust).
inline int dequant(int level, int qp, int pos) { return (level * kNorm[qp % 6][pos_class(pos)]) << (qp / 6); }

// Appends a NAL unit (header byte, then the RBSP with emulation prevention bytes) to `nal`.
bool build_nal(ByteBuffer* nal, int ref_idc, int type, ConstByteView rbsp) {
  bool ok = nal->put8(static_cast<uint8_t>((ref_idc << 5) | type));
  int zeros = 0;
  for (const uint8_t b : rbsp) {
    if (zeros >= 2 && b <= 3) {
      ok = nal->put8(3) && ok;
      zeros = 0;
    }
    ok = nal->put8(b) && ok;
    zeros = b == 0 ? zeros + 1 : 0;
  }
  return ok;
}

// A plane set with a size that is a multiple of the macroblock size.
struct Pic {
  ByteView mem, y, u, v;  // y, u and v are slices of mem
  uint32_t w = 0, h = 0;  // luma size (padded)
  uint32_t cw() const { return w / 2; }
  uint32_t ch() const { return h / 2; }
};

struct MbInfo {
  int16_t mvx = 0, mvy = 0;
  uint8_t inter = 0;
  uint8_t nz[24] = {};  // total_coeff per 4x4 block: 16 luma (raster), 4 Cb, 4 Cr
};

struct Neighbour {
  bool available = false;
  bool inter = false;
  int mvx = 0, mvy = 0;
};

// Level_idc for a picture of this many macroblocks (Table A-1, MaxFS).
int level_for(uint32_t mbw, uint32_t mbh) {
  static const struct { int idc; uint32_t max_fs; } kLevels[] = {{10, 99},    {11, 396},   {21, 792},    {22, 1620},   {31, 3600},   {32, 5120},
                                                              {40, 8192}, {42, 8704},  {50, 22080},  {51, 36864},  {60, 139264}};
  const uint64_t mbs = uint64_t{mbw} * mbh;
  const uint32_t longest = mbw > mbh ? mbw : mbh;
  for (const auto& l : kLevels) {
    if (mbs <= l.max_fs && uint64_t{longest} * longest <= uint64_t{l.max_fs} * 8) return l.idc;
  }
  return 0;
}

}  // namespace

class Encoder {
 public:
  explicit Encoder(allocators::IAllocator& allocator) : a_(allocator), bw_(allocator), sps_(allocator), pps_(allocator), scratch_(allocator) {}
  ~Encoder() {
    free_pic(&src_);
    free_pic(&rec_);
    free_pic(&ref_);
    release_view(a_, mbs_);
  }
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  bool configure(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, uint32_t quality, uint32_t key_interval);
  bool encode(const VideoFrame& in, bool force_key, ByteBuffer* out, bool* keyframe);
  bool reconstruction(VideoFrame* frame) const;
  bool avcc(ByteBuffer* out) const;

 private:
  // The decisions and data of the macroblock being coded.
  struct Mb {
    bool intra = true;
    int i16_mode = 2, chroma_mode = 0;
    int mvx = 0, mvy = 0;
    int cbp_luma = 0, cbp_chroma = 0;  // cbp_luma: bit per 8x8 block (inter) or 15 / 0 for the Intra16x16 AC flag
    int dc[16];                        // Intra16x16 luma DC levels, scan order
    int ac[16][16];                    // luma levels per 4x4 block (raster block index), scan order
    int cdc[2][4];                     // chroma DC levels
    int cac[2][4][16];                 // chroma AC levels, scan order (entry 0 unused)
    uint8_t pred_y[256];
    uint8_t pred_c[2][64];
    uint8_t nz[24];
  };

  bool alloc_pic(Pic* p, uint32_t w, uint32_t h);
  void free_pic(Pic* p) {
    release_view(a_, p->mem);
    *p = Pic();
  }

  bool write_parameter_sets();
  void write_slice_header(bool idr, bool pslice);
  bool code_picture(bool idr, bool pslice);

  // Neighbour helpers
  const MbInfo* mb_at(int x, int y) const { return (x < 0 || y < 0 || x >= static_cast<int>(mbw_) || y >= static_cast<int>(mbh_)) ? nullptr : &mbs_[static_cast<uint64_t>(y) * mbw_ + x]; }
  Neighbour neighbour(int x, int y) const;
  void predict_mv(int mbx, int mby, int* px, int* py) const;
  void skip_mv(int mbx, int mby, int* px, int* py) const;
  int nc_for(int bx, int by, int plane, int mbx, int mby, const uint8_t* cur_nz) const;

  // Prediction
  bool pred_i16(int mode, int mbx, int mby, uint8_t* pred) const;
  bool pred_chroma(int mode, int mbx, int mby, int plane, uint8_t* pred) const;
  void pred_inter(int mbx, int mby, int mvx, int mvy);
  uint32_t sad16(int mbx, int mby, const uint8_t* pred) const;
  uint32_t sad_mv(int mbx, int mby, int mvx, int mvy) const;  // block fully inside the picture
  int mv_cost(int dx, int dy) const;

  // Residual
  void quant_luma_i16(int mbx, int mby);
  void quant_luma_inter(int mbx, int mby);
  void quant_chroma(int mbx, int mby, bool intra);
  void reconstruct(int mbx, int mby);
  void write_macroblock(int mbx, int mby, bool pslice, int pred_mvx, int pred_mvy);

  allocators::IAllocator& a_;
  BitWriter bw_;
  ByteBuffer sps_, pps_;  // NAL units without start codes
  ByteBuffer scratch_;
  uint32_t width_ = 0, height_ = 0, mbw_ = 0, mbh_ = 0;
  uint32_t fps_num_ = 30, fps_den_ = 1, key_interval_ = 1;
  int qp_ = 26, qpc_ = 26, level_ = 10;
  Pic src_, rec_, ref_;
  ArrayView<MbInfo> mbs_;
  Mb m_;
  uint32_t skip_run_ = 0;
  uint32_t frame_num_ = 0, idr_id_ = 0, since_key_ = 0;
  bool have_ref_ = false;
  bool configured_ = false;
};

// ---------------------------------------------------------------------------------------------------------------
// Setup

bool Encoder::alloc_pic(Pic* p, uint32_t w, uint32_t h) {
  ByteView& mem = p->mem;
  const uint64_t luma = uint64_t{w} * h, chroma = luma / 4;
  mem = allocate_view<uint8_t>(a_, luma + 2 * chroma);
  if (mem.empty()) return false;
  mem.fill(0);
  p->w = w;
  p->h = h;
  p->y = mem.subview(0, luma);
  p->u = mem.subview(luma, chroma);
  p->v = mem.subview(luma + chroma, chroma);
  return true;
}

bool Encoder::configure(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, uint32_t quality, uint32_t key_interval) {
  if (width < 2 || height < 2 || (width & 1) || (height & 1) || width > VideoFormat::kMaxDimension || height > VideoFormat::kMaxDimension) return false;
  if (fps_num == 0 || fps_den == 0) return false;
  width_ = width;
  height_ = height;
  mbw_ = (width + 15) / 16;
  mbh_ = (height + 15) / 16;
  level_ = level_for(mbw_, mbh_);
  if (level_ == 0) return false;
  fps_num_ = fps_num;
  fps_den_ = fps_den;
  key_interval_ = key_interval == 0 ? 1 : key_interval;
  const int q = static_cast<int>(clip3(1, 100, static_cast<int>(quality)));
  qp_ = clip3(6, 51, 51 - (q * 45 + 50) / 100);
  qpc_ = kChromaQp[qp_];
  if (!alloc_pic(&src_, mbw_ * 16, mbh_ * 16) || !alloc_pic(&rec_, mbw_ * 16, mbh_ * 16) || !alloc_pic(&ref_, mbw_ * 16, mbh_ * 16)) return false;
  mbs_ = allocate_view<MbInfo>(a_, uint64_t{mbw_} * mbh_);
  if (mbs_.empty()) return false;
  for (MbInfo& m : mbs_) m = MbInfo();
  frame_num_ = idr_id_ = since_key_ = 0;
  have_ref_ = false;
  configured_ = write_parameter_sets();
  return configured_;
}

bool Encoder::write_parameter_sets() {
  sps_.clear();
  pps_.clear();
  bw_.reset();
  bw_.put(66, 8);  // profile_idc: Baseline
  bw_.put(0, 8);   // constraint flags, reserved bits
  bw_.put(static_cast<uint32_t>(level_), 8);
  bw_.ue(0);  // seq_parameter_set_id
  bw_.ue(4);  // log2_max_frame_num_minus4: frame_num has 8 bits
  bw_.ue(2);  // pic_order_cnt_type: output order is decoding order
  bw_.ue(1);  // max_num_ref_frames
  bw_.bit(0);  // gaps_in_frame_num_value_allowed_flag
  bw_.ue(mbw_ - 1);
  bw_.ue(mbh_ - 1);
  bw_.bit(1);  // frame_mbs_only_flag
  bw_.bit(1);  // direct_8x8_inference_flag
  const uint32_t crop_r = (mbw_ * 16 - width_) / 2, crop_b = (mbh_ * 16 - height_) / 2;
  if (crop_r || crop_b) {
    bw_.bit(1);
    bw_.ue(0);
    bw_.ue(crop_r);
    bw_.ue(0);
    bw_.ue(crop_b);
  } else {
    bw_.bit(0);
  }
  bw_.bit(1);     // vui_parameters_present_flag
  bw_.bit(0);     // aspect_ratio_info_present_flag
  bw_.bit(0);     // overscan_info_present_flag
  bw_.bit(1);     // video_signal_type_present_flag
  bw_.put(5, 3);  // video_format: unspecified
  bw_.bit(1);     // video_full_range_flag
  bw_.bit(0);     // colour_description_present_flag
  bw_.bit(0);     // chroma_loc_info_present_flag
  bw_.bit(1);     // timing_info_present_flag
  bw_.put(fps_den_, 32);
  bw_.put(fps_num_ * 2u, 32);
  bw_.bit(1);  // fixed_frame_rate_flag
  bw_.bit(0);  // nal_hrd_parameters_present_flag
  bw_.bit(0);  // vcl_hrd_parameters_present_flag
  bw_.bit(0);  // pic_struct_present_flag
  bw_.bit(1);  // bitstream_restriction_flag
  bw_.bit(1);  // motion_vectors_over_pic_boundaries_flag
  bw_.ue(0);   // max_bytes_per_pic_denom
  bw_.ue(0);   // max_bits_per_mb_denom
  bw_.ue(16);  // log2_max_mv_length_horizontal
  bw_.ue(16);  // log2_max_mv_length_vertical
  bw_.ue(0);   // max_num_reorder_frames
  bw_.ue(1);   // max_dec_frame_buffering
  bw_.trailing();
  if (!bw_.ok() || !build_nal(&sps_, 3, 7, bw_.bytes())) return false;

  bw_.reset();
  bw_.ue(0);        // pic_parameter_set_id
  bw_.ue(0);        // seq_parameter_set_id
  bw_.bit(0);       // entropy_coding_mode_flag: CAVLC
  bw_.bit(0);       // bottom_field_pic_order_in_frame_present_flag
  bw_.ue(0);        // num_slice_groups_minus1
  bw_.ue(0);        // num_ref_idx_l0_default_active_minus1
  bw_.ue(0);        // num_ref_idx_l1_default_active_minus1
  bw_.bit(0);       // weighted_pred_flag
  bw_.put(0, 2);    // weighted_bipred_idc
  bw_.se(qp_ - 26);  // pic_init_qp_minus26
  bw_.se(0);        // pic_init_qs_minus26
  bw_.se(0);        // chroma_qp_index_offset
  bw_.bit(1);       // deblocking_filter_control_present_flag
  bw_.bit(0);       // constrained_intra_pred_flag
  bw_.bit(0);       // redundant_pic_cnt_present_flag
  bw_.trailing();
  return bw_.ok() && build_nal(&pps_, 3, 8, bw_.bytes());
}

bool Encoder::avcc(ByteBuffer* out) const {
  if (!configured_) return false;
  const ConstByteView sps = sps_.view(), pps = pps_.view();
  bool ok = out->put8(1) && out->put8(66) && out->put8(0) && out->put8(static_cast<uint8_t>(level_)) && out->put8(0xFF) && out->put8(0xE1);
  ok = ok && out->put8(static_cast<uint8_t>(sps.size() >> 8)) && out->put8(static_cast<uint8_t>(sps.size())) && out->append(sps);
  ok = ok && out->put8(1) && out->put8(static_cast<uint8_t>(pps.size() >> 8)) && out->put8(static_cast<uint8_t>(pps.size())) && out->append(pps);
  return ok;
}

bool Encoder::reconstruction(VideoFrame* frame) const {
  if (!configured_ || !have_ref_) return false;
  frame->format = {width_, height_, PixelFormat::I420};
  frame->plane[0] = ref_.y.data();
  frame->plane[1] = ref_.u.data();
  frame->plane[2] = ref_.v.data();
  frame->stride[0] = ref_.w;
  frame->stride[1] = frame->stride[2] = ref_.cw();
  frame->limited_range = false;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Neighbours and motion vector prediction

Neighbour Encoder::neighbour(int x, int y) const {
  Neighbour n;
  const MbInfo* m = mb_at(x, y);
  if (!m) return n;
  n.available = true;
  n.inter = m->inter != 0;
  if (n.inter) {
    n.mvx = m->mvx;
    n.mvy = m->mvy;
  }
  return n;
}

static int median3(int a, int b, int c) { return imax(imin(a, b), imin(imax(a, b), c)); }

// 8.4.1.3: the predicted vector of a 16x16 partition (reference index 0).
void Encoder::predict_mv(int mbx, int mby, int* px, int* py) const {
  Neighbour a = neighbour(mbx - 1, mby), b = neighbour(mbx, mby - 1);
  // The top-right macroblock is available only if it exists in the picture (it is always already coded).
  Neighbour c = (mbx + 1 < static_cast<int>(mbw_)) ? neighbour(mbx + 1, mby - 1) : Neighbour();
  if (!c.available) c = neighbour(mbx - 1, mby - 1);
  if (!b.available && !c.available && a.available) {
    b = a;
    c = a;
  }
  const bool ra = a.inter, rb = b.inter, rc = c.inter;  // refIdx == 0
  const int same = (ra ? 1 : 0) + (rb ? 1 : 0) + (rc ? 1 : 0);
  if (same == 1) {
    const Neighbour& n = ra ? a : rb ? b : c;
    *px = n.mvx;
    *py = n.mvy;
    return;
  }
  *px = median3(a.inter ? a.mvx : 0, b.inter ? b.mvx : 0, c.inter ? c.mvx : 0);
  *py = median3(a.inter ? a.mvy : 0, b.inter ? b.mvy : 0, c.inter ? c.mvy : 0);
}

// 8.4.1.1: the vector of a skipped macroblock.
void Encoder::skip_mv(int mbx, int mby, int* px, int* py) const {
  const Neighbour a = neighbour(mbx - 1, mby), b = neighbour(mbx, mby - 1);
  if (!a.available || !b.available || (a.inter && a.mvx == 0 && a.mvy == 0) || (b.inter && b.mvx == 0 && b.mvy == 0)) {
    *px = *py = 0;
    return;
  }
  predict_mv(mbx, mby, px, py);
}

// 9.2.1: nC of a 4x4 block. plane 0 luma (bx, by in 0..3), 1 and 2 chroma (0..1).
int Encoder::nc_for(int bx, int by, int plane, int mbx, int mby, const uint8_t* cur_nz) const {
  const int w = plane == 0 ? 4 : 2;
  const int base = plane == 0 ? 0 : 16 + (plane - 1) * 4;
  int na = -1, nb = -1;
  if (bx > 0) {
    na = cur_nz[base + by * w + bx - 1];
  } else if (const MbInfo* m = mb_at(mbx - 1, mby)) {
    na = m->nz[base + by * w + w - 1];
  }
  if (by > 0) {
    nb = cur_nz[base + (by - 1) * w + bx];
  } else if (const MbInfo* m = mb_at(mbx, mby - 1)) {
    nb = m->nz[base + (w - 1) * w + bx];
  }
  if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
  if (na >= 0) return na;
  if (nb >= 0) return nb;
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Prediction

// 8.3.3: Intra16x16 prediction from the reconstructed neighbours. False if the mode needs samples that are not there.
bool Encoder::pred_i16(int mode, int mbx, int mby, uint8_t* pred) const {
  const uint32_t W = rec_.w;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const bool left = mbx > 0, top = mby > 0;
  if (mode == 0) {  // vertical
    if (!top) return false;
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) pred[y * 16 + x] = rec_.y[base - W + x];
    return true;
  }
  if (mode == 1) {  // horizontal
    if (!left) return false;
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) pred[y * 16 + x] = rec_.y[base + uint64_t{static_cast<uint32_t>(y)} * W - 1];
    return true;
  }
  if (mode != 2) return false;
  int sum = 0, dc;
  if (top) for (int x = 0; x < 16; ++x) sum += rec_.y[base - W + x];
  if (left) for (int y = 0; y < 16; ++y) sum += rec_.y[base + uint64_t{static_cast<uint32_t>(y)} * W - 1];
  if (top && left) dc = (sum + 16) >> 5;
  else if (top || left) dc = (sum + 8) >> 4;
  else dc = 128;
  for (int i = 0; i < 256; ++i) pred[i] = static_cast<uint8_t>(dc);
  return true;
}

// 8.3.4: chroma prediction of one plane (0 DC, 1 horizontal, 2 vertical).
bool Encoder::pred_chroma(int mode, int mbx, int mby, int plane, uint8_t* pred) const {
  const uint32_t W = rec_.cw();
  const ByteView& p = plane == 0 ? rec_.u : rec_.v;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * W + static_cast<uint32_t>(mbx) * 8;
  const bool left = mbx > 0, top = mby > 0;
  if (mode == 2) {
    if (!top) return false;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) pred[y * 8 + x] = p[base - W + x];
    return true;
  }
  if (mode == 1) {
    if (!left) return false;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) pred[y * 8 + x] = p[base + uint64_t{static_cast<uint32_t>(y)} * W - 1];
    return true;
  }
  if (mode != 0) return false;
  for (int by = 0; by < 2; ++by) {
    for (int bx = 0; bx < 2; ++bx) {
      int st = 0, sl = 0;
      if (top) for (int x = 0; x < 4; ++x) st += p[base - W + bx * 4 + x];
      if (left) for (int y = 0; y < 4; ++y) sl += p[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W - 1];
      int dc;
      if ((bx == 0 && by == 0) || (bx == 1 && by == 1)) {
        if (top && left) dc = (st + sl + 4) >> 3;
        else if (left) dc = (sl + 2) >> 2;
        else if (top) dc = (st + 2) >> 2;
        else dc = 128;
      } else if (bx == 1) {  // by == 0: prefers the top samples
        if (top) dc = (st + 2) >> 2;
        else if (left) dc = (sl + 2) >> 2;
        else dc = 128;
      } else {  // bx == 0, by == 1: prefers the left samples
        if (left) dc = (sl + 2) >> 2;
        else if (top) dc = (st + 2) >> 2;
        else dc = 128;
      }
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) pred[(by * 4 + y) * 8 + bx * 4 + x] = static_cast<uint8_t>(dc);
    }
  }
  return true;
}

// 8.4.2: the motion-compensated prediction of the macroblock from the reference picture. The vector is in quarter
// pixels and always a whole number of pixels for luma; chroma gets the bilinear filter. Reference coordinates
// outside the picture are clamped to its edge.
void Encoder::pred_inter(int mbx, int mby, int mvx, int mvy) {
  const int W = static_cast<int>(ref_.w), H = static_cast<int>(ref_.h);
  const int x0 = mbx * 16 + (mvx >> 2), y0 = mby * 16 + (mvy >> 2);
  for (int y = 0; y < 16; ++y) {
    const int sy = clip3(0, H - 1, y0 + y);
    for (int x = 0; x < 16; ++x) m_.pred_y[y * 16 + x] = ref_.y[static_cast<uint64_t>(sy) * W + clip3(0, W - 1, x0 + x)];
  }
  const int CW = W / 2, CH = H / 2;
  const int cx0 = mbx * 8 + (mvx >> 3), cy0 = mby * 8 + (mvy >> 3);
  const int fx = mvx & 7, fy = mvy & 7;
  for (int c = 0; c < 2; ++c) {
    const ByteView& p = c == 0 ? ref_.u : ref_.v;
    for (int y = 0; y < 8; ++y) {
      const int ya = clip3(0, CH - 1, cy0 + y), yb = clip3(0, CH - 1, cy0 + y + 1);
      for (int x = 0; x < 8; ++x) {
        const int xa = clip3(0, CW - 1, cx0 + x), xb = clip3(0, CW - 1, cx0 + x + 1);
        const int A = p[static_cast<uint64_t>(ya) * CW + xa], B = p[static_cast<uint64_t>(ya) * CW + xb];
        const int C = p[static_cast<uint64_t>(yb) * CW + xa], D = p[static_cast<uint64_t>(yb) * CW + xb];
        m_.pred_c[c][y * 8 + x] = static_cast<uint8_t>(((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B + (8 - fx) * fy * C + fx * fy * D + 32) >> 6);
      }
    }
  }
}

uint32_t Encoder::sad16(int mbx, int mby, const uint8_t* pred) const {
  const uint32_t W = src_.w;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  uint32_t sad = 0;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) sad += static_cast<uint32_t>(iabs(int{src_.y[base + uint64_t{static_cast<uint32_t>(y)} * W + x]} - pred[y * 16 + x]));
  return sad;
}

uint32_t Encoder::sad_mv(int mbx, int mby, int mvx, int mvy) const {
  const uint32_t W = src_.w;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int64_t shift = static_cast<int64_t>(mvy >> 2) * W + (mvx >> 2);
  uint32_t sad = 0;
  for (int y = 0; y < 16; ++y) {
    const uint64_t row = base + uint64_t{static_cast<uint32_t>(y)} * W;
    for (int x = 0; x < 16; ++x) sad += static_cast<uint32_t>(iabs(int{src_.y[row + x]} - int{ref_.y[static_cast<uint64_t>(static_cast<int64_t>(row + x) + shift)]}));
  }
  return sad;
}

// An estimate of the cost of a vector difference, in SAD units.
int Encoder::mv_cost(int dx, int dy) const {
  auto bits = [](int v) {
    int n = 1, x = iabs(v) + 1;
    while (x > 1) {
      x >>= 1;
      n += 2;
    }
    return n;
  };
  return (1 + qp_ / 8) * (bits(dx) + bits(dy));
}

// ---------------------------------------------------------------------------------------------------------------
// Residual: quantisation

void Encoder::quant_luma_i16(int mbx, int mby) {
  const uint32_t W = src_.w;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int qpd = qp_ / 6, qpm = qp_ % 6;
  const int shift = 15 + qpd, round = (1 << shift) / 3;
  int dcm[16];
  bool any_ac = false;
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    int r[16], w[16];
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        r[y * 4 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + bx * 4 + x]} - m_.pred_y[(by * 4 + y) * 16 + bx * 4 + x];
    forward4x4(r, w);
    dcm[blk] = w[0];
    m_.ac[blk][0] = 0;
    for (int i = 1; i < 16; ++i) {
      const int p = kZigzag4x4[i];
      m_.ac[blk][i] = quantize(w[p], kMf[qpm][pos_class(p)], shift, round);
      any_ac = any_ac || m_.ac[blk][i] != 0;
    }
  }
  // The DC coefficients: a 4x4 Hadamard transform halved, then a quantiser one bit coarser (as the reference encoder does).
  int t[16], h[16];
  for (int i = 0; i < 4; ++i) {
    const int a = dcm[i * 4], b = dcm[i * 4 + 1], c = dcm[i * 4 + 2], d = dcm[i * 4 + 3];
    t[i * 4 + 0] = a + b + c + d;
    t[i * 4 + 1] = a + b - c - d;
    t[i * 4 + 2] = a - b - c + d;
    t[i * 4 + 3] = a - b + c - d;
  }
  for (int j = 0; j < 4; ++j) {
    const int a = t[j], b = t[4 + j], c = t[8 + j], d = t[12 + j];
    h[j] = a + b + c + d;
    h[4 + j] = a + b - c - d;
    h[8 + j] = a - b - c + d;
    h[12 + j] = a - b + c - d;
  }
  for (int i = 0; i < 16; ++i) m_.dc[i] = quantize(h[kZigzag4x4[i]], kMf[qpm][0], shift + 2, 4 * round);
  m_.cbp_luma = any_ac ? 15 : 0;
  if (!any_ac)
    for (int blk = 0; blk < 16; ++blk)
      for (int i = 1; i < 16; ++i) m_.ac[blk][i] = 0;
}

void Encoder::quant_luma_inter(int mbx, int mby) {
  const uint32_t W = src_.w;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int qpd = qp_ / 6, qpm = qp_ % 6;
  const int shift = 15 + qpd, round = (1 << shift) / 6;
  int cbp = 0;
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    int r[16], w[16];
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        r[y * 4 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + bx * 4 + x]} - m_.pred_y[(by * 4 + y) * 16 + bx * 4 + x];
    forward4x4(r, w);
    bool nz = false;
    for (int i = 0; i < 16; ++i) {
      const int p = kZigzag4x4[i];
      m_.ac[blk][i] = quantize(w[p], kMf[qpm][pos_class(p)], shift, round);
      nz = nz || m_.ac[blk][i] != 0;
    }
    if (nz) cbp |= 1 << ((by >> 1) * 2 + (bx >> 1));
  }
  m_.cbp_luma = cbp;
}

void Encoder::quant_chroma(int mbx, int mby, bool intra) {
  const uint32_t W = src_.cw();
  const int qpd = qpc_ / 6, qpm = qpc_ % 6;
  const int shift = 15 + qpd, round = (1 << shift) / (intra ? 3 : 6);
  bool any_dc = false, any_ac = false;
  for (int c = 0; c < 2; ++c) {
    const ByteView& s = c == 0 ? src_.u : src_.v;
    const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * W + static_cast<uint32_t>(mbx) * 8;
    int dcv[4];
    for (int blk = 0; blk < 4; ++blk) {
      const int bx = blk & 1, by = blk >> 1;
      int r[16], w[16];
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) r[y * 4 + x] = int{s[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + bx * 4 + x]} - m_.pred_c[c][(by * 4 + y) * 8 + bx * 4 + x];
      forward4x4(r, w);
      dcv[blk] = w[0];
      m_.cac[c][blk][0] = 0;
      for (int i = 1; i < 16; ++i) {
        const int p = kZigzag4x4[i];
        m_.cac[c][blk][i] = quantize(w[p], kMf[qpm][pos_class(p)], shift, round);
        any_ac = any_ac || m_.cac[c][blk][i] != 0;
      }
    }
    const int h[4] = {dcv[0] + dcv[1] + dcv[2] + dcv[3], dcv[0] - dcv[1] + dcv[2] - dcv[3], dcv[0] + dcv[1] - dcv[2] - dcv[3], dcv[0] - dcv[1] - dcv[2] + dcv[3]};
    for (int i = 0; i < 4; ++i) {
      m_.cdc[c][i] = quantize(h[i], kMf[qpm][0], shift + 1, 2 * round);
      any_dc = any_dc || m_.cdc[c][i] != 0;
    }
  }
  m_.cbp_chroma = any_ac ? 2 : any_dc ? 1 : 0;
  if (m_.cbp_chroma < 2)
    for (int c = 0; c < 2; ++c)
      for (int b = 0; b < 4; ++b)
        for (int i = 1; i < 16; ++i) m_.cac[c][b][i] = 0;
  if (m_.cbp_chroma == 0)
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 4; ++i) m_.cdc[c][i] = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Reconstruction (8.5): prediction plus the inverse-transformed levels, as the decoder will compute it.

void Encoder::reconstruct(int mbx, int mby) {
  const uint32_t W = rec_.w;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) rec_.y[base + uint64_t{static_cast<uint32_t>(y)} * W + x] = m_.pred_y[y * 16 + x];

  int dcl[16] = {};
  const bool i16 = m_.intra;
  if (i16) {  // 8.5.10: inverse Hadamard of the DC levels, scaled
    int c[16], t[16], f[16];
    for (int i = 0; i < 16; ++i) c[kZigzag4x4[i]] = m_.dc[i];
    for (int i = 0; i < 4; ++i) {
      const int a = c[i * 4], b = c[i * 4 + 1], cc = c[i * 4 + 2], d = c[i * 4 + 3];
      t[i * 4 + 0] = a + b + cc + d;
      t[i * 4 + 1] = a + b - cc - d;
      t[i * 4 + 2] = a - b - cc + d;
      t[i * 4 + 3] = a - b + cc - d;
    }
    for (int j = 0; j < 4; ++j) {
      const int a = t[j], b = t[4 + j], cc = t[8 + j], d = t[12 + j];
      f[j] = a + b + cc + d;
      f[4 + j] = a + b - cc - d;
      f[8 + j] = a - b - cc + d;
      f[12 + j] = a - b + cc - d;
    }
    const int ls = 16 * kNorm[qp_ % 6][0], qpd = qp_ / 6;
    for (int i = 0; i < 16; ++i) {
      const int64_t v = static_cast<int64_t>(f[i]) * ls;
      dcl[i] = static_cast<int>(qpd >= 6 ? v << (qpd - 6) : (v + (1 << (5 - qpd))) >> (6 - qpd));
    }
  }
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    const bool coded = i16 || ((m_.cbp_luma >> ((by >> 1) * 2 + (bx >> 1))) & 1);
    int d[16] = {};
    bool any = false;
    if (coded) {
      for (int i = i16 ? 1 : 0; i < 16; ++i) {
        if (m_.ac[blk][i] == 0) continue;
        const int p = kZigzag4x4[i];
        d[p] = dequant(m_.ac[blk][i], qp_, p);
        any = true;
      }
    }
    if (i16) {
      d[0] = dcl[blk];
      any = any || d[0] != 0;
    }
    if (any) inverse4x4_add(d, rec_.y, base + uint64_t{static_cast<uint32_t>(by * 4)} * W + bx * 4, W);
  }

  const uint32_t CW = rec_.cw();
  const uint64_t cbase = uint64_t{static_cast<uint32_t>(mby)} * 8 * CW + static_cast<uint32_t>(mbx) * 8;
  for (int c = 0; c < 2; ++c) {
    ByteView& p = c == 0 ? rec_.u : rec_.v;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) p[cbase + uint64_t{static_cast<uint32_t>(y)} * CW + x] = m_.pred_c[c][y * 8 + x];
    if (m_.cbp_chroma == 0) continue;
    const int a = m_.cdc[c][0], b = m_.cdc[c][1], cc = m_.cdc[c][2], dd = m_.cdc[c][3];
    const int f[4] = {a + b + cc + dd, a - b + cc - dd, a + b - cc - dd, a - b - cc + dd};
    const int ls = 16 * kNorm[qpc_ % 6][0];
    for (int blk = 0; blk < 4; ++blk) {
      int d[16] = {};
      d[0] = static_cast<int>((static_cast<int64_t>(f[blk]) * ls << (qpc_ / 6)) >> 5);
      if (m_.cbp_chroma == 2) {
        for (int i = 1; i < 16; ++i) {
          if (m_.cac[c][blk][i] == 0) continue;
          const int pos = kZigzag4x4[i];
          d[pos] = dequant(m_.cac[c][blk][i], qpc_, pos);
        }
      }
      inverse4x4_add(d, p, cbase + uint64_t{static_cast<uint32_t>((blk >> 1) * 4)} * CW + (blk & 1) * 4, CW);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Macroblock syntax

void Encoder::write_macroblock(int mbx, int mby, bool pslice, int pred_mvx, int pred_mvy) {
  BitWriter& w = bw_;
  uint8_t nz[24] = {};
  if (pslice) {
    w.ue(skip_run_);
    skip_run_ = 0;
  }
  if (m_.intra) {
    const int type = 1 + m_.i16_mode + 4 * m_.cbp_chroma + (m_.cbp_luma ? 12 : 0);
    w.ue(static_cast<uint32_t>((pslice ? 5 : 0) + type));
    w.ue(static_cast<uint32_t>(m_.chroma_mode));
    w.se(0);  // mb_qp_delta
    int dc[16];
    for (int i = 0; i < 16; ++i) dc[i] = m_.dc[i];
    write_block(w, nc_for(0, 0, 0, mbx, mby, nz), dc, 16);
  } else {
    w.ue(0);  // P_L0_16x16 (one reference picture: no ref_idx)
    w.se(m_.mvx - pred_mvx);
    w.se(m_.mvy - pred_mvy);
    const int cbp = m_.cbp_luma | (m_.cbp_chroma << 4);
    int code = 0;
    while (code < 48 && kCbpInter[code] != cbp) ++code;
    w.ue(static_cast<uint32_t>(code));
    if (cbp != 0) w.se(0);
  }
  if (m_.intra || m_.cbp_luma != 0 || m_.cbp_chroma != 0) {
    for (int b8 = 0; b8 < 4; ++b8) {
      if (!((m_.cbp_luma >> b8) & 1)) continue;
      for (int s = 0; s < 4; ++s) {
        const int bx = (b8 & 1) * 2 + (s & 1), by = (b8 >> 1) * 2 + (s >> 1);
        const int blk = by * 4 + bx;
        const int nc = nc_for(bx, by, 0, mbx, mby, nz);
        if (m_.intra) {
          nz[blk] = static_cast<uint8_t>(write_block(w, nc, &m_.ac[blk][1], 15));
        } else {
          nz[blk] = static_cast<uint8_t>(write_block(w, nc, m_.ac[blk], 16));
        }
      }
    }
    if (m_.cbp_chroma > 0) {
      for (int c = 0; c < 2; ++c) write_block(w, -1, m_.cdc[c], 4);
      if (m_.cbp_chroma == 2) {
        for (int c = 0; c < 2; ++c) {
          for (int s = 0; s < 4; ++s) {
            const int nc = nc_for(s & 1, s >> 1, 1 + c, mbx, mby, nz);
            nz[16 + c * 4 + s] = static_cast<uint8_t>(write_block(w, nc, &m_.cac[c][s][1], 15));
          }
        }
      }
    }
  }
  for (int i = 0; i < 24; ++i) m_.nz[i] = nz[i];
}

// ---------------------------------------------------------------------------------------------------------------
// Pictures

void Encoder::write_slice_header(bool idr, bool pslice) {
  BitWriter& w = bw_;
  w.ue(0);                  // first_mb_in_slice
  w.ue(pslice ? 5 : 7);     // slice_type: P / I, every slice of the picture alike
  w.ue(0);                  // pic_parameter_set_id
  w.put(frame_num_, 8);     // frame_num
  if (idr) w.ue(idr_id_);   // idr_pic_id
  if (pslice) {
    w.bit(0);  // num_ref_idx_active_override_flag
    w.bit(0);  // ref_pic_list_modification_flag_l0
  }
  if (idr) {
    w.bit(0);  // no_output_of_prior_pics_flag
    w.bit(0);  // long_term_reference_flag
  } else {
    w.bit(0);  // adaptive_ref_pic_marking_mode_flag
  }
  w.se(0);  // slice_qp_delta
  w.ue(1);  // disable_deblocking_filter_idc
}

bool Encoder::code_picture(bool idr, bool pslice) {
  bw_.reset();
  write_slice_header(idr, pslice);
  skip_run_ = 0;
  for (MbInfo& m : mbs_) m = MbInfo();
  for (int mby = 0; mby < static_cast<int>(mbh_); ++mby) {
    for (int mbx = 0; mbx < static_cast<int>(mbw_); ++mbx) {
      MbInfo& info = mbs_[static_cast<uint64_t>(mby) * mbw_ + mbx];
      Mb& m = m_;
      // Intra candidate: the Intra16x16 mode with the smallest SAD.
      uint8_t ipred[256];
      uint32_t intra_sad = 0xFFFFFFFFu;
      int best_mode = 2;
      for (int mode = 0; mode < 3; ++mode) {
        if (!pred_i16(mode, mbx, mby, ipred)) continue;
        const uint32_t s = sad16(mbx, mby, ipred) + (mode == 2 ? 0 : 16);
        if (s < intra_sad) {
          intra_sad = s;
          best_mode = mode;
        }
      }
      bool use_inter = false;
      int mvx = 0, mvy = 0, pmx = 0, pmy = 0, smx = 0, smy = 0;
      if (pslice) {
        predict_mv(mbx, mby, &pmx, &pmy);
        skip_mv(mbx, mby, &smx, &smy);
        const int W = static_cast<int>(ref_.w), H = static_cast<int>(ref_.h);
        auto inside = [&](int vx, int vy) {
          const int x = mbx * 16 + (vx >> 2), y = mby * 16 + (vy >> 2);
          return (vx & 3) == 0 && (vy & 3) == 0 && x >= 0 && y >= 0 && x + 16 <= W && y + 16 <= H && iabs(vx) <= 64 && iabs(vy) <= 64;
        };
        int best_cost = 0x7FFFFFFF, bx = 0, by = 0;
        auto try_mv = [&](int vx, int vy, int bonus) {
          if (!inside(vx, vy)) return;
          const int cost = static_cast<int>(sad_mv(mbx, mby, vx, vy)) + mv_cost(vx - pmx, vy - pmy) - bonus;
          if (cost < best_cost) {
            best_cost = cost;
            bx = vx;
            by = vy;
          }
        };
        try_mv(0, 0, 0);
        try_mv(smx, smy, 2 * (1 + qp_ / 8) + 8);  // a skipped macroblock costs next to nothing
        try_mv(pmx, pmy, 0);
        if (const MbInfo* l = mb_at(mbx - 1, mby)) if (l->inter) try_mv(l->mvx, l->mvy, 0);
        if (const MbInfo* t = mb_at(mbx, mby - 1)) if (t->inter) try_mv(t->mvx, t->mvy, 0);
        for (int iter = 0; iter < 24; ++iter) {  // large diamond until the centre wins, then the small one
          const int cx = bx, cy = by;
          static const int kLarge[8][2] = {{0, -8}, {0, 8}, {-8, 0}, {8, 0}, {-4, -4}, {4, -4}, {-4, 4}, {4, 4}};
          for (const auto& d : kLarge) try_mv(cx + d[0], cy + d[1], 0);
          if (bx == cx && by == cy) break;
        }
        {
          const int cx = bx, cy = by;
          static const int kSmall[4][2] = {{0, -4}, {0, 4}, {-4, 0}, {4, 0}};
          for (const auto& d : kSmall) try_mv(cx + d[0], cy + d[1], 0);
        }
        // Intra wins only when the best match is clearly worse.
        use_inter = best_cost < 0x7FFFFFFF && static_cast<uint32_t>(best_cost < 0 ? 0 : best_cost) <= intra_sad + 64;
        if (use_inter) {
          mvx = bx;
          mvy = by;
        }
      }

      m.intra = !use_inter;
      bool skipped = false;
      if (m.intra) {
        m.i16_mode = best_mode;
        pred_i16(best_mode, mbx, mby, m.pred_y);
        // Chroma mode with the smallest SAD over both planes.
        uint32_t best = 0xFFFFFFFFu;
        m.chroma_mode = 0;
        for (int mode = 0; mode < 3; ++mode) {
          uint8_t cp[2][64];
          if (!pred_chroma(mode, mbx, mby, 0, cp[0]) || !pred_chroma(mode, mbx, mby, 1, cp[1])) continue;
          uint32_t s = 0;
          for (int c = 0; c < 2; ++c) {
            const ByteView& sp = c == 0 ? src_.u : src_.v;
            const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * src_.cw() + static_cast<uint32_t>(mbx) * 8;
            for (int y = 0; y < 8; ++y)
              for (int x = 0; x < 8; ++x) s += static_cast<uint32_t>(iabs(int{sp[base + uint64_t{static_cast<uint32_t>(y)} * src_.cw() + x]} - cp[c][y * 8 + x]));
          }
          if (s < best) {
            best = s;
            m.chroma_mode = mode;
          }
        }
        pred_chroma(m.chroma_mode, mbx, mby, 0, m.pred_c[0]);
        pred_chroma(m.chroma_mode, mbx, mby, 1, m.pred_c[1]);
        quant_luma_i16(mbx, mby);
        quant_chroma(mbx, mby, true);
      } else {
        m.mvx = mvx;
        m.mvy = mvy;
        pred_inter(mbx, mby, mvx, mvy);
        quant_luma_inter(mbx, mby);
        quant_chroma(mbx, mby, false);
        skipped = mvx == smx && mvy == smy && m.cbp_luma == 0 && m.cbp_chroma == 0;
      }
      reconstruct(mbx, mby);
      if (skipped) {
        ++skip_run_;
        for (int i = 0; i < 24; ++i) m.nz[i] = 0;
      } else {
        write_macroblock(mbx, mby, pslice, pmx, pmy);
      }
      info.inter = m.intra ? 0 : 1;
      info.mvx = static_cast<int16_t>(m.intra ? 0 : m.mvx);
      info.mvy = static_cast<int16_t>(m.intra ? 0 : m.mvy);
      for (int i = 0; i < 24; ++i) info.nz[i] = m.nz[i];
    }
  }
  if (pslice && skip_run_ > 0) bw_.ue(skip_run_);
  bw_.trailing();
  return bw_.ok();
}

bool Encoder::encode(const VideoFrame& in, bool force_key, ByteBuffer* out, bool* keyframe) {
  if (!configured_ || in.format.pixel_format != PixelFormat::I420 || in.format.width != width_ || in.format.height != height_) return false;
  for (uint32_t p = 0; p < 3; ++p)
    if (!in.plane[p] || in.stride[p] < in.format.plane_width(p)) return false;
  // Copy the picture into the macroblock-aligned source, repeating the last row and column into the padding.
  for (uint32_t p = 0; p < 3; ++p) {
    const uint32_t pw = in.format.plane_width(p), ph = in.format.plane_height(p);
    ByteView dst = p == 0 ? src_.y : p == 1 ? src_.u : src_.v;
    const uint32_t dw = p == 0 ? src_.w : src_.cw(), dh = p == 0 ? src_.h : src_.ch();
    for (uint32_t y = 0; y < dh; ++y) {
      const uint8_t* row = in.plane[p] + uint64_t{y < ph ? y : ph - 1} * in.stride[p];
      const ConstByteView srow(row, pw);
      for (uint32_t x = 0; x < dw; ++x) dst[uint64_t{y} * dw + x] = srow[x < pw ? x : pw - 1];
    }
  }
  const bool idr = force_key || !have_ref_ || since_key_ + 1 >= key_interval_;
  if (idr) {
    frame_num_ = 0;
    since_key_ = 0;
  } else {
    ++since_key_;
  }
  const uint64_t mark = out->size();
  bool ok = code_picture(idr, !idr);
  if (!ok) {
    out->truncate(mark);
    return false;
  }
  static const uint8_t kStart[4] = {0, 0, 0, 1};
  if (idr) {
    ok = ok && out->append(ConstByteView(kStart, 4)) && out->append(sps_.view()) && out->append(ConstByteView(kStart, 4)) && out->append(pps_.view());
  }
  scratch_.clear();
  ok = ok && build_nal(&scratch_, idr ? 3 : 2, idr ? 5 : 1, bw_.bytes()) && out->append(ConstByteView(kStart, 4)) && out->append(scratch_.view());
  if (!ok) {
    out->truncate(mark);
    return false;
  }
  // The picture just coded is the reference for the next one.
  const Pic t = ref_;
  ref_ = rec_;
  rec_ = t;
  have_ref_ = true;
  if (idr) idr_id_ ^= 1;
  frame_num_ = (frame_num_ + 1) & 255;
  *keyframe = idr;
  return true;
}

}  // namespace h264

bool H264Encoder::configure(uint32_t width, uint32_t height, uint32_t fps_numerator, uint32_t fps_denominator, uint32_t quality, uint32_t keyframe_interval) {
  core_ = UniquePointer<h264::Encoder>::make(allocator_, allocator_);
  return core_ && core_->configure(width, height, fps_numerator, fps_denominator, quality, keyframe_interval);
}

H264Encoder::~H264Encoder() {}

bool H264Encoder::encode(const VideoFrame& i420, bool force_keyframe, ByteBuffer* out, bool* keyframe) {
  return core_ && core_->encode(i420, force_keyframe, out, keyframe);
}

bool H264Encoder::reconstruction(VideoFrame* frame) const { return core_ && core_->reconstruction(frame); }

bool H264Encoder::avcc(ByteBuffer* out) const { return core_ && core_->avcc(out); }

}  // namespace hardware
