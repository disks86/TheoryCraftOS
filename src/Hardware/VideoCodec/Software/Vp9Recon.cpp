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

// VP9 decoding, part 2: intra prediction, motion compensation, the inverse transforms and the block reconstruction loops. A port of the
// decoder side of libvpx (BSD-3-Clause, Copyright (c) The WebM Project authors). All pixels are 16-bit whatever the bit depth.
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"

namespace hardware {
namespace vp9 {

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int imin(int a, int b) { return a < b ? a : b; }

// ---- inverse transforms (the 1-D kernels are generated from libvpx's vpx_dsp/inv_txfm.c) --------------------------------------------

constexpr int DCT_CONST_BITS = 14;
constexpr int UNIT_QUANT_SHIFT = 2;
constexpr int64_t kValidTxfmMagnitude = int64_t{1} << 25;
constexpr int cospi_1_64 = 16364, cospi_2_64 = 16305, cospi_3_64 = 16207, cospi_4_64 = 16069, cospi_5_64 = 15893, cospi_6_64 = 15679,
              cospi_7_64 = 15426, cospi_8_64 = 15137, cospi_9_64 = 14811, cospi_10_64 = 14449, cospi_11_64 = 14053, cospi_12_64 = 13623,
              cospi_13_64 = 13160, cospi_14_64 = 12665, cospi_15_64 = 12140, cospi_16_64 = 11585, cospi_17_64 = 11003, cospi_18_64 = 10394,
              cospi_19_64 = 9760, cospi_20_64 = 9102, cospi_21_64 = 8423, cospi_22_64 = 7723, cospi_23_64 = 7005, cospi_24_64 = 6270,
              cospi_25_64 = 5520, cospi_26_64 = 4756, cospi_27_64 = 3981, cospi_28_64 = 3196, cospi_29_64 = 2404, cospi_30_64 = 1606,
              cospi_31_64 = 804;
constexpr int sinpi_1_9 = 5283, sinpi_2_9 = 9929, sinpi_3_9 = 13377, sinpi_4_9 = 15212;

#define ROUND_POWER_OF_TWO(value, n) (((value) + (1 << ((n) - 1))) >> (n))
#define HIGHBD_WRAPLOW(x, bd) (static_cast<int32_t>(x))
#define memset __builtin_memset

inline TranHigh dct_const_round_shift(TranHigh input) { return ROUND_POWER_OF_TWO(input, DCT_CONST_BITS); }
inline bool detect_invalid_highbd_input(const TranLow* input, int size) {
  for (int i = 0; i < size; ++i) {
    const int64_t v = input[i];
    if ((v < 0 ? -v : v) >= kValidTxfmMagnitude) return true;
  }
  return false;
}
using tran_low_t = TranLow;
using tran_high_t = TranHigh;

#include "Hardware/VideoCodec/Software/Vp9Transform.inc"

#undef memset

using Transform1D = void (*)(const tran_low_t*, tran_low_t*, int);

inline uint16_t clip_pixel_add(uint16_t dest, int64_t trans, int bd) {
  const int64_t v = static_cast<int64_t>(dest) + trans;
  const int64_t maxv = (int64_t{1} << bd) - 1;
  return static_cast<uint16_t>(v < 0 ? 0 : v > maxv ? maxv : v);
}

// A two-dimensional inverse transform added to the prediction at `dest` (rows first, then columns). `n` is the size, `shift` the final rounding.
void inverse_2d(Transform1D rows, Transform1D cols, int n, int shift, const TranLow* input, uint16_t* dest, int stride, int bd) {
  TranLow out[32 * 32];
  TranLow temp_in[32], temp_out[32];
  for (int i = 0; i < n; ++i) {
    bool any = false;
    for (int j = 0; j < n; ++j) any = any || input[i * n + j] != 0;
    if (any) rows(input + i * n, out + i * n, bd);
    else
      for (int j = 0; j < n; ++j) out[i * n + j] = 0;
  }
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) temp_in[j] = out[j * n + i];
    cols(temp_in, temp_out, bd);
    for (int j = 0; j < n; ++j) {
      uint16_t& d = dest[j * stride + i];
      d = clip_pixel_add(d, ROUND_POWER_OF_TWO(static_cast<int64_t>(temp_out[j]), shift), bd);
    }
  }
}

void idct32_wrapper(const tran_low_t* in, tran_low_t* out, int bd) { highbd_idct32_c(in, out, bd); }

