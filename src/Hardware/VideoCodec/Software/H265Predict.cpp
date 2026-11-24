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


// H.265 decoding, part 4: intra prediction (8.4.4.2), inter prediction (8.5.3.3), and scaling, inverse transform and reconstruction of residual blocks (8.6). Every sample
// path takes the bit depth: 8-bit pictures go through the uint8_t instantiation, where it is the constant 8; deeper ones (up to 16 bits, with the range extension's
// extended precision: CoeffMin / CoeffMax, the scaling and transform shifts) through the uint16_t one. Chroma follows the chroma format (SubWidthC / SubHeightC).
#include "Hardware/VideoCodec/Software/H265Core.hpp"

namespace hardware {
namespace h265 {

namespace {
constexpr int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
constexpr int imin(int a, int b) { return a < b ? a : b; }
constexpr int iabs(int a) { return a < 0 ? -a : a; }
// The bit depth of a component for sample type Pel: 8 for 8-bit pictures (a constant, so the 8-bit paths fold it away), the SPS's otherwise.
template <typename Pel>
constexpr int depth_of(int sps_depth) { return sizeof(Pel) == 1 ? 8 : sps_depth; }
template <typename Pel>
inline Pel clip_sample(int v, int max_value) { return static_cast<Pel>(v < 0 ? 0 : v > max_value ? max_value : v); }

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
  if (cur_->deep) predict_intra_px<uint16_t>(x0, y0, log2_size, c_idx, mode);
  else predict_intra_px<uint8_t>(x0, y0, log2_size, c_idx, mode);
}

template <typename Pel>
void Core::predict_intra_px(int x0, int y0, int log2_size, int c_idx, int mode) {
  const Sps& sps = *sp_;
  const int bd = depth_of<Pel>(c_idx ? bd_c_ : bd_y_);
  const int max_value = (1 << bd) - 1;
  const int n = 1 << log2_size;
  const int fx = c_idx ? sub_w_ : 0, fy = c_idx ? sub_h_ : 0;   // log2 of SubWidthC / SubHeightC for chroma
  Picture* pic = cur_;
  Pel* plane = samples<Pel>(pic->plane[c_idx]);
  const int stride = pic->stride[c_idx] / static_cast<int>(sizeof(Pel));
  // reference samples: ref[0 .. 2n-1] is the left column from the bottom (p[-1][2n-1]) up to p[-1][0], ref[2n] is the corner, ref[2n+1 .. 4n] the top row p[0..2n-1][-1]
  Pel ref[4 * 32 * 2 + 1];
  bool avail[4 * 32 * 2 + 1];
  const int total = 4 * n + 1;
  const int unit_x = 4 >> fx, unit_y = 4 >> fy;   // samples covered by one 4x4 luma block, across and down
  const int x_cur = x0 << fx, y_cur = y0 << fy;
  auto sample_ok = [&](int xn, int yn) {
    if (!available(x_cur, y_cur, xn << fx, yn << fy)) return false;
    if (pp_->constrained_intra_pred && (pred_mode_[static_cast<uint64_t>((yn << fy) >> 2) * w4_ + ((xn << fx) >> 2)] & 3) != 1) return false;
    return true;
  };
  bool any = false;
  for (int i = 0; i < 2 * n; i += unit_y) {   // left column, bottom to top: index i corresponds to row 2n-1-i
    const int yn = y0 + 2 * n - 1 - i;
    const bool ok = sample_ok(x0 - 1, yn);
    for (int k = 0; k < unit_y && i + k < 2 * n; ++k) {
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
  for (int i = 0; i < 2 * n; i += unit_x) {
    const bool ok = sample_ok(x0 + i, y0 - 1);
    for (int k = 0; k < unit_x && i + k < 2 * n; ++k) {
      avail[2 * n + 1 + i + k] = ok;
      if (ok) ref[2 * n + 1 + i + k] = plane[static_cast<uint64_t>(y0 - 1) * stride + x0 + i + k];
    }
    any = any || ok;
  }
  // 8.4.4.2.2: substitution of unavailable samples (none there: 1 << (bitDepth - 1))
  if (!any) {
    for (int i = 0; i < total; ++i) ref[i] = static_cast<Pel>(1 << (bd - 1));
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
  Pel left[65], top[65];   // index 0 is the corner, 1..2n the samples
  left[0] = top[0] = ref[2 * n];
  for (int i = 0; i < 2 * n; ++i) {
    left[1 + i] = ref[2 * n - 1 - i];
    top[1 + i] = ref[2 * n + 1 + i];
  }
  // 8.4.4.2.3: filtering of neighbouring samples, of luma and of 4:4:4 chroma (not with intra_smoothing_disabled_flag); the strong filter is luma's alone
  if ((c_idx == 0 || cat_ == 3) && !sps.intra_smoothing_disabled && mode != 1 && n != 4) {
    const int dist = imin(iabs(mode - 26), iabs(mode - 10));
    const int thres = n == 8 ? 7 : n == 16 ? 1 : 0;
    if (dist > thres) {
      Pel fl[65], ft[65];
      const int smooth_limit = 1 << (bd - 5);   // (8 at 8 bits)
      if (sps.strong_intra_smoothing && c_idx == 0 && n == 32 && iabs(top[0] + top[2 * n] - 2 * top[n]) < smooth_limit && iabs(left[0] + left[2 * n] - 2 * left[n]) < smooth_limit) {
        fl[0] = ft[0] = left[0];
        for (int i = 0; i < 63; ++i) {
          fl[1 + i] = static_cast<Pel>(((63 - i) * left[0] + (i + 1) * left[64] + 32) >> 6);
          ft[1 + i] = static_cast<Pel>(((63 - i) * top[0] + (i + 1) * top[64] + 32) >> 6);
        }
        fl[64] = left[64];
        ft[64] = top[64];
      } else {
        fl[0] = ft[0] = static_cast<Pel>((left[1] + 2 * left[0] + top[1] + 2) >> 2);
        for (int i = 1; i < 2 * n; ++i) {
          fl[i] = static_cast<Pel>((left[i + 1] + 2 * left[i] + left[i - 1] + 2) >> 2);
          ft[i] = static_cast<Pel>((top[i + 1] + 2 * top[i] + top[i - 1] + 2) >> 2);
        }
        // (the neighbour of index 1 on the corner side is the corner itself)
        fl[1] = static_cast<Pel>((left[2] + 2 * left[1] + left[0] + 2) >> 2);
        ft[1] = static_cast<Pel>((top[2] + 2 * top[1] + top[0] + 2) >> 2);
        fl[2 * n] = left[2 * n];
        ft[2 * n] = top[2 * n];
      }
      for (int i = 0; i <= 2 * n; ++i) { left[i] = fl[i]; top[i] = ft[i]; }
    }
  }
  auto L = [&](int y) { return static_cast<int>(left[y + 1]); };   // p[-1][y], y = -1 .. 2n-1
  auto T = [&](int x) { return static_cast<int>(top[x + 1]); };    // p[x][-1]
  Pel* dst = plane + static_cast<uint64_t>(y0) * stride + x0;
  // disableIntraBoundaryFilter (8.4.4.2.6): the edge filters of the vertical and horizontal modes are off for lossless blocks when implicit RDPCM is on
  const bool boundary_filter = c_idx == 0 && n < 32 && !(sps.implicit_rdpcm && cu_transquant_bypass_);
  if (mode == 0) {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        dst[static_cast<uint64_t>(y) * stride + x] = static_cast<Pel>(((n - 1 - x) * L(y) + (x + 1) * T(n) + (n - 1 - y) * T(x) + (y + 1) * L(n) + n) >> (log2_size + 1));
  } else if (mode == 1) {
    int sum = n;
    for (int i = 0; i < n; ++i) sum += T(i) + L(i);
    const int dc = sum >> (log2_size + 1);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) dst[static_cast<uint64_t>(y) * stride + x] = static_cast<Pel>(dc);
    if (c_idx == 0 && n < 32) {
      dst[0] = static_cast<Pel>((L(0) + 2 * dc + T(0) + 2) >> 2);
      for (int x = 1; x < n; ++x) dst[x] = static_cast<Pel>((T(x) + 3 * dc + 2) >> 2);
      for (int y = 1; y < n; ++y) dst[static_cast<uint64_t>(y) * stride] = static_cast<Pel>((L(y) + 3 * dc + 2) >> 2);
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
          dst[static_cast<uint64_t>(y) * stride + x] = static_cast<Pel>(fact ? ((32 - fact) * r[x + idx + 1] + fact * r[x + idx + 2] + 16) >> 5 : r[x + idx + 1]);
      }
      if (mode == 26 && boundary_filter)
        for (int y = 0; y < n; ++y) dst[static_cast<uint64_t>(y) * stride] = clip_sample<Pel>(T(0) + ((L(y) - L(-1)) >> 1), max_value);
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
          dst[static_cast<uint64_t>(y) * stride + x] = static_cast<Pel>(fact ? ((32 - fact) * r[y + idx + 1] + fact * r[y + idx + 2] + 16) >> 5 : r[y + idx + 1]);
      }
      if (mode == 10 && boundary_filter)
        for (int x = 0; x < n; ++x) dst[x] = clip_sample<Pel>(L(0) + ((T(x) - T(-1)) >> 1), max_value);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Inter prediction

namespace {
// A block of the reference plane with the borders replicated, as 8.5.3.3.3 reads outside the picture.
template <typename Pel>
void fetch_patch(const Pel* plane, int stride, int pw, int ph, int x, int y, int w, int h, Pel* out, int out_stride) {
  for (int j = 0; j < h; ++j) {
    const int yy = clip3(0, ph - 1, y + j);
    for (int i = 0; i < w; ++i) out[j * out_stride + i] = plane[static_cast<uint64_t>(yy) * stride + clip3(0, pw - 1, x + i)];
  }
}

// The shifts of 8.5.3.3.3: shift1 = Min(4, BitDepth - 8) after the first filter stage, shift2 = 6 after the second, shift3 = Max(2, 14 - BitDepth) for whole-sample
// positions (constants at 8 bits). The intermediate predictions have 14 bits up to 12 bits a sample, BitDepth + 2 above: they are held in 32-bit integers.
template <typename Pel>
constexpr int interp_shift1(int bd) { return sizeof(Pel) == 1 ? 0 : imin(4, bd - 8); }
template <typename Pel>
constexpr int interp_shift3(int bd) { return sizeof(Pel) == 1 ? 6 : (14 - bd < 2 ? 2 : 14 - bd); }

// Sample interpolation (8.5.3.3.3.1 luma: 8 taps, quarter samples; 8.5.3.3.3.2 chroma: 4 taps, eighth samples): the intermediate prediction of a w x h block at
// integer position (xi, yi) with fraction (fx, fy); `tmp` holds the first stage ((64 + 7) x 64 values: a member of the Core, not the stack).
template <typename Pel, int kTaps>
void interpolate(const Pel* plane, int stride, int pw, int ph, int xi, int yi, const int8_t* filter_x, const int8_t* filter_y, bool frac_x, bool frac_y, int w, int h,
                 int bd, int32_t* out, int32_t* tmp) {
  constexpr int kBefore = kTaps / 2 - 1;
  const int shift1 = interp_shift1<Pel>(bd), shift3 = interp_shift3<Pel>(bd);
  Pel patch[(64 + kTaps - 1) * (64 + kTaps - 1)];
  const int ps = w + kTaps - 1;
  fetch_patch(plane, stride, pw, ph, xi - kBefore, yi - kBefore, w + kTaps - 1, h + kTaps - 1, patch, ps);
  if (!frac_x && !frac_y) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) out[y * w + x] = static_cast<int32_t>(patch[(y + kBefore) * ps + x + kBefore]) << shift3;
  } else if (!frac_y) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < kTaps; ++k) s += filter_x[k] * patch[(y + kBefore) * ps + x + k];
        out[y * w + x] = s >> shift1;
      }
  } else if (!frac_x) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < kTaps; ++k) s += filter_y[k] * patch[(y + k) * ps + x + kBefore];
        out[y * w + x] = s >> shift1;
      }
  } else {
    for (int y = 0; y < h + kTaps - 1; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < kTaps; ++k) s += filter_x[k] * patch[y * ps + x + k];
        tmp[y * w + x] = s >> shift1;
      }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int64_t s = 0;
        for (int k = 0; k < kTaps; ++k) s += static_cast<int64_t>(filter_y[k]) * tmp[(y + k) * w + x];
        out[y * w + x] = static_cast<int32_t>(s >> 6);
      }
  }
}
}  // namespace

