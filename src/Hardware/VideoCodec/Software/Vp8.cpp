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

// VP8 decoding: a port of the decoder side of libvpx (BSD-3-Clause, Copyright (c) The WebM Project authors) whose large tables are
// generated into Vp8Tables.inc. The whole frame is reconstructed first and loop filtered afterwards (intra prediction reads unfiltered
// pixels); references are read with their coordinates clamped to the macroblock-aligned picture, which is what libvpx's replicated
// borders amount to.
#include "Hardware/VideoCodec/Software/Vp8.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {
namespace vp8 {

namespace {

#include "Hardware/VideoCodec/Software/Vp8Tables.inc"

enum Mode : uint8_t { DC_PRED, V_PRED, H_PRED, TM_PRED, B_PRED, NEARESTMV, NEARMV, ZEROMV, NEWMV, SPLITMV };
enum BMode : uint8_t { B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_LD_PRED, B_RD_PRED, B_VR_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED };
enum Ref : uint8_t { INTRA_FRAME, LAST_FRAME, GOLDEN_FRAME, ALTREF_FRAME };

constexpr int8_t kBmodeTree[18] = {-B_DC_PRED, 2, -B_TM_PRED, 4, -B_VE_PRED, 6, 8, 12, -B_HE_PRED, 10, -B_RD_PRED, -B_VR_PRED, -B_LD_PRED, 14, -B_VL_PRED, 16, -B_HD_PRED, -B_HU_PRED};
constexpr int8_t kYmodeTree[8] = {-DC_PRED, 2, 4, 6, -V_PRED, -H_PRED, -TM_PRED, -B_PRED};
constexpr int8_t kKfYmodeTree[8] = {-B_PRED, 2, 4, 6, -DC_PRED, -V_PRED, -H_PRED, -TM_PRED};
constexpr int8_t kUvModeTree[6] = {-DC_PRED, 2, -V_PRED, 4, -H_PRED, -TM_PRED};
constexpr int8_t kSmallMvTree[14] = {2, 8, 4, 6, -0, -1, -2, -3, 10, 12, -4, -5, -6, -7};

constexpr uint8_t kYmodeProb[4] = {112, 86, 140, 37};
constexpr uint8_t kKfYmodeProb[4] = {145, 156, 163, 128};
constexpr uint8_t kUvModeProb[3] = {162, 101, 204};
constexpr uint8_t kKfUvModeProb[3] = {142, 114, 183};
constexpr uint8_t kBmodeProb[9] = {120, 90, 79, 133, 87, 85, 80, 111, 151};

constexpr int kModeContexts[6][4] = {{7, 1, 1, 143}, {14, 18, 14, 107}, {135, 64, 57, 68}, {60, 56, 128, 65}, {159, 134, 128, 34}, {234, 188, 128, 28}};
constexpr uint8_t kSubMvRefProb[8][3] = {{147, 136, 18}, {223, 1, 34}, {106, 145, 1}, {208, 1, 1}, {179, 121, 1}, {223, 1, 34}, {179, 121, 1}, {208, 1, 1}};

// Motion vector probabilities: is_short, sign, 7 for the short tree, 10 for the long bits.
constexpr int kMvpIsShort = 0, kMvpSign = 1, kMvpShort = 2, kMvpBits = 9, kMvpCount = 19, kMvLongWidth = 10;
constexpr uint8_t kMvDefault[2][kMvpCount] = {
    {162, 128, 225, 146, 172, 147, 214, 39, 156, 128, 129, 132, 75, 145, 178, 206, 239, 254, 254},
    {164, 128, 204, 170, 119, 235, 140, 230, 228, 128, 130, 130, 74, 148, 180, 203, 236, 254, 254}};
constexpr uint8_t kMvUpdate[2][kMvpCount] = {
    {237, 246, 253, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 250, 250, 252, 254, 254},
    {231, 243, 245, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 251, 251, 254, 254, 254}};

constexpr uint8_t kMbsplitFillCount[4] = {8, 8, 4, 1};
constexpr uint8_t kMbsplitFillOffset[4][16] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                                               {0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15},
                                               {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15},
                                               {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};
constexpr uint8_t kMbsplitOffset[4][16] = {{0, 8}, {0, 2}, {0, 2, 8, 10}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};

constexpr uint8_t kBands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
constexpr uint8_t kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr uint8_t kCat3[] = {173, 148, 140, 0};
constexpr uint8_t kCat4[] = {176, 155, 140, 135, 0};
constexpr uint8_t kCat5[] = {180, 157, 141, 134, 130, 0};
constexpr uint8_t kCat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};

constexpr int16_t kSixtap[8][6] = {{0, 0, 128, 0, 0, 0},   {0, -6, 123, 12, -1, 0}, {2, -11, 108, 36, -8, 1}, {0, -9, 93, 50, -6, 0},
                                   {3, -16, 77, 77, -16, 3}, {0, -6, 50, 93, -9, 0},  {1, -8, 36, 108, -11, 2}, {0, -1, 12, 123, -6, 0}};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline uint8_t clamp255(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
inline int iabs(int v) { return v < 0 ? -v : v; }

// The pointer to plane[pos] for code that then touches plane[pos + lo] .. plane[pos + hi] only. Both ends are checked here, once, so the
// pixel loops that follow can use plain pointer arithmetic.
inline uint8_t* at(ByteView plane, int64_t pos, int64_t lo, int64_t hi) {
  plane.subview(static_cast<uint64_t>(pos + lo), static_cast<uint64_t>(hi - lo + 1));
  return plane.data() + pos;
}

// --- the boolean entropy decoder ---------------------------------------------------------------------------------------------------------

class BoolDecoder {
 public:
  // Reading past the end gives zero bits, as libvpx does.
  void start(ConstByteView data) {
    data_ = data;
    pos_ = 0;
    value_ = 0;
    count_ = -8;
    range_ = 255;
    fill();
  }
  int read(int prob) {
    const uint32_t split = 1 + (((range_ - 1) * static_cast<uint32_t>(prob)) >> 8);
    if (count_ < 0) fill();
    const uint64_t big = uint64_t{split} << 56;
    uint32_t range = split;
    int bit = 0;
    if (value_ >= big) {
      range = range_ - split;
      value_ -= big;
      bit = 1;
    }
    const int shift = __builtin_clz(range) - 24;
    range_ = range << shift;
    value_ <<= shift;
    count_ -= shift;
    return bit;
  }
  int bit() { return read(128); }
  int literal(int bits) {
    int v = 0;
    while (bits-- > 0) v = (v << 1) | bit();
    return v;
  }
  // A tree of the form libvpx writes: positive entries are the next node, others the negated leaf.
  int tree(ArrayView<const int8_t> t, ArrayView<const uint8_t> probs) {
    int i = 0;
    while ((i = t[static_cast<uint64_t>(i + read(probs[static_cast<uint64_t>(i >> 1)]))]) > 0) {
    }
    return -i;
  }

 private:
  void fill() {
    int shift = 48 - count_;
    while (shift >= 0) {
      if (pos_ >= data_.size()) {
        count_ += 0x40000000;
        break;
      }
      value_ |= uint64_t{data_[pos_++]} << shift;
      count_ += 8;
      shift -= 8;
    }
  }

  ConstByteView data_;
  uint64_t pos_ = 0, value_ = 0;
  int count_ = -8;
  uint32_t range_ = 255;
};

// --- state -------------------------------------------------------------------------------------------------------------------------------

struct Mv {
  int16_t row = 0, col = 0;
  bool zero() const { return row == 0 && col == 0; }
  bool operator==(const Mv& o) const { return row == o.row && col == o.col; }
  bool operator!=(const Mv& o) const { return !(*this == o); }
};

// MODE_INFO of libvpx: what the first partition says about a macroblock.
struct MbInfo {
  Mv mv;
  Mv bmv[16];
  uint8_t bmode[16] = {};
  uint8_t mode = DC_PRED, uv_mode = DC_PRED, ref_frame = INTRA_FRAME, segment_id = 0, skip = 0, is_4x4 = 0, partitioning = 0, need_clamp = 0;
};

// Probabilities that frames can change and, when refresh_entropy_probs is 0, give back.
struct Probs {
  uint8_t coef[4][8][3][11];
  uint8_t ymode[4];
  uint8_t uvmode[3];
  uint8_t mvc[2][kMvpCount];
};

// A picture with the macroblock-aligned size of the stream.
struct Frame {
  ArrayView<uint8_t> mem;
  ByteView plane[3];
  uint32_t stride[3] = {0, 0, 0};
  uint32_t pw[3] = {0, 0, 0}, ph[3] = {0, 0, 0};
};

struct Dequant {
  int y1dc, y1ac, y2dc, y2ac, uvdc, uvac;
};

}  // namespace

class Core {
 public:
  explicit Core(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Core() { release(); }
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  bool decode(ConstByteView packet);
  void reset() { have_key_ = false; }
  bool shown() const { return shown_; }
  bool key_frame() const { return key_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  // The picture decode() made (valid when shown()): the display-sized part of the planes.
  const Frame& shown_frame() const { return frames_[cur_]; }

 private:
  bool alloc(uint32_t w, uint32_t h);
  void release();
  MbInfo& mi(int r, int c) { return mi_[static_cast<uint64_t>((r + 1) * mi_stride_ + c + 1)]; }

  void init_key_frame();
  bool parse_header(ConstByteView packet, uint64_t pos, uint32_t first_size, bool key, int version);
  void decode_modes();
  void decode_kf_modes(MbInfo& m, int r, int c);
  void decode_inter_modes(MbInfo& m, int r, int c);
  void decode_split_mv(MbInfo& m, int r, int c, Mv best, int l, int rt, int t, int b);
  int read_mv_component(const uint8_t (&p)[kMvpCount]);
  int read_bmode_kf(const MbInfo& m, int r, int c, int b);
  void reconstruct();
  void decode_mb(int r, int c, MbInfo& m, BoolDecoder& tokens);
  int read_tokens(int c, const MbInfo& m, BoolDecoder& bd);
  int get_coeffs(BoolDecoder& bd, int type, int ctx, int n, int16_t* out);
  void reset_token_context(int c, const MbInfo& m);
  void predict_intra(Frame& f, int r, int c, MbInfo& m, const Dequant& dq);
  void predict_inter(Frame& f, int r, int c, MbInfo& m);
  void inter_block(const Frame& ref, int plane, int bx, int by, int w, int h, int mvr, int mvc, ByteView dst, int64_t dpos, int dstride);
  void add_residual(Frame& f, int r, int c, const MbInfo& m, const Dequant& dq);
  void loop_filter(Frame& f);
  void filter_mb(Frame& f, int r, int c, const MbInfo& m);
  void update_references();

  allocators::IAllocator& allocator_;
  Frame frames_[4];
  int cur_ = 0, lst_ = 0, gld_ = 0, alt_ = 0;
  ArrayView<MbInfo> mi_;
  ArrayView<uint8_t> above_ctx_;   // 9 per macroblock column: y[4] u[2] v[2] y2
  uint8_t left_ctx_[9] = {};
  uint32_t width_ = 0, height_ = 0, mb_cols_ = 0, mb_rows_ = 0, mi_stride_ = 0;
  bool allocated_ = false, have_key_ = false, shown_ = false, key_ = false;

  // The frame being decoded.
  BoolDecoder bc_;
  BoolDecoder parts_[8];
  uint32_t num_parts_ = 1;
  int version_ = 0;
  bool bilinear_ = false, full_pixel_ = false;
  bool refresh_entropy_ = true, refresh_last_ = true, refresh_golden_ = false, refresh_alt_ = false;
  int copy_to_gf_ = 0, copy_to_arf_ = 0;
  bool mb_no_coeff_skip_ = false;
  int prob_skip_false_ = 0, prob_intra_ = 0, prob_last_ = 0, prob_gf_ = 0;
  int filter_type_ = 0, filter_level_ = 0, sharpness_ = 0;
  int base_q_ = 0, y1dc_dq_ = 0, y2dc_dq_ = 0, y2ac_dq_ = 0, uvdc_dq_ = 0, uvac_dq_ = 0;
  Dequant dq_[4] = {};

  // Carried from frame to frame.
  Probs fc_ = {}, lfc_ = {};
  bool seg_enabled_ = false, seg_update_map_ = false, seg_update_data_ = false, seg_abs_ = false;
  int seg_data_[2][4] = {};
  uint8_t seg_tree_probs_[3] = {255, 255, 255};
  bool lf_delta_enabled_ = false;
  int ref_lf_delta_[4] = {}, mode_lf_delta_[4] = {};
  int sign_bias_[4] = {};

  int16_t coef_[25 * 16] = {};
  uint8_t eobs_[25] = {};
};

namespace {

// --- inverse transforms --------------------------------------------------------------------------------------------------------------------

constexpr int kCospi8Sqrt2Minus1 = 20091;
constexpr int kSinpi8Sqrt2 = 35468;

// Adds the inverse DCT of `in` (already dequantized) to the 4x4 block at dst.
void idct_add(const int16_t* in, uint8_t* dst, int stride) {
  int16_t out[16];
  for (int i = 0; i < 4; ++i) {
    const int a1 = in[i] + in[8 + i], b1 = in[i] - in[8 + i];
    int t1 = (in[4 + i] * kSinpi8Sqrt2) >> 16;
    int t2 = in[12 + i] + ((in[12 + i] * kCospi8Sqrt2Minus1) >> 16);
    const int c1 = t1 - t2;
    t1 = in[4 + i] + ((in[4 + i] * kCospi8Sqrt2Minus1) >> 16);
    t2 = (in[12 + i] * kSinpi8Sqrt2) >> 16;
    const int d1 = t1 + t2;
    out[i] = static_cast<int16_t>(a1 + d1);
    out[12 + i] = static_cast<int16_t>(a1 - d1);
    out[4 + i] = static_cast<int16_t>(b1 + c1);
    out[8 + i] = static_cast<int16_t>(b1 - c1);
  }
  for (int i = 0; i < 4; ++i) {
    const int16_t* ip = out + i * 4;
    const int a1 = ip[0] + ip[2], b1 = ip[0] - ip[2];
    int t1 = (ip[1] * kSinpi8Sqrt2) >> 16;
    int t2 = ip[3] + ((ip[3] * kCospi8Sqrt2Minus1) >> 16);
    const int c1 = t1 - t2;
    t1 = ip[1] + ((ip[1] * kCospi8Sqrt2Minus1) >> 16);
    t2 = (ip[3] * kSinpi8Sqrt2) >> 16;
    const int d1 = t1 + t2;
    const int16_t r0 = static_cast<int16_t>((a1 + d1 + 4) >> 3), r3 = static_cast<int16_t>((a1 - d1 + 4) >> 3);
    const int16_t r1 = static_cast<int16_t>((b1 + c1 + 4) >> 3), r2 = static_cast<int16_t>((b1 - c1 + 4) >> 3);
    uint8_t* d = dst + i * stride;
    d[0] = clamp255(d[0] + r0);
    d[1] = clamp255(d[1] + r1);
    d[2] = clamp255(d[2] + r2);
    d[3] = clamp255(d[3] + r3);
  }
}

void dc_only_idct_add(int16_t dc, uint8_t* dst, int stride) {
  const int a1 = (dc + 4) >> 3;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) dst[r * stride + c] = clamp255(dst[r * stride + c] + a1);
}

// The second-order transform of the luma DC coefficients; the results go to the first coefficient of each of the 16 blocks.
void inv_walsh(const int16_t* in, int16_t* mb) {
  int16_t out[16];
  for (int i = 0; i < 4; ++i) {
    const int a1 = in[i] + in[12 + i], b1 = in[4 + i] + in[8 + i], c1 = in[4 + i] - in[8 + i], d1 = in[i] - in[12 + i];
    out[i] = static_cast<int16_t>(a1 + b1);
    out[4 + i] = static_cast<int16_t>(c1 + d1);
    out[8 + i] = static_cast<int16_t>(a1 - b1);
    out[12 + i] = static_cast<int16_t>(d1 - c1);
  }
  for (int i = 0; i < 4; ++i) {
    const int16_t* ip = out + i * 4;
    const int a1 = ip[0] + ip[3], b1 = ip[1] + ip[2], c1 = ip[1] - ip[2], d1 = ip[0] - ip[3];
    const int a2 = a1 + b1, b2 = c1 + d1, c2 = a1 - b1, d2 = d1 - c1;
    mb[(i * 4 + 0) * 16] = static_cast<int16_t>((a2 + 3) >> 3);
    mb[(i * 4 + 1) * 16] = static_cast<int16_t>((b2 + 3) >> 3);
    mb[(i * 4 + 2) * 16] = static_cast<int16_t>((c2 + 3) >> 3);
    mb[(i * 4 + 3) * 16] = static_cast<int16_t>((d2 + 3) >> 3);
  }
}

// --- intra prediction ----------------------------------------------------------------------------------------------------------------------

inline int avg2(int a, int b) { return (a + b + 1) >> 1; }
inline int avg3(int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; }

// A: A[0] is the pixel above and left, A[1..8] the row above (four of them, then four above-right); L the four to the left.
void predict_4x4(int mode, const uint8_t* A, const uint8_t* L, uint8_t* out) {
  const int X = A[0], a = A[1], b = A[2], c = A[3], d = A[4], e = A[5], f = A[6], g = A[7], h = A[8];
  const int I = L[0], J = L[1], K = L[2], Lc = L[3];
#define DST(x, y) out[(y) * 4 + (x)]
  switch (mode) {
    case B_DC_PRED: {
      const int v = (a + b + c + d + I + J + K + Lc + 4) >> 3;
      for (int i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(v);
      break;
    }
    case B_TM_PRED:
      for (int r = 0; r < 4; ++r)
        for (int cc = 0; cc < 4; ++cc) DST(cc, r) = clamp255(L[r] + A[1 + cc] - X);
      break;
    case B_VE_PRED:
      for (int cc = 0; cc < 4; ++cc) {
        const uint8_t v = static_cast<uint8_t>(avg3(A[cc], A[cc + 1], A[cc + 2]));
        for (int r = 0; r < 4; ++r) DST(cc, r) = v;
      }
      break;
    case B_HE_PRED: {
      const int v[4] = {avg3(X, I, J), avg3(I, J, K), avg3(J, K, Lc), avg3(K, Lc, Lc)};
      for (int r = 0; r < 4; ++r)
        for (int cc = 0; cc < 4; ++cc) DST(cc, r) = static_cast<uint8_t>(v[r]);
      break;
    }
    case B_LD_PRED:
      DST(0, 0) = static_cast<uint8_t>(avg3(a, b, c));
      DST(1, 0) = DST(0, 1) = static_cast<uint8_t>(avg3(b, c, d));
      DST(2, 0) = DST(1, 1) = DST(0, 2) = static_cast<uint8_t>(avg3(c, d, e));
      DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = static_cast<uint8_t>(avg3(d, e, f));
      DST(3, 1) = DST(2, 2) = DST(1, 3) = static_cast<uint8_t>(avg3(e, f, g));
      DST(3, 2) = DST(2, 3) = static_cast<uint8_t>(avg3(f, g, h));
      DST(3, 3) = static_cast<uint8_t>(avg3(g, h, h));
      break;
    case B_RD_PRED:
      DST(0, 3) = static_cast<uint8_t>(avg3(J, K, Lc));
      DST(1, 3) = DST(0, 2) = static_cast<uint8_t>(avg3(I, J, K));
      DST(2, 3) = DST(1, 2) = DST(0, 1) = static_cast<uint8_t>(avg3(X, I, J));
      DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = static_cast<uint8_t>(avg3(a, X, I));
      DST(3, 2) = DST(2, 1) = DST(1, 0) = static_cast<uint8_t>(avg3(b, a, X));
      DST(3, 1) = DST(2, 0) = static_cast<uint8_t>(avg3(c, b, a));
      DST(3, 0) = static_cast<uint8_t>(avg3(d, c, b));
      break;
    case B_VR_PRED:
      DST(0, 0) = DST(1, 2) = static_cast<uint8_t>(avg2(X, a));
      DST(1, 0) = DST(2, 2) = static_cast<uint8_t>(avg2(a, b));
      DST(2, 0) = DST(3, 2) = static_cast<uint8_t>(avg2(b, c));
      DST(3, 0) = static_cast<uint8_t>(avg2(c, d));
      DST(0, 3) = static_cast<uint8_t>(avg3(K, J, I));
      DST(0, 2) = static_cast<uint8_t>(avg3(J, I, X));
      DST(0, 1) = DST(1, 3) = static_cast<uint8_t>(avg3(I, X, a));
      DST(1, 1) = DST(2, 3) = static_cast<uint8_t>(avg3(X, a, b));
      DST(2, 1) = DST(3, 3) = static_cast<uint8_t>(avg3(a, b, c));
      DST(3, 1) = static_cast<uint8_t>(avg3(b, c, d));
      break;
    case B_VL_PRED:
      DST(0, 0) = static_cast<uint8_t>(avg2(a, b));
      DST(1, 0) = DST(0, 2) = static_cast<uint8_t>(avg2(b, c));
      DST(2, 0) = DST(1, 2) = static_cast<uint8_t>(avg2(c, d));
      DST(3, 0) = DST(2, 2) = static_cast<uint8_t>(avg2(d, e));
      DST(3, 2) = static_cast<uint8_t>(avg3(e, f, g));
      DST(0, 1) = static_cast<uint8_t>(avg3(a, b, c));
      DST(1, 1) = DST(0, 3) = static_cast<uint8_t>(avg3(b, c, d));
      DST(2, 1) = DST(1, 3) = static_cast<uint8_t>(avg3(c, d, e));
      DST(3, 1) = DST(2, 3) = static_cast<uint8_t>(avg3(d, e, f));
      DST(3, 3) = static_cast<uint8_t>(avg3(f, g, h));
      break;
    case B_HD_PRED:
      DST(0, 0) = DST(2, 1) = static_cast<uint8_t>(avg2(I, X));
      DST(0, 1) = DST(2, 2) = static_cast<uint8_t>(avg2(J, I));
      DST(0, 2) = DST(2, 3) = static_cast<uint8_t>(avg2(K, J));
      DST(0, 3) = static_cast<uint8_t>(avg2(Lc, K));
      DST(3, 0) = static_cast<uint8_t>(avg3(a, b, c));
      DST(2, 0) = static_cast<uint8_t>(avg3(X, a, b));
      DST(1, 0) = DST(3, 1) = static_cast<uint8_t>(avg3(I, X, a));
      DST(1, 1) = DST(3, 2) = static_cast<uint8_t>(avg3(J, I, X));
      DST(1, 2) = DST(3, 3) = static_cast<uint8_t>(avg3(K, J, I));
      DST(1, 3) = static_cast<uint8_t>(avg3(Lc, K, J));
      break;
    default:  // B_HU_PRED
      DST(0, 0) = static_cast<uint8_t>(avg2(I, J));
      DST(2, 0) = DST(0, 1) = static_cast<uint8_t>(avg2(J, K));
      DST(2, 1) = DST(0, 2) = static_cast<uint8_t>(avg2(K, Lc));
      DST(1, 0) = static_cast<uint8_t>(avg3(I, J, K));
      DST(3, 0) = DST(1, 1) = static_cast<uint8_t>(avg3(J, K, Lc));
      DST(3, 1) = DST(1, 2) = static_cast<uint8_t>(avg3(K, Lc, Lc));
      DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = static_cast<uint8_t>(Lc);
      break;
  }
#undef DST
}

// Whole-block prediction (16x16 luma or 8x8 chroma) from `above[-1..n-1]` and `left[0..n-1]`.
void predict_block(int mode, int n, bool have_up, bool have_left, const uint8_t* above, const uint8_t* left, uint8_t* dst, int stride) {
  switch (mode) {
    case DC_PRED: {
      int sum = 0, count = 0;
      if (have_up) {
        for (int i = 0; i < n; ++i) sum += above[i];
        count += n;
      }
      if (have_left) {
        for (int i = 0; i < n; ++i) sum += left[i];
        count += n;
      }
      const int shift = n == 16 ? 3 : 2;   // log2(n) - 1
      int v = 128;
      if (count == 2 * n) v = (sum + n) >> (shift + 2);
      else if (count == n) v = (sum + n / 2) >> (shift + 1);
      for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) dst[r * stride + c] = static_cast<uint8_t>(v);
      break;
    }
    case V_PRED:
      for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) dst[r * stride + c] = above[c];
      break;
    case H_PRED:
      for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) dst[r * stride + c] = left[r];
      break;
    default:  // TM_PRED
      for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) dst[r * stride + c] = clamp255(left[r] + above[c] - above[-1]);
      break;
  }
}