void iwht4x4_add(const TranLow* input, uint16_t* dest, int stride, int bd) {
  TranLow output[16];
  const TranLow* ip = input;
  TranLow* op = output;
  for (int i = 0; i < 4; i++) {
    int64_t a1 = ip[0] >> UNIT_QUANT_SHIFT, c1 = ip[1] >> UNIT_QUANT_SHIFT, d1 = ip[2] >> UNIT_QUANT_SHIFT, b1 = ip[3] >> UNIT_QUANT_SHIFT;
    a1 += c1;
    d1 -= b1;
    const int64_t e1 = (a1 - d1) >> 1;
    b1 = e1 - b1;
    c1 = e1 - c1;
    a1 -= b1;
    d1 += c1;
    op[0] = static_cast<TranLow>(a1);
    op[1] = static_cast<TranLow>(b1);
    op[2] = static_cast<TranLow>(c1);
    op[3] = static_cast<TranLow>(d1);
    ip += 4;
    op += 4;
  }
  ip = output;
  for (int i = 0; i < 4; i++) {
    int64_t a1 = ip[4 * 0], c1 = ip[4 * 1], d1 = ip[4 * 2], b1 = ip[4 * 3];
    a1 += c1;
    d1 -= b1;
    const int64_t e1 = (a1 - d1) >> 1;
    b1 = e1 - b1;
    c1 = e1 - c1;
    a1 -= b1;
    d1 += c1;
    dest[stride * 0] = clip_pixel_add(dest[stride * 0], a1, bd);
    dest[stride * 1] = clip_pixel_add(dest[stride * 1], b1, bd);
    dest[stride * 2] = clip_pixel_add(dest[stride * 2], c1, bd);
    dest[stride * 3] = clip_pixel_add(dest[stride * 3], d1, bd);
    ip++;
    dest++;
  }
}

// ---- intra prediction -----------------------------------------------------------------------------------------------------------------

inline int avg2(int a, int b) { return (a + b + 1) >> 1; }
inline int avg3(int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; }

