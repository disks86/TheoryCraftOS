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


// H.265 decoding, part 4: intra prediction (8.4.4.2), inter prediction (8.5.3.3), and scaling, inverse transform and reconstruction of residual blocks (8.6).
#include "Hardware/VideoCodec/Software/H265Core.hpp"

namespace hardware {
namespace h265 {

namespace {
constexpr int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
constexpr int imin(int a, int b) { return a < b ? a : b; }
constexpr int iabs(int a) { return a < 0 ? -a : a; }
inline uint8_t clip_pixel(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

const int8_t kTransform[32][32] =
#include "Hardware/VideoCodec/Software/H265Transform.inc"
    ;
const int8_t kDst4[4][4] = {{29, 55, 74, 84}, {74, 74, 0, -74}, {84, -29, -74, 55}, {55, -84, 74, -29}};

// Table 8-5 / 8-6
const int8_t kIntraPredAngle[35] = {0, 0, 32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26, -32, -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32};
const int16_t kInvAngle[35] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -4096, -1638, -910, -630, -482, -390, -315, -256, -315, -390, -482, -630, -910, -1638, -4096, 0, 0, 0, 0, 0, 0, 0, 0, 0};

const int8_t kLumaFilter[4][8] = {{0, 0, 0, 64, 0, 0, 0, 0}, {-1, 4, -10, 58, 17, -5, 1, 0}, {-1, 4, -11, 40, 40, -11, 4, -1}, {0, 1, -5, 17, 58, -10, 4, -1}};
const int8_t kChromaFilter[8][4] = {{0, 64, 0, 0}, {-2, 58, 10, -2}, {-4, 54, 16, -2}, {-6, 46, 28, -4}, {-4, 36, 36, -4}, {-4, 28, 46, -6}, {-2, 16, 54, -4}, {-2, 10, 58, -2}};
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Intra prediction

void Core::predict_intra(int x0, int y0, int log2_size, int c_idx, int mode) {
  const Sps& sps = *sp_;
  const int n = 1 << log2_size;
  const int f = c_idx ? 2 : 1;
  Picture* pic = cur_;
  uint8_t* plane = pic->plane[c_idx].data();
  const int stride = pic->stride[c_idx];
  // reference samples: ref[0 .. 2n-1] is the left column from the bottom (p[-1][2n-1]) up to p[-1][0], ref[2n] is the corner, ref[2n+1 .. 4n] the top row p[0..2n-1][-1]
  uint8_t ref[4 * 32 * 2 + 1];
  bool avail[4 * 32 * 2 + 1];
  const int total = 4 * n + 1;
  const int unit = c_idx ? 2 : 4;   // samples covered by one 4x4 luma block
  const int x_cur = x0 * f, y_cur = y0 * f;
  auto sample_ok = [&](int xn, int yn) {
    if (!available(x_cur, y_cur, xn * f, yn * f)) return false;
    if (pp_->constrained_intra_pred && (pred_mode_[static_cast<uint64_t>((yn * f) >> 2) * w4_ + ((xn * f) >> 2)] & 3) != 1) return false;
    return true;
  };
  bool any = false;
  for (int i = 0; i < 2 * n; i += unit) {   // left column, bottom to top: index i corresponds to row 2n-1-i
    const int yn = y0 + 2 * n - 1 - i;
    const bool ok = sample_ok(x0 - 1, yn);
    for (int k = 0; k < unit && i + k < 2 * n; ++k) {
      avail[i + k] = ok;
      if (ok) ref[i + k] = plane[static_cast<uint64_t>(yn - k) * stride + x0 - 1];
    }
    any = any || ok;
  }
  {
    const bool ok = sample_ok(x0 - 1, y0 - 1);
    avail[2 * n] = ok;
    if (ok) ref[2 * n] = plane[static_cast<uint64_t>(y0 - 1) * stride + x0 - 1];
    any = any || ok;
  }
  for (int i = 0; i < 2 * n; i += unit) {
    const bool ok = sample_ok(x0 + i, y0 - 1);
    for (int k = 0; k < unit && i + k < 2 * n; ++k) {
      avail[2 * n + 1 + i + k] = ok;
      if (ok) ref[2 * n + 1 + i + k] = plane[static_cast<uint64_t>(y0 - 1) * stride + x0 + i + k];
    }
    any = any || ok;
  }
  // 8.4.4.2.2: substitution of unavailable samples
  if (!any) {
    for (int i = 0; i < total; ++i) ref[i] = 128;
  } else {
    if (!avail[0]) {
      int j = 1;
      while (j < total && !avail[j]) ++j;
      ref[0] = ref[j];
    }
    for (int i = 1; i < total; ++i)
      if (!avail[i]) ref[i] = ref[i - 1];
  }
  // p(-1, y) = left[y]: ref[2n-1-y]; p(x, -1) = top[x]: ref[2n+1+x]; p(-1,-1) = ref[2n]
  uint8_t left[65], top[65];   // index 0 is the corner, 1..2n the samples
  left[0] = top[0] = ref[2 * n];
  for (int i = 0; i < 2 * n; ++i) {
    left[1 + i] = ref[2 * n - 1 - i];
    top[1 + i] = ref[2 * n + 1 + i];
  }
  // 8.4.4.2.3: filtering of neighbouring samples
  if (c_idx == 0 && mode != 1 && n != 4) {
    const int dist = imin(iabs(mode - 26), iabs(mode - 10));
    const int thres = n == 8 ? 7 : n == 16 ? 1 : 0;
    if (dist > thres) {
      uint8_t fl[65], ft[65];
      if (sps.strong_intra_smoothing && n == 32 && iabs(top[0] + top[2 * n] - 2 * top[n]) < 8 && iabs(left[0] + left[2 * n] - 2 * left[n]) < 8) {
        fl[0] = ft[0] = left[0];
        for (int i = 0; i < 63; ++i) {
          fl[1 + i] = static_cast<uint8_t>(((63 - i) * left[0] + (i + 1) * left[64] + 32) >> 6);
          ft[1 + i] = static_cast<uint8_t>(((63 - i) * top[0] + (i + 1) * top[64] + 32) >> 6);
        }
        fl[64] = left[64];
        ft[64] = top[64];
      } else {
        fl[0] = ft[0] = static_cast<uint8_t>((left[1] + 2 * left[0] + top[1] + 2) >> 2);
        for (int i = 1; i < 2 * n; ++i) {
          fl[i] = static_cast<uint8_t>((left[i + 1] + 2 * left[i] + left[i - 1] + 2) >> 2);
          ft[i] = static_cast<uint8_t>((top[i + 1] + 2 * top[i] + top[i - 1] + 2) >> 2);
        }
        // (the neighbour of index 1 on the corner side is the corner itself)
        fl[1] = static_cast<uint8_t>((left[2] + 2 * left[1] + left[0] + 2) >> 2);
        ft[1] = static_cast<uint8_t>((top[2] + 2 * top[1] + top[0] + 2) >> 2);
        fl[2 * n] = left[2 * n];
        ft[2 * n] = top[2 * n];
      }
      for (int i = 0; i <= 2 * n; ++i) { left[i] = fl[i]; top[i] = ft[i]; }
    }
  }
  auto L = [&](int y) { return static_cast<int>(left[y + 1]); };   // p[-1][y], y = -1 .. 2n-1
  auto T = [&](int x) { return static_cast<int>(top[x + 1]); };    // p[x][-1]
  uint8_t* dst = plane + static_cast<uint64_t>(y0) * stride + x0;
  if (mode == 0) {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        dst[static_cast<uint64_t>(y) * stride + x] = static_cast<uint8_t>(((n - 1 - x) * L(y) + (x + 1) * T(n) + (n - 1 - y) * T(x) + (y + 1) * L(n) + n) >> (log2_size + 1));
  } else if (mode == 1) {
    int sum = n;
    for (int i = 0; i < n; ++i) sum += T(i) + L(i);
    const int dc = sum >> (log2_size + 1);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) dst[static_cast<uint64_t>(y) * stride + x] = static_cast<uint8_t>(dc);
    if (c_idx == 0 && n < 32) {
      dst[0] = static_cast<uint8_t>((L(0) + 2 * dc + T(0) + 2) >> 2);
      for (int x = 1; x < n; ++x) dst[x] = static_cast<uint8_t>((T(x) + 3 * dc + 2) >> 2);
      for (int y = 1; y < n; ++y) dst[static_cast<uint64_t>(y) * stride] = static_cast<uint8_t>((L(y) + 3 * dc + 2) >> 2);
    }
  } else {
    const int angle = kIntraPredAngle[mode];
    int refbuf[3 * 32 + 3];
    int* r = refbuf + 32 + 1;   // r[-n .. 2n]
    if (mode >= 18) {
      for (int x = 0; x <= n; ++x) r[x] = T(x - 1);
      if (angle < 0) {
        const int last = (n * angle) >> 5;
        if (last < -1)
          for (int x = last; x <= -1; ++x) r[x] = L(-1 + ((x * kInvAngle[mode] + 128) >> 8));
      } else {
        for (int x = n + 1; x <= 2 * n; ++x) r[x] = T(x - 1);
      }
      for (int y = 0; y < n; ++y) {
        const int idx = ((y + 1) * angle) >> 5, fact = ((y + 1) * angle) & 31;
        for (int x = 0; x < n; ++x)
          dst[static_cast<uint64_t>(y) * stride + x] = static_cast<uint8_t>(fact ? ((32 - fact) * r[x + idx + 1] + fact * r[x + idx + 2] + 16) >> 5 : r[x + idx + 1]);
      }
      if (mode == 26 && c_idx == 0 && n < 32)
        for (int y = 0; y < n; ++y) dst[static_cast<uint64_t>(y) * stride] = clip_pixel(T(0) + ((L(y) - L(-1)) >> 1));
    } else {
      for (int x = 0; x <= n; ++x) r[x] = L(x - 1);
      if (angle < 0) {
        const int last = (n * angle) >> 5;
        if (last < -1)
          for (int x = last; x <= -1; ++x) r[x] = T(-1 + ((x * kInvAngle[mode] + 128) >> 8));
      } else {
        for (int x = n + 1; x <= 2 * n; ++x) r[x] = L(x - 1);
      }
      for (int x = 0; x < n; ++x) {
        const int idx = ((x + 1) * angle) >> 5, fact = ((x + 1) * angle) & 31;
        for (int y = 0; y < n; ++y)
          dst[static_cast<uint64_t>(y) * stride + x] = static_cast<uint8_t>(fact ? ((32 - fact) * r[y + idx + 1] + fact * r[y + idx + 2] + 16) >> 5 : r[y + idx + 1]);
      }
      if (mode == 10 && c_idx == 0 && n < 32)
        for (int x = 0; x < n; ++x) dst[x] = clip_pixel(L(0) + ((T(x) - T(-1)) >> 1));
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Inter prediction

namespace {
// A block of the reference plane with the borders replicated, as 8.5.3.3.3 reads outside the picture.
void fetch_patch(const uint8_t* plane, int stride, int pw, int ph, int x, int y, int w, int h, uint8_t* out, int out_stride) {
  for (int j = 0; j < h; ++j) {
    const int yy = clip3(0, ph - 1, y + j);
    for (int i = 0; i < w; ++i) out[j * out_stride + i] = plane[static_cast<uint64_t>(yy) * stride + clip3(0, pw - 1, x + i)];
  }
}

// Luma sample interpolation (8.5.3.3.3.1): the 14-bit intermediate prediction of a w x h block at integer position (xi, yi) with quarter-sample fraction (fx, fy).
void interpolate_luma(const uint8_t* plane, int stride, int pw, int ph, int xi, int yi, int fx, int fy, int w, int h, int16_t* out) {
  uint8_t patch[(64 + 7) * (64 + 7)];
  const int ps = w + 7;
  fetch_patch(plane, stride, pw, ph, xi - 3, yi - 3, w + 7, h + 7, patch, ps);
  if (fx == 0 && fy == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) out[y * w + x] = static_cast<int16_t>(patch[(y + 3) * ps + x + 3] << 6);
  } else if (fy == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < 8; ++k) s += kLumaFilter[fx][k] * patch[(y + 3) * ps + x + k];
        out[y * w + x] = static_cast<int16_t>(s);
      }
  } else if (fx == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < 8; ++k) s += kLumaFilter[fy][k] * patch[(y + k) * ps + x + 3];
        out[y * w + x] = static_cast<int16_t>(s);
      }
  } else {
    int16_t tmp[(64 + 7) * 64];
    for (int y = 0; y < h + 7; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < 8; ++k) s += kLumaFilter[fx][k] * patch[y * ps + x + k];
        tmp[y * w + x] = static_cast<int16_t>(s);
      }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < 8; ++k) s += kLumaFilter[fy][k] * tmp[(y + k) * w + x];
        out[y * w + x] = static_cast<int16_t>(s >> 6);
      }
  }
}