// --- loop filter kernels -------------------------------------------------------------------------------------------------------------------

inline int sclamp(int t) { return t < -128 ? -128 : t > 127 ? 127 : t; }

// `s` points at q0; `step` is the distance between pixels across the edge, `along` between the pixels along it.
inline bool filter_mask(int limit, int blimit, const uint8_t* s, int step) {
  const int p3 = s[-4 * step], p2 = s[-3 * step], p1 = s[-2 * step], p0 = s[-step], q0 = s[0], q1 = s[step], q2 = s[2 * step], q3 = s[3 * step];
  return !(iabs(p3 - p2) > limit || iabs(p2 - p1) > limit || iabs(p1 - p0) > limit || iabs(q1 - q0) > limit || iabs(q2 - q1) > limit ||
           iabs(q3 - q2) > limit || iabs(p0 - q0) * 2 + iabs(p1 - q1) / 2 > blimit);
}
inline bool high_edge_variance(int thresh, const uint8_t* s, int step) { return iabs(s[-2 * step] - s[-step]) > thresh || iabs(s[step] - s[0]) > thresh; }

inline void normal_filter(bool hev, uint8_t* s, int step) {
  const int ps1 = static_cast<int8_t>(s[-2 * step] ^ 0x80), ps0 = static_cast<int8_t>(s[-step] ^ 0x80), qs0 = static_cast<int8_t>(s[0] ^ 0x80),
            qs1 = static_cast<int8_t>(s[step] ^ 0x80);
  int fv = hev ? sclamp(ps1 - qs1) : 0;
  fv = sclamp(fv + 3 * (qs0 - ps0));
  const int f1 = sclamp(fv + 4) >> 3, f2 = sclamp(fv + 3) >> 3;
  s[0] = static_cast<uint8_t>(sclamp(qs0 - f1) ^ 0x80);
  s[-step] = static_cast<uint8_t>(sclamp(ps0 + f2) ^ 0x80);
  const int outer = hev ? 0 : (f1 + 1) >> 1;
  s[step] = static_cast<uint8_t>(sclamp(qs1 - outer) ^ 0x80);
  s[-2 * step] = static_cast<uint8_t>(sclamp(ps1 + outer) ^ 0x80);
}