// The directional predictors of libvpx's intrapred.c on 16-bit samples. `above` points at the row above the block (index -1 is the corner);
// `left` is the column to its left.
void predict_directional(int mode, int bs, uint16_t* dst, int stride, const uint16_t* above, const uint16_t* left) {
  auto D = [&](int x, int y) -> uint16_t& { return dst[x + y * stride]; };
  if (bs == 4) {
    switch (mode) {
      case D207_PRED: {
        const int I = left[0], J = left[1], K = left[2], L = left[3];
        D(0, 0) = static_cast<uint16_t>(avg2(I, J));
        D(2, 0) = D(0, 1) = static_cast<uint16_t>(avg2(J, K));
        D(2, 1) = D(0, 2) = static_cast<uint16_t>(avg2(K, L));
        D(1, 0) = static_cast<uint16_t>(avg3(I, J, K));
        D(3, 0) = D(1, 1) = static_cast<uint16_t>(avg3(J, K, L));
        D(3, 1) = D(1, 2) = static_cast<uint16_t>(avg3(K, L, L));
        D(3, 2) = D(2, 2) = D(0, 3) = D(1, 3) = D(2, 3) = D(3, 3) = static_cast<uint16_t>(L);
        return;
      }
      case D63_PRED: {
        const int A = above[0], B = above[1], C = above[2], Dd = above[3], E = above[4], F = above[5], G = above[6];
        D(0, 0) = static_cast<uint16_t>(avg2(A, B));
        D(1, 0) = D(0, 2) = static_cast<uint16_t>(avg2(B, C));
        D(2, 0) = D(1, 2) = static_cast<uint16_t>(avg2(C, Dd));
        D(3, 0) = D(2, 2) = static_cast<uint16_t>(avg2(Dd, E));
        D(3, 2) = static_cast<uint16_t>(avg2(E, F));
        D(0, 1) = static_cast<uint16_t>(avg3(A, B, C));
        D(1, 1) = D(0, 3) = static_cast<uint16_t>(avg3(B, C, Dd));
        D(2, 1) = D(1, 3) = static_cast<uint16_t>(avg3(C, Dd, E));
        D(3, 1) = D(2, 3) = static_cast<uint16_t>(avg3(Dd, E, F));
        D(3, 3) = static_cast<uint16_t>(avg3(E, F, G));
        return;
      }
      case D45_PRED: {
        const int A = above[0], B = above[1], C = above[2], Dd = above[3], E = above[4], F = above[5], G = above[6], H = above[7];
        D(0, 0) = static_cast<uint16_t>(avg3(A, B, C));
        D(1, 0) = D(0, 1) = static_cast<uint16_t>(avg3(B, C, Dd));
        D(2, 0) = D(1, 1) = D(0, 2) = static_cast<uint16_t>(avg3(C, Dd, E));
        D(3, 0) = D(2, 1) = D(1, 2) = D(0, 3) = static_cast<uint16_t>(avg3(Dd, E, F));
        D(3, 1) = D(2, 2) = D(1, 3) = static_cast<uint16_t>(avg3(E, F, G));
        D(3, 2) = D(2, 3) = static_cast<uint16_t>(avg3(F, G, H));
        D(3, 3) = static_cast<uint16_t>(H);
        return;
      }
      case D117_PRED: {
        const int I = left[0], J = left[1], K = left[2], X = above[-1], A = above[0], B = above[1], C = above[2], Dd = above[3];
        D(0, 0) = D(1, 2) = static_cast<uint16_t>(avg2(X, A));
        D(1, 0) = D(2, 2) = static_cast<uint16_t>(avg2(A, B));
        D(2, 0) = D(3, 2) = static_cast<uint16_t>(avg2(B, C));
        D(3, 0) = static_cast<uint16_t>(avg2(C, Dd));
        D(0, 3) = static_cast<uint16_t>(avg3(K, J, I));
        D(0, 2) = static_cast<uint16_t>(avg3(J, I, X));
        D(0, 1) = D(1, 3) = static_cast<uint16_t>(avg3(I, X, A));
        D(1, 1) = D(2, 3) = static_cast<uint16_t>(avg3(X, A, B));
        D(2, 1) = D(3, 3) = static_cast<uint16_t>(avg3(A, B, C));
        D(3, 1) = static_cast<uint16_t>(avg3(B, C, Dd));
        return;
      }
      case D135_PRED: {
        const int I = left[0], J = left[1], K = left[2], L = left[3], X = above[-1], A = above[0], B = above[1], C = above[2], Dd = above[3];
        D(0, 3) = static_cast<uint16_t>(avg3(J, K, L));
        D(1, 3) = D(0, 2) = static_cast<uint16_t>(avg3(I, J, K));
        D(2, 3) = D(1, 2) = D(0, 1) = static_cast<uint16_t>(avg3(X, I, J));
        D(3, 3) = D(2, 2) = D(1, 1) = D(0, 0) = static_cast<uint16_t>(avg3(A, X, I));
        D(3, 2) = D(2, 1) = D(1, 0) = static_cast<uint16_t>(avg3(B, A, X));
        D(3, 1) = D(2, 0) = static_cast<uint16_t>(avg3(C, B, A));
        D(3, 0) = static_cast<uint16_t>(avg3(Dd, C, B));
        return;
      }
      default: {  // D153_PRED
        const int I = left[0], J = left[1], K = left[2], L = left[3], X = above[-1], A = above[0], B = above[1], C = above[2];
        D(0, 0) = D(2, 1) = static_cast<uint16_t>(avg2(I, X));
        D(0, 1) = D(2, 2) = static_cast<uint16_t>(avg2(J, I));
        D(0, 2) = D(2, 3) = static_cast<uint16_t>(avg2(K, J));
        D(0, 3) = static_cast<uint16_t>(avg2(L, K));
        D(3, 0) = static_cast<uint16_t>(avg3(A, B, C));
        D(2, 0) = static_cast<uint16_t>(avg3(X, A, B));
        D(1, 0) = D(3, 1) = static_cast<uint16_t>(avg3(I, X, A));
        D(1, 1) = D(3, 2) = static_cast<uint16_t>(avg3(J, I, X));
        D(1, 2) = D(3, 3) = static_cast<uint16_t>(avg3(K, J, I));
        D(1, 3) = static_cast<uint16_t>(avg3(L, K, J));
        return;
      }
    }
  }
  switch (mode) {
    case D207_PRED: {
      uint16_t* d = dst;
      for (int r = 0; r < bs - 1; ++r) d[r * stride] = static_cast<uint16_t>(avg2(left[r], left[r + 1]));
      d[(bs - 1) * stride] = left[bs - 1];
      d++;
      for (int r = 0; r < bs - 2; ++r) d[r * stride] = static_cast<uint16_t>(avg3(left[r], left[r + 1], left[r + 2]));
      d[(bs - 2) * stride] = static_cast<uint16_t>(avg3(left[bs - 2], left[bs - 1], left[bs - 1]));
      d[(bs - 1) * stride] = left[bs - 1];
      d++;
      for (int c = 0; c < bs - 2; ++c) d[(bs - 1) * stride + c] = left[bs - 1];
      for (int r = bs - 2; r >= 0; --r)
        for (int c = 0; c < bs - 2; ++c) d[r * stride + c] = d[(r + 1) * stride + c - 2];
      return;
    }
    case D63_PRED: {
      for (int c = 0; c < bs; ++c) {
        dst[c] = static_cast<uint16_t>(avg2(above[c], above[c + 1]));
        dst[stride + c] = static_cast<uint16_t>(avg3(above[c], above[c + 1], above[c + 2]));
      }
      int size = bs - 2;
      for (int r = 2; r < bs; r += 2, --size) {
        for (int c = 0; c < size; ++c) {
          dst[(r + 0) * stride + c] = dst[(r >> 1) + c];
          dst[(r + 1) * stride + c] = dst[stride + (r >> 1) + c];
        }
        for (int c = size; c < bs; ++c) {
          dst[(r + 0) * stride + c] = above[bs - 1];
          dst[(r + 1) * stride + c] = above[bs - 1];
        }
      }
      return;
    }
    case D45_PRED: {
      const uint16_t above_right = above[bs - 1];
      for (int x = 0; x < bs - 1; ++x) dst[x] = static_cast<uint16_t>(avg3(above[x], above[x + 1], above[x + 2]));
      dst[bs - 1] = above_right;
      uint16_t* d = dst + stride;
      int size = bs - 2;
      for (int x = 1; x < bs; ++x, --size) {
        for (int c = 0; c < size; ++c) d[c] = dst[x + c];
        for (int c = size; c < bs; ++c) d[c] = above_right;
        d += stride;
      }
      return;
    }
    case D117_PRED: {
      uint16_t* d = dst;
      for (int c = 0; c < bs; c++) d[c] = static_cast<uint16_t>(avg2(above[c - 1], above[c]));
      d += stride;
      d[0] = static_cast<uint16_t>(avg3(left[0], above[-1], above[0]));
      for (int c = 1; c < bs; c++) d[c] = static_cast<uint16_t>(avg3(above[c - 2], above[c - 1], above[c]));
      d += stride;
      d[0] = static_cast<uint16_t>(avg3(above[-1], left[0], left[1]));
      for (int r = 3; r < bs; ++r) d[(r - 2) * stride] = static_cast<uint16_t>(avg3(left[r - 3], left[r - 2], left[r - 1]));
      for (int r = 2; r < bs; ++r) {
        for (int c = 1; c < bs; c++) d[c] = d[-2 * stride + c - 1];
        d += stride;
      }
      return;
    }
    case D135_PRED: {
      uint16_t border[32 + 32 - 1];
      for (int i = 0; i < bs - 2; ++i) border[i] = static_cast<uint16_t>(avg3(left[bs - 3 - i], left[bs - 2 - i], left[bs - 1 - i]));
      border[bs - 2] = static_cast<uint16_t>(avg3(above[-1], left[0], left[1]));
      border[bs - 1] = static_cast<uint16_t>(avg3(left[0], above[-1], above[0]));
      border[bs - 0] = static_cast<uint16_t>(avg3(above[-1], above[0], above[1]));
      for (int i = 0; i < bs - 2; ++i) border[bs + 1 + i] = static_cast<uint16_t>(avg3(above[i], above[i + 1], above[i + 2]));
      for (int i = 0; i < bs; ++i)
        for (int c = 0; c < bs; ++c) dst[i * stride + c] = border[bs - 1 - i + c];
      return;
    }
    default: {  // D153_PRED
      uint16_t* d = dst;
      d[0] = static_cast<uint16_t>(avg2(above[-1], left[0]));
      for (int r = 1; r < bs; r++) d[r * stride] = static_cast<uint16_t>(avg2(left[r - 1], left[r]));
      d++;
      d[0] = static_cast<uint16_t>(avg3(left[0], above[-1], above[0]));
      d[stride] = static_cast<uint16_t>(avg3(above[-1], left[0], left[1]));
      for (int r = 2; r < bs; r++) d[r * stride] = static_cast<uint16_t>(avg3(left[r - 2], left[r - 1], left[r]));
      d++;
      for (int c = 0; c < bs - 2; c++) d[c] = static_cast<uint16_t>(avg3(above[c - 1], above[c], above[c + 1]));
      d += stride;
      for (int r = 1; r < bs; ++r) {
        for (int c = 0; c < bs - 2; c++) d[c] = d[-stride + c - 2];
        d += stride;
      }
      return;
    }
  }
}