// Chroma sample interpolation (8.5.3.3.3.2): eighth-sample fractions.
void interpolate_chroma(const uint8_t* plane, int stride, int pw, int ph, int xi, int yi, int fx, int fy, int w, int h, int16_t* out) {
  uint8_t patch[(32 + 3) * (32 + 3)];
  const int ps = w + 3;
  fetch_patch(plane, stride, pw, ph, xi - 1, yi - 1, w + 3, h + 3, patch, ps);
  if (fx == 0 && fy == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) out[y * w + x] = static_cast<int16_t>(patch[(y + 1) * ps + x + 1] << 6);
  } else if (fy == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < 4; ++k) s += kChromaFilter[fx][k] * patch[(y + 1) * ps + x + k];
        out[y * w + x] = static_cast<int16_t>(s);
      }
  } else if (fx == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < 4; ++k) s += kChromaFilter[fy][k] * patch[(y + k) * ps + x + 1];
        out[y * w + x] = static_cast<int16_t>(s);
      }
  } else {
    int16_t tmp[(32 + 3) * 32];
    for (int y = 0; y < h + 3; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < 4; ++k) s += kChromaFilter[fx][k] * patch[y * ps + x + k];
        tmp[y * w + x] = static_cast<int16_t>(s);
      }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < 4; ++k) s += kChromaFilter[fy][k] * tmp[(y + k) * w + x];
        out[y * w + x] = static_cast<int16_t>(s >> 6);
      }
  }
}
}  // namespace