inline void mb_filter(bool hev, uint8_t* s, int step) {
  const int ps2 = static_cast<int8_t>(s[-3 * step] ^ 0x80), ps1 = static_cast<int8_t>(s[-2 * step] ^ 0x80), ps0 = static_cast<int8_t>(s[-step] ^ 0x80);
  int qs0 = static_cast<int8_t>(s[0] ^ 0x80);
  const int qs1 = static_cast<int8_t>(s[step] ^ 0x80), qs2 = static_cast<int8_t>(s[2 * step] ^ 0x80);
  int fv = sclamp(ps1 - qs1);
  fv = sclamp(fv + 3 * (qs0 - ps0));
  int f2 = hev ? fv : 0;
  const int f1 = sclamp(f2 + 4) >> 3;
  f2 = sclamp(f2 + 3) >> 3;
  qs0 = sclamp(qs0 - f1);
  const int ps0b = sclamp(ps0 + f2);
  if (hev) fv = 0;
  int u = sclamp((63 + fv * 27) >> 7);
  s[0] = static_cast<uint8_t>(sclamp(qs0 - u) ^ 0x80);
  s[-step] = static_cast<uint8_t>(sclamp(ps0b + u) ^ 0x80);
  u = sclamp((63 + fv * 18) >> 7);
  s[step] = static_cast<uint8_t>(sclamp(qs1 - u) ^ 0x80);
  s[-2 * step] = static_cast<uint8_t>(sclamp(ps1 + u) ^ 0x80);
  u = sclamp((63 + fv * 9) >> 7);
  s[2 * step] = static_cast<uint8_t>(sclamp(qs2 - u) ^ 0x80);
  s[-3 * step] = static_cast<uint8_t>(sclamp(ps2 + u) ^ 0x80);
}

// Filters `count` pixels along an edge. `s` is q0 of the first one.
void loop_edge(uint8_t* s, int step, int along, int count, int blimit, int limit, int thresh, bool mb) {
  for (int i = 0; i < count; ++i, s += along) {
    if (!filter_mask(limit, blimit, s, step)) continue;
    const bool hev = high_edge_variance(thresh, s, step);
    if (mb) mb_filter(hev, s, step);
    else normal_filter(hev, s, step);
  }
}

void simple_edge(uint8_t* s, int step, int along, int blimit) {
  for (int i = 0; i < 16; ++i, s += along) {
    const int p1 = s[-2 * step], p0 = s[-step], q0 = s[0], q1 = s[step];
    if (iabs(p0 - q0) * 2 + iabs(p1 - q1) / 2 > blimit) continue;
    const int sp1 = static_cast<int8_t>(p1 ^ 0x80), sp0 = static_cast<int8_t>(p0 ^ 0x80), sq0 = static_cast<int8_t>(q0 ^ 0x80),
              sq1 = static_cast<int8_t>(q1 ^ 0x80);
    int fv = sclamp(sp1 - sq1);
    fv = sclamp(fv + 3 * (sq0 - sp0));
    const int f1 = sclamp(fv + 4) >> 3, f2 = sclamp(fv + 3) >> 3;
    s[0] = static_cast<uint8_t>(sclamp(sq0 - f1) ^ 0x80);
    s[-step] = static_cast<uint8_t>(sclamp(sp0 + f2) ^ 0x80);
  }
}

}  // namespace