enum { NEED_LEFT = 1 << 1, NEED_ABOVE = 1 << 2, NEED_ABOVERIGHT = 1 << 3 };
const uint8_t kExtendModes[INTRA_MODES] = {NEED_ABOVE | NEED_LEFT, NEED_ABOVE, NEED_LEFT, NEED_ABOVERIGHT, NEED_LEFT | NEED_ABOVE,
                                           NEED_LEFT | NEED_ABOVE, NEED_LEFT | NEED_ABOVE, NEED_LEFT, NEED_ABOVERIGHT, NEED_LEFT | NEED_ABOVE};

}  // namespace

// ---- Core: intra ---------------------------------------------------------------------------------------------------------------------------

// Predicts the transform block at 4x4-unit offset (x, y) of the current block in `plane` from the already reconstructed neighbours.
void Core::predict_intra_block(int plane, int tx_size, int mode, int aoff, int loff) {
  Frame& f = frames_[new_fb_idx_];
  const int bs = 4 << tx_size;
  const int stride = static_cast<int>(f.stride[plane]);
  const int bw = 1 << plane_[plane].n4_wl;
  const bool up_available = loff || above_mi_ != nullptr;
  const bool left_available = aoff || left_mi_ != nullptr;
  const bool right_available = (aoff + (1 << tx_size)) < bw;
  const int frame_width = f.aligned_w[plane], frame_height = f.aligned_h[plane];
  const int x0 = (-mb_to_left_edge_ >> (3 + plane_[plane].ss_x)) + aoff * 4;
  const int y0 = (-mb_to_top_edge_ >> (3 + plane_[plane].ss_y)) + loff * 4;
  ArrayView<uint16_t> pix = f.plane[plane];
  const uint64_t origin = static_cast<uint64_t>(y0) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(x0);
  auto px = [&](int dx, int dy) -> uint16_t { return pix[static_cast<uint64_t>(static_cast<int64_t>(origin) + int64_t{dy} * stride + dx)]; };
  const int base = 128 << (bit_depth_ - 8);
  uint16_t left_col[32];
  uint16_t above_data[64 + 16];
  uint16_t* above_row = above_data + 16;
  const int need = kExtendModes[mode];
  if (need & NEED_LEFT) {
    if (left_available) {
      if (mb_to_bottom_edge_ < 0 && y0 + bs > frame_height) {
        const int extend_bottom = frame_height - y0;
        int i = 0;
        for (; i < extend_bottom; ++i) left_col[i] = px(-1, i);
        for (; i < bs; ++i) left_col[i] = px(-1, extend_bottom - 1);
      } else {
        for (int i = 0; i < bs; ++i) left_col[i] = px(-1, i);
      }
    } else {
      for (int i = 0; i < bs; ++i) left_col[i] = static_cast<uint16_t>(base + 1);
    }
  }
  auto copy_above = [&](int count) {
    for (int i = 0; i < count; ++i) above_row[i] = px(i, -1);
  };
  if (need & NEED_ABOVE) {
    if (up_available) {
      if (mb_to_right_edge_ < 0) {
        if (x0 + bs <= frame_width) {
          copy_above(bs);
        } else if (x0 <= frame_width) {
          const int r = frame_width - x0;
          copy_above(r);
          for (int i = r; i < bs; ++i) above_row[i] = above_row[r - 1];
        }
      } else {
        copy_above(bs);
      }
      above_row[-1] = left_available ? px(-1, -1) : static_cast<uint16_t>(base + 1);
    } else {
      for (int i = 0; i < bs; ++i) above_row[i] = static_cast<uint16_t>(base - 1);
      above_row[-1] = static_cast<uint16_t>(base - 1);
    }
  }
  if (need & NEED_ABOVERIGHT) {
    if (up_available) {
      if (mb_to_right_edge_ < 0) {
        if (x0 + 2 * bs <= frame_width) {
          if (right_available && bs == 4) {
            copy_above(2 * bs);
          } else {
            copy_above(bs);
            for (int i = bs; i < 2 * bs; ++i) above_row[i] = above_row[bs - 1];
          }
        } else if (x0 + bs <= frame_width) {
          const int r = frame_width - x0;
          if (right_available && bs == 4) {
            copy_above(r);
            for (int i = r; i < 2 * bs; ++i) above_row[i] = above_row[r - 1];
          } else {
            copy_above(bs);
            for (int i = bs; i < 2 * bs; ++i) above_row[i] = above_row[bs - 1];
          }
        } else if (x0 <= frame_width) {
          const int r = frame_width - x0;
          copy_above(r);
          for (int i = r; i < 2 * bs; ++i) above_row[i] = above_row[r - 1];
        }
      } else {
        copy_above(bs);
        if (bs == 4 && right_available) {
          for (int i = bs; i < 2 * bs; ++i) above_row[i] = px(i, -1);
        } else {
          for (int i = bs; i < 2 * bs; ++i) above_row[i] = above_row[bs - 1];
        }
      }
      above_row[-1] = left_available ? px(-1, -1) : static_cast<uint16_t>(base + 1);
    } else {
      for (int i = 0; i < 2 * bs; ++i) above_row[i] = static_cast<uint16_t>(base - 1);
      above_row[-1] = static_cast<uint16_t>(base - 1);
    }
  }
  // The destination block (bounds-checked once).
  const uint64_t last = origin + static_cast<uint64_t>(bs - 1) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(bs);
  uint16_t* dst = pix.subview(origin, last - origin).data();
  const int maxv = (1 << bit_depth_) - 1;
  switch (mode) {
    case DC_PRED: {
      int sum = 0, count = 0;
      if (up_available) {
        for (int i = 0; i < bs; ++i) sum += above_row[i];
        count += bs;
      }
      if (left_available) {
        for (int i = 0; i < bs; ++i) sum += left_col[i];
        count += bs;
      }
      const int dc = count ? (sum + (count >> 1)) / count : base;
      for (int r = 0; r < bs; ++r)
        for (int c = 0; c < bs; ++c) dst[r * stride + c] = static_cast<uint16_t>(dc);
      break;
    }
    case V_PRED:
      for (int r = 0; r < bs; ++r)
        for (int c = 0; c < bs; ++c) dst[r * stride + c] = above_row[c];
      break;
    case H_PRED:
      for (int r = 0; r < bs; ++r)
        for (int c = 0; c < bs; ++c) dst[r * stride + c] = left_col[r];
      break;
    case TM_PRED: {
      const int ytop_left = above_row[-1];
      for (int r = 0; r < bs; ++r)
        for (int c = 0; c < bs; ++c) dst[r * stride + c] = static_cast<uint16_t>(clampi(left_col[r] + above_row[c] - ytop_left, 0, maxv));
      break;
    }
    default: predict_directional(mode, bs, dst, stride, above_row, left_col); break;
  }
}