void Core::predict_inter(int x0, int y0, int w, int h, const MvField& m) {
  if (cur_->deep) predict_inter_px<uint16_t>(x0, y0, w, h, m);
  else predict_inter_px<uint8_t>(x0, y0, w, h, m);
}

template <typename Pel>
void Core::predict_inter_px(int x0, int y0, int w, int h, const MvField& m) {
  const Sps& sps = *sp_;
  const bool weighted = sh_.type == kSliceP ? pp_->weighted_pred : (sh_.type == kSliceB ? pp_->weighted_bipred : false);
  for (int c = 0; c < (cat_ != 0 ? 3 : 1); ++c) {
    const int bd = depth_of<Pel>(c ? bd_c_ : bd_y_);
    const int max_value = (1 << bd) - 1;
    const int sx = c ? sub_w_ : 0, sy = c ? sub_h_ : 0;
    const int cw = w >> sx, ch = h >> sy;
    const int cx = x0 >> sx, cy = y0 >> sy;
    const int pw = sps.width >> sx, ph = sps.height >> sy;
    int32_t* pred[2] = {pred_buf_[0], pred_buf_[1]};
    int lists[2], used = 0;
    for (int l = 0; l < 2; ++l) {
      if (!(m.pred_flags & (1 << l))) continue;
      const Picture* ref = slice_->ref_pic[l][m.ref_idx[l]];
      // (a reference of another sample size or chroma format cannot be read; a conforming stream has none)
      if (!ref || ref->deep != cur_->deep || ref->chroma_format != cur_->chroma_format) continue;
      const Pel* rp = samples<Pel>(ref->plane[c]);
      const int rs = ref->stride[c] / static_cast<int>(sizeof(Pel));
      if (c == 0) {
        const int fx = m.mv[l][0] & 3, fy = m.mv[l][1] & 3;
        interpolate<Pel, 8>(rp, rs, pw, ph, x0 + (m.mv[l][0] >> 2), y0 + (m.mv[l][1] >> 2), kLumaFilter[fx], kLumaFilter[fy], fx != 0, fy != 0, w, h, bd, pred[used], interp_tmp_);
      } else {
        // mvCLX = mvLX * 2 / SubWidthC (SubHeightC) (8-228), in eighths of a chroma sample
        const int mvx = sx ? m.mv[l][0] : m.mv[l][0] * 2, mvy = sy ? m.mv[l][1] : m.mv[l][1] * 2;
        const int fx = mvx & 7, fy = mvy & 7;
        interpolate<Pel, 4>(rp, rs, pw, ph, cx + (mvx >> 3), cy + (mvy >> 3), kChromaFilter[fx], kChromaFilter[fy], fx != 0, fy != 0, cw, ch, bd, pred[used], interp_tmp_);
      }
      lists[used++] = l;
    }
    Picture* cur = cur_;
    const int stride = cur->stride[c] / static_cast<int>(sizeof(Pel));
    Pel* dst = samples<Pel>(cur->plane[c]) + static_cast<uint64_t>(cy) * stride + cx;
    if (used == 0) continue;
    // 8.5.3.3.4.3: the explicit offsets are in units of 8-bit samples, scaled to the bit depth (WpOffsetBdShiftY / C), or of samples of the bit depth with
    // high_precision_offsets_enabled_flag
    const int offset_shift = sps.high_precision_offsets ? 0 : bd - 8;
    auto weight = [&](int idx, int* wt, int* off) {
      const int l = lists[idx], r = m.ref_idx[l];
      if (c == 0) { *wt = sh_.luma_weight[l][r]; *off = sh_.luma_offset[l][r]; }
      else { *wt = sh_.chroma_weight[l][r][c - 1]; *off = sh_.chroma_offset[l][r][c - 1]; }
      *off *= 1 << offset_shift;
    };
    const int denom = c == 0 ? sh_.luma_log2_denom : sh_.chroma_log2_denom;
    const int shift1 = 14 - bd < 2 ? 2 : 14 - bd;   // Max(2, 14 - bitDepth): 6 at 8 bits
    if (used == 1) {
      if (!weighted) {
        // 8.5.3.3.4.2: (predSamples + offset1) >> shift1
        const int offset = 1 << (shift1 - 1);
        for (int y = 0; y < ch; ++y)
          for (int x = 0; x < cw; ++x) dst[static_cast<uint64_t>(y) * stride + x] = clip_sample<Pel>((pred[0][y * cw + x] + offset) >> shift1, max_value);
      } else {
        int wt, off;
        weight(0, &wt, &off);
        const int log2wd = denom + shift1;
        for (int y = 0; y < ch; ++y)
          for (int x = 0; x < cw; ++x) {
            const int64_t p = static_cast<int64_t>(pred[0][y * cw + x]) * wt;
            const int64_t v = log2wd >= 1 ? ((p + (int64_t{1} << (log2wd - 1))) >> log2wd) + off : p + off;
            dst[static_cast<uint64_t>(y) * stride + x] = clip_sample<Pel>(static_cast<int>(v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : v), max_value);
          }
      }
    } else {
      if (!weighted) {
        // (predSamplesL0 + predSamplesL1 + offset2) >> shift2, shift2 = Max(3, 15 - bitDepth)
        const int shift2 = shift1 + 1, offset = 1 << (shift2 - 1);
        for (int y = 0; y < ch; ++y)
          for (int x = 0; x < cw; ++x) dst[static_cast<uint64_t>(y) * stride + x] = clip_sample<Pel>((pred[0][y * cw + x] + pred[1][y * cw + x] + offset) >> shift2, max_value);
      } else {
        int w0, o0, w1, o1;
        weight(0, &w0, &o0);
        weight(1, &w1, &o1);
        const int log2wd = denom + shift1;
        for (int y = 0; y < ch; ++y)
          for (int x = 0; x < cw; ++x) {
            const int64_t v = (static_cast<int64_t>(pred[0][y * cw + x]) * w0 + static_cast<int64_t>(pred[1][y * cw + x]) * w1 + ((int64_t{o0} + o1 + 1) << log2wd)) >> (log2wd + 1);
            dst[static_cast<uint64_t>(y) * stride + x] = clip_sample<Pel>(static_cast<int>(v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : v), max_value);
          }
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Scaling, transformation and reconstruction (8.6.2 - 8.6.4)

void Core::reconstruct_block(int x0, int y0, int log2_size, int c_idx, int intra_mode, bool coded, int res_scale) {
  if (cur_->deep) reconstruct_px<uint16_t>(x0, y0, log2_size, c_idx, intra_mode, coded, res_scale);
  else reconstruct_px<uint8_t>(x0, y0, log2_size, c_idx, intra_mode, coded, res_scale);
}

// 8.6.2 - 8.6.4 and the range extension's residual modifications: the residual of the levels in coeff_ (of the block residual_coding() read last) into r, which may be
// coeff_ itself (the levels are read into resid_ before r is written).
void Core::residual_px(int log2_size, int c_idx, int intra_mode, int32_t* r) {
  const Sps& sps = *sp_;
  const int n = 1 << log2_size;
  const int bd = c_idx ? bd_c_ : bd_y_;
  // transform_skip_rotation_enabled_flag: a 4x4 intra block without a transform is coded rotated by 180 degrees
  const bool rotate = sps.transform_skip_rotation && n == 4 && cur_intra_ && (ts_flag_ || cu_transquant_bypass_);
  // RDPCM (8.6.8): explicit for inter blocks, implicit for intra blocks of the horizontal (10) and vertical (26) modes, both without a transform
  int rdpcm = 0;   // 1: horizontal (each residual adds the one left of it), 2: vertical
  if (ts_flag_ || cu_transquant_bypass_) {
    if (rdpcm_) rdpcm = rdpcm_;
    else if (cur_intra_ && sps.implicit_rdpcm && (intra_mode == 10 || intra_mode == 26)) rdpcm = intra_mode == 10 ? 1 : 2;
  }
  auto accumulate = [&]() {
    if (rdpcm == 1) {
      for (int y = 0; y < n; ++y)
        for (int x = 1; x < n; ++x) r[y * n + x] += r[y * n + x - 1];
    } else if (rdpcm == 2) {
      for (int y = 1; y < n; ++y)
        for (int x = 0; x < n; ++x) r[y * n + x] += r[(y - 1) * n + x];
    }
  };
  if (cu_transquant_bypass_) {
    for (int i = 0; i < n * n; ++i) resid_[i] = rotate ? coeff_[n * n - 1 - i] : coeff_[i];
    for (int i = 0; i < n * n; ++i) r[i] = resid_[i];   // (r may be coeff_ itself)
    accumulate();
    return;
  }
  // 8.6.3 scaling: qP = Qp'Y or Qp'C, bdShift = BitDepth + Log2(nTbS) + 10 - log2TransformRange, the results clipped to CoeffMin .. CoeffMax
  static const int kLevelScale[6] = {40, 45, 51, 57, 64, 72};
  const int log2_range = sps.log2_transform_range(bd);
  const int64_t coeff_max = (int64_t{1} << log2_range) - 1, coeff_min = -(int64_t{1} << log2_range);
  const int qp = c_idx == 0 ? qp_y_ + qp_bd_y_ : chroma_qp(c_idx);
  const int bd_shift = bd + log2_size + 10 - log2_range;
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
      d[y * n + x] = static_cast<int32_t>(v < coeff_min ? coeff_min : v > coeff_max ? coeff_max : v);
    }
  // 8.6.2: the residual is (r + (1 << (bdShift - 1))) >> bdShift, bdShift = Max(20 - BitDepth, extended_precision_processing_flag ? 11 : 0) (12 at 8 bits)
  const int final_shift = 20 - bd > (sps.extended_precision ? 11 : 0) ? 20 - bd : (sps.extended_precision ? 11 : 0);
  const int64_t final_round = int64_t{1} << (final_shift - 1);
  if (ts_flag_) {
    // 8.6.4.2: r = d << tsShift, tsShift = (extended_precision_processing_flag ? Min(5, bdShift - 2) : 5) + Log2(nTbS)
    const int ts_shift = (sps.extended_precision ? imin(5, final_shift - 2) : 5) + log2_size;
    for (int i = 0; i < n * n; ++i) {
      const int64_t v = rotate ? d[n * n - 1 - i] : d[i];
      r[i] = static_cast<int32_t>((v * (int64_t{1} << ts_shift) + final_round) >> final_shift);
    }
    accumulate();
    return;
  }
  const bool dst_matrix = cur_intra_ && c_idx == 0 && n == 4;
  const int step = 32 / n;
  int32_t g[32 * 32];
  // first stage: columns (vertical), then clipped to CoeffMin .. CoeffMax; second stage: rows (horizontal)
  for (int x = 0; x < n; ++x) {
    for (int y = 0; y < n; ++y) {
      int64_t e = 0;
      for (int j = 0; j < n; ++j) {
        const int cj = d[j * n + x];
        if (cj == 0) continue;
        e += static_cast<int64_t>(dst_matrix ? kDst4[j][y] : kTransform[j * step][y]) * cj;
      }
      const int64_t t = (e + 64) >> 7;
      g[y * n + x] = static_cast<int32_t>(t < coeff_min ? coeff_min : t > coeff_max ? coeff_max : t);
    }
  }
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      int64_t e = 0;
      for (int j = 0; j < n; ++j) e += static_cast<int64_t>(dst_matrix ? kDst4[j][x] : kTransform[j * step][x]) * g[y * n + j];
      r[y * n + x] = static_cast<int32_t>((e + final_round) >> final_shift);
    }
}

template <typename Pel>
void Core::reconstruct_px(int x0, int y0, int log2_size, int c_idx, int intra_mode, bool coded, int res_scale) {
  const int bd = depth_of<Pel>(c_idx ? bd_c_ : bd_y_);
  const int max_value = (1 << bd) - 1;
  const int n = 1 << log2_size;
  Picture* pic = cur_;
  const int stride = pic->stride[c_idx] / static_cast<int>(sizeof(Pel));
  Pel* dst = samples<Pel>(pic->plane[c_idx]) + static_cast<uint64_t>(y0) * stride + x0;
  int32_t* r = coeff_;   // (the residual replaces the levels)
  if (coded) {
    residual_px(log2_size, c_idx, intra_mode, r);
  } else {
    for (int i = 0; i < n * n; ++i) r[i] = 0;
  }
  if (c_idx == 0 && pp_->cross_component_prediction)
    for (int i = 0; i < n * n; ++i) res_y_[i] = r[i];   // the luma residual, which the chroma blocks of the unit may take a part of
  if (c_idx > 0 && res_scale != 0) {
    // 8.6.6: rC += (ResScaleVal * ((rY << BitDepthC) >> BitDepthY)) >> 3
    for (int i = 0; i < n * n; ++i)
      r[i] += static_cast<int32_t>((static_cast<int64_t>(res_scale) * ((static_cast<int64_t>(res_y_[i]) * (int64_t{1} << bd_c_)) >> bd_y_)) >> 3);
  }
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) dst[static_cast<uint64_t>(y) * stride + x] = clip_sample<Pel>(dst[static_cast<uint64_t>(y) * stride + x] + r[y * n + x], max_value);
}

void Core::fill_grey(Picture* p) {
  for (int c = 0; c < 3; ++c) {
    if (!p->deep) {
      for (uint64_t i = 0; i < p->plane[c].size(); ++i) p->plane[c][i] = 128;
      continue;
    }
    const uint16_t grey = static_cast<uint16_t>(1u << ((c ? p->depth_chroma : p->depth_luma) - 1));
    uint16_t* words = samples<uint16_t>(p->plane[c]);
    for (uint64_t i = 0; i < p->plane[c].size() / 2; ++i) words[i] = grey;
  }
}

}  // namespace h265
}  // namespace hardware