// --- frame buffers -------------------------------------------------------------------------------------------------------------------------

void Core::release() {
  for (Frame& f : frames_) {
    release_view(allocator_, f.mem);
    for (int p = 0; p < 3; ++p) f.plane[p] = ByteView();
  }
  release_view(allocator_, mi_);
  release_view(allocator_, above_ctx_);
  allocated_ = false;
}

bool Core::alloc(uint32_t w, uint32_t h) {
  release();
  have_key_ = false;
  const uint32_t cols = (w + 15) >> 4, rows = (h + 15) >> 4;
  if (uint64_t{w} * h > (uint64_t{1} << 26)) return false;
  mb_cols_ = cols;
  mb_rows_ = rows;
  mi_stride_ = cols + 1;
  const uint64_t luma = uint64_t{cols} * 16 * rows * 16, chroma = uint64_t{cols} * 8 * rows * 8;
  for (Frame& f : frames_) {
    f.mem = allocate_view<uint8_t>(allocator_, luma + 2 * chroma);
    if (f.mem.empty()) {
      release();
      return false;
    }
    f.mem.fill(0);
    f.pw[0] = cols * 16;
    f.ph[0] = rows * 16;
    f.pw[1] = f.pw[2] = cols * 8;
    f.ph[1] = f.ph[2] = rows * 8;
    for (int p = 0; p < 3; ++p) f.stride[p] = f.pw[p];
    f.plane[0] = f.mem.subview(0, luma);
    f.plane[1] = f.mem.subview(luma, chroma);
    f.plane[2] = f.mem.subview(luma + chroma, chroma);
  }
  mi_ = allocate_view<MbInfo>(allocator_, uint64_t{rows + 1} * mi_stride_ + 1);
  above_ctx_ = allocate_view<uint8_t>(allocator_, uint64_t{cols} * 9);
  if (mi_.empty() || above_ctx_.empty()) {
    release();
    return false;
  }
  mi_.fill(MbInfo());
  above_ctx_.fill(0);
  width_ = w;
  height_ = h;
  cur_ = lst_ = gld_ = alt_ = 0;
  allocated_ = true;
  return true;
}

// --- header --------------------------------------------------------------------------------------------------------------------------------

void Core::init_key_frame() {
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < kMvpCount; ++j) fc_.mvc[i][j] = kMvDefault[i][j];
  for (int i = 0; i < 4; ++i) fc_.ymode[i] = kYmodeProb[i];
  for (int i = 0; i < 3; ++i) fc_.uvmode[i] = kUvModeProb[i];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j)
      for (int k = 0; k < 3; ++k)
        for (int l = 0; l < 11; ++l) fc_.coef[i][j][k][l] = default_coef_probs[i][j][k][l];
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 4; ++j) seg_data_[i][j] = 0;
  seg_abs_ = false;
  for (int i = 0; i < 4; ++i) ref_lf_delta_[i] = mode_lf_delta_[i] = 0;
  refresh_golden_ = refresh_alt_ = true;
  copy_to_gf_ = copy_to_arf_ = 0;
  sign_bias_[GOLDEN_FRAME] = sign_bias_[ALTREF_FRAME] = 0;
}

namespace {
int dc_quant(int q, int delta) { return dc_qlookup[clampi(q + delta, 0, 127)]; }
int ac_quant(int q, int delta) { return ac_qlookup[clampi(q + delta, 0, 127)]; }
}  // namespace

bool Core::parse_header(ConstByteView packet, uint64_t pos, uint32_t first_size, bool key, int version) {
  version_ = version;
  bilinear_ = version != 0 && version <= 3;
  full_pixel_ = version == 3;
  BoolDecoder& bc = bc_;
  bc.start(packet.subview(pos));
  if (key) {
    bc.bit();   // color space
    bc.bit();   // clamping type (the decoder always clamps)
  }
  seg_enabled_ = bc.bit();
  if (seg_enabled_) {
    seg_update_map_ = bc.bit();
    seg_update_data_ = bc.bit();
    if (seg_update_data_) {
      seg_abs_ = bc.bit();
      for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 4; ++j) {
          int v = 0;
          if (bc.bit()) {
            v = bc.literal(i == 0 ? 7 : 6);
            if (bc.bit()) v = -v;
          }
          seg_data_[i][j] = v;
        }
      }
    }
    if (seg_update_map_) {
      for (int i = 0; i < 3; ++i) seg_tree_probs_[i] = bc.bit() ? static_cast<uint8_t>(bc.literal(8)) : 255;
    }
  } else {
    seg_update_map_ = seg_update_data_ = false;
  }
  filter_type_ = bc.bit();
  filter_level_ = bc.literal(6);
  sharpness_ = bc.literal(3);
  lf_delta_enabled_ = bc.bit();
  if (lf_delta_enabled_ && bc.bit()) {
    for (int i = 0; i < 4; ++i) {
      if (bc.bit()) {
        int v = bc.literal(6);
        if (bc.bit()) v = -v;
        ref_lf_delta_[i] = v;
      }
    }
    for (int i = 0; i < 4; ++i) {
      if (bc.bit()) {
        int v = bc.literal(6);
        if (bc.bit()) v = -v;
        mode_lf_delta_[i] = v;
      }
    }
  }

  // Token partitions: their count is the next two bits; their sizes follow the first partition.
  num_parts_ = 1u << bc.literal(2);
  const uint64_t sizes_at = pos + first_size;
  const uint64_t table = uint64_t{3} * (num_parts_ - 1);
  if (packet.size() < sizes_at || packet.size() - sizes_at < table) return false;
  uint64_t start = sizes_at + table;
  for (uint32_t i = 0; i < num_parts_; ++i) {
    const uint64_t left = packet.size() - start;
    uint64_t size = left;
    if (i + 1 < num_parts_) {
      const uint64_t at = sizes_at + uint64_t{3} * i;
      size = packet[at] | (uint64_t{packet[at + 1]} << 8) | (uint64_t{packet[at + 2]} << 16);
    }
    if (size > left) return false;
    parts_[i].start(packet.subview(start, size));
    start += size;
  }

  base_q_ = bc.literal(7);
  auto delta_q = [&bc]() {
    int v = 0;
    if (bc.bit()) {
      v = bc.literal(4);
      if (bc.bit()) v = -v;
    }
    return v;
  };
  y1dc_dq_ = delta_q();
  y2dc_dq_ = delta_q();
  y2ac_dq_ = delta_q();
  uvdc_dq_ = delta_q();
  uvac_dq_ = delta_q();

  if (!key) {
    refresh_golden_ = bc.bit();
    refresh_alt_ = bc.bit();
    copy_to_gf_ = refresh_golden_ ? 0 : bc.literal(2);
    copy_to_arf_ = refresh_alt_ ? 0 : bc.literal(2);
    sign_bias_[GOLDEN_FRAME] = bc.bit();
    sign_bias_[ALTREF_FRAME] = bc.bit();
    if (copy_to_gf_ == 3 || copy_to_arf_ == 3) return false;
  }
  refresh_entropy_ = bc.bit();
  if (!refresh_entropy_) lfc_ = fc_;
  refresh_last_ = key || bc.bit();
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j)
      for (int k = 0; k < 3; ++k)
        for (int l = 0; l < 11; ++l)
          if (bc.read(vp8_coef_update_probs[i][j][k][l])) fc_.coef[i][j][k][l] = static_cast<uint8_t>(bc.literal(8));

  // The dequantizer of each segment.
  for (int s = 0; s < 4; ++s) {
    int q = base_q_;
    if (seg_enabled_) q = clampi(seg_abs_ ? seg_data_[0][s] : base_q_ + seg_data_[0][s], 0, 127);
    Dequant& d = dq_[s];
    d.y1dc = dc_quant(q, y1dc_dq_);
    d.y1ac = ac_quant(q, 0);
    d.y2dc = dc_quant(q, y2dc_dq_) * 2;
    d.y2ac = (ac_quant(q, y2ac_dq_) * 101581) >> 16;
    if (d.y2ac < 8) d.y2ac = 8;
    d.uvdc = dc_quant(q, uvdc_dq_);
    if (d.uvdc > 132) d.uvdc = 132;
    d.uvac = ac_quant(q, uvac_dq_);
  }
  return true;
}

// --- modes and motion vectors -------------------------------------------------------------------------------------------------------------

int Core::read_bmode_kf(const MbInfo& m, int r, int c, int b) {
  auto implied = [](const MbInfo& n, int block) -> int {
    switch (n.mode) {
      case B_PRED: return n.bmode[block];
      case V_PRED: return B_VE_PRED;
      case H_PRED: return B_HE_PRED;
      case TM_PRED: return B_TM_PRED;
      default: return B_DC_PRED;
    }
  };
  int above, left;
  if (b >> 2) above = m.bmode[b - 4];
  else above = implied(mi(r - 1, c), b + 12);
  if (b & 3) left = m.bmode[b - 1];
  else left = implied(mi(r, c - 1), b + 3);
  return bc_.tree(kBmodeTree, ArrayView<const uint8_t>(vp8_kf_bmode_prob[above][left]));
}

void Core::decode_kf_modes(MbInfo& m, int r, int c) {
  m.ref_frame = INTRA_FRAME;
  m.mv = Mv();
  m.mode = static_cast<uint8_t>(bc_.tree(kKfYmodeTree, kKfYmodeProb));
  m.is_4x4 = 0;
  if (m.mode == B_PRED) {
    m.is_4x4 = 1;
    for (int i = 0; i < 16; ++i) m.bmode[i] = static_cast<uint8_t>(read_bmode_kf(m, r, c, i));
  }
  m.uv_mode = static_cast<uint8_t>(bc_.tree(kUvModeTree, kKfUvModeProb));
}

int Core::read_mv_component(const uint8_t (&p)[kMvpCount]) {
  int x = 0;
  if (bc_.read(p[kMvpIsShort])) {
    for (int i = 0; i < 3; ++i) x += bc_.read(p[kMvpBits + i]) << i;
    for (int i = kMvLongWidth - 1; i > 3; --i) x += bc_.read(p[kMvpBits + i]) << i;
    if (!(x & 0xFFF0) || bc_.read(p[kMvpBits + 3])) x += 8;
  } else {
    x = bc_.tree(kSmallMvTree, ArrayView<const uint8_t>(p).subview(kMvpShort));
  }
  if (x && bc_.read(p[kMvpSign])) x = -x;
  return x;
}