// ---- Core: transforms -------------------------------------------------------------------------------------------------------------------------

void Core::inverse_transform_add(int plane, int tx_size, int tx_type, int eob, uint64_t dst_index) {
  (void)eob;
  Frame& f = frames_[new_fb_idx_];
  const int n = 4 << tx_size;
  const int stride = static_cast<int>(f.stride[plane]);
  ArrayView<uint16_t> pix = f.plane[plane];
  const uint64_t span = static_cast<uint64_t>(n - 1) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(n);
  uint16_t* dst = pix.subview(dst_index, span).data();
  if (lossless_) {
    iwht4x4_add(dqcoeff_, dst, stride, bit_depth_);
  } else {
    switch (tx_size) {
      case TX_4X4: {
        static const Transform1D kRows[4] = {vpx_highbd_idct4_c, vpx_highbd_idct4_c, vpx_highbd_iadst4_c, vpx_highbd_iadst4_c};
        static const Transform1D kCols[4] = {vpx_highbd_idct4_c, vpx_highbd_iadst4_c, vpx_highbd_idct4_c, vpx_highbd_iadst4_c};
        inverse_2d(kRows[tx_type], kCols[tx_type], 4, 4, dqcoeff_, dst, stride, bit_depth_);
        break;
      }
      case TX_8X8: {
        static const Transform1D kRows[4] = {vpx_highbd_idct8_c, vpx_highbd_idct8_c, vpx_highbd_iadst8_c, vpx_highbd_iadst8_c};
        static const Transform1D kCols[4] = {vpx_highbd_idct8_c, vpx_highbd_iadst8_c, vpx_highbd_idct8_c, vpx_highbd_iadst8_c};
        inverse_2d(kRows[tx_type], kCols[tx_type], 8, 5, dqcoeff_, dst, stride, bit_depth_);
        break;
      }
      case TX_16X16: {
        static const Transform1D kRows[4] = {vpx_highbd_idct16_c, vpx_highbd_idct16_c, vpx_highbd_iadst16_c, vpx_highbd_iadst16_c};
        static const Transform1D kCols[4] = {vpx_highbd_idct16_c, vpx_highbd_iadst16_c, vpx_highbd_idct16_c, vpx_highbd_iadst16_c};
        inverse_2d(kRows[tx_type], kCols[tx_type], 16, 6, dqcoeff_, dst, stride, bit_depth_);
        break;
      }
      default: inverse_2d(idct32_wrapper, idct32_wrapper, 32, 6, dqcoeff_, dst, stride, bit_depth_); break;
    }
  }
  const int count = 16 << (tx_size << 1);
  for (int i = 0; i < count; ++i) dqcoeff_[i] = 0;
}