void Core::predict_inter(int x0, int y0, int w, int h, const MvField& m) {
  const Sps& sps = *sp_;
  const bool weighted = sh_.type == kSliceP ? pp_->weighted_pred : (sh_.type == kSliceB ? pp_->weighted_bipred : false);
  for (int c = 0; c < 3; ++c) {
    const int cw = c ? w / 2 : w, ch = c ? h / 2 : h;
    const int cx = c ? x0 / 2 : x0, cy = c ? y0 / 2 : y0;
    const int pw = c ? sps.width / 2 : sps.width, ph = c ? sps.height / 2 : sps.height;
    int16_t* pred[2] = {pred_buf_[0], pred_buf_[1]};
    int lists[2], used = 0;
    for (int l = 0; l < 2; ++l) {
      if (!(m.pred_flags & (1 << l))) continue;
      const Picture* ref = slice_->ref_pic[l][m.ref_idx[l]];
      if (!ref) continue;
      if (c == 0) {
        interpolate_luma(ref->plane[0].data(), ref->stride[0], pw, ph, x0 + (m.mv[l][0] >> 2), y0 + (m.mv[l][1] >> 2), m.mv[l][0] & 3, m.mv[l][1] & 3, w, h, pred[used]);
      } else {
        interpolate_chroma(ref->plane[c].data(), ref->stride[c], pw, ph, cx + (m.mv[l][0] >> 3), cy + (m.mv[l][1] >> 3), m.mv[l][0] & 7, m.mv[l][1] & 7, cw, ch, pred[used]);
      }
      lists[used++] = l;
    }
    Picture* cur = cur_;
    uint8_t* dst = cur->plane[c].data() + static_cast<uint64_t>(cy) * cur->stride[c] + cx;
    const int stride = cur->stride[c];
    if (used == 0) continue;
    auto weight = [&](int idx, int* wt, int* off) {
      const int l = lists[idx], r = m.ref_idx[l];
      if (c == 0) { *wt = sh_.luma_weight[l][r]; *off = sh_.luma_offset[l][r]; }
      else { *wt = sh_.chroma_weight[l][r][c - 1]; *off = sh_.chroma_offset[l][r][c - 1]; }
    };
    const int denom = c == 0 ? sh_.luma_log2_denom : sh_.chroma_log2_denom;
    if (used == 1) {
      if (!weighted) {
        for (int y = 0; y < ch; ++y)
          for (int x = 0; x < cw; ++x) dst[static_cast<uint64_t>(y) * stride + x] = clip_pixel((pred[0][y * cw + x] + 32) >> 6);
      } else {
        int wt, off;
        weight(0, &wt, &off);
        const int log2wd = denom + 6;
        for (int y = 0; y < ch; ++y)
          for (int x = 0; x < cw; ++x) {
            const int p = pred[0][y * cw + x];
            dst[static_cast<uint64_t>(y) * stride + x] = clip_pixel(log2wd >= 1 ? ((p * wt + (1 << (log2wd - 1))) >> log2wd) + off : p * wt + off);
          }
      }
    } else {
      if (!weighted) {
        for (int y = 0; y < ch; ++y)
          for (int x = 0; x < cw; ++x) dst[static_cast<uint64_t>(y) * stride + x] = clip_pixel((pred[0][y * cw + x] + pred[1][y * cw + x] + 64) >> 7);
      } else {
        int w0, o0, w1, o1;
        weight(0, &w0, &o0);
        weight(1, &w1, &o1);
        const int log2wd = denom + 6;
        for (int y = 0; y < ch; ++y)
          for (int x = 0; x < cw; ++x)
            dst[static_cast<uint64_t>(y) * stride + x] = clip_pixel((pred[0][y * cw + x] * w0 + pred[1][y * cw + x] * w1 + ((o0 + o1 + 1) << log2wd)) >> (log2wd + 1));
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Scaling, transformation and reconstruction (8.6.2 - 8.6.4)

void Core::reconstruct_block(int x0, int y0, int log2_size, int c_idx) {
  const Sps& sps = *sp_;
  const int n = 1 << log2_size;
  Picture* pic = cur_;
  const int stride = pic->stride[c_idx];
  uint8_t* dst = pic->plane[c_idx].data() + static_cast<uint64_t>(y0) * stride + x0;
  if (cu_transquant_bypass_) {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) dst[static_cast<uint64_t>(y) * stride + x] = clip_pixel(dst[static_cast<uint64_t>(y) * stride + x] + coeff_[y * n + x]);
    return;
  }
  // 8.6.4.2: scaling
  static const int kLevelScale[6] = {40, 45, 51, 57, 64, 72};
  const int qp = c_idx == 0 ? qp_y_ : chroma_qp(c_idx);
  const int bd_shift = log2_size + 3;
  const int64_t scale = static_cast<int64_t>(kLevelScale[qp % 6]) << (qp / 6);
  const bool use_list = sps.scaling_list_enabled && !(ts_flag_ && n > 4);
  const ScalingList* sl = nullptr;
  int size_id = log2_size - 2, matrix_id = 0;
  if (use_list) {
    sl = pp_->scaling_list_present ? &pp_->scaling : &sps.scaling;
    matrix_id = (cur_intra_ ? 0 : 3) + c_idx;
  }
  int32_t* d = resid_;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const int c = coeff_[y * n + x];
      if (c == 0) { d[y * n + x] = 0; continue; }
      int mfac = 16;
      if (use_list) {
        if (size_id == 0) mfac = sl->list[0][matrix_id][y * 4 + x];
        else if (size_id == 1) mfac = sl->list[1][matrix_id][y * 8 + x];
        else if (x == 0 && y == 0) mfac = sl->dc[size_id][matrix_id];
        else if (size_id == 2) mfac = sl->list[2][matrix_id][(y >> 1) * 8 + (x >> 1)];
        else mfac = sl->list[3][matrix_id][(y >> 2) * 8 + (x >> 2)];
      }
      const int64_t v = (static_cast<int64_t>(c) * mfac * scale + (int64_t{1} << (bd_shift - 1))) >> bd_shift;
      d[y * n + x] = static_cast<int32_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
  int32_t* r = coeff_;   // the coefficient levels are no longer needed: reuse the array for the residual
  if (ts_flag_) {
    for (int i = 0; i < n * n; ++i) r[i] = ((d[i] << 7) + 2048) >> 12;
  } else {
    const bool dst_matrix = cur_intra_ && c_idx == 0 && n == 4;
    const int step = 32 / n;
    int32_t g[32 * 32];
    // first stage: columns (vertical), then clipped; second stage: rows (horizontal)
    for (int x = 0; x < n; ++x) {
      for (int y = 0; y < n; ++y) {
        int64_t e = 0;
        for (int j = 0; j < n; ++j) {
          const int cj = d[j * n + x];
          if (cj == 0) continue;
          e += static_cast<int64_t>(dst_matrix ? kDst4[j][y] : kTransform[j * step][y]) * cj;
        }
        const int64_t t = (e + 64) >> 7;
        g[y * n + x] = static_cast<int32_t>(t < -32768 ? -32768 : t > 32767 ? 32767 : t);
      }
    }
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        int64_t e = 0;
        for (int j = 0; j < n; ++j) e += static_cast<int64_t>(dst_matrix ? kDst4[j][x] : kTransform[j * step][x]) * g[y * n + j];
        r[y * n + x] = static_cast<int32_t>((e + 2048) >> 12);
      }
  }
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) dst[static_cast<uint64_t>(y) * stride + x] = clip_pixel(dst[static_cast<uint64_t>(y) * stride + x] + r[y * n + x]);
}

}  // namespace h265
}  // namespace hardware