void Core::decode_split_mv(MbInfo& m, int r, int c, Mv best, int left_edge, int right_edge, int top_edge, int bottom_edge) {
  int s = 3, num_p = 16;
  if (bc_.read(110)) {
    s = 2;
    num_p = 4;
    if (bc_.read(111)) {
      s = bc_.read(150);
      num_p = 2;
    }
  }
  const MbInfo& left_mb = mi(r, c - 1);
  const MbInfo& above_mb = mi(r - 1, c);
  for (int j = 0; j < num_p; ++j) {
    const int k = kMbsplitOffset[s][j];
    Mv leftmv, abovemv;
    if (!(k & 3)) leftmv = left_mb.mode != SPLITMV ? left_mb.mv : left_mb.bmv[k + 3];
    else leftmv = m.bmv[k - 1];
    if (!(k >> 2)) abovemv = above_mb.mode != SPLITMV ? above_mb.mv : above_mb.bmv[k + 12];
    else abovemv = m.bmv[k - 4];
    const int lez = leftmv.zero(), aez = abovemv.zero(), lea = leftmv == abovemv;
    const uint8_t* prob = kSubMvRefProb[(aez << 2) | (lez << 1) | lea];
    Mv blockmv;
    if (bc_.read(prob[0])) {
      if (bc_.read(prob[1])) {
        if (bc_.read(prob[2])) {
          blockmv.row = static_cast<int16_t>(read_mv_component(fc_.mvc[0]) * 2 + best.row);
          blockmv.col = static_cast<int16_t>(read_mv_component(fc_.mvc[1]) * 2 + best.col);
        }
      } else {
        blockmv = abovemv;
      }
    } else {
      blockmv = leftmv;
    }
    m.need_clamp |= blockmv.col < left_edge || blockmv.col > right_edge || blockmv.row < top_edge || blockmv.row > bottom_edge;
    const uint8_t* fill = &kMbsplitFillOffset[s][j * kMbsplitFillCount[s]];
    for (int n = 0; n < kMbsplitFillCount[s]; ++n) m.bmv[fill[n]] = blockmv;
  }
  m.partitioning = static_cast<uint8_t>(s);
}

void Core::decode_inter_modes(MbInfo& m, int r, int c) {
  m.ref_frame = static_cast<uint8_t>(bc_.read(prob_intra_));
  if (m.ref_frame) {
    enum { CNT_INTRA, CNT_NEAREST, CNT_NEAR, CNT_SPLITMV };
    int cnt[4] = {0, 0, 0, 0};
    Mv near_mvs[4];
    int ni = 0, ci = 0;
    const MbInfo& above = mi(r - 1, c);
    const MbInfo& left = mi(r, c - 1);
    const MbInfo& aboveleft = mi(r - 1, c - 1);
    m.need_clamp = 0;
    if (bc_.read(prob_last_)) m.ref_frame = static_cast<uint8_t>(2 + bc_.read(prob_gf_));
    auto bias = [&](const MbInfo& n, Mv v) {
      if (sign_bias_[n.ref_frame] != sign_bias_[m.ref_frame]) {
        v.row = static_cast<int16_t>(-v.row);
        v.col = static_cast<int16_t>(-v.col);
      }
      return v;
    };
    if (above.ref_frame != INTRA_FRAME) {
      if (!above.mv.zero()) {
        near_mvs[++ni] = bias(above, above.mv);
        ++ci;
      }
      cnt[ci] += 2;
    }
    if (left.ref_frame != INTRA_FRAME) {
      if (!left.mv.zero()) {
        const Mv v = bias(left, left.mv);
        if (v != near_mvs[ni]) {
          near_mvs[++ni] = v;
          ++ci;
        }
        cnt[ci] += 2;
      } else {
        cnt[CNT_INTRA] += 2;
      }
    }
    if (aboveleft.ref_frame != INTRA_FRAME) {
      if (!aboveleft.mv.zero()) {
        const Mv v = bias(aboveleft, aboveleft.mv);
        if (v != near_mvs[ni]) {
          near_mvs[++ni] = v;
          ++ci;
        }
        cnt[ci] += 1;
      } else {
        cnt[CNT_INTRA] += 1;
      }
    }
    // Distances to the picture edges in 1/8 pixels, as libvpx's xd->mb_to_*_edge.
    const int to_left = -((c * 16) << 3), to_right = (((int)mb_cols_ - 1 - c) * 16) << 3;
    const int to_top = -((r * 16) << 3), to_bottom = (((int)mb_rows_ - 1 - r) * 16) << 3;
    auto clamp_mv = [&](Mv v) {
      if (v.col < to_left - (16 << 3)) v.col = static_cast<int16_t>(to_left - (16 << 3));
      else if (v.col > to_right + (16 << 3)) v.col = static_cast<int16_t>(to_right + (16 << 3));
      if (v.row < to_top - (16 << 3)) v.row = static_cast<int16_t>(to_top - (16 << 3));
      else if (v.row > to_bottom + (16 << 3)) v.row = static_cast<int16_t>(to_bottom + (16 << 3));
      return v;
    };
    if (bc_.read(kModeContexts[cnt[CNT_INTRA]][0])) {
      cnt[CNT_NEAREST] += (cnt[CNT_SPLITMV] > 0) & (near_mvs[ni] == near_mvs[CNT_NEAREST]);
      if (cnt[CNT_NEAR] > cnt[CNT_NEAREST]) {
        const int t = cnt[CNT_NEAREST];
        cnt[CNT_NEAREST] = cnt[CNT_NEAR];
        cnt[CNT_NEAR] = t;
        const Mv tm = near_mvs[CNT_NEAREST];
        near_mvs[CNT_NEAREST] = near_mvs[CNT_NEAR];
        near_mvs[CNT_NEAR] = tm;
      }
      if (bc_.read(kModeContexts[cnt[CNT_NEAREST]][1])) {
        if (bc_.read(kModeContexts[cnt[CNT_NEAR]][2])) {
          const int left_edge = to_left - (16 << 3), right_edge = to_right + (16 << 3), top_edge = to_top - (16 << 3), bottom_edge = to_bottom + (16 << 3);
          const int near_index = CNT_INTRA + (cnt[CNT_NEAREST] >= cnt[CNT_INTRA]);
          near_mvs[near_index] = clamp_mv(near_mvs[near_index]);
          cnt[CNT_SPLITMV] = ((above.mode == SPLITMV) + (left.mode == SPLITMV)) * 2 + (aboveleft.mode == SPLITMV);
          if (bc_.read(kModeContexts[cnt[CNT_SPLITMV]][3])) {
            decode_split_mv(m, r, c, near_mvs[near_index], left_edge, right_edge, top_edge, bottom_edge);
            m.mv = m.bmv[15];
            m.mode = SPLITMV;
            m.is_4x4 = 1;
          } else {
            Mv v;
            v.row = static_cast<int16_t>(read_mv_component(fc_.mvc[0]) * 2);
            v.col = static_cast<int16_t>(read_mv_component(fc_.mvc[1]) * 2);
            v.row = static_cast<int16_t>(v.row + near_mvs[near_index].row);
            v.col = static_cast<int16_t>(v.col + near_mvs[near_index].col);
            m.mv = v;
            m.need_clamp = v.col < left_edge || v.col > right_edge || v.row < top_edge || v.row > bottom_edge;
            m.mode = NEWMV;
          }
        } else {
          m.mode = NEARMV;
          m.mv = clamp_mv(near_mvs[CNT_NEAR]);
        }
      } else {
        m.mode = NEARESTMV;
        m.mv = clamp_mv(near_mvs[CNT_NEAREST]);
      }
    } else {
      m.mode = ZEROMV;
      m.mv = Mv();
    }
    if (m.mode != SPLITMV)
      for (Mv& b : m.bmv) b = m.mv;
  } else {
    m.mv = Mv();
    m.mode = static_cast<uint8_t>(bc_.tree(kYmodeTree, fc_.ymode));
    m.is_4x4 = 0;
    if (m.mode == B_PRED) {
      m.is_4x4 = 1;
      for (int j = 0; j < 16; ++j) m.bmode[j] = static_cast<uint8_t>(bc_.tree(kBmodeTree, kBmodeProb));
    }
    m.uv_mode = static_cast<uint8_t>(bc_.tree(kUvModeTree, fc_.uvmode));
  }
}

void Core::decode_modes() {
  mb_no_coeff_skip_ = bc_.bit();
  prob_skip_false_ = mb_no_coeff_skip_ ? bc_.literal(8) : 0;
  if (!key_) {
    prob_intra_ = bc_.literal(8);
    prob_last_ = bc_.literal(8);
    prob_gf_ = bc_.literal(8);
    if (bc_.bit())
      for (int i = 0; i < 4; ++i) fc_.ymode[i] = static_cast<uint8_t>(bc_.literal(8));
    if (bc_.bit())
      for (int i = 0; i < 3; ++i) fc_.uvmode[i] = static_cast<uint8_t>(bc_.literal(8));
    for (int i = 0; i < 2; ++i)
      for (int j = 0; j < kMvpCount; ++j)
        if (bc_.read(kMvUpdate[i][j])) {
          const int x = bc_.literal(7);
          fc_.mvc[i][j] = static_cast<uint8_t>(x ? x << 1 : 1);
        }
  }
  for (uint32_t r = 0; r < mb_rows_; ++r) {
    for (uint32_t c = 0; c < mb_cols_; ++c) {
      MbInfo& m = mi(static_cast<int>(r), static_cast<int>(c));
      if (seg_update_map_) {
        if (bc_.read(seg_tree_probs_[0])) m.segment_id = static_cast<uint8_t>(2 + bc_.read(seg_tree_probs_[2]));
        else m.segment_id = static_cast<uint8_t>(bc_.read(seg_tree_probs_[1]));
      } else if (key_) {
        m.segment_id = 0;
      }
      m.skip = mb_no_coeff_skip_ ? static_cast<uint8_t>(bc_.read(prob_skip_false_)) : 0;
      m.is_4x4 = 0;
      m.need_clamp = 0;
      if (key_) decode_kf_modes(m, static_cast<int>(r), static_cast<int>(c));
      else decode_inter_modes(m, static_cast<int>(r), static_cast<int>(c));
    }
  }
}