// ---- Core: inter -------------------------------------------------------------------------------------------------------------------------------

namespace {

inline int scaled_x(int val, const ScaleFactors& sf) { return static_cast<int>((static_cast<int64_t>(val) * sf.x_scale_fp) >> 14); }
inline int scaled_y(int val, const ScaleFactors& sf) { return static_cast<int>((static_cast<int64_t>(val) * sf.y_scale_fp) >> 14); }

inline int round_mv_comp_q4(int value) { return (value < 0 ? value - 2 : value + 2) / 4; }
inline int round_mv_comp_q2(int value) { return (value < 0 ? value - 1 : value + 1) / 2; }

const int16_t (*filter_kernel(int interp_filter))[8] {
  switch (interp_filter) {
    case EIGHTTAP_SMOOTH: return sub_pel_filters_8lp;
    case EIGHTTAP_SHARP: return sub_pel_filters_8s;
    case BILINEAR: return bilinear_filters;
    default: return sub_pel_filters_8;
  }
}

}  // namespace

// One motion-compensated block of one plane (libvpx's dec_build_inter_predictors). (x, y) is the offset inside the current block in pixels of
// the plane, (w, h) the size to predict, (bw, bh) the size of the whole block of the plane.
void Core::predict_inter_block(int plane, int bw, int bh, int x, int y, int w, int h, const Mv& mv, const RefBuffer& ref_buf, int ref) {
  Frame& cur = frames_[new_fb_idx_];
  const Frame& rf = frames_[ref_buf.idx];
  const ScaleFactors& sf = ref_buf.sf;
  const int ss_x = plane_[plane].ss_x, ss_y = plane_[plane].ss_y;
  const int cur_stride = static_cast<int>(cur.stride[plane]);
  const uint64_t dst_origin = static_cast<uint64_t>(((cur_row_ * 8) >> ss_y) + y) * static_cast<uint64_t>(cur_stride) +
                              static_cast<uint64_t>(((cur_col_ * 8) >> ss_x) + x);
  const int frame_width = rf.crop_w[plane], frame_height = rf.crop_h[plane];
  const int ref_stride = static_cast<int>(rf.stride[plane]);
  const bool is_scaled = sf.scaled();
  int x0, y0, scaled_mv_row, scaled_mv_col, xs, ys;
  if (is_scaled) {
    const int spel_left = (4 + bw) << 4, spel_right = spel_left - 16, spel_top = (4 + bh) << 4, spel_bottom = spel_top - 16;
    int mv_row = mv.row * (1 << (1 - ss_y)), mv_col = mv.col * (1 << (1 - ss_x));
    mv_col = clampi(mv_col, mb_to_left_edge_ * (1 << (1 - ss_x)) - spel_left, mb_to_right_edge_ * (1 << (1 - ss_x)) + spel_right);
    mv_row = clampi(mv_row, mb_to_top_edge_ * (1 << (1 - ss_y)) - spel_top, mb_to_bottom_edge_ * (1 << (1 - ss_y)) + spel_bottom);
    const int x_start = -mb_to_left_edge_ >> (3 + ss_x), y_start = -mb_to_top_edge_ >> (3 + ss_y);
    x0 = scaled_x(x_start + x, sf);
    y0 = scaled_y(y_start + y, sf);
    const int mi_x = cur_col_ * 8, mi_y = cur_row_ * 8;
    const int x_off_q4 = scaled_x((mi_x + x) << 4, sf) & 15, y_off_q4 = scaled_y((mi_y + y) << 4, sf) & 15;
    scaled_mv_row = scaled_y(mv_row, sf) + y_off_q4;
    scaled_mv_col = scaled_x(mv_col, sf) + x_off_q4;
    xs = sf.x_step_q4;
    ys = sf.y_step_q4;
  } else {
    x0 = (-mb_to_left_edge_ >> (3 + ss_x)) + x;
    y0 = (-mb_to_top_edge_ >> (3 + ss_y)) + y;
    scaled_mv_row = mv.row * (1 << (1 - ss_y));
    scaled_mv_col = mv.col * (1 << (1 - ss_x));
    xs = ys = 16;
  }
  const int subpel_x = scaled_mv_col & 15, subpel_y = scaled_mv_row & 15;
  x0 += scaled_mv_col >> 4;
  y0 += scaled_mv_row >> 4;
  // The source patch (with the picture edge replicated) the filters read from.
  const int src_w = (((w - 1) * xs + subpel_x) >> 4) + 8;
  const int src_h = (((h - 1) * ys + subpel_y) >> 4) + 8;
  if (src_w > kPatchSize || src_h > kPatchSize) {
    corrupted_ = true;
    return;
  }
  ArrayView<const uint16_t> rpix(rf.plane[plane].data(), rf.plane[plane].size());
  for (int r = 0; r < src_h; ++r) {
    const int sy = clampi(y0 - 3 + r, 0, frame_height - 1);
    const uint64_t row = static_cast<uint64_t>(sy) * static_cast<uint64_t>(ref_stride);
    for (int c = 0; c < src_w; ++c) patch_[static_cast<uint64_t>(r) * kPatchSize + static_cast<uint64_t>(c)] = rpix[row + static_cast<uint64_t>(clampi(x0 - 3 + c, 0, frame_width - 1))];
  }
  const int16_t(*kernel)[8] = filter_kernel(cur_->interp_filter);
  const int maxv = (1 << bit_depth_) - 1;
  // Horizontal pass into temp_ (src_h rows), then vertical pass into the picture.
  for (int r = 0; r < src_h; ++r) {
    int x_q4 = subpel_x;
    for (int c = 0; c < w; ++c) {
      const uint16_t* s = &patch_[static_cast<uint64_t>(r) * kPatchSize + static_cast<uint64_t>(x_q4 >> 4)];
      const int16_t* k = kernel[x_q4 & 15];
      int sum = 0;
      for (int t = 0; t < 8; ++t) sum += s[t] * k[t];
      temp_[static_cast<uint64_t>(r) * 64 + static_cast<uint64_t>(c)] = static_cast<uint16_t>(clampi((sum + 64) >> 7, 0, maxv));
      x_q4 += xs;
    }
  }
  const uint64_t dst_span = static_cast<uint64_t>(h - 1) * static_cast<uint64_t>(cur_stride) + static_cast<uint64_t>(w);
  uint16_t* dst = cur.plane[plane].subview(dst_origin, dst_span).data();
  for (int c = 0; c < w; ++c) {
    int y_q4 = subpel_y;
    for (int r = 0; r < h; ++r) {
      const int16_t* k = kernel[y_q4 & 15];
      const uint16_t* s = &temp_[static_cast<uint64_t>(y_q4 >> 4) * 64 + static_cast<uint64_t>(c)];
      int sum = 0;
      for (int t = 0; t < 8; ++t) sum += s[t * 64] * k[t];
      const int v = clampi((sum + 64) >> 7, 0, maxv);
      uint16_t& d = dst[r * cur_stride + c];
      d = static_cast<uint16_t>(ref ? (d + v + 1) >> 1 : v);
      y_q4 += ys;
    }
  }
}

