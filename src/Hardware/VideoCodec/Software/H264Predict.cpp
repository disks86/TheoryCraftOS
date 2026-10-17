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

// H.264 decoding, part 4: reconstruction - intra prediction (8.3), inter prediction (8.4) and the inverse
// transforms (8.5).

#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h264 {

namespace {

enum MbKind { kI4x4 = 0, kI8x8, kI16, kPcm, kInter };

inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
inline uint8_t clip_pixel(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

const uint8_t kZOrder[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};  // raster -> decoding order
const uint8_t kBlkRaster[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};

// Neighbouring samples of an N x N block: t[0] is the corner, t[1 + x] the row above (x up to 2N - 1), l[1 + y] the
// column to the left.
struct Neighbours {
  int t[17];
  int l[9];
};

// The directional modes shared by 4x4 and 8x8 blocks (8.3.1.2.4 - 8.3.1.2.9, 8.3.2.2.4 - 8.3.2.2.9); the samples
// of an 8x8 block are the filtered ones.
void predict_directional(int n, int mode, const Neighbours& nb, bool has_top, bool has_left, uint8_t* dst, int stride) {
  auto T = [&](int x) { return nb.t[x + 1]; };
  auto L = [&](int y) { return nb.l[y + 1]; };
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      int v = 128;
      switch (mode) {
        case 0: v = T(x); break;
        case 1: v = L(y); break;
        case 2: {
          int sum = 0;
          if (has_top && has_left) {
            for (int i = 0; i < n; ++i) sum += T(i) + L(i);
            v = (sum + n) >> (n == 4 ? 3 : 4);
          } else if (has_left) {
            for (int i = 0; i < n; ++i) sum += L(i);
            v = (sum + n / 2) >> (n == 4 ? 2 : 3);
          } else if (has_top) {
            for (int i = 0; i < n; ++i) sum += T(i);
            v = (sum + n / 2) >> (n == 4 ? 2 : 3);
          } else {
            v = 128;
          }
          break;
        }
        case 3:
          v = (x == n - 1 && y == n - 1) ? (T(2 * n - 2) + 3 * T(2 * n - 1) + 2) >> 2
                                         : (T(x + y) + 2 * T(x + y + 1) + T(x + y + 2) + 2) >> 2;
          break;
        case 4:
          if (x > y) v = (T(x - y - 2) + 2 * T(x - y - 1) + T(x - y) + 2) >> 2;
          else if (x < y) v = (L(y - x - 2) + 2 * L(y - x - 1) + L(y - x) + 2) >> 2;
          else v = (T(0) + 2 * T(-1) + L(0) + 2) >> 2;
          break;
        case 5: {
          const int z = 2 * x - y;
          if (z >= 0 && !(z & 1)) v = (T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 1) >> 1;
          else if (z >= 0) v = (T(x - (y >> 1) - 2) + 2 * T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 2) >> 2;
          else if (z == -1) v = (L(0) + 2 * L(-1) + T(0) + 2) >> 2;
          else v = (L(y - 2 * x - 1) + 2 * L(y - 2 * x - 2) + L(y - 2 * x - 3) + 2) >> 2;
          break;
        }
        case 6: {
          const int z = 2 * y - x;
          if (z >= 0 && !(z & 1)) v = (L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 1) >> 1;
          else if (z >= 0) v = (L(y - (x >> 1) - 2) + 2 * L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 2) >> 2;
          else if (z == -1) v = (L(0) + 2 * L(-1) + T(0) + 2) >> 2;
          else v = (T(x - 2 * y - 1) + 2 * T(x - 2 * y - 2) + T(x - 2 * y - 3) + 2) >> 2;
          break;
        }
        case 7:
          if (!(y & 1)) v = (T(x + (y >> 1)) + T(x + (y >> 1) + 1) + 1) >> 1;
          else v = (T(x + (y >> 1)) + 2 * T(x + (y >> 1) + 1) + T(x + (y >> 1) + 2) + 2) >> 2;
          break;
        default: {
          const int z = x + 2 * y;
          const int zmax = 2 * n - 3;
          if (z > zmax) v = L(n - 1);
          else if (z == zmax) v = (L(n - 2) + 3 * L(n - 1) + 2) >> 2;
          else if (!(z & 1)) v = (L(y + (x >> 1)) + L(y + (x >> 1) + 1) + 1) >> 1;
          else v = (L(y + (x >> 1)) + 2 * L(y + (x >> 1) + 1) + L(y + (x >> 1) + 2) + 2) >> 2;
          break;
        }
      }
      dst[y * stride + x] = static_cast<uint8_t>(v);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Transforms

void Core::idct4x4_add(uint8_t* dst, int stride, int32_t* c) {
  int32_t t[16];
  for (int i = 0; i < 4; ++i) {
    const int32_t d0 = c[i * 4], d1 = c[i * 4 + 1], d2 = c[i * 4 + 2], d3 = c[i * 4 + 3];
    const int32_t e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
    t[i * 4] = e0 + e3;
    t[i * 4 + 1] = e1 + e2;
    t[i * 4 + 2] = e1 - e2;
    t[i * 4 + 3] = e0 - e3;
  }
  for (int j = 0; j < 4; ++j) {
    const int32_t f0 = t[j], f1 = t[4 + j], f2 = t[8 + j], f3 = t[12 + j];
    const int32_t g0 = f0 + f2, g1 = f0 - f2, g2 = (f1 >> 1) - f3, g3 = f1 + (f3 >> 1);
    const int32_t r[4] = {g0 + g3, g1 + g2, g1 - g2, g0 - g3};
    for (int i = 0; i < 4; ++i) dst[i * stride + j] = clip_pixel(dst[i * stride + j] + ((r[i] + 32) >> 6));
  }
}

void Core::idct8x8_add(uint8_t* dst, int stride, int32_t* c) {
  int32_t t[64];
  auto one = [](const int32_t* in, int step, int32_t* out, int ostep) {
    const int32_t d0 = in[0], d1 = in[step], d2 = in[2 * step], d3 = in[3 * step], d4 = in[4 * step], d5 = in[5 * step],
                  d6 = in[6 * step], d7 = in[7 * step];
    const int32_t a0 = d0 + d4, a1 = -d3 + d5 - d7 - (d7 >> 1), a2 = d0 - d4, a3 = d1 + d7 - d3 - (d3 >> 1);
    const int32_t a4 = (d2 >> 1) - d6, a5 = -d1 + d7 + d5 + (d5 >> 1), a6 = d2 + (d6 >> 1), a7 = d3 + d5 + d1 + (d1 >> 1);
    const int32_t b0 = a0 + a6, b1 = a1 + (a7 >> 2), b2 = a2 + a4, b3 = a3 + (a5 >> 2);
    const int32_t b4 = a2 - a4, b5 = (a3 >> 2) - a5, b6 = a0 - a6, b7 = a7 - (a1 >> 2);
    out[0] = b0 + b7;
    out[ostep] = b2 + b5;
    out[2 * ostep] = b4 + b3;
    out[3 * ostep] = b6 + b1;
    out[4 * ostep] = b6 - b1;
    out[5 * ostep] = b4 - b3;
    out[6 * ostep] = b2 - b5;
    out[7 * ostep] = b0 - b7;
  };
  for (int i = 0; i < 8; ++i) one(c + i * 8, 1, t + i * 8, 1);
  int32_t u[64];
  for (int j = 0; j < 8; ++j) one(t + j, 8, u + j, 8);
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 8; ++j) dst[i * stride + j] = clip_pixel(dst[i * stride + j] + ((u[i * 8 + j] + 32) >> 6));
}

// ---------------------------------------------------------------------------------------------------------------
// Intra prediction

void Core::predict_intra4x4(int bx, int by, int mode) {
  uint8_t* base = cur_->y.data() + static_cast<int64_t>(mby_ * 16 + by * 4) * cur_->stride + mbx_ * 16 + bx * 4;
  const int stride = cur_->stride;
  const bool has_left = bx > 0 || intra_nb_ok(-1, 0);
  const bool has_top = by > 0 || intra_nb_ok(0, -1);
  bool has_tl;
  if (bx > 0 && by > 0) has_tl = true;
  else if (bx == 0 && by > 0) has_tl = intra_nb_ok(-1, 0);
  else if (by == 0 && bx > 0) has_tl = intra_nb_ok(0, -1);
  else has_tl = intra_nb_ok(-1, -1);
  bool has_tr;
  if (by == 0) has_tr = bx < 3 ? intra_nb_ok(0, -1) : intra_nb_ok(1, -1);
  else if (bx == 3) has_tr = false;
  else has_tr = kZOrder[(by - 1) * 4 + bx + 1] < kZOrder[by * 4 + bx];
  Neighbours nb;
  for (int i = 0; i < 17; ++i) nb.t[i] = 128;
  for (int i = 0; i < 9; ++i) nb.l[i] = 128;
  if (has_top) {
    for (int x = 0; x < 4; ++x) nb.t[1 + x] = base[x - stride];
    for (int x = 4; x < 8; ++x) nb.t[1 + x] = has_tr ? base[x - stride] : nb.t[4];
  }
  if (has_left)
    for (int y = 0; y < 4; ++y) nb.l[1 + y] = base[y * stride - 1];
  if (has_tl) nb.t[0] = nb.l[0] = base[-stride - 1];
  predict_directional(4, mode, nb, has_top, has_left, base, stride);
}

void Core::predict_intra8x8(int b8, int mode) {
  const int bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
  uint8_t* base = cur_->y.data() + static_cast<int64_t>(mby_ * 16 + by * 4) * cur_->stride + mbx_ * 16 + bx * 4;
  const int stride = cur_->stride;
  const bool has_left = bx > 0 || intra_nb_ok(-1, 0);
  const bool has_top = by > 0 || intra_nb_ok(0, -1);
  bool has_tl;
  if (bx > 0 && by > 0) has_tl = true;
  else if (bx == 0 && by > 0) has_tl = intra_nb_ok(-1, 0);
  else if (by == 0 && bx > 0) has_tl = intra_nb_ok(0, -1);
  else has_tl = intra_nb_ok(-1, -1);
  bool has_tr;
  if (b8 == 0) has_tr = intra_nb_ok(0, -1);
  else if (b8 == 1) has_tr = intra_nb_ok(1, -1);
  else if (b8 == 2) has_tr = true;
  else has_tr = false;
  int top[16], left[8], tl = 128;
  for (int i = 0; i < 16; ++i) top[i] = 128;
  for (int i = 0; i < 8; ++i) left[i] = 128;
  if (has_top) {
    for (int x = 0; x < 8; ++x) top[x] = base[x - stride];
    for (int x = 8; x < 16; ++x) top[x] = has_tr ? base[x - stride] : top[7];
  }
  if (has_left)
    for (int y = 0; y < 8; ++y) left[y] = base[y * stride - 1];
  if (has_tl) tl = base[-stride - 1];
  // 8.3.2.2.1: filter the reference samples.
  Neighbours nb;
  for (int i = 0; i < 17; ++i) nb.t[i] = 128;
  for (int i = 0; i < 9; ++i) nb.l[i] = 128;
  if (has_top) {
    nb.t[1] = has_tl ? (tl + 2 * top[0] + top[1] + 2) >> 2 : (3 * top[0] + top[1] + 2) >> 2;
    for (int x = 1; x < 15; ++x) nb.t[1 + x] = (top[x - 1] + 2 * top[x] + top[x + 1] + 2) >> 2;
    nb.t[16] = (top[14] + 3 * top[15] + 2) >> 2;
  }
  if (has_tl) {
    if (!has_top || !has_left) {
      if (has_top) nb.t[0] = (3 * tl + top[0] + 2) >> 2;
      else if (has_left) nb.t[0] = (3 * tl + left[0] + 2) >> 2;
      else nb.t[0] = tl;
    } else {
      nb.t[0] = (top[0] + 2 * tl + left[0] + 2) >> 2;
    }
    nb.l[0] = nb.t[0];
  }
  if (has_left) {
    nb.l[1] = has_tl ? (tl + 2 * left[0] + left[1] + 2) >> 2 : (3 * left[0] + left[1] + 2) >> 2;
    for (int y = 1; y < 7; ++y) nb.l[1 + y] = (left[y - 1] + 2 * left[y] + left[y + 1] + 2) >> 2;
    nb.l[8] = (left[6] + 3 * left[7] + 2) >> 2;
  }
  predict_directional(8, mode, nb, has_top, has_left, base, stride);
}

void Core::predict_intra16(int mode) {
  uint8_t* base = cur_->y.data() + static_cast<int64_t>(mby_ * 16) * cur_->stride + mbx_ * 16;
  const int stride = cur_->stride;
  const bool has_top = intra_nb_ok(0, -1), has_left = intra_nb_ok(-1, 0), has_tl = intra_nb_ok(-1, -1);
  switch (mode) {
    case 0:
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) base[y * stride + x] = has_top ? base[x - stride] : 128;
      break;
    case 1:
      for (int y = 0; y < 16; ++y) {
        const uint8_t v = has_left ? base[y * stride - 1] : 128;
        for (int x = 0; x < 16; ++x) base[y * stride + x] = v;
      }
      break;
    case 2: {
      int sum = 0, v;
      if (has_top && has_left) {
        for (int i = 0; i < 16; ++i) sum += base[i - stride] + base[i * stride - 1];
        v = (sum + 16) >> 5;
      } else if (has_left) {
        for (int i = 0; i < 16; ++i) sum += base[i * stride - 1];
        v = (sum + 8) >> 4;
      } else if (has_top) {
        for (int i = 0; i < 16; ++i) sum += base[i - stride];
        v = (sum + 8) >> 4;
      } else {
        v = 128;
      }
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) base[y * stride + x] = static_cast<uint8_t>(v);
      break;
    }
    default: {
      if (!(has_top && has_left && has_tl)) {
        for (int y = 0; y < 16; ++y)
          for (int x = 0; x < 16; ++x) base[y * stride + x] = 128;
        break;
      }
      int h = 0, v = 0;
      for (int i = 0; i < 8; ++i) {
        const int a = base[8 + i - stride], b = (6 - i) >= 0 ? base[6 - i - stride] : base[-stride - 1];
        h += (i + 1) * (a - b);
        const int c = base[(8 + i) * stride - 1], d = (6 - i) >= 0 ? base[(6 - i) * stride - 1] : base[-stride - 1];
        v += (i + 1) * (c - d);
      }
      const int a = 16 * (base[15 * stride - 1] + base[15 - stride]);
      const int b = (5 * h + 32) >> 6, c = (5 * v + 32) >> 6;
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) base[y * stride + x] = clip_pixel((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
      break;
    }
  }
}

void Core::predict_chroma(int mode) {
  const bool has_top = intra_nb_ok(0, -1), has_left = intra_nb_ok(-1, 0), has_tl = intra_nb_ok(-1, -1);
  const int stride = cur_->cstride;
  for (int plane = 0; plane < 2; ++plane) {
    uint8_t* base = (plane == 0 ? cur_->u.data() : cur_->v.data()) + static_cast<int64_t>(mby_ * 8) * stride + mbx_ * 8;
    switch (mode) {
      case 0: {
        for (int cy = 0; cy < 2; ++cy) {
          for (int cx = 0; cx < 2; ++cx) {
            int st = 0, sl = 0;
            if (has_top)
              for (int i = 0; i < 4; ++i) st += base[cx * 4 + i - stride];
            if (has_left)
              for (int i = 0; i < 4; ++i) sl += base[(cy * 4 + i) * stride - 1];
            int v;
            if ((cx == 0 && cy == 0) || (cx == 1 && cy == 1)) {
              if (has_top && has_left) v = (st + sl + 4) >> 3;
              else if (has_left) v = (sl + 2) >> 2;
              else if (has_top) v = (st + 2) >> 2;
              else v = 128;
            } else if (cx == 1 && cy == 0) {
              if (has_top) v = (st + 2) >> 2;
              else if (has_left) v = (sl + 2) >> 2;
              else v = 128;
            } else {
              if (has_left) v = (sl + 2) >> 2;
              else if (has_top) v = (st + 2) >> 2;
              else v = 128;
            }
            for (int y = 0; y < 4; ++y)
              for (int x = 0; x < 4; ++x) base[(cy * 4 + y) * stride + cx * 4 + x] = static_cast<uint8_t>(v);
          }
        }
        break;
      }
      case 1:
        for (int y = 0; y < 8; ++y) {
          const uint8_t v = has_left ? base[y * stride - 1] : 128;
          for (int x = 0; x < 8; ++x) base[y * stride + x] = v;
        }
        break;
      case 2:
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x) base[y * stride + x] = has_top ? base[x - stride] : 128;
        break;
      default: {
        if (!(has_top && has_left && has_tl)) {
          for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) base[y * stride + x] = 128;
          break;
        }
        int h = 0, v = 0;
        for (int i = 0; i < 4; ++i) {
          const int a = base[4 + i - stride], b = (2 - i) >= 0 ? base[2 - i - stride] : base[-stride - 1];
          h += (i + 1) * (a - b);
          const int c = base[(4 + i) * stride - 1], d = (2 - i) >= 0 ? base[(2 - i) * stride - 1] : base[-stride - 1];
          v += (i + 1) * (c - d);
        }
        const int a = 16 * (base[7 * stride - 1] + base[7 - stride]);
        const int b = (34 * h + 32) >> 6, c = (34 * v + 32) >> 6;
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x) base[y * stride + x] = clip_pixel((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Inter prediction

namespace {

// Luma sample interpolation (8.4.2.2.1) of a W x H block at quarter-sample position (xq, yq).
void mc_luma(const uint8_t* src, int stride, int pw, int ph, int xq, int yq, int w, int h, uint8_t* out, int ostride) {
  const int xi = xq >> 2, yi = yq >> 2, xf = xq & 3, yf = yq & 3;
  int win[21][21];
  for (int y = 0; y < h + 5; ++y) {
    const int sy = clip3(0, ph - 1, yi - 2 + y);
    const uint8_t* row = src + static_cast<int64_t>(sy) * stride;
    for (int x = 0; x < w + 5; ++x) win[y][x] = row[clip3(0, pw - 1, xi - 2 + x)];
  }
  auto G = [&](int x, int y) { return win[y + 2][x + 2]; };
  auto hraw = [&](int x, int y) {
    return G(x - 2, y) - 5 * G(x - 1, y) + 20 * G(x, y) + 20 * G(x + 1, y) - 5 * G(x + 2, y) + G(x + 3, y);
  };
  auto vraw = [&](int x, int y) {
    return G(x, y - 2) - 5 * G(x, y - 1) + 20 * G(x, y) + 20 * G(x, y + 1) - 5 * G(x, y + 2) + G(x, y + 3);
  };
  auto B = [&](int x, int y) { return clip3(0, 255, (hraw(x, y) + 16) >> 5); };
  auto H = [&](int x, int y) { return clip3(0, 255, (vraw(x, y) + 16) >> 5); };
  auto J = [&](int x, int y) {
    const int j = hraw(x, y - 2) - 5 * hraw(x, y - 1) + 20 * hraw(x, y) + 20 * hraw(x, y + 1) - 5 * hraw(x, y + 2) + hraw(x, y + 3);
    return clip3(0, 255, (j + 512) >> 10);
  };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int v;
      switch (yf * 4 + xf) {
        case 0: v = G(x, y); break;
        case 1: v = (G(x, y) + B(x, y) + 1) >> 1; break;
        case 2: v = B(x, y); break;
        case 3: v = (G(x + 1, y) + B(x, y) + 1) >> 1; break;
        case 4: v = (G(x, y) + H(x, y) + 1) >> 1; break;
        case 5: v = (B(x, y) + H(x, y) + 1) >> 1; break;
        case 6: v = (B(x, y) + J(x, y) + 1) >> 1; break;
        case 7: v = (B(x, y) + H(x + 1, y) + 1) >> 1; break;
        case 8: v = H(x, y); break;
        case 9: v = (H(x, y) + J(x, y) + 1) >> 1; break;
        case 10: v = J(x, y); break;
        case 11: v = (J(x, y) + H(x + 1, y) + 1) >> 1; break;
        case 12: v = (G(x, y + 1) + H(x, y) + 1) >> 1; break;
        case 13: v = (H(x, y) + B(x, y + 1) + 1) >> 1; break;
        case 14: v = (J(x, y) + B(x, y + 1) + 1) >> 1; break;
        default: v = (H(x + 1, y) + B(x, y + 1) + 1) >> 1; break;
      }
      out[y * ostride + x] = static_cast<uint8_t>(v);
    }
  }
}

// Chroma sample interpolation (8.4.2.2.2): eighth-sample position (xe, ye).
void mc_chroma(const uint8_t* src, int stride, int pw, int ph, int xe, int ye, int w, int h, uint8_t* out, int ostride) {
  const int xi = xe >> 3, yi = ye >> 3, xf = xe & 7, yf = ye & 7;
  for (int y = 0; y < h; ++y) {
    const uint8_t* r0 = src + static_cast<int64_t>(clip3(0, ph - 1, yi + y)) * stride;
    const uint8_t* r1 = src + static_cast<int64_t>(clip3(0, ph - 1, yi + y + 1)) * stride;
    for (int x = 0; x < w; ++x) {
      const int x0 = clip3(0, pw - 1, xi + x), x1 = clip3(0, pw - 1, xi + x + 1);
      const int v = ((8 - xf) * (8 - yf) * r0[x0] + xf * (8 - yf) * r0[x1] + (8 - xf) * yf * r1[x0] + xf * yf * r1[x1] + 32) >> 6;
      out[y * ostride + x] = static_cast<uint8_t>(v);
    }
  }
}

}  // namespace

void Core::mc_partition(int bx, int by, int w, int h) {
  const int b8 = (by >> 1) * 2 + (bx >> 1);
  const int refs[2] = {cmotion_->ref[0][b8], cmotion_->ref[1][b8]};
  const int px = mbx_ * 16 + bx * 4, py = mby_ * 16 + by * 4;
  const int W = w * 4, H = h * 4;
  const int pw = mb_w_ * 16, ph = mb_h_ * 16;
  uint8_t pred[2][3][16 * 16];  // per list: luma, Cb, Cr (chroma uses 8x8 of the buffer)
  int used[2] = {0, 0};
  for (int l = 0; l < 2; ++l) {
    if (refs[l] < 0) continue;
    const Picture* ref = ref_list_[l][refs[l]];
    if (!ref) continue;
    used[l] = 1;
    const int mvx = cmotion_->mv[l][by * 4 + bx][0], mvy = cmotion_->mv[l][by * 4 + bx][1];
    mc_luma(ref->y.data(), ref->stride, pw, ph, px * 4 + mvx, py * 4 + mvy, W, H, pred[l][0], 16);
    mc_chroma(ref->u.data(), ref->cstride, pw / 2, ph / 2, (px / 2) * 8 + mvx, (py / 2) * 8 + mvy, W / 2, H / 2, pred[l][1], 16);
    mc_chroma(ref->v.data(), ref->cstride, pw / 2, ph / 2, (px / 2) * 8 + mvx, (py / 2) * 8 + mvy, W / 2, H / 2, pred[l][2], 16);
  }
  if (!used[0] && !used[1]) return;
  const bool is_b = sh_.type == kSliceB;
  const bool explicit_w = (!is_b && pp_->weighted_pred) || (is_b && pp_->weighted_bipred_idc == 1);
  const bool implicit_w = is_b && pp_->weighted_bipred_idc == 2 && used[0] && used[1];
  for (int plane = 0; plane < 3; ++plane) {
    const int pwid = plane == 0 ? W : W / 2, phei = plane == 0 ? H : H / 2;
    uint8_t* dst;
    int dstride;
    if (plane == 0) {
      dst = cur_->y.data() + static_cast<int64_t>(py) * cur_->stride + px;
      dstride = cur_->stride;
    } else {
      dst = (plane == 1 ? cur_->u.data() : cur_->v.data()) + static_cast<int64_t>(py / 2) * cur_->cstride + px / 2;
      dstride = cur_->cstride;
    }
    for (int y = 0; y < phei; ++y) {
      for (int x = 0; x < pwid; ++x) {
        int v;
        if (used[0] && used[1]) {
          const int p0 = pred[0][plane][y * 16 + x], p1 = pred[1][plane][y * 16 + x];
          if (explicit_w) {
            const int log_wd = plane == 0 ? sh_.luma_log2_denom : sh_.chroma_log2_denom;
            const int w0 = plane == 0 ? sh_.luma_weight[0][refs[0]] : sh_.chroma_weight[0][refs[0]][plane - 1];
            const int w1 = plane == 0 ? sh_.luma_weight[1][refs[1]] : sh_.chroma_weight[1][refs[1]][plane - 1];
            const int o0 = plane == 0 ? sh_.luma_offset[0][refs[0]] : sh_.chroma_offset[0][refs[0]][plane - 1];
            const int o1 = plane == 0 ? sh_.luma_offset[1][refs[1]] : sh_.chroma_offset[1][refs[1]][plane - 1];
            v = clip3(0, 255, ((p0 * w0 + p1 * w1 + (1 << log_wd)) >> (log_wd + 1)) + ((o0 + o1 + 1) >> 1));
          } else if (implicit_w) {
            const int w1 = implicit_w_[refs[0]][refs[1]], w0 = 64 - w1;
            v = clip3(0, 255, (p0 * w0 + p1 * w1 + 32) >> 6);
          } else {
            v = (p0 + p1 + 1) >> 1;
          }
        } else {
          const int l = used[0] ? 0 : 1;
          const int p = pred[l][plane][y * 16 + x];
          if (explicit_w) {
            const int log_wd = plane == 0 ? sh_.luma_log2_denom : sh_.chroma_log2_denom;
            const int wt = plane == 0 ? sh_.luma_weight[l][refs[l]] : sh_.chroma_weight[l][refs[l]][plane - 1];
            const int o = plane == 0 ? sh_.luma_offset[l][refs[l]] : sh_.chroma_offset[l][refs[l]][plane - 1];
            if (log_wd >= 1) v = clip3(0, 255, ((p * wt + (1 << (log_wd - 1))) >> log_wd) + o);
            else v = clip3(0, 255, p * wt + o);
          } else {
            v = p;
          }
        }
        dst[y * dstride + x] = static_cast<uint8_t>(v);
      }
    }
  }
}

void Core::inter_predict() {
  for (int i = 0; i < rect_count_; ++i) mc_partition(rect_[i][0], rect_[i][1], rect_[i][2], rect_[i][3]);
}

// ---------------------------------------------------------------------------------------------------------------
// Residual and reconstruction

void Core::residual_luma4x4(int bx, int by) {
  const int blk = by * 4 + bx;
  if (!coef_present_[0][blk]) return;
  uint8_t* dst = cur_->y.data() + static_cast<int64_t>(mby_ * 16 + by * 4) * cur_->stride + mbx_ * 16 + bx * 4;
  idct4x4_add(dst, cur_->stride, &coef_[0][blk * 16]);
}

void Core::residual_luma8x8(int b8) {
  if (!coef_present_[0][b8]) return;
  const int bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
  uint8_t* dst = cur_->y.data() + static_cast<int64_t>(mby_ * 16 + by * 4) * cur_->stride + mbx_ * 16 + bx * 4;
  idct8x8_add(dst, cur_->stride, &coef_[0][b8 * 64]);
}

void Core::residual_chroma() {
  for (int plane = 1; plane < 3; ++plane) {
    uint8_t* base = (plane == 1 ? cur_->u.data() : cur_->v.data()) + static_cast<int64_t>(mby_ * 8) * cur_->cstride + mbx_ * 8;
    for (int s = 0; s < 4; ++s) {
      if (!coef_present_[plane][s]) continue;
      idct4x4_add(base + (s >> 1) * 4 * cur_->cstride + (s & 1) * 4, cur_->cstride, &coef_[plane][s * 16]);
    }
  }
}

void Core::reconstruct_mb() {
  if (mb_type_ == kPcm) return;
  if (mb_type_ == kInter) {
    inter_predict();
    if (cm_->t8x8) {
      for (int b8 = 0; b8 < 4; ++b8) residual_luma8x8(b8);
    } else {
      for (int blk = 0; blk < 16; ++blk) residual_luma4x4(blk & 3, blk >> 2);
    }
    residual_chroma();
    return;
  }
  if (mb_type_ == kI16) {
    predict_intra16(i16_mode_);
    for (int blk = 0; blk < 16; ++blk) {
      if (dc_luma_[blk]) {
        coef_[0][blk * 16] = dc_luma_[blk];
        coef_present_[0][blk] = true;
      }
      residual_luma4x4(blk & 3, blk >> 2);
    }
  } else if (mb_type_ == kI8x8) {
    for (int b8 = 0; b8 < 4; ++b8) {
      predict_intra8x8(b8, cm_->ipred[((b8 >> 1) * 2) * 4 + (b8 & 1) * 2]);
      residual_luma8x8(b8);
    }
  } else {
    for (int i = 0; i < 16; ++i) {
      const int r = kBlkRaster[i];
      const int bx = r & 3, by = r >> 2;
      predict_intra4x4(bx, by, cm_->ipred[r]);
      residual_luma4x4(bx, by);
    }
  }
  predict_chroma(cm_->chroma_mode);
  residual_chroma();
}

}  // namespace h264
}  // namespace hardware