// --- tokens --------------------------------------------------------------------------------------------------------------------------------

// Returns the position of the last nonzero coefficient plus one (0 if there is none). `n` is where in the zigzag to start.
int Core::get_coeffs(BoolDecoder& bd, int type, int ctx, int n, int16_t* out) {
  const uint8_t(&prob)[8][3][11] = fc_.coef[type];
  const uint8_t* p = prob[n][ctx];
  if (!bd.read(p[0])) return 0;
  for (;;) {
    ++n;
    if (!bd.read(p[1])) {
      p = prob[kBands[n]][0];
    } else {
      int v;
      if (!bd.read(p[2])) {
        p = prob[kBands[n]][1];
        v = 1;
      } else {
        if (!bd.read(p[3])) {
          if (!bd.read(p[4])) v = 2;
          else v = 3 + bd.read(p[5]);
        } else if (!bd.read(p[6])) {
          if (!bd.read(p[7])) {
            v = 5 + bd.read(159);
          } else {
            v = 7 + 2 * bd.read(165);
            v += bd.read(145);
          }
        } else {
          const int bit1 = bd.read(p[8]);
          const int bit0 = bd.read(p[9 + bit1]);
          const int cat = 2 * bit1 + bit0;
          const uint8_t* tab = cat == 0 ? kCat3 : cat == 1 ? kCat4 : cat == 2 ? kCat5 : kCat6;
          v = 0;
          for (; *tab; ++tab) v += v + bd.read(*tab);
          v += 3 + (8 << cat);
        }
        p = prob[kBands[n]][2];
      }
      out[kZigzag[n - 1]] = static_cast<int16_t>(bd.bit() ? -v : v);
      if (n == 16 || !bd.read(p[0])) return n;
    }
    if (n == 16) return 16;
  }
}

void Core::reset_token_context(int c, const MbInfo& m) {
  ByteView a = above_ctx_.subview(uint64_t{9} * static_cast<uint64_t>(c), 9);
  const int n = m.is_4x4 ? 8 : 9;   // the Y2 context is left alone by macroblocks that have none
  for (int i = 0; i < n; ++i) a[i] = left_ctx_[i] = 0;
}

int Core::read_tokens(int c, const MbInfo& m, BoolDecoder& bd) {
  ByteView a = above_ctx_.subview(uint64_t{9} * static_cast<uint64_t>(c), 9);
  int eobtotal = 0, type, skip_dc;
  if (!m.is_4x4) {
    const int nz = get_coeffs(bd, 1, a[8] + left_ctx_[8], 0, coef_ + 24 * 16);
    a[8] = left_ctx_[8] = nz > 0;
    eobs_[24] = static_cast<uint8_t>(nz);
    eobtotal += nz - 16;
    type = 0;
    skip_dc = 1;
  } else {
    type = 3;
    skip_dc = 0;
  }
  for (int i = 0; i < 16; ++i) {
    uint8_t& ac = a[i & 3];
    uint8_t& lc = left_ctx_[(i & 0xc) >> 2];
    int nz = get_coeffs(bd, type, ac + lc, skip_dc, coef_ + i * 16);
    ac = lc = nz > 0;
    nz += skip_dc;
    eobs_[i] = static_cast<uint8_t>(nz);
    eobtotal += nz;
  }
  for (int i = 16; i < 24; ++i) {
    uint8_t& ac = a[4 + ((i > 19) << 1) + (i & 1)];
    uint8_t& lc = left_ctx_[4 + ((i > 19) << 1) + ((i & 3) > 1)];
    const int nz = get_coeffs(bd, 2, ac + lc, 0, coef_ + i * 16);
    ac = lc = nz > 0;
    eobs_[i] = static_cast<uint8_t>(nz);
    eobtotal += nz;
  }
  return eobtotal;
}

// --- reconstruction ------------------------------------------------------------------------------------------------------------------------

void Core::predict_intra(Frame& f, int r, int c, MbInfo& m, const Dequant& dq) {
  const int stride_y = static_cast<int>(f.stride[0]), stride_c = static_cast<int>(f.stride[1]);
  const int64_t ypos = int64_t{r} * 16 * stride_y + c * 16, cpos = int64_t{r} * 8 * stride_c + c * 8;

  // The row above and the column to the left, with the values libvpx puts outside the picture: 127 above (also above-left), 129 to the left.
  uint8_t ay[1 + 16 + 4], ly[16], au[1 + 8], av[1 + 8], lu[8], lv[8];
  if (r == 0) {
    for (uint8_t& v : ay) v = 127;
    for (uint8_t& v : au) v = 127;
    for (uint8_t& v : av) v = 127;
  } else {
    const uint8_t* y = at(f.plane[0], ypos - stride_y, c > 0 ? -1 : 0, 15 + (c + 1 < static_cast<int>(mb_cols_) ? 4 : 0));
    ay[0] = c > 0 ? y[-1] : 129;
    for (int i = 0; i < 16; ++i) ay[1 + i] = y[i];
    for (int i = 0; i < 4; ++i) ay[17 + i] = c + 1 < static_cast<int>(mb_cols_) ? y[16 + i] : y[15];
    const uint8_t* u = at(f.plane[1], cpos - stride_c, c > 0 ? -1 : 0, 7);
    const uint8_t* v = at(f.plane[2], cpos - stride_c, c > 0 ? -1 : 0, 7);
    au[0] = c > 0 ? u[-1] : 129;
    av[0] = c > 0 ? v[-1] : 129;
    for (int i = 0; i < 8; ++i) {
      au[1 + i] = u[i];
      av[1 + i] = v[i];
    }
  }
  for (int i = 0; i < 16; ++i) ly[i] = c > 0 ? f.plane[0][static_cast<uint64_t>(ypos + int64_t{i} * stride_y - 1)] : 129;
  for (int i = 0; i < 8; ++i) {
    lu[i] = c > 0 ? f.plane[1][static_cast<uint64_t>(cpos + int64_t{i} * stride_c - 1)] : 129;
    lv[i] = c > 0 ? f.plane[2][static_cast<uint64_t>(cpos + int64_t{i} * stride_c - 1)] : 129;
  }

  predict_block(m.uv_mode, 8, r > 0, c > 0, au + 1, lu, at(f.plane[1], cpos, 0, 7 * stride_c + 7), stride_c);
  predict_block(m.uv_mode, 8, r > 0, c > 0, av + 1, lv, at(f.plane[2], cpos, 0, 7 * stride_c + 7), stride_c);

  if (m.mode != B_PRED) {
    predict_block(m.mode, 16, r > 0, c > 0, ay + 1, ly, at(f.plane[0], ypos, 0, 15 * stride_y + 15), stride_y);
    return;
  }
  if (m.skip) for (uint8_t& e : eobs_) e = 0;
  for (int b = 0; b < 16; ++b) {
    const int bx = b & 3, by = b >> 2;
    const int64_t pos = ypos + int64_t{by} * 4 * stride_y + bx * 4;
    uint8_t* dst = at(f.plane[0], pos, 0, 3 * stride_y + 3);
    uint8_t A[9], L[4], out[16];
    if (by == 0) {
      for (int i = 0; i < 9; ++i) A[i] = ay[4 * bx + i];
    } else {
      const uint8_t* up = at(f.plane[0], pos - stride_y, -1, 7);
      A[0] = bx > 0 ? up[-1] : ly[4 * by - 1];
      for (int i = 0; i < 4; ++i) A[1 + i] = up[i];
      for (int i = 0; i < 4; ++i) A[5 + i] = bx < 3 ? up[4 + i] : ay[17 + i];
    }
    for (int i = 0; i < 4; ++i) L[i] = bx > 0 ? dst[i * stride_y - 1] : ly[4 * by + i];
    predict_4x4(m.bmode[b], A, L, out);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) dst[y * stride_y + x] = out[y * 4 + x];
    if (eobs_[b]) {
      int16_t* q = coef_ + b * 16;
      if (eobs_[b] > 1) {
        int16_t in[16];
        in[0] = static_cast<int16_t>(q[0] * dq.y1dc);
        for (int i = 1; i < 16; ++i) in[i] = static_cast<int16_t>(q[i] * dq.y1ac);
        idct_add(in, dst, stride_y);
      } else {
        dc_only_idct_add(static_cast<int16_t>(q[0] * dq.y1dc), dst, stride_y);
      }
    }
  }
}

// Copies the w x h block at (x, y) of a plane, with coordinates clamped into the plane, to `out` (stride w).
static void fetch_clamped(const Frame& f, int plane, int x, int y, int w, int h, uint8_t* out) {
  const int pw = static_cast<int>(f.pw[plane]), ph = static_cast<int>(f.ph[plane]);
  const uint64_t stride = f.stride[plane];
  ByteView pl = f.plane[plane];
  for (int j = 0; j < h; ++j) {
    const int sy = clampi(y + j, 0, ph - 1);
    ByteView row = pl.subview(static_cast<uint64_t>(sy) * stride, static_cast<uint64_t>(pw));
    for (int i = 0; i < w; ++i) out[j * w + i] = row[static_cast<uint64_t>(clampi(x + i, 0, pw - 1))];
  }
}