void Core::build_inter_predictors(int mi_row, int mi_col) {
  (void)mi_row;
  (void)mi_col;
  const ModeInfo* mi = cur_;
  const bool is_compound = mi->has_second_ref();
  for (int ref = 0; ref < 1 + is_compound; ++ref) {
    const RefBuffer& rb = frame_refs_[mi->ref_frame[ref] - LAST_FRAME];
    if (!rb.sf.valid() || rb.idx < 0) {
      corrupted_ = true;
      return;
    }
    for (int plane = 0; plane < 3; ++plane) {
      const PlaneInfo& pd = plane_[plane];
      const int n4w_x4 = 4 * pd.n4_w, n4h_x4 = 4 * pd.n4_h;
      if (mi->sb_type < BLOCK_8X8) {
        int i = 0;
        for (int y = 0; y < pd.n4_h; ++y) {
          for (int x = 0; x < pd.n4_w; ++x) {
            const int block = i++;
            const int ss_idx = ((pd.ss_x > 0) << 1) | (pd.ss_y > 0);
            const SubBlockInfo* b = mi->bmi;
            Mv mv;
            auto pred_q2 = [&](int b0, int b1) {
              mv.row = static_cast<int16_t>(round_mv_comp_q2(b[b0].mv[ref].row + b[b1].mv[ref].row));
              mv.col = static_cast<int16_t>(round_mv_comp_q2(b[b0].mv[ref].col + b[b1].mv[ref].col));
            };
            switch (ss_idx) {
              case 0: mv = b[block].mv[ref]; break;
              case 1: pred_q2(block, block + 2); break;
              case 2: pred_q2(block, block + 1); break;
              default:
                mv.row = static_cast<int16_t>(round_mv_comp_q4(b[0].mv[ref].row + b[1].mv[ref].row + b[2].mv[ref].row + b[3].mv[ref].row));
                mv.col = static_cast<int16_t>(round_mv_comp_q4(b[0].mv[ref].col + b[1].mv[ref].col + b[2].mv[ref].col + b[3].mv[ref].col));
                break;
            }
            predict_inter_block(plane, n4w_x4, n4h_x4, 4 * x, 4 * y, 4, 4, mv, rb, ref);
            if (corrupted_) return;
          }
        }
      } else {
        predict_inter_block(plane, n4w_x4, n4h_x4, 0, 0, n4w_x4, n4h_x4, mi->mv[ref], rb, ref);
        if (corrupted_) return;
      }
    }
  }
}