// Predicts the w x h block whose top left is (bx, by) in the plane from `ref`, displaced by the motion vector (in 1/8 pixels).
void Core::inter_block(const Frame& ref, int plane, int bx, int by, int w, int h, int mvr, int mvc, ByteView dst, int64_t dpos, int dstride) {
  const int sx = bx + (mvc >> 3), sy = by + (mvr >> 3);
  const int fx = mvc & 7, fy = mvr & 7;
  uint8_t* d = at(dst, dpos, 0, int64_t{h - 1} * dstride + w - 1);
  // The source area: two pixels before and three after the block, which is what the six-tap filter reaches.
  uint8_t tmp[21 * 21];
  const int tw = w + 5, th = h + 5;
  fetch_clamped(ref, plane, sx - 2, sy - 2, tw, th, tmp);
  const uint8_t* src = tmp + 2 * tw + 2;
  if (!fx && !fy) {
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) d[j * dstride + i] = src[j * tw + i];
    return;
  }
  if (!bilinear_) {
    // Horizontal pass over all rows the vertical pass needs, then the vertical pass; each rounds and clamps.
    uint8_t mid[21 * 16];
    const int16_t* hf = kSixtap[fx];
    const int16_t* vf = kSixtap[fy];
    for (int j = -2; j < h + 3; ++j) {
      for (int i = 0; i < w; ++i) {
        const uint8_t* s = src + j * tw + i;
        const int t = s[-2] * hf[0] + s[-1] * hf[1] + s[0] * hf[2] + s[1] * hf[3] + s[2] * hf[4] + s[3] * hf[5] + 64;
        mid[(j + 2) * w + i] = clamp255(t >> 7);
      }
    }
    for (int j = 0; j < h; ++j) {
      for (int i = 0; i < w; ++i) {
        const uint8_t* s = mid + (j + 2) * w + i;
        const int t = s[-2 * w] * vf[0] + s[-w] * vf[1] + s[0] * vf[2] + s[w] * vf[3] + s[2 * w] * vf[4] + s[3 * w] * vf[5] + 64;
        d[j * dstride + i] = clamp255(t >> 7);
      }
    }
  } else {
    uint16_t mid[17 * 16];
    const int h0 = 128 - fx * 16, h1 = fx * 16, v0 = 128 - fy * 16, v1 = fy * 16;
    for (int j = 0; j < h + 1; ++j)
      for (int i = 0; i < w; ++i) mid[j * w + i] = static_cast<uint16_t>((src[j * tw + i] * h0 + src[j * tw + i + 1] * h1 + 64) >> 7);
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) d[j * dstride + i] = static_cast<uint8_t>((mid[j * w + i] * v0 + mid[(j + 1) * w + i] * v1 + 64) >> 7);
  }
}

void Core::predict_inter(Frame& f, int r, int c, MbInfo& m) {
  const Frame& ref = frames_[m.ref_frame == LAST_FRAME ? lst_ : m.ref_frame == GOLDEN_FRAME ? gld_ : alt_];
  const int stride_y = static_cast<int>(f.stride[0]), stride_c = static_cast<int>(f.stride[1]);
  const int64_t ypos = int64_t{r} * 16 * stride_y + c * 16, cpos = int64_t{r} * 8 * stride_c + c * 8;
  const int to_left = -((c * 16) << 3), to_right = (((int)mb_cols_ - 1 - c) * 16) << 3;
  const int to_top = -((r * 16) << 3), to_bottom = (((int)mb_rows_ - 1 - r) * 16) << 3;
  const int fullpixel_mask = full_pixel_ ? ~7 : ~0;

  auto clamp_mv = [&](Mv v) {
    if (v.col < to_left - (19 << 3)) v.col = static_cast<int16_t>(to_left - (16 << 3));
    else if (v.col > to_right + (18 << 3)) v.col = static_cast<int16_t>(to_right + (16 << 3));
    if (v.row < to_top - (19 << 3)) v.row = static_cast<int16_t>(to_top - (16 << 3));
    else if (v.row > to_bottom + (18 << 3)) v.row = static_cast<int16_t>(to_bottom + (16 << 3));
    return v;
  };
  auto clamp_uvmv = [&](Mv v) {
    if (2 * v.col < to_left - (19 << 3)) v.col = static_cast<int16_t>((to_left - (16 << 3)) >> 1);
    if (2 * v.col > to_right + (18 << 3)) v.col = static_cast<int16_t>((to_right + (16 << 3)) >> 1);
    if (2 * v.row < to_top - (19 << 3)) v.row = static_cast<int16_t>((to_top - (16 << 3)) >> 1);
    if (2 * v.row > to_bottom + (18 << 3)) v.row = static_cast<int16_t>((to_bottom + (16 << 3)) >> 1);
    return v;
  };

  if (m.mode != SPLITMV) {
    Mv mv = m.mv;
    if (m.need_clamp) mv = clamp_mv(mv);
    inter_block(ref, 0, c * 16, r * 16, 16, 16, mv.row, mv.col, f.plane[0], ypos, stride_y);
    int row = mv.row, col = mv.col;
    row += 1 | (row >> 31);
    col += 1 | (col >> 31);
    row /= 2;
    col /= 2;
    row &= fullpixel_mask;
    col &= fullpixel_mask;
    Mv uv;
    uv.row = static_cast<int16_t>(row);
    uv.col = static_cast<int16_t>(col);
    uv = clamp_uvmv(uv);
    inter_block(ref, 1, c * 8, r * 8, 8, 8, uv.row, uv.col, f.plane[1], cpos, stride_c);
    inter_block(ref, 2, c * 8, r * 8, 8, 8, uv.row, uv.col, f.plane[2], cpos, stride_c);
    return;
  }

  if (m.partitioning < 3) {
    static const int blocks[4] = {0, 2, 8, 10};
    for (int b : blocks) {
      Mv mv = m.bmv[b];
      if (m.need_clamp) mv = clamp_mv(mv);
      const int bx = (b & 3) * 4, by = (b >> 2) * 4;
      inter_block(ref, 0, c * 16 + bx, r * 16 + by, 8, 8, mv.row, mv.col, f.plane[0], ypos + int64_t{by} * stride_y + bx, stride_y);
    }
  } else {
    for (int b = 0; b < 16; ++b) {
      Mv mv = m.bmv[b];
      if (m.need_clamp) mv = clamp_mv(mv);
      const int bx = (b & 3) * 4, by = (b >> 2) * 4;
      inter_block(ref, 0, c * 16 + bx, r * 16 + by, 4, 4, mv.row, mv.col, f.plane[0], ypos + int64_t{by} * stride_y + bx, stride_y);
    }
  }
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2; ++j) {
      const int yo = i * 8 + j * 2;
      int row = m.bmv[yo].row + m.bmv[yo + 1].row + m.bmv[yo + 4].row + m.bmv[yo + 5].row;
      int col = m.bmv[yo].col + m.bmv[yo + 1].col + m.bmv[yo + 4].col + m.bmv[yo + 5].col;
      row += 4 + ((row >> 31) * 8);
      col += 4 + ((col >> 31) * 8);
      Mv uv;
      uv.row = static_cast<int16_t>((row / 8) & fullpixel_mask);
      uv.col = static_cast<int16_t>((col / 8) & fullpixel_mask);
      if (m.need_clamp) uv = clamp_uvmv(uv);
      const int bx = j * 4, by = i * 4;
      inter_block(ref, 1, c * 8 + bx, r * 8 + by, 4, 4, uv.row, uv.col, f.plane[1], cpos + int64_t{by} * stride_c + bx, stride_c);
      inter_block(ref, 2, c * 8 + bx, r * 8 + by, 4, 4, uv.row, uv.col, f.plane[2], cpos + int64_t{by} * stride_c + bx, stride_c);
    }
  }
}

void Core::add_residual(Frame& f, int r, int c, const MbInfo& m, const Dequant& dq) {
  const int stride_y = static_cast<int>(f.stride[0]), stride_c = static_cast<int>(f.stride[1]);
  const int64_t ypos = int64_t{r} * 16 * stride_y + c * 16, cpos = int64_t{r} * 8 * stride_c + c * 8;
  if (m.mode != B_PRED) {   // (B_PRED adds the luma residual as it predicts each block)
    int dcf = dq.y1dc;
    if (!m.is_4x4) {
      int16_t* y2 = coef_ + 24 * 16;
      if (eobs_[24] > 1) {
        int16_t in[16];
        in[0] = static_cast<int16_t>(y2[0] * dq.y2dc);
        for (int i = 1; i < 16; ++i) in[i] = static_cast<int16_t>(y2[i] * dq.y2ac);
        inv_walsh(in, coef_);
      } else {
        const int a1 = (static_cast<int16_t>(y2[0] * dq.y2dc) + 3) >> 3;
        for (int i = 0; i < 16; ++i) coef_[i * 16] = static_cast<int16_t>(a1);
      }
      dcf = 1;   // the DC of each block came out of the second-order transform
    }
    for (int b = 0; b < 16; ++b) {
      const int64_t pos = ypos + int64_t{b >> 2} * 4 * stride_y + (b & 3) * 4;
      uint8_t* dst = at(f.plane[0], pos, 0, 3 * stride_y + 3);
      int16_t* q = coef_ + b * 16;
      if (eobs_[b] > 1) {
        int16_t in[16];
        in[0] = static_cast<int16_t>(q[0] * dcf);
        for (int i = 1; i < 16; ++i) in[i] = static_cast<int16_t>(q[i] * dq.y1ac);
        idct_add(in, dst, stride_y);
      } else {
        dc_only_idct_add(static_cast<int16_t>(q[0] * dcf), dst, stride_y);
      }
    }
  }
  for (int p = 1; p < 3; ++p) {
    for (int b = 0; b < 4; ++b) {
      const int blk = 16 + (p - 1) * 4 + b;
      const int64_t pos = cpos + int64_t{b >> 1} * 4 * stride_c + (b & 1) * 4;
      uint8_t* dst = at(f.plane[p], pos, 0, 3 * stride_c + 3);
      int16_t* q = coef_ + blk * 16;
      if (eobs_[blk] > 1) {
        int16_t in[16];
        in[0] = static_cast<int16_t>(q[0] * dq.uvdc);
        for (int i = 1; i < 16; ++i) in[i] = static_cast<int16_t>(q[i] * dq.uvac);
        idct_add(in, dst, stride_c);
      } else {
        dc_only_idct_add(static_cast<int16_t>(q[0] * dq.uvdc), dst, stride_c);
      }
    }
  }
}