// ---- Core: the block loops -----------------------------------------------------------------------------------------------------------------------

void Core::reconstruct_block(BoolDecoder& r, int mi_row, int mi_col, int bsize) {
  ModeInfo* mi = cur_;
  Frame& f = frames_[new_fb_idx_];
  const bool less8x8 = bsize < BLOCK_8X8;
  auto uv_tx_size = [&](int plane) {
    return plane ? static_cast<int>(uv_txsize_lookup[mi->sb_type][mi->tx_size][ss_x_][ss_y_]) : static_cast<int>(mi->tx_size);
  };
  auto setup_plane = [&](int plane, int* tx_size, int* max_w, int* max_h) {
    const PlaneInfo& pd = plane_[plane];
    *tx_size = uv_tx_size(plane);
    *max_w = pd.n4_w + (mb_to_right_edge_ >= 0 ? 0 : mb_to_right_edge_ >> (5 + pd.ss_x));
    *max_h = pd.n4_h + (mb_to_bottom_edge_ >= 0 ? 0 : mb_to_bottom_edge_ >> (5 + pd.ss_y));
    max_blocks_wide_ = mb_to_right_edge_ >= 0 ? 0 : *max_w;
    max_blocks_high_ = mb_to_bottom_edge_ >= 0 ? 0 : *max_h;
  };
  auto dst_index = [&](int plane, int row, int col) {
    const PlaneInfo& pd = plane_[plane];
    return static_cast<uint64_t>(((mi_row * 8) >> pd.ss_y) + 4 * row) * f.stride[plane] + static_cast<uint64_t>(((mi_col * 8) >> pd.ss_x) + 4 * col);
  };
  if (!mi->is_inter()) {
    for (int plane = 0; plane < 3; ++plane) {
      int tx_size, max_w, max_h;
      setup_plane(plane, &tx_size, &max_w, &max_h);
      const int step = 1 << tx_size;
      for (int row = 0; row < max_h; row += step) {
        for (int col = 0; col < max_w; col += step) {
          int mode = plane == 0 ? mi->mode : mi->uv_mode;
          if (mi->sb_type < BLOCK_8X8 && plane == 0) mode = mi->bmi[(row << 1) + col].mode;
          predict_intra_block(plane, tx_size, mode, col, row);
          if (!mi->skip) {
            const int tx_type = (plane || lossless_) ? DCT_DCT : intra_mode_to_tx_type_lookup[mode];
            const int eob = decode_block_tokens(r, plane, tx_size, tx_type, col, row, mi->segment_id);
            if (eob > 0) inverse_transform_add(plane, tx_size, tx_type, eob, dst_index(plane, row, col));
          }
        }
      }
    }
    return;
  }
  build_inter_predictors(mi_row, mi_col);
  if (corrupted_ || mi->skip) return;
  int eobtotal = 0;
  for (int plane = 0; plane < 3; ++plane) {
    int tx_size, max_w, max_h;
    setup_plane(plane, &tx_size, &max_w, &max_h);
    const int step = 1 << tx_size;
    for (int row = 0; row < max_h; row += step) {
      for (int col = 0; col < max_w; col += step) {
        const int eob = decode_block_tokens(r, plane, tx_size, DCT_DCT, col, row, mi->segment_id);
        if (eob > 0) inverse_transform_add(plane, tx_size, DCT_DCT, eob, dst_index(plane, row, col));
        eobtotal += eob;
      }
    }
  }
  if (!less8x8 && eobtotal == 0) mi->skip = 1;
}

}  // namespace vp9
}  // namespace hardware