void Core::decode_mb(int r, int c, MbInfo& m, BoolDecoder& tokens) {
  bool coded = false;
  if (m.skip) {
    reset_token_context(c, m);
  } else {
    for (int16_t& v : coef_) v = 0;
    const int total = read_tokens(c, m, tokens);
    m.skip = total == 0;
    coded = true;
  }
  const Dequant& dq = dq_[m.segment_id & 3];
  Frame& f = frames_[cur_];
  if (m.ref_frame == INTRA_FRAME) {
    if (!coded) for (int16_t& v : coef_) v = 0;
    predict_intra(f, r, c, m, dq);
  } else {
    predict_inter(f, r, c, m);
  }
  if (!m.skip) add_residual(f, r, c, m, dq);
}

void Core::reconstruct() {
  for (uint8_t& v : above_ctx_) v = 0;
  for (uint32_t r = 0; r < mb_rows_; ++r) {
    BoolDecoder& tokens = parts_[r & (num_parts_ - 1)];
    for (uint8_t& v : left_ctx_) v = 0;
    for (uint32_t c = 0; c < mb_cols_; ++c) decode_mb(static_cast<int>(r), static_cast<int>(c), mi(static_cast<int>(r), static_cast<int>(c)), tokens);
  }
}

// --- loop filter ---------------------------------------------------------------------------------------------------------------------------

void Core::filter_mb(Frame& f, int r, int c, const MbInfo& m) {
  // The level of this macroblock: the frame's, a segment's, then adjusted by the reference frame and mode deltas.
  int lvl = filter_level_;
  if (seg_enabled_) {
    lvl = seg_abs_ ? seg_data_[1][m.segment_id & 3] : lvl + seg_data_[1][m.segment_id & 3];
    lvl = clampi(lvl, 0, 63);
  }
  if (lf_delta_enabled_) {
    lvl += ref_lf_delta_[m.ref_frame];
    int mode_index = 1;
    if (m.ref_frame == INTRA_FRAME) {
      if (m.mode == B_PRED) lvl += mode_lf_delta_[0];
    } else {
      mode_index = m.mode == ZEROMV ? 1 : m.mode == SPLITMV ? 3 : 2;
      lvl += mode_lf_delta_[mode_index];
    }
    lvl = clampi(lvl, 0, 63);
  }
  if (lvl == 0) return;
  int interior = lvl >> (sharpness_ > 0);
  interior >>= (sharpness_ > 4);
  if (sharpness_ > 0 && interior > 9 - sharpness_) interior = 9 - sharpness_;
  if (interior < 1) interior = 1;
  const int blim = 2 * lvl + interior, mblim = 2 * (lvl + 2) + interior;
  const int hev = key_ ? (lvl >= 40 ? 2 : lvl >= 15 ? 1 : 0) : (lvl >= 40 ? 3 : lvl >= 20 ? 2 : lvl >= 15 ? 1 : 0);
  const bool skip_inner = m.mode != B_PRED && m.mode != SPLITMV && m.skip;

  const int sy = static_cast<int>(f.stride[0]), sc = static_cast<int>(f.stride[1]);
  const int64_t ypos = int64_t{r} * 16 * sy + c * 16, cpos = int64_t{r} * 8 * sc + c * 8;
  if (filter_type_ == 0) {
    // Vertical edges (filtering across columns), then horizontal ones.
    if (c > 0) {
      loop_edge(at(f.plane[0], ypos, -4, 15 * sy + 3), 1, sy, 16, mblim, interior, hev, true);
      loop_edge(at(f.plane[1], cpos, -4, 7 * sc + 3), 1, sc, 8, mblim, interior, hev, true);
      loop_edge(at(f.plane[2], cpos, -4, 7 * sc + 3), 1, sc, 8, mblim, interior, hev, true);
    }
    if (!skip_inner) {
      for (int x = 4; x < 16; x += 4) loop_edge(at(f.plane[0], ypos + x, -4, 15 * sy + 3), 1, sy, 16, blim, interior, hev, false);
      loop_edge(at(f.plane[1], cpos + 4, -4, 7 * sc + 3), 1, sc, 8, blim, interior, hev, false);
      loop_edge(at(f.plane[2], cpos + 4, -4, 7 * sc + 3), 1, sc, 8, blim, interior, hev, false);
    }
    if (r > 0) {
      loop_edge(at(f.plane[0], ypos, -4 * sy, 3 * sy + 15), sy, 1, 16, mblim, interior, hev, true);
      loop_edge(at(f.plane[1], cpos, -4 * sc, 3 * sc + 7), sc, 1, 8, mblim, interior, hev, true);
      loop_edge(at(f.plane[2], cpos, -4 * sc, 3 * sc + 7), sc, 1, 8, mblim, interior, hev, true);
    }
    if (!skip_inner) {
      for (int y = 4; y < 16; y += 4) loop_edge(at(f.plane[0], ypos + int64_t{y} * sy, -4 * sy, 3 * sy + 15), sy, 1, 16, blim, interior, hev, false);
      loop_edge(at(f.plane[1], cpos + int64_t{4} * sc, -4 * sc, 3 * sc + 7), sc, 1, 8, blim, interior, hev, false);
      loop_edge(at(f.plane[2], cpos + int64_t{4} * sc, -4 * sc, 3 * sc + 7), sc, 1, 8, blim, interior, hev, false);
    }
  } else {
    if (c > 0) simple_edge(at(f.plane[0], ypos, -2, 15 * sy + 1), 1, sy, mblim);
    if (!skip_inner)
      for (int x = 4; x < 16; x += 4) simple_edge(at(f.plane[0], ypos + x, -2, 15 * sy + 1), 1, sy, blim);
    if (r > 0) simple_edge(at(f.plane[0], ypos, -2 * sy, sy + 15), sy, 1, mblim);
    if (!skip_inner)
      for (int y = 4; y < 16; y += 4) simple_edge(at(f.plane[0], ypos + int64_t{y} * sy, -2 * sy, sy + 15), sy, 1, blim);
  }
}

void Core::loop_filter(Frame& f) {
  for (uint32_t r = 0; r < mb_rows_; ++r)
    for (uint32_t c = 0; c < mb_cols_; ++c) filter_mb(f, static_cast<int>(r), static_cast<int>(c), mi(static_cast<int>(r), static_cast<int>(c)));
}

// --- one frame -----------------------------------------------------------------------------------------------------------------------------

void Core::update_references() {
  if (copy_to_arf_ == 1) alt_ = lst_;
  else if (copy_to_arf_ == 2) alt_ = gld_;
  if (copy_to_gf_ == 1) gld_ = lst_;
  else if (copy_to_gf_ == 2) gld_ = alt_;
  if (refresh_golden_) gld_ = cur_;
  if (refresh_alt_) alt_ = cur_;
  if (refresh_last_) lst_ = cur_;
}

bool Core::decode(ConstByteView d) {
  shown_ = false;
  if (d.size() < 3) return false;
  const uint32_t tag = d[0] | (uint32_t{d[1]} << 8) | (uint32_t{d[2]} << 16);
  const bool key = (tag & 1) == 0;
  const int version = (tag >> 1) & 7;
  const bool show = (tag >> 4) & 1;
  const uint32_t first_size = tag >> 5;
  uint64_t pos = 3;
  uint32_t w = width_, h = height_;
  if (key) {
    if (d.size() - pos < 7 || d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a) return false;
    w = (d[6] | (uint32_t{d[7]} << 8)) & 0x3fff;
    h = (d[8] | (uint32_t{d[9]} << 8)) & 0x3fff;
    pos += 7;
    if (w == 0 || h == 0) return false;
  } else if (!have_key_) {
    return false;
  }
  if (first_size == 0 || d.size() - pos < first_size) return false;
  if (key && (!allocated_ || w != width_ || h != height_) && !alloc(w, h)) return false;

  // A new frame goes into a buffer no reference uses.
  int free_buffer = 0;
  while (free_buffer == lst_ || free_buffer == gld_ || free_buffer == alt_) ++free_buffer;
  cur_ = free_buffer;
  key_ = key;
  if (key) init_key_frame();
  if (!parse_header(d, pos, first_size, key, version)) return false;
  decode_modes();
  reconstruct();
  if (filter_level_) loop_filter(frames_[cur_]);
  if (!refresh_entropy_) fc_ = lfc_;
  update_references();
  if (key) have_key_ = true;
  shown_ = show;
  return true;
}

}  // namespace vp8

// --- the decoder ---------------------------------------------------------------------------------------------------------------------------

Vp8Decoder::~Vp8Decoder() {}

void Vp8Decoder::reset() {
  if (core_) core_->reset();
  pending_ = false;
}

// Crops the picture to the display size and copies it out.
bool Vp8Decoder::convert() {
  const vp8::Frame& f = core_->shown_frame();
  const VideoFormat format{core_->width(), core_->height(), PixelFormat::I420};
  if (!format.valid() || !out_.allocate(format)) return false;
  for (uint32_t plane = 0; plane < 3; ++plane) {
    ByteView dst = out_.plane(plane);
    const uint32_t dst_stride = format.plane_stride(plane), w = format.plane_width(plane), h = format.plane_height(plane);
    for (uint32_t y = 0; y < h; ++y) {
      if (!dst.subview(uint64_t{y} * dst_stride, w).copy_from(f.plane[plane].subview(uint64_t{y} * f.stride[plane], w))) return false;
    }
  }
  format_ = format;
  return true;
}

bool Vp8Decoder::decode(ConstByteView packet, int64_t pts_us) {
  if (!core_) core_ = UniquePointer<vp8::Core>::make(allocator_, allocator_);
  if (!core_) return false;
  if (packet.empty()) {   // a dropped frame: the last picture stays up
    if (!has_picture_) return false;
    pending_ = true;
    pts_ = pts_us;
    keyframe_ = false;
    return true;
  }
  if (!core_->decode(packet)) return false;
  if (core_->shown()) {
    if (!convert()) return false;
    pending_ = true;
    has_picture_ = true;
    pts_ = pts_us;
    keyframe_ = core_->key_frame();
  }
  return true;
}

bool Vp8Decoder::receive(VideoFrame* frame) {
  if (!pending_) return false;
  pending_ = false;
  *frame = out_.frame(pts_, keyframe_);
  frame->limited_range = true;
  return true;
}

}  // namespace hardware
