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

// The H.264 picture encoder (H264PictureEncoder.hpp): decisions, prediction, transforms, reconstruction and the in-loop filter; the syntax is H264PictureEncoderSyntax.cpp.
// Everything it predicts from is what a decoder has at the same point (the reconstruction of the picture so far, the program's reference pictures and their motion), so the
// reconstruction it writes is bit for bit what a decoder makes of the slices. Clause numbers are ITU-T H.264's.

#include "Hardware/VideoCodec/Software/H264Core.hpp"
#include "Hardware/VideoCodec/Software/H264PictureEncoderCore.hpp"
#include "Hardware/VideoCodec/Software/MotionEstimator.hpp"

namespace hardware {
namespace h264 {

#include "Hardware/VideoCodec/Software/H264Tables.inc"

namespace {

const int kMf[6][3] = {{13107, 5243, 8066}, {11916, 4660, 7490}, {10082, 4194, 6554}, {9362, 3647, 5825}, {8192, 3355, 5243}, {7282, 2893, 4559}};
const int kNorm[6][3] = {{10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23}};
// 8x8: the forward scale (x264's) and normAdjust8x8 (8-317), by the six position classes of 8.5.9.
const int kMf8[6][6] = {{13107, 11428, 20972, 12222, 16777, 15481}, {11916, 10826, 19174, 11058, 14980, 14290}, {10082, 8943, 15978, 9675, 12710, 11985},
                        {9362, 8228, 14913, 8931, 11984, 11259},     {8192, 7346, 13159, 7740, 10486, 9777},     {7282, 6428, 11570, 6830, 9118, 8640}};
const int kNorm8[6][6] = {{20, 18, 32, 19, 25, 24}, {22, 19, 35, 21, 28, 26}, {26, 23, 42, 24, 33, 31}, {28, 25, 45, 26, 35, 33}, {32, 28, 51, 30, 40, 38}, {36, 32, 58, 34, 46, 43}};
const int kMaxLevel = 2047;   // keeps CAVLC's level_prefix within 15 (no escape beyond what every profile allows)
// x264's lambdas: for SAD-based decisions, and (0.85 * 2^((QP - 12) / 3)) for SSD-based ones.
const uint8_t kLambda[52] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 23, 25, 29, 32};
const uint16_t kLambda2[52] = {1,  1,  1,  1,  1,  1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,    2,    3,    3,    4,    5,    7,    9,    11,   14,   17,
                               22, 27, 34, 43, 54, 69, 86, 109, 137, 173, 218, 274, 345, 435, 548, 691, 870, 1097, 1382, 1741, 2193, 2763, 3482, 4387, 5527, 6963};

inline int pos_class(int p) {
  const int i = p >> 2, j = p & 3;
  return ((i & 1) == 0 && (j & 1) == 0) ? 0 : ((i & 1) && (j & 1)) ? 1 : 2;
}
inline int pos_class8(int p) {
  const int i = p >> 3, j = p & 7;
  if (i % 4 == 0 && j % 4 == 0) return 0;
  if (i % 2 == 1 && j % 2 == 1) return 1;
  if (i % 4 == 2 && j % 4 == 2) return 2;
  if ((i % 4 == 0 && j % 2 == 1) || (i % 2 == 1 && j % 4 == 0)) return 3;
  if ((i % 4 == 0 && j % 4 == 2) || (i % 4 == 2 && j % 4 == 0)) return 4;
  return 5;
}
inline int quantize(int w, int mf, int shift, int round) {
  const int64_t q = (static_cast<int64_t>(iabs(w)) * mf + round) >> shift;
  const int level = static_cast<int>(q > kMaxLevel ? kMaxLevel : q);
  return w < 0 ? -level : level;
}
inline int level_bits(int v) {
  if (v == 0) return 0;
  int a = iabs(v), n = 2;
  while (a > 1) {
    a >>= 1;
    n += 2;
  }
  return n;
}
void forward4x4(ArrayView<const int> r, ArrayView<int> w) {
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
// The 8x8 forward transform (the integer approximation x264 uses; any forward transform gives a valid stream, the decoder's inverse is 8.5.13).
void forward8x8(ArrayView<const int> r, ArrayView<int> w) {
  auto one = [](ArrayView<const int> in, int step, ArrayView<int> out, int ostep) {
    const int s07 = in[0] + in[7 * step], s16 = in[step] + in[6 * step], s25 = in[2 * step] + in[5 * step], s34 = in[3 * step] + in[4 * step];
    const int a0 = s07 + s34, a1 = s16 + s25, a2 = s07 - s34, a3 = s16 - s25;
    const int d07 = in[0] - in[7 * step], d16 = in[step] - in[6 * step], d25 = in[2 * step] - in[5 * step], d34 = in[3 * step] - in[4 * step];
    const int a4 = d16 + d25 + (d07 + (d07 >> 1));
    const int a5 = d07 - d34 - (d25 + (d25 >> 1));
    const int a6 = d07 + d34 - (d16 + (d16 >> 1));
    const int a7 = d16 - d25 + (d34 + (d34 >> 1));
    out[0] = a0 + a1;
    out[ostep] = a4 + (a7 >> 2);
    out[2 * ostep] = a2 + (a3 >> 1);
    out[3 * ostep] = a5 + (a6 >> 2);
    out[4 * ostep] = a0 - a1;
    out[5 * ostep] = a6 - (a5 >> 2);
    out[6 * ostep] = (a2 >> 1) - a3;
    out[7 * ostep] = (a4 >> 2) - a7;
  };
  int t[64];
  const ArrayView<int> tv(t);
  for (int i = 0; i < 8; ++i) one(r.subview(static_cast<uint64_t>(i * 8)), 1, tv.subview(static_cast<uint64_t>(i * 8)), 1);
  for (int j = 0; j < 8; ++j) one(tv.subview(static_cast<uint64_t>(j)), 8, w.subview(static_cast<uint64_t>(j)), 8);
}
// 8.5.12: dequantised coefficients (raster) transformed and added to the samples at `offset` of `dst`.
void inverse4x4_add(ArrayView<const int> d, ByteView dst, uint64_t offset, uint32_t stride) {
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
// 8.5.13
void inverse8x8_add(ArrayView<const int> c, ByteView dst, uint64_t offset, uint32_t stride) {
  auto one = [](ArrayView<const int> in, int step, ArrayView<int> out, int ostep) {
    const int d0 = in[0], d1 = in[step], d2 = in[2 * step], d3 = in[3 * step], d4 = in[4 * step], d5 = in[5 * step], d6 = in[6 * step], d7 = in[7 * step];
    const int a0 = d0 + d4, a1 = -d3 + d5 - d7 - (d7 >> 1), a2 = d0 - d4, a3 = d1 + d7 - d3 - (d3 >> 1);
    const int a4 = (d2 >> 1) - d6, a5 = -d1 + d7 + d5 + (d5 >> 1), a6 = d2 + (d6 >> 1), a7 = d3 + d5 + d1 + (d1 >> 1);
    const int b0 = a0 + a6, b1 = a1 + (a7 >> 2), b2 = a2 + a4, b3 = a3 + (a5 >> 2);
    const int b4 = a2 - a4, b5 = (a3 >> 2) - a5, b6 = a0 - a6, b7 = a7 - (a1 >> 2);
    out[0] = b0 + b7;
    out[ostep] = b2 + b5;
    out[2 * ostep] = b4 + b3;
    out[3 * ostep] = b6 + b1;
    out[4 * ostep] = b6 - b1;
    out[5 * ostep] = b4 - b3;
    out[6 * ostep] = b2 - b5;
    out[7 * ostep] = b0 - b7;
  };
  int t[64], u[64];
  const ArrayView<int> tv(t), uv(u);
  for (int i = 0; i < 8; ++i) one(c.subview(static_cast<uint64_t>(i * 8)), 1, tv.subview(static_cast<uint64_t>(i * 8)), 1);
  for (int j = 0; j < 8; ++j) one(tv.subview(static_cast<uint64_t>(j)), 8, uv.subview(static_cast<uint64_t>(j)), 8);
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 8; ++j) {
      const uint64_t at = offset + uint64_t{static_cast<uint32_t>(i)} * stride + static_cast<uint32_t>(j);
      dst[at] = clip_pixel(dst[at] + ((u[i * 8 + j] + 32) >> 6));
    }
}
inline int dequant(int level, int qp, int pos) { return (level * kNorm[qp % 6][pos_class(pos)]) << (qp / 6); }
// 8.5.12.1 with flat scaling (LevelScale8x8 = 16 * normAdjust8x8)
inline int dequant8(int level, int qp, int pos) {
  const int64_t v = static_cast<int64_t>(level) * 16 * kNorm8[qp % 6][pos_class8(pos)];
  const int q6 = qp / 6;
  return static_cast<int>(q6 >= 6 ? v << (q6 - 6) : (v + (int64_t{1} << (5 - q6))) >> (6 - q6));
}

// Neighbouring samples of an N x N intra block: t[0] the corner, t[1 + x] the row above (x up to 2N - 1), l[1 + y] the column to the left.
struct IntraNeighbours {
  int t[17];
  int l[9];
};
// The directional modes of 4x4 / 8x8 blocks (8.3.1.2.x, 8.3.2.2.x; the samples of an 8x8 block are the filtered ones), into pred (n x n).
void predict_directional(int n, int mode, const IntraNeighbours& nb, bool has_top, bool has_left, ByteView pred) {
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
          }
          break;
        }
        case 3: v = (x == n - 1 && y == n - 1) ? (T(2 * n - 2) + 3 * T(2 * n - 1) + 2) >> 2 : (T(x + y) + 2 * T(x + y + 1) + T(x + y + 2) + 2) >> 2; break;
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
          const int z = x + 2 * y, zmax = 2 * n - 3;
          if (z > zmax) v = L(n - 1);
          else if (z == zmax) v = (L(n - 2) + 3 * L(n - 1) + 2) >> 2;
          else if (!(z & 1)) v = (L(y + (x >> 1)) + L(y + (x >> 1) + 1) + 1) >> 1;
          else v = (L(y + (x >> 1)) + 2 * L(y + (x >> 1) + 1) + L(y + (x >> 1) + 2) + 2) >> 2;
          break;
        }
      }
      pred[y * n + x] = static_cast<uint8_t>(v);
    }
  }
}

// 8.7.2.3 / 8.7.2.4: filters `count` sample lines across one edge of `plane`; q0 of line i is at q + i * along, q1 at q0 + across.
void filter_edge(ByteView plane, int64_t q, int64_t across, int64_t along, int count, const int* bs, int index_a, int alpha, int beta, bool chroma, int lines_per_bs) {
  auto px = [&](int64_t at) -> uint8_t& { return plane[static_cast<uint64_t>(at)]; };
  for (int i = 0; i < count; ++i) {
    const int s = bs[i / lines_per_bs];
    if (s == 0) continue;
    const int64_t at = q + i * along;
    const int p0 = px(at - across), p1 = px(at - 2 * across), q0 = px(at), q1 = px(at + across);
    if (!(iabs(p0 - q0) < alpha && iabs(p1 - p0) < beta && iabs(q1 - q0) < beta)) continue;
    if (s < 4) {
      const int tc0 = kTc0[index_a * 4 + s];
      if (chroma) {
        const int tc = tc0 + 1;
        const int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
        px(at - across) = clip_pixel(p0 + delta);
        px(at) = clip_pixel(q0 - delta);
      } else {
        const int p2 = px(at - 3 * across), q2 = px(at + 2 * across);
        const int ap = iabs(p2 - p0), aq = iabs(q2 - q0);
        const int tc = tc0 + (ap < beta) + (aq < beta);
        const int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
        px(at - across) = clip_pixel(p0 + delta);
        px(at) = clip_pixel(q0 - delta);
        if (ap < beta) px(at - 2 * across) = static_cast<uint8_t>(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
        if (aq < beta) px(at + across) = static_cast<uint8_t>(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
      }
    } else if (chroma) {
      px(at - across) = static_cast<uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
      px(at) = static_cast<uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
    } else {
      const int p2 = px(at - 3 * across), q2 = px(at + 2 * across);
      const int ap = iabs(p2 - p0), aq = iabs(q2 - q0);
      const bool small = iabs(p0 - q0) < ((alpha >> 2) + 2);
      if (ap < beta && small) {
        const int p3 = px(at - 4 * across);
        px(at - across) = static_cast<uint8_t>((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
        px(at - 2 * across) = static_cast<uint8_t>((p2 + p1 + p0 + q0 + 2) >> 2);
        px(at - 3 * across) = static_cast<uint8_t>((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
      } else {
        px(at - across) = static_cast<uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
      }
      if (aq < beta && small) {
        const int q3 = px(at + 3 * across);
        px(at) = static_cast<uint8_t>((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
        px(at + across) = static_cast<uint8_t>((p0 + q0 + q1 + q2 + 2) >> 2);
        px(at + 2 * across) = static_cast<uint8_t>((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
      } else {
        px(at) = static_cast<uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
      }
    }
  }
}

inline int b8_of(int blk) { return (blk >> 3) * 2 + ((blk & 3) >> 1); }   // the 8x8 block of a raster 4x4 block

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Neighbours and motion vector prediction

// The motion of 8x8 block b8 of the macroblock at (x, y) in list `list`.
PictureCoder::Neighbour PictureCoder::neighbour(int x, int y, int b8, int list) const {
  Neighbour n;
  const MbData* m = mb_at(x, y);
  if (!m) return n;
  n.available = true;
  if (m->intra || m->ref[list][b8] < 0) return n;
  n.ref = m->ref[list][b8];
  n.mv = {m->mv[list][b8][0], m->mv[list][b8][1]};
  return n;
}

// 8.4.1.3: the predicted vector of a 16x16 partition with reference index `ref` of list `list` (A: the left macroblock's block 1, B: the upper one's block 2, C: the
// upper right one's block 2, else D: the upper left one's block 3).
Vec PictureCoder::predict_mv(int mbx, int mby, int list, int ref) const {
  Neighbour a = neighbour(mbx - 1, mby, 1, list), b = neighbour(mbx, mby - 1, 2, list), c = neighbour(mbx + 1, mby - 1, 2, list);
  if (!c.available) c = neighbour(mbx - 1, mby - 1, 3, list);
  if (!b.available && !c.available && a.available) {
    b = a;
    c = a;
  }
  const int same = (a.ref == ref) + (b.ref == ref) + (c.ref == ref);
  if (same == 1) return a.ref == ref ? a.mv : b.ref == ref ? b.mv : c.mv;
  return {median3(a.mv.x, b.mv.x, c.mv.x), median3(a.mv.y, b.mv.y, c.mv.y)};
}

// 8.4.1.1: the vector of a P_Skip macroblock.
Vec PictureCoder::skip_mv(int mbx, int mby) const {
  const Neighbour a = neighbour(mbx - 1, mby, 1, 0), b = neighbour(mbx, mby - 1, 2, 0);
  if (!a.available || !b.available || (a.ref == 0 && a.mv == Vec{}) || (b.ref == 0 && b.mv == Vec{})) return {};
  return predict_mv(mbx, mby, 0, 0);
}

// 8.4.1.2: the direct prediction of the macroblock (direct_8x8_inference_flag 1: the co-located corner blocks), spatial (8.4.1.2.2) or temporal (8.4.1.2.3). False if it
// cannot be made exactly as a decoder makes it (the co-located picture's motion is not known, or a co-located block's reference is not in list 0).
bool PictureCoder::direct_motion(int mbx, int mby, DirectMotion* d) const {
  const H264EncPicture& p = *pic_;
  const uint64_t a = addr(mbx, mby);
  const H264EncReference& l1 = p.list[1][0];
  // the co-located 8x8 block's list 0 motion (else list 1's); false: intra
  auto colocated = [&](int b8, int* refidx, uint32_t* ident, Vec* mv) {
    const H264EncMbMotion& c = l1.motion[a];
    if (c.intra) return false;
    const int list = c.ref[0][b8] >= 0 ? 0 : 1;
    if (c.ref[list][b8] < 0) return false;
    *refidx = c.ref[list][b8];
    *ident = c.ident[list][b8];
    *mv = {c.mv[list][b8][0], c.mv[list][b8][1]};
    return true;
  };
  if (p.direct == H264EncDirect::Spatial) {
    int ref[2];
    for (int l = 0; l < 2; ++l) {
      const Neighbour na = neighbour(mbx - 1, mby, 1, l), nb = neighbour(mbx, mby - 1, 2, l);
      Neighbour nc = neighbour(mbx + 1, mby - 1, 2, l);
      if (!nc.available) nc = neighbour(mbx - 1, mby - 1, 3, l);
      auto min_positive = [](int x, int y) { return (x >= 0 && y >= 0) ? imin(x, y) : imax(x, y); };
      ref[l] = min_positive(na.ref, min_positive(nb.ref, nc.ref));
      if (ref[l] < 0) ref[l] = -1;
    }
    if (ref[0] < 0 && ref[1] < 0) {
      for (int l = 0; l < 2; ++l)
        for (int b = 0; b < 4; ++b) {
          d->ref[l][b] = 0;
          d->mv[l][b] = {};
        }
      return true;
    }
    Vec mvp[2];
    for (int l = 0; l < 2; ++l)
      if (ref[l] >= 0) mvp[l] = predict_mv(mbx, mby, l, ref[l]);
    const bool l1_short = !l1.long_term;
    if (l1_short && !col_known_) return false;
    for (int b = 0; b < 4; ++b) {
      bool col_zero = false;
      if (l1_short) {
        int ri = -1;
        uint32_t id = 0;
        Vec mv;
        col_zero = colocated(b, &ri, &id, &mv) && ri == 0 && mv.x >= -1 && mv.x <= 1 && mv.y >= -1 && mv.y <= 1;
      }
      for (int l = 0; l < 2; ++l) {
        d->ref[l][b] = static_cast<int8_t>(ref[l]);
        d->mv[l][b] = (ref[l] < 0 || (ref[l] == 0 && col_zero)) ? Vec{} : mvp[l];
      }
    }
    return true;
  }
  if (!col_known_) return false;
  const int cur_poc = static_cast<int32_t>(p.poc);
  for (int b = 0; b < 4; ++b) {
    int ri = -1;
    uint32_t id = 0;
    Vec col_mv;
    int r0 = 0;
    Vec mv0, mv1;
    if (colocated(b, &ri, &id, &col_mv)) {
      // MapColToList0: the lowest list 0 index of the picture the co-located block referred to
      r0 = -1;
      for (uint64_t i = 0; i < p.list[0].size() && r0 < 0; ++i)
        if (p.list[0][i].identity == id) r0 = static_cast<int>(i);
      if (r0 < 0) return false;
      const H264EncReference& p0 = p.list[0][static_cast<uint64_t>(r0)];
      if (p0.long_term || l1.poc - p0.poc == 0) {
        mv0 = col_mv;
      } else {
        const int td = clip3(-128, 127, l1.poc - p0.poc), tb = clip3(-128, 127, cur_poc - p0.poc);
        const int tx = (16384 + iabs(td / 2)) / td;
        const int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
        mv0 = {(dsf * col_mv.x + 128) >> 8, (dsf * col_mv.y + 128) >> 8};
        mv1 = {mv0.x - col_mv.x, mv0.y - col_mv.y};
      }
    }
    d->ref[0][b] = static_cast<int8_t>(r0);
    d->ref[1][b] = 0;
    d->mv[0][b] = mv0;
    d->mv[1][b] = mv1;
  }
  return true;
}

// 8.3.2.1: the predicted Intra8x8 mode of block b8 (cur: the modes chosen so far in this macroblock).
int PictureCoder::predicted_i8_mode(int mbx, int mby, int b8, ArrayView<const int> cur) const {
  int modes[2];
  for (int n = 0; n < 2; ++n) {
    const bool inside = n == 0 ? (b8 & 1) != 0 : (b8 & 2) != 0;
    if (inside) {
      modes[n] = cur[n == 0 ? b8 - 1 : b8 - 2];
      continue;
    }
    const MbData* m = n == 0 ? mb_at(mbx - 1, mby) : mb_at(mbx, mby - 1);
    if (!m || (!m->intra && pic_->constrained_intra_pred)) return 2;   // (dcPredModePredictedFlag)
    if (!m->inxn) {
      modes[n] = 2;
      continue;
    }
    const int nb8 = n == 0 ? b8 + 1 : b8 + 2;   // (the left one's block to the right, the upper one's block below)
    modes[n] = m->ipred[((nb8 >> 1) * 2) * 4 + (nb8 & 1) * 2];
  }
  return imin(modes[0], modes[1]);
}

// ---------------------------------------------------------------------------------------------------------------
// Prediction

// 8.3.3: Intra16x16 prediction (0 vertical, 1 horizontal, 2 DC, 3 plane). False if the mode needs samples that are not available.
bool PictureCoder::pred_i16(int mode, int mbx, int mby, ByteView pred) const {
  const uint32_t W = rec_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const bool left = intra_available(mbx - 1, mby), top = intra_available(mbx, mby - 1), corner = intra_available(mbx - 1, mby - 1);
  auto T = [&](int x) -> int { return rec_.y[base - W + static_cast<uint64_t>(x)]; };       // p[x, -1]
  auto L = [&](int y) -> int { return rec_.y[base + static_cast<uint64_t>(y) * W - 1]; };   // p[-1, y]
  if (mode == 0) {
    if (!top) return false;
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) pred[y * 16 + x] = static_cast<uint8_t>(T(x));
    return true;
  }
  if (mode == 1) {
    if (!left) return false;
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) pred[y * 16 + x] = static_cast<uint8_t>(L(y));
    return true;
  }
  if (mode == 2) {
    int sum = 0, dc;
    if (top)
      for (int x = 0; x < 16; ++x) sum += T(x);
    if (left)
      for (int y = 0; y < 16; ++y) sum += L(y);
    if (top && left) dc = (sum + 16) >> 5;
    else if (top || left) dc = (sum + 8) >> 4;
    else dc = 128;
    for (int i = 0; i < 256; ++i) pred[i] = static_cast<uint8_t>(dc);
    return true;
  }
  if (mode != 3 || !top || !left || !corner) return false;
  const int corner_sample = rec_.y[base - W - 1];
  int H = 0, V = 0;
  for (int i = 0; i < 8; ++i) {
    H += (i + 1) * (T(8 + i) - (6 - i >= 0 ? T(6 - i) : corner_sample));
    V += (i + 1) * (L(8 + i) - (6 - i >= 0 ? L(6 - i) : corner_sample));
  }
  const int a = 16 * (L(15) + T(15)), b = (5 * H + 32) >> 6, c = (5 * V + 32) >> 6;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) pred[y * 16 + x] = clip_pixel((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
  return true;
}

// 8.3.4: chroma prediction of one plane (0 DC, 1 horizontal, 2 vertical, 3 plane).
bool PictureCoder::pred_chroma(int mode, int mbx, int mby, int plane, ByteView pred) const {
  const uint32_t W = rec_.width / 2;
  const ByteView& p = plane == 0 ? rec_.u : rec_.v;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * W + static_cast<uint32_t>(mbx) * 8;
  const bool left = intra_available(mbx - 1, mby), top = intra_available(mbx, mby - 1), corner = intra_available(mbx - 1, mby - 1);
  auto T = [&](int x) -> int { return p[base - W + static_cast<uint64_t>(x)]; };
  auto L = [&](int y) -> int { return p[base + static_cast<uint64_t>(y) * W - 1]; };
  if (mode == 2) {
    if (!top) return false;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) pred[y * 8 + x] = static_cast<uint8_t>(T(x));
    return true;
  }
  if (mode == 1) {
    if (!left) return false;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) pred[y * 8 + x] = static_cast<uint8_t>(L(y));
    return true;
  }
  if (mode == 3) {
    if (!top || !left || !corner) return false;
    const int corner_sample = p[base - W - 1];
    int H = 0, V = 0;
    for (int i = 0; i < 4; ++i) {
      H += (i + 1) * (T(4 + i) - (2 - i >= 0 ? T(2 - i) : corner_sample));
      V += (i + 1) * (L(4 + i) - (2 - i >= 0 ? L(2 - i) : corner_sample));
    }
    const int a = 16 * (L(7) + T(7)), b = (34 * H + 32) >> 6, c = (34 * V + 32) >> 6;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) pred[y * 8 + x] = clip_pixel((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
    return true;
  }
  if (mode != 0) return false;
  for (int by = 0; by < 2; ++by) {
    for (int bx = 0; bx < 2; ++bx) {
      int st = 0, sl = 0;
      if (top)
        for (int x = 0; x < 4; ++x) st += T(bx * 4 + x);
      if (left)
        for (int y = 0; y < 4; ++y) sl += L(by * 4 + y);
      int dc;
      if ((bx == 0 && by == 0) || (bx == 1 && by == 1)) {
        if (top && left) dc = (st + sl + 4) >> 3;
        else if (left) dc = (sl + 2) >> 2;
        else if (top) dc = (st + 2) >> 2;
        else dc = 128;
      } else if (bx == 1) {
        if (top) dc = (st + 2) >> 2;
        else if (left) dc = (sl + 2) >> 2;
        else dc = 128;
      } else {
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

// 8.3.2.2: Intra8x8 prediction of block b8 (8x8 into pred) from the reconstruction so far, its reference samples filtered (8.3.2.2.1). False if the mode needs samples
// that are not available.
bool PictureCoder::pred_i8(int mode, int mbx, int mby, int b8, ByteView pred) const {
  const uint32_t W = rec_.width;
  const int bx = (b8 & 1) * 8, by = (b8 >> 1) * 8;
  const uint64_t base = (uint64_t{static_cast<uint32_t>(mby)} * 16 + static_cast<uint32_t>(by)) * W + static_cast<uint32_t>(mbx) * 16 + static_cast<uint32_t>(bx);
  const bool has_left = (b8 & 1) ? true : intra_available(mbx - 1, mby);
  const bool has_top = (b8 & 2) ? true : intra_available(mbx, mby - 1);
  const bool has_tl = b8 == 3 ? true : b8 == 1 ? intra_available(mbx, mby - 1) : b8 == 2 ? intra_available(mbx - 1, mby) : intra_available(mbx - 1, mby - 1);
  const bool has_tr = b8 == 0 ? intra_available(mbx, mby - 1) : b8 == 1 ? intra_available(mbx + 1, mby - 1) : b8 == 2;
  const bool need_top = mode == 0 || mode == 3 || mode == 7 || mode == 4 || mode == 5 || mode == 6;
  const bool need_left = mode == 1 || mode == 8 || mode == 4 || mode == 5 || mode == 6;
  if (mode < 0 || mode > 8 || (need_top && !has_top) || (need_left && !has_left) || ((mode == 4 || mode == 5 || mode == 6) && !has_tl)) return false;
  int top[16], left[8], tl = 128;
  for (int& v : top) v = 128;
  for (int& v : left) v = 128;
  if (has_top) {
    for (int x = 0; x < 8; ++x) top[x] = rec_.y[base - W + static_cast<uint64_t>(x)];
    for (int x = 8; x < 16; ++x) top[x] = has_tr ? rec_.y[base - W + static_cast<uint64_t>(x)] : top[7];
  }
  if (has_left)
    for (int y = 0; y < 8; ++y) left[y] = rec_.y[base + static_cast<uint64_t>(y) * W - 1];
  if (has_tl) tl = rec_.y[base - W - 1];
  IntraNeighbours nb;
  for (int& v : nb.t) v = 128;
  for (int& v : nb.l) v = 128;
  if (has_top) {
    nb.t[1] = has_tl ? (tl + 2 * top[0] + top[1] + 2) >> 2 : (3 * top[0] + top[1] + 2) >> 2;
    for (int x = 1; x < 15; ++x) nb.t[1 + x] = (top[x - 1] + 2 * top[x] + top[x + 1] + 2) >> 2;
    nb.t[16] = (top[14] + 3 * top[15] + 2) >> 2;
  }
  if (has_tl) {
    if (has_top && has_left) nb.t[0] = (top[0] + 2 * tl + left[0] + 2) >> 2;
    else if (has_top) nb.t[0] = (3 * tl + top[0] + 2) >> 2;
    else if (has_left) nb.t[0] = (3 * tl + left[0] + 2) >> 2;
    else nb.t[0] = tl;
    nb.l[0] = nb.t[0];
  }
  if (has_left) {
    nb.l[1] = has_tl ? (tl + 2 * left[0] + left[1] + 2) >> 2 : (3 * left[0] + left[1] + 2) >> 2;
    for (int y = 1; y < 7; ++y) nb.l[1 + y] = (left[y - 1] + 2 * left[y] + left[y + 1] + 2) >> 2;
    nb.l[8] = (left[6] + 3 * left[7] + 2) >> 2;
  }
  predict_directional(8, mode, nb, has_top, has_left, pred);
  return true;
}

// A w x h luma block at (x, y) predicted from list[list][ref] with `mv` (8.4.2.2.1).
void PictureCoder::pred_luma(int list, int ref, Vec mv, int x, int y, int w, int h, ByteView out) const {
  const H264EncPlanes& r = pic_->list[list][static_cast<uint64_t>(ref)].picture;
  const LumaPlane plane{r.y, static_cast<int>(r.width), static_cast<int>(r.height), static_cast<int>(r.width)};
  h264_luma_prediction(plane, x + (mv.x >> 2), y + (mv.y >> 2), mv.x & 3, mv.y & 3, w, h, out.first(static_cast<uint64_t>(w) * static_cast<uint64_t>(h)));
}

// 8.4.2.2.2: a w x h chroma block at chroma (x, y): eighth samples, bilinear, edge samples repeated.
void PictureCoder::pred_chroma_inter(int list, int ref, Vec mv, int x, int y, int w, int h, ByteView out_u, ByteView out_v) const {
  const H264EncPlanes& r = pic_->list[list][static_cast<uint64_t>(ref)].picture;
  const int CW = static_cast<int>(r.width / 2), CH = static_cast<int>(r.height / 2);
  const int cx0 = x + (mv.x >> 3), cy0 = y + (mv.y >> 3), fx = mv.x & 7, fy = mv.y & 7;
  for (int c = 0; c < 2; ++c) {
    const ConstByteView& p = c == 0 ? r.u : r.v;
    const ByteView out = c == 0 ? out_u : out_v;
    for (int yy = 0; yy < h; ++yy) {
      const int ya = clip3(0, CH - 1, cy0 + yy), yb = clip3(0, CH - 1, cy0 + yy + 1);
      for (int xx = 0; xx < w; ++xx) {
        const int xa = clip3(0, CW - 1, cx0 + xx), xb = clip3(0, CW - 1, cx0 + xx + 1);
        const int A = p[static_cast<uint64_t>(ya) * CW + xa], B = p[static_cast<uint64_t>(ya) * CW + xb];
        const int C = p[static_cast<uint64_t>(yb) * CW + xa], D = p[static_cast<uint64_t>(yb) * CW + xb];
        out[yy * w + xx] = static_cast<uint8_t>(((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B + (8 - fx) * fy * C + fx * fy * D + 32) >> 6);
      }
    }
  }
}

// The inter prediction of the macroblock from its 8x8 blocks' motion (m_.ref8 / mv8), into pred_y / pred_c (default weighted: bi-prediction averages).
void PictureCoder::build_inter_prediction(int mbx, int mby) {
  Mb& m = m_;
  bool uniform = true;
  for (int l = 0; l < 2; ++l)
    for (int b = 1; b < 4; ++b) uniform = uniform && m.ref8[l][b] == m.ref8[l][0] && m.mv8[l][b] == m.mv8[l][0];
  const int parts = uniform ? 1 : 4, size = uniform ? 16 : 8;
  for (int b = 0; b < parts; ++b) {
    const int ox = (b & 1) * 8, oy = (b >> 1) * 8;
    uint8_t py[2][256], pu[2][64], pv[2][64];
    int used[2], n = 0;
    for (int l = 0; l < 2; ++l) {
      if (m.ref8[l][b] < 0) continue;
      pred_luma(l, m.ref8[l][b], m.mv8[l][b], mbx * 16 + ox, mby * 16 + oy, size, size, ByteView(py[n]));
      pred_chroma_inter(l, m.ref8[l][b], m.mv8[l][b], mbx * 8 + ox / 2, mby * 8 + oy / 2, size / 2, size / 2, ByteView(pu[n]), ByteView(pv[n]));
      used[n++] = l;
    }
    (void)used;
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        const int v = n == 2 ? (py[0][y * size + x] + py[1][y * size + x] + 1) >> 1 : py[0][y * size + x];
        m.pred_y[(oy + y) * 16 + ox + x] = static_cast<uint8_t>(v);
      }
    const int cs = size / 2;
    for (int y = 0; y < cs; ++y)
      for (int x = 0; x < cs; ++x) {
        const int i = y * cs + x, o = (oy / 2 + y) * 8 + ox / 2 + x;
        m.pred_c[0][o] = static_cast<uint8_t>(n == 2 ? (pu[0][i] + pu[1][i] + 1) >> 1 : pu[0][i]);
        m.pred_c[1][o] = static_cast<uint8_t>(n == 2 ? (pv[0][i] + pv[1][i] + 1) >> 1 : pv[0][i]);
      }
  }
}

uint32_t PictureCoder::sad16(int mbx, int mby, ConstByteView pred) const {
  const uint32_t W = src_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  uint32_t sad = 0;
  for (int y = 0; y < 16; ++y) {
    const ConstByteView row = src_.y.subview(base + uint64_t{static_cast<uint32_t>(y)} * W, 16);
    for (int x = 0; x < 16; ++x) sad += static_cast<uint32_t>(iabs(int{row[static_cast<uint64_t>(x)]} - pred[y * 16 + x]));
  }
  return sad;
}

// The sum of the 4x4 Hadamard transforms' magnitudes of the luma residual, halved (what encoders report as SATD).
uint32_t PictureCoder::satd16(int mbx, int mby, ConstByteView pred) const {
  const uint32_t W = src_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  uint32_t total = 0;
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = (blk & 3) * 4, by = (blk >> 2) * 4;
    int d[16], t[16];
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) d[y * 4 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(by + y)} * W + static_cast<uint32_t>(bx + x)]} - pred[(by + y) * 16 + bx + x];
    for (int i = 0; i < 4; ++i) {
      const int a = d[i * 4] + d[i * 4 + 1], b = d[i * 4] - d[i * 4 + 1], c = d[i * 4 + 2] + d[i * 4 + 3], e = d[i * 4 + 2] - d[i * 4 + 3];
      t[i * 4] = a + c;
      t[i * 4 + 1] = b + e;
      t[i * 4 + 2] = a - c;
      t[i * 4 + 3] = b - e;
    }
    uint32_t s = 0;
    for (int j = 0; j < 4; ++j) {
      const int a = t[j] + t[4 + j], b = t[j] - t[4 + j], c = t[8 + j] + t[12 + j], e = t[8 + j] - t[12 + j];
      s += static_cast<uint32_t>(iabs(a + c) + iabs(b + e) + iabs(a - c) + iabs(b - e));
    }
    total += s / 2;
  }
  return total;
}

int PictureCoder::ref_cost(int list, int ref) const {
  const uint64_t n = pic_->list[list].size();
  if (n <= 1) return 0;
  if (n == 2) return lambda_;
  return lambda_ * ue_bits(static_cast<uint32_t>(ref));
}

// Intra refresh: a macroblock above the refreshed rows predicts only from references clean that far down.
bool PictureCoder::usable_reference(int mby, int list, int ref) const {
  const H264EncPicture& p = *pic_;
  if (!p.refresh || static_cast<uint32_t>(mby) >= p.refresh_begin) return true;
  return p.list[list][static_cast<uint64_t>(ref)].clean_rows >= p.refresh_begin;
}

// Whether the w x h block at (bx, by) of the macroblock may predict from list[list][ref] with `mv`: only from the reference's exact part, and with an intra refresh a
// macroblock of the refreshed rows only from the reference's rows above the refresh (luma 6-tap and chroma bilinear reach, and the 3 / 1 lines the in-loop filter changes
// above the first row not refreshed).
bool PictureCoder::vector_allowed(int mbx, int mby, int list, int ref, Vec mv, int bx, int by, int w, int h) const {
  const H264EncPicture& p = *pic_;
  const H264EncReference& r = p.list[list][static_cast<uint64_t>(ref)];
  const int x0 = mbx * 16 + bx, y0 = mby * 16 + by;
  // (the last luma and chroma sample a prediction reads: the 6-tap filter reaches 3 further, the bilinear one 1)
  const int luma_last_x = x0 + (mv.x >> 2) + w - 1 + ((mv.x & 3) ? 3 : 0), luma_last_y = y0 + (mv.y >> 2) + h - 1 + ((mv.y & 3) ? 3 : 0);
  const int chroma_last_x = x0 / 2 + (mv.x >> 3) + w / 2 - 1 + ((mv.x & 7) ? 1 : 0), chroma_last_y = y0 / 2 + (mv.y >> 3) + h / 2 - 1 + ((mv.y & 7) ? 1 : 0);
  if (r.exact_width && (luma_last_x >= static_cast<int>(r.exact_width) || chroma_last_x >= static_cast<int>(r.exact_width / 2))) return false;
  if (r.exact_height && (luma_last_y >= static_cast<int>(r.exact_height) || chroma_last_y >= static_cast<int>(r.exact_height / 2))) return false;
  if (!p.refresh || static_cast<uint32_t>(mby) >= p.refresh_begin) return true;
  if (!usable_reference(mby, list, ref)) return false;
  return luma_last_y < static_cast<int>(p.refresh_begin) * 16 - 3 && chroma_last_y < static_cast<int>(p.refresh_begin) * 8 - 1;
}

bool PictureCoder::direct_allowed(int mbx, int mby, const DirectMotion& d) const {
  for (int l = 0; l < 2; ++l)
    for (int b = 0; b < 4; ++b) {
      if (d.ref[l][b] < 0) continue;
      if (static_cast<uint64_t>(d.ref[l][b]) >= pic_->list[l].size()) return false;
      if (!vector_allowed(mbx, mby, l, d.ref[l][b], d.mv[l][b], (b & 1) * 8, (b >> 1) * 8, 8, 8)) return false;
    }
  return true;
}

bool PictureCoder::search(int mbx, int mby, int list, int ref, Vec pred, Vec* best, int* cost) const {
  const H264EncPicture& p = *pic_;
  uint8_t buf[256];
  auto cost_of = [&](Vec v) {
    pred_luma(list, ref, v, mbx * 16, mby * 16, 16, 16, buf);
    return static_cast<int>(sad16(mbx, mby, buf)) + mv_cost(v, pred);
  };
  // the hint of this macroblock for this reference, if any
  bool hinted = false;
  Vec hint;
  if (!p.hints.empty()) {
    const H264EncHint& h = p.hints[addr(mbx, mby)];
    if (h.ref[list] == ref) {
      hinted = true;
      hint = {h.mv[list][0], h.mv[list][1]};
    }
  }
  const bool limited = hinted && p.hint_mode == H264EncHintMode::StartHintLimited;
  const int limit = static_cast<int>(p.hint_limit > 1024 ? 1024 : p.hint_limit) * 4;
  // (vectors stay within what every level allows: the search keeps to +-64 samples vertically, +-256 horizontally)
  auto allowed = [&](Vec v) {
    if (!(v.x >= -1024 && v.x <= 1020 && v.y >= -256 && v.y <= 252)) return false;
    if (limited && (iabs(v.x - hint.x) > limit || iabs(v.y - hint.y) > limit)) return false;
    return vector_allowed(mbx, mby, list, ref, v, 0, 0, 16, 16);
  };
  Vec b{};
  int bc = 0x7FFFFFFF;
  auto consider = [&](Vec v) {
    if (!allowed(v)) return;
    const int c = cost_of(v);
    if (c < bc) {
      bc = c;
      b = v;
    }
  };
  auto whole = [](Vec v) { return Vec{v.x & ~3, v.y & ~3}; };
  if (hinted) {
    consider(hint);
    consider(whole(hint));
  }
  if (!hinted || p.hint_mode == H264EncHintMode::Full) {
    consider({0, 0});
    consider(whole(pred));
    for (int dir = 0; dir < 3; ++dir) {
      const int x = dir == 0 ? mbx - 1 : dir == 1 ? mbx : mbx + 1, y = dir == 0 ? mby : mby - 1;
      const Neighbour n = neighbour(x, y, dir == 0 ? 1 : 2, list);
      if (n.ref >= 0) consider(whole(n.mv));
    }
  }
  if (bc == 0x7FFFFFFF) {
    // (nothing the constraints allow among the starting points: the co-located block, if allowed)
    consider({0, 0});
    if (bc == 0x7FFFFFFF) return false;
  }
  for (int iter = 0; iter < 32; ++iter) {
    const Vec c = b;
    static const int kLarge[8][2] = {{0, -8}, {0, 8}, {-8, 0}, {8, 0}, {-4, -4}, {4, -4}, {-4, 4}, {4, 4}};
    for (const auto& d : kLarge) consider({c.x + d[0], c.y + d[1]});
    if (b == c) break;
  }
  for (int iter = 0; iter < 8; ++iter) {
    const Vec c = b;
    static const int kSmall[4][2] = {{0, -4}, {0, 4}, {-4, 0}, {4, 0}};
    for (const auto& d : kSmall) consider({c.x + d[0], c.y + d[1]});
    if (b == c) break;
  }
  static const int kAround[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
  if (p.precision != H264EncPrecision::Full) {
    const Vec c = b;
    for (const auto& d : kAround) consider({c.x + 2 * d[0], c.y + 2 * d[1]});
  }
  if (p.precision == H264EncPrecision::Quarter) {
    const Vec c = b;
    for (const auto& d : kAround) consider({c.x + d[0], c.y + d[1]});
  }
  *best = b;
  *cost = bc + ref_cost(list, ref);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Residual

void PictureCoder::quant_luma_i16(int mbx, int mby) {
  const uint32_t W = src_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int qp = m_.qp, qpd = qp / 6, qpm = qp % 6;
  const int shift = 15 + qpd, round = (1 << shift) / 3;
  int dcm[16];
  bool any_ac = false;
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    int r[16], w[16];
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        r[y * 4 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + static_cast<uint32_t>(bx * 4 + x)]} - m_.pred_y[(by * 4 + y) * 16 + bx * 4 + x];
    forward4x4(r, w);
    dcm[blk] = w[0];
    m_.ac[blk][0] = 0;
    for (int i = 1; i < 16; ++i) {
      const int p = kZigzag4x4[i];
      m_.ac[blk][i] = quantize(w[p], kMf[qpm][pos_class(p)], shift, round);
      any_ac = any_ac || m_.ac[blk][i] != 0;
    }
  }
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

// Inter luma with 4x4 transforms: m_.ac and the coded block pattern; *cost the SSD of the reconstruction plus lambda times an estimate of the bits.
bool PictureCoder::quant_luma_4x4(int mbx, int mby, uint64_t* cost) {
  const uint32_t W = src_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int qp = m_.qp, qpd = qp / 6, qpm = qp % 6;
  const int shift = 15 + qpd, round = (1 << shift) / 6;
  int cbp = 0;
  uint64_t bits = 0;
  uint8_t rec[256];
  for (int i = 0; i < 256; ++i) rec[i] = m_.pred_y[i];
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    int r[16], w[16];
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        r[y * 4 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + static_cast<uint32_t>(bx * 4 + x)]} - m_.pred_y[(by * 4 + y) * 16 + bx * 4 + x];
    forward4x4(r, w);
    for (int i = 0; i < 16; ++i) {
      const int p = kZigzag4x4[i];
      m_.ac[blk][i] = quantize(w[p], kMf[qpm][pos_class(p)], shift, round);
      bits += static_cast<uint64_t>(level_bits(m_.ac[blk][i]));
      if (m_.ac[blk][i]) cbp |= 1 << ((by >> 1) * 2 + (bx >> 1));
    }
  }
  // (an 8x8 block with only a lone small coefficient is not worth its bits: x264's decimation, roughly)
  for (int b8 = 0; b8 < 4; ++b8) {
    if (!((cbp >> b8) & 1)) continue;
    int score = 0;
    for (int s = 0; s < 4; ++s) {
      const int blk = ((b8 >> 1) * 2 + (s >> 1)) * 4 + (b8 & 1) * 2 + (s & 1);
      for (int i = 0; i < 16; ++i) score += m_.ac[blk][i] == 0 ? 0 : iabs(m_.ac[blk][i]) > 1 ? 16 : 3;
    }
    if (score >= 4) continue;
    cbp &= ~(1 << b8);
    for (int s = 0; s < 4; ++s) {
      const int blk = ((b8 >> 1) * 2 + (s >> 1)) * 4 + (b8 & 1) * 2 + (s & 1);
      for (int i = 0; i < 16; ++i) m_.ac[blk][i] = 0;
    }
  }
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    if (!((cbp >> ((by >> 1) * 2 + (bx >> 1))) & 1)) continue;
    int d[16] = {};
    for (int i = 0; i < 16; ++i)
      if (m_.ac[blk][i]) d[kZigzag4x4[i]] = dequant(m_.ac[blk][i], qp, kZigzag4x4[i]);
    inverse4x4_add(d, ByteView(rec, 256), static_cast<uint64_t>(by * 4 * 16 + bx * 4), 16);
  }
  uint64_t ssd = 0;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      const int e = int{src_.y[base + uint64_t{static_cast<uint32_t>(y)} * W + static_cast<uint32_t>(x)]} - rec[y * 16 + x];
      ssd += static_cast<uint64_t>(e * e);
    }
  m_.cbp_luma = cbp;
  *cost = ssd + uint64_t{kLambda2[qp]} * bits;
  return cbp != 0;
}

// Luma with 8x8 transforms (inter, or with `intra` the intra rounding): m_.l8 and the coded block pattern; *cost as quant_luma_4x4's.
bool PictureCoder::quant_luma_8x8(int mbx, int mby, bool intra, uint64_t* cost) {
  const uint32_t W = src_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int qp = m_.qp, qpd = qp / 6, qpm = qp % 6;
  const int shift = 16 + qpd, round = (1 << shift) / (intra ? 3 : 6);
  int cbp = 0;
  uint64_t bits = 0;
  uint8_t rec[256];
  for (int i = 0; i < 256; ++i) rec[i] = m_.pred_y[i];
  for (int b8 = 0; b8 < 4; ++b8) {
    const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
    int r[64], w[64];
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x)
        r[y * 8 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(oy + y)} * W + static_cast<uint32_t>(ox + x)]} - m_.pred_y[(oy + y) * 16 + ox + x];
    forward8x8(r, w);
    int score = 0;
    for (int i = 0; i < 64; ++i) {
      const int p = kZigzag8x8[i];
      m_.l8[b8][i] = quantize(w[p], kMf8[qpm][pos_class8(p)], shift, round);
      bits += static_cast<uint64_t>(level_bits(m_.l8[b8][i]));
      score += m_.l8[b8][i] == 0 ? 0 : iabs(m_.l8[b8][i]) > 1 ? 16 : 3;
    }
    if (score == 0 || (!intra && score < 4)) {
      for (int i = 0; i < 64; ++i) m_.l8[b8][i] = 0;
      continue;
    }
    cbp |= 1 << b8;
    int d[64] = {};
    for (int i = 0; i < 64; ++i)
      if (m_.l8[b8][i]) d[kZigzag8x8[i]] = dequant8(m_.l8[b8][i], qp, kZigzag8x8[i]);
    inverse8x8_add(d, ByteView(rec, 256), static_cast<uint64_t>(oy * 16 + ox), 16);
  }
  uint64_t ssd = 0;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      const int e = int{src_.y[base + uint64_t{static_cast<uint32_t>(y)} * W + static_cast<uint32_t>(x)]} - rec[y * 16 + x];
      ssd += static_cast<uint64_t>(e * e);
    }
  m_.cbp_luma = cbp;
  *cost = ssd + uint64_t{kLambda2[qp]} * bits;
  return cbp != 0;
}

void PictureCoder::quant_chroma(int mbx, int mby, bool intra) {
  const uint32_t W = src_.width / 2;
  const int qpd = m_.qpc / 6, qpm = m_.qpc % 6;
  const int shift = 15 + qpd, round = (1 << shift) / (intra ? 3 : 6);
  bool any_dc = false, any_ac = false;
  for (int c = 0; c < 2; ++c) {
    const ConstByteView& s = c == 0 ? src_.u : src_.v;
    const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * W + static_cast<uint32_t>(mbx) * 8;
    int dcv[4];
    for (int blk = 0; blk < 4; ++blk) {
      const int bx = blk & 1, by = blk >> 1;
      int r[16], w[16];
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
          r[y * 4 + x] = int{s[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + static_cast<uint32_t>(bx * 4 + x)]} - m_.pred_c[c][(by * 4 + y) * 8 + bx * 4 + x];
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

// 8.5: luma prediction plus the inverse-transformed levels, as the decoder computes it (Intra8x8 macroblocks are reconstructed as they are coded, in code_i8).
void PictureCoder::reconstruct_luma(int mbx, int mby) {
  const uint32_t W = rec_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) rec_.y[base + uint64_t{static_cast<uint32_t>(y)} * W + static_cast<uint32_t>(x)] = m_.pred_y[y * 16 + x];
  const int qp = m_.qp;
  if (m_.t8x8) {
    for (int b8 = 0; b8 < 4; ++b8) {
      if (!((m_.cbp_luma >> b8) & 1)) continue;
      int d[64] = {};
      for (int i = 0; i < 64; ++i)
        if (m_.l8[b8][i]) d[kZigzag8x8[i]] = dequant8(m_.l8[b8][i], qp, kZigzag8x8[i]);
      inverse8x8_add(d, rec_.y, base + uint64_t{static_cast<uint32_t>((b8 >> 1) * 8)} * W + static_cast<uint32_t>((b8 & 1) * 8), W);
    }
    return;
  }
  int dcl[16] = {};
  const bool i16 = m_.kind == Kind::I16;
  if (i16) {   // 8.5.10: the DC levels, inverse Hadamard, scaled
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
    const int ls = 16 * kNorm[qp % 6][0], qpd = qp / 6;
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
        d[p] = dequant(m_.ac[blk][i], qp, p);
        any = true;
      }
    }
    if (i16) {
      d[0] = dcl[blk];
      any = any || d[0] != 0;
    }
    if (any) inverse4x4_add(d, rec_.y, base + uint64_t{static_cast<uint32_t>(by * 4)} * W + static_cast<uint32_t>(bx * 4), W);
  }
}

void PictureCoder::reconstruct_chroma(int mbx, int mby) {
  const uint32_t CW = rec_.width / 2;
  const uint64_t cbase = uint64_t{static_cast<uint32_t>(mby)} * 8 * CW + static_cast<uint32_t>(mbx) * 8;
  const int qpc = m_.qpc;
  for (int c = 0; c < 2; ++c) {
    ByteView& p = c == 0 ? rec_.u : rec_.v;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) p[cbase + uint64_t{static_cast<uint32_t>(y)} * CW + static_cast<uint32_t>(x)] = m_.pred_c[c][y * 8 + x];
    if (m_.cbp_chroma == 0) continue;
    const int a = m_.cdc[c][0], b = m_.cdc[c][1], cc = m_.cdc[c][2], dd = m_.cdc[c][3];
    const int f[4] = {a + b + cc + dd, a - b + cc - dd, a + b - cc - dd, a - b - cc + dd};
    const int ls = 16 * kNorm[qpc % 6][0];
    for (int blk = 0; blk < 4; ++blk) {
      int d[16] = {};
      d[0] = static_cast<int>((static_cast<int64_t>(f[blk]) * ls << (qpc / 6)) >> 5);
      if (m_.cbp_chroma == 2) {
        for (int i = 1; i < 16; ++i) {
          if (m_.cac[c][blk][i] == 0) continue;
          const int pos = kZigzag4x4[i];
          d[pos] = dequant(m_.cac[c][blk][i], qpc, pos);
        }
      }
      inverse4x4_add(d, p, cbase + uint64_t{static_cast<uint32_t>((blk >> 1) * 4)} * CW + static_cast<uint32_t>((blk & 1) * 4), CW);
    }
  }
}

// Intra8x8: block by block, the mode with the least SAD (and mode bits), the block transformed, quantised and reconstructed at once (the next block predicts from it).
int PictureCoder::code_i8(int mbx, int mby) {
  const uint32_t W = rec_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int qp = m_.qp, qpd = qp / 6, qpm = qp % 6;
  const int shift = 16 + qpd, round = (1 << shift) / 3;
  int total = lambda_ * 6;
  int cbp = 0;
  for (int b8 = 0; b8 < 4; ++b8) {
    const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
    const int predicted = predicted_i8_mode(mbx, mby, b8, m_.i8_mode);
    int best = -1, best_cost = 0x7FFFFFFF;
    uint8_t pred[64], best_pred[64];
    for (int mode = 0; mode < 9; ++mode) {
      if (!pred_i8(mode, mbx, mby, b8, pred)) continue;
      int sad = 0;
      for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) sad += iabs(int{src_.y[base + uint64_t{static_cast<uint32_t>(oy + y)} * W + static_cast<uint32_t>(ox + x)]} - pred[y * 8 + x]);
      const int c = sad + lambda_ * (mode == predicted ? 1 : 4);
      if (c < best_cost) {
        best_cost = c;
        best = mode;
        for (int i = 0; i < 64; ++i) best_pred[i] = pred[i];
      }
    }
    m_.i8_mode[b8] = best;   // (mode 2 always has its samples)
    m_.i8_pred[b8] = predicted;
    total += best_cost;
    int r[64], w[64];
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) {
        m_.pred_y[(oy + y) * 16 + ox + x] = best_pred[y * 8 + x];
        r[y * 8 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(oy + y)} * W + static_cast<uint32_t>(ox + x)]} - best_pred[y * 8 + x];
      }
    forward8x8(r, w);
    bool nz = false;
    for (int i = 0; i < 64; ++i) {
      const int p = kZigzag8x8[i];
      m_.l8[b8][i] = quantize(w[p], kMf8[qpm][pos_class8(p)], shift, round);
      nz = nz || m_.l8[b8][i] != 0;
    }
    const uint64_t at = base + uint64_t{static_cast<uint32_t>(oy)} * W + static_cast<uint32_t>(ox);
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) rec_.y[at + uint64_t{static_cast<uint32_t>(y)} * W + static_cast<uint32_t>(x)] = best_pred[y * 8 + x];
    if (nz) {
      cbp |= 1 << b8;
      int d[64] = {};
      for (int i = 0; i < 64; ++i)
        if (m_.l8[b8][i]) d[kZigzag8x8[i]] = dequant8(m_.l8[b8][i], qp, kZigzag8x8[i]);
      inverse8x8_add(d, rec_.y, at, W);
    }
  }
  m_.cbp_luma = cbp;
  return total;
}

// ---------------------------------------------------------------------------------------------------------------
// Macroblocks

void PictureCoder::code_mb(int mbx, int mby) {
  const H264EncPicture& p = *pic_;
  const uint64_t a = addr(mbx, mby);
  MbData& info = mbs_[a];
  info = MbData();
  info.slice = slice_;
  Mb& m = m_;
  m.t8x8 = false;
  m.skip = false;
  m.cbf_dc = 0;
  m.cbp_luma = m.cbp_chroma = 0;
  m.chroma_mode = 0;
  for (int l = 0; l < 2; ++l) {
    m.mv[l] = m.mvp[l] = {};
    m.ref[l] = 0;
    for (int b = 0; b < 4; ++b) {
      m.ref8[l][b] = -1;
      m.mv8[l][b] = {};
    }
  }
  for (int b = 0; b < 4; ++b) m.i8_mode[b] = m.i8_pred[b] = 2;
  const uint64_t bits_before = bw_.bits() + (cabac() ? cw_.state().outstanding : 0);
  // the macroblock's QP
  int qp = p.qp;
  if (!p.qp_map.empty()) qp = p.qp_map_absolute ? p.qp_map[a] : qp + p.qp_map[a];
  qp = clip3(0, 51, qp);
  m.qp = qp;
  m.qpc = kChromaQp[qp];
  lambda_ = kLambda[qp];

  const bool inter_slice = p_slice() || b_slice();
  const bool force_intra = !inter_slice || (p.refresh && static_cast<uint32_t>(mby) >= p.refresh_begin && static_cast<uint32_t>(mby) < p.refresh_end);
  const bool unchanged = !force_intra && !p.unchanged.empty() && p.unchanged[a] && p.unchanged_ref < p.list[0].size() &&
                         vector_allowed(mbx, mby, 0, static_cast<int>(p.unchanged_ref), {}, 0, 0, 16, 16);

  // intra: the Intra16x16 mode with the least SAD
  uint8_t ipred[256];
  int i16_cost = 0x7FFFFFFF, best_mode = 2;
  for (int mode = 0; mode < 4; ++mode) {
    if (!pred_i16(mode, mbx, mby, ipred)) continue;
    const int c = static_cast<int>(sad16(mbx, mby, ipred)) + lambda_ * (mode == 2 ? 4 : 6);
    if (c < i16_cost) {
      i16_cost = c;
      best_mode = mode;
    }
  }
  // inter
  int inter_cost = 0x7FFFFFFF, direct_cost = 0x7FFFFFFF;
  DirectMotion dm;
  if (unchanged) {
    m.kind = Kind::Inter;
    m.pred_flags = 1;
    m.ref[0] = static_cast<int>(p.unchanged_ref);
    m.mv[0] = {};
    m.mvp[0] = predict_mv(mbx, mby, 0, m.ref[0]);
  } else if (!force_intra) {
    int best_cost[2] = {0x7FFFFFFF, 0x7FFFFFFF};
    int best_ref[2] = {0, 0};
    Vec best_mv[2] = {};
    for (int l = 0; l < (b_slice() ? 2 : 1); ++l) {
      for (uint64_t r = 0; r < p.list[l].size(); ++r) {
        if (!usable_reference(mby, l, static_cast<int>(r))) continue;
        const Vec pred = predict_mv(mbx, mby, l, static_cast<int>(r));
        Vec v;
        int c = 0;
        if (!search(mbx, mby, l, static_cast<int>(r), pred, &v, &c)) continue;
        if (c < best_cost[l]) {
          best_cost[l] = c;
          best_ref[l] = static_cast<int>(r);
          best_mv[l] = v;
        }
      }
    }
    if (p_slice()) {
      m.pred_flags = 1;
      if (best_cost[0] != 0x7FFFFFFF) inter_cost = best_cost[0] + lambda_ * 1;
      // P_Skip costs next to nothing: taken when its prediction is about as good
      const Vec smv = skip_mv(mbx, mby);
      if (vector_allowed(mbx, mby, 0, 0, smv, 0, 0, 16, 16)) {
        uint8_t sp[256];
        pred_luma(0, 0, smv, mbx * 16, mby * 16, 16, 16, sp);
        const int skip_cost = static_cast<int>(sad16(mbx, mby, sp));
        if (inter_cost == 0x7FFFFFFF || skip_cost <= inter_cost + lambda_ * 4) {
          best_ref[0] = 0;
          best_mv[0] = smv;
          inter_cost = skip_cost;
        }
      }
    } else if (b_slice()) {
      const int c0 = best_cost[0] == 0x7FFFFFFF ? 0x7FFFFFFF : best_cost[0] + lambda_ * 3, c1 = best_cost[1] == 0x7FFFFFFF ? 0x7FFFFFFF : best_cost[1] + lambda_ * 3;
      int cb = 0x7FFFFFFF;
      if (c0 != 0x7FFFFFFF && c1 != 0x7FFFFFFF) {
        uint8_t p0[256], p1[256];
        pred_luma(0, best_ref[0], best_mv[0], mbx * 16, mby * 16, 16, 16, p0);
        pred_luma(1, best_ref[1], best_mv[1], mbx * 16, mby * 16, 16, 16, p1);
        for (int i = 0; i < 256; ++i) p0[i] = static_cast<uint8_t>((p0[i] + p1[i] + 1) >> 1);
        const Vec pr0 = predict_mv(mbx, mby, 0, best_ref[0]), pr1 = predict_mv(mbx, mby, 1, best_ref[1]);
        cb = static_cast<int>(sad16(mbx, mby, p0)) + mv_cost(best_mv[0], pr0) + mv_cost(best_mv[1], pr1) + ref_cost(0, best_ref[0]) + ref_cost(1, best_ref[1]) + lambda_ * 5;
      }
      if (cb != 0x7FFFFFFF || c0 != 0x7FFFFFFF || c1 != 0x7FFFFFFF) {
        m.pred_flags = cb <= c0 && cb <= c1 ? 3 : c0 <= c1 ? 1 : 2;
        inter_cost = m.pred_flags == 3 ? cb : m.pred_flags == 1 ? c0 : c1;
      }
      // direct prediction: no vectors or references to code
    if (p.direct != H264EncDirect::None && direct_motion(mbx, mby, &dm) && direct_allowed(mbx, mby, dm)) {
        for (int l = 0; l < 2; ++l)
          for (int b = 0; b < 4; ++b) {
            m.ref8[l][b] = dm.ref[l][b];
            m.mv8[l][b] = dm.mv[l][b];
          }
        build_inter_prediction(mbx, mby);
        direct_cost = static_cast<int>(sad16(mbx, mby, m.pred_y)) + lambda_ * 1;
      }
    }
    for (int l = 0; l < 2; ++l) {
      m.ref[l] = best_ref[l];
      m.mv[l] = best_mv[l];
      if (m.pred_flags & (1 << l)) m.mvp[l] = predict_mv(mbx, mby, l, best_ref[l]);
    }
  }
  // Intra8x8 (codes the macroblock's luma into the reconstruction: weighed last)
  int i8_cost = 0x7FFFFFFF;
  if (!unchanged && p.transform_8x8) {
    const int best_other = imin(imin(i16_cost, inter_cost), direct_cost);
    if (force_intra || i16_cost <= best_other + 8 * lambda_ || best_other > 256 * lambda_) i8_cost = code_i8(mbx, mby);
  }
  if (unchanged) m.kind = Kind::Inter;
  else if (i8_cost < i16_cost && i8_cost <= inter_cost && i8_cost <= direct_cost) m.kind = Kind::I8;
  else if (i16_cost <= inter_cost && i16_cost <= direct_cost) m.kind = Kind::I16;
  else if (direct_cost < inter_cost) m.kind = Kind::Direct;
  else m.kind = Kind::Inter;

  const bool intra = m.kind == Kind::I16 || m.kind == Kind::I8;
  if (intra) {
    if (m.kind == Kind::I16) {
      m.i16_mode = best_mode;
      pred_i16(best_mode, mbx, mby, m.pred_y);
    } else {
      m.t8x8 = true;
    }
    uint32_t best = 0xFFFFFFFFu;
    for (int mode = 0; mode < 4; ++mode) {
      uint8_t cp[2][64];
      if (!pred_chroma(mode, mbx, mby, 0, cp[0]) || !pred_chroma(mode, mbx, mby, 1, cp[1])) continue;
      uint32_t s = 0;
      const uint32_t CW = src_.width / 2;
      const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * CW + static_cast<uint32_t>(mbx) * 8;
      for (int c = 0; c < 2; ++c) {
        const ConstByteView& sp = c == 0 ? src_.u : src_.v;
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x) s += static_cast<uint32_t>(iabs(int{sp[base + uint64_t{static_cast<uint32_t>(y)} * CW + static_cast<uint32_t>(x)]} - cp[c][y * 8 + x]));
      }
      if (s < best) {
        best = s;
        m.chroma_mode = mode;
      }
    }
    pred_chroma(m.chroma_mode, mbx, mby, 0, m.pred_c[0]);
    pred_chroma(m.chroma_mode, mbx, mby, 1, m.pred_c[1]);
    if (m.kind == Kind::I16) quant_luma_i16(mbx, mby);
    quant_chroma(mbx, mby, true);
  } else {
    if (m.kind == Kind::Inter) {
      for (int l = 0; l < 2; ++l)
        for (int b = 0; b < 4; ++b) {
          m.ref8[l][b] = (m.pred_flags & (1 << l)) ? static_cast<int8_t>(m.ref[l]) : static_cast<int8_t>(-1);
          m.mv8[l][b] = (m.pred_flags & (1 << l)) ? m.mv[l] : Vec{};
        }
    } else {
      for (int l = 0; l < 2; ++l)
        for (int b = 0; b < 4; ++b) {
          m.ref8[l][b] = dm.ref[l][b];
          m.mv8[l][b] = dm.mv[l][b];
        }
    }
    build_inter_prediction(mbx, mby);
    if (unchanged) {
      for (auto& blk : m.ac)
        for (int& v : blk) v = 0;
      m.cbp_luma = m.cbp_chroma = 0;
      for (auto& c : m.cdc)
        for (int& v : c) v = 0;
      for (auto& c : m.cac)
        for (auto& b : c)
          for (int& v : b) v = 0;
    } else {
      uint64_t cost4 = 0, cost8 = 0;
      const bool any4 = quant_luma_4x4(mbx, mby, &cost4);
      const int cbp4 = m.cbp_luma;
      if (p.transform_8x8 && any4) {
        int saved[16][16];
        for (int b = 0; b < 16; ++b)
          for (int i = 0; i < 16; ++i) saved[b][i] = m.ac[b][i];
        const bool any8 = quant_luma_8x8(mbx, mby, false, &cost8);
        if (any8 && cost8 < cost4) {
          m.t8x8 = true;
        } else {
          m.cbp_luma = cbp4;
          for (int b = 0; b < 16; ++b)
            for (int i = 0; i < 16; ++i) m.ac[b][i] = saved[b][i];
        }
      }
      quant_chroma(mbx, mby, false);
    }
    if (m.cbp_luma == 0) m.t8x8 = false;   // (transform_size_8x8_flag is not coded, so 0)
    m.skip = m.cbp_luma == 0 && m.cbp_chroma == 0 &&
             ((p_slice() && m.kind == Kind::Inter && m.ref[0] == 0 && m.mv[0] == skip_mv(mbx, mby)) || (b_slice() && m.kind == Kind::Direct));
  }
  // The QP the macroblock has: a delta is coded only with residual (an Intra16x16 macroblock always has one); without, it keeps the one before.
  const bool coded_qp = m.kind == Kind::I16 || m.cbp_luma != 0 || m.cbp_chroma != 0;
  int delta = 0;
  if (coded_qp) {
    delta = m.qp - qp_prev_;
    if (delta > 25) delta -= 52;
    if (delta < -26) delta += 52;
  } else {
    m.qp = qp_prev_;
    m.qpc = kChromaQp[m.qp];
  }
  if (m.kind != Kind::I8) reconstruct_luma(mbx, mby);
  reconstruct_chroma(mbx, mby);
  for (uint8_t& n : m.nz) n = 0;
  if (m.skip) {
    write_skip(mbx, mby);
    ++stats_.skipped;
  } else {
    write_mb(mbx, mby, delta);
    if (intra) ++stats_.intra;
    else ++stats_.inter;
  }
  if (m.kind == Kind::Direct) ++stats_.direct;
  dqp_nonzero_ = !m.skip && coded_qp && delta != 0;
  qp_prev_ = m.qp;

  // what the picture keeps
  info.intra = intra;
  info.skip = m.skip;
  info.direct = m.kind == Kind::Direct;
  info.i16 = m.kind == Kind::I16;
  info.inxn = m.kind == Kind::I8;
  info.t8x8 = m.t8x8;
  info.qp = static_cast<uint8_t>(m.qp);
  info.cbp = static_cast<uint8_t>(m.cbp_luma | (m.cbp_chroma << 4));
  info.chroma_mode = intra ? static_cast<uint8_t>(m.chroma_mode) : 0;
  info.cbf_dc = m.cbf_dc;
  for (int i = 0; i < 16; ++i) info.ipred[i] = static_cast<int8_t>(m.kind == Kind::I8 ? m.i8_mode[b8_of(i)] : 2);
  if (!intra) {
    for (int l = 0; l < 2; ++l) {
      for (int b = 0; b < 4; ++b) {
        if (m.ref8[l][b] < 0) continue;
        info.ref[l][b] = m.ref8[l][b];
        info.ident[l][b] = p.list[l][static_cast<uint64_t>(m.ref8[l][b])].identity;
        info.mv[l][b][0] = static_cast<int16_t>(m.mv8[l][b].x);
        info.mv[l][b][1] = static_cast<int16_t>(m.mv8[l][b].y);
      }
      if (m.kind == Kind::Inter && !m.skip && (m.pred_flags & (1 << l))) {
        info.mvd[l][0] = static_cast<int16_t>(imin(iabs(m.mv[l].x - m.mvp[l].x), 127));
        info.mvd[l][1] = static_cast<int16_t>(imin(iabs(m.mv[l].y - m.mvp[l].y), 127));
      }
    }
    const int l = m.ref8[0][0] >= 0 ? 0 : 1;
    stats_.vector_x_sum += m.mv8[l][0].x;
    stats_.vector_y_sum += m.mv8[l][0].y;
  }
  for (int i = 0; i < 24; ++i) info.nz[i] = m.nz[i];
  info.nzmask = 0;
  for (int i = 0; i < 16; ++i) {
    const bool coded = m.t8x8 ? ((m.cbp_luma >> b8_of(i)) & 1) != 0 : m.nz[i] != 0;
    if (coded) info.nzmask = static_cast<uint16_t>(info.nzmask | (1u << i));
  }
  stats_.qp_sum += static_cast<uint64_t>(m.qp);
  if (!outputs_.motion.empty()) {
    H264EncMbMotion& mo = outputs_.motion[a];
    mo = H264EncMbMotion();
    mo.intra = intra ? 1 : 0;
    for (int l = 0; l < 2; ++l)
      for (int b = 0; b < 4; ++b) {
        mo.ref[l][b] = info.ref[l][b];
        mo.ident[l][b] = info.ref[l][b] >= 0 ? static_cast<uint32_t>(info.ident[l][b]) : 0;
        mo.mv[l][b][0] = info.mv[l][b][0];
        mo.mv[l][b][1] = info.mv[l][b][1];
      }
  }
  if (!outputs_.report.empty()) {
    H264EncMbReport& r = outputs_.report[a];
    r.qp = static_cast<uint8_t>(m.qp);
    r.satd = satd16(mbx, mby, m.pred_y);
    const uint64_t bits_after = bw_.bits() + (cabac() ? cw_.state().outstanding : 0);
    r.bits = static_cast<uint32_t>(bits_after - bits_before);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The in-loop filter (8.7), once the picture is coded

// 8.7.2.1: the strength of the edge between 4x4 block blk_p of p and blk_q of q.
int PictureCoder::boundary_strength(const MbData& p, const MbData& q, int blk_p, int blk_q, bool mb_edge) const {
  if (p.intra || q.intra) return mb_edge ? 4 : 3;
  if (((p.nzmask >> blk_p) & 1) || ((q.nzmask >> blk_q) & 1)) return 2;
  const int bp = b8_of(blk_p), bq = b8_of(blk_q);
  int64_t rp[2], rq[2];
  for (int l = 0; l < 2; ++l) {
    rp[l] = p.ref[l][bp] >= 0 ? p.ident[l][bp] : -1;
    rq[l] = q.ref[l][bq] >= 0 ? q.ident[l][bq] : -1;
  }
  const int np = (rp[0] >= 0) + (rp[1] >= 0), nq = (rq[0] >= 0) + (rq[1] >= 0);
  if (np != nq) return 1;
  auto far = [](const int16_t* a, const int16_t* b) { return iabs(a[0] - b[0]) >= 4 || iabs(a[1] - b[1]) >= 4; };
  const int16_t* vp[2] = {p.mv[0][bp], p.mv[1][bp]};
  const int16_t* vq[2] = {q.mv[0][bq], q.mv[1][bq]};
  if (np == 1) {
    const int lp = rp[0] >= 0 ? 0 : 1, lq = rq[0] >= 0 ? 0 : 1;
    if (rp[lp] != rq[lq]) return 1;
    return far(vp[lp], vq[lq]) ? 1 : 0;
  }
  if (np == 0) return 0;
  if (!((rp[0] == rq[0] && rp[1] == rq[1]) || (rp[0] == rq[1] && rp[1] == rq[0]))) return 1;
  if (rp[0] != rp[1]) {
    if (rp[0] == rq[0]) return (far(vp[0], vq[0]) || far(vp[1], vq[1])) ? 1 : 0;
    return (far(vp[0], vq[1]) || far(vp[1], vq[0])) ? 1 : 0;
  }
  return ((far(vp[0], vq[0]) || far(vp[1], vq[1])) && (far(vp[0], vq[1]) || far(vp[1], vq[0]))) ? 1 : 0;
}

void PictureCoder::deblock_mb(int mbx, int mby) {
  const uint8_t idc = pic_->disable_deblocking_filter_idc;
  if (idc == 1) return;
  const MbData& c = mbs_[addr(mbx, mby)];
  const bool left_ok = mbx > 0 && !(idc == 2 && mbs_[addr(mbx - 1, mby)].slice != c.slice);
  const bool top_ok = mby > 0 && !(idc == 2 && mbs_[addr(mbx, mby - 1)].slice != c.slice);
  int bs[2][4][4];
  for (int dir = 0; dir < 2; ++dir) {
    for (int e = 0; e < 4; ++e) {
      for (int s = 0; s < 4; ++s) {
        bs[dir][e][s] = 0;
        if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
        const MbData& p = e == 0 ? (dir == 0 ? mbs_[addr(mbx - 1, mby)] : mbs_[addr(mbx, mby - 1)]) : c;
        const int blk_q = dir == 0 ? s * 4 + e : e * 4 + s;
        const int blk_p = dir == 0 ? (e == 0 ? s * 4 + 3 : s * 4 + e - 1) : (e == 0 ? 12 + s : (e - 1) * 4 + s);
        bs[dir][e][s] = boundary_strength(p, c, blk_p, blk_q, e == 0);
      }
    }
  }
  const int64_t ls = rec_.width;
  const int64_t ly = static_cast<int64_t>(mby) * 16 * ls + mbx * 16;
  for (int dir = 0; dir < 2; ++dir) {
    for (int e = 0; e < 4; ++e) {
      if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
      if (c.t8x8 && (e & 1)) continue;   // (8x8 transform: the edges inside the 8x8 blocks are not filtered)
      const MbData& p = e == 0 ? (dir == 0 ? mbs_[addr(mbx - 1, mby)] : mbs_[addr(mbx, mby - 1)]) : c;
      const int qpav = (p.qp + c.qp + 1) >> 1;
      const int index_a = clip3(0, 51, qpav), index_b = clip3(0, 51, qpav);
      const int alpha = kAlpha[index_a], beta = kBeta[index_b];
      if (alpha == 0 || beta == 0) continue;
      filter_edge(rec_.y, dir == 0 ? ly + e * 4 : ly + e * 4 * ls, dir == 0 ? 1 : ls, dir == 0 ? ls : 1, 16, bs[dir][e], index_a, alpha, beta, false, 4);
    }
  }
  const int64_t cs = rec_.width / 2;
  for (int plane = 0; plane < 2; ++plane) {
    ByteView pl = plane == 0 ? rec_.u : rec_.v;
    const int64_t base = static_cast<int64_t>(mby) * 8 * cs + mbx * 8;
    for (int dir = 0; dir < 2; ++dir) {
      for (int e = 0; e < 2; ++e) {
        if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
        const MbData& p = e == 0 ? (dir == 0 ? mbs_[addr(mbx - 1, mby)] : mbs_[addr(mbx, mby - 1)]) : c;
        const int qpav = (kChromaQp[p.qp] + kChromaQp[c.qp] + 1) >> 1;
        const int index_a = clip3(0, 51, qpav), index_b = clip3(0, 51, qpav);
        const int alpha = kAlpha[index_a], beta = kBeta[index_b];
        if (alpha == 0 || beta == 0) continue;
        filter_edge(pl, dir == 0 ? base + e * 4 : base + e * 4 * cs, dir == 0 ? 1 : cs, dir == 0 ? cs : 1, 8, bs[dir][e * 2], index_a, alpha, beta, true, 2);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Slices and pictures

PictureCoder::Resume PictureCoder::save() const {
  Resume r;
  r.bits = bw_.mark();
  if (cabac()) r.cabac = cw_.state();
  r.skip_run = skip_run_;
  r.qp_prev = qp_prev_;
  r.dqp_nonzero = dqp_nonzero_;
  r.stats = stats_;
  r.epb_scanned = epb_scanned_;
  r.epb_count = epb_count_;
  r.epb_zeros = epb_zeros_;
  return r;
}
void PictureCoder::restore(const Resume& r) {
  bw_.rewind(r.bits);
  if (cabac()) cw_.restore(r.cabac);
  skip_run_ = r.skip_run;
  qp_prev_ = r.qp_prev;
  dqp_nonzero_ = r.dqp_nonzero;
  stats_ = r.stats;
  epb_scanned_ = r.epb_scanned;
  epb_count_ = r.epb_count;
  epb_zeros_ = r.epb_zeros;
}

// A bound on the bytes of the slice's NAL unit if it ended now (start code, header, the RBSP so far, what ending it adds, emulation prevention bytes).
uint64_t PictureCoder::projected_slice_bytes() {
  const ConstByteView done = bw_.bytes();
  for (; epb_scanned_ < done.size(); ++epb_scanned_) {
    const uint8_t b = done[epb_scanned_];
    if (epb_zeros_ >= 2 && b <= 3) {
      ++epb_count_;
      epb_zeros_ = 0;
    }
    epb_zeros_ = b == 0 ? epb_zeros_ + 1 : 0;
  }
  uint64_t tail = static_cast<uint64_t>(bw_.pending_bits()) + 1 + 8;
  tail += cabac() ? cw_.pending_bits() + 2 : (skip_run_ ? static_cast<uint64_t>(ue_bits(skip_run_)) : 0);
  const uint64_t tail_bytes = (tail + 7) / 8;
  return 5 + done.size() + tail_bytes + epb_count_ + (tail_bytes + 1) / 2;
}

H264EncResult PictureCoder::encode(const H264EncPicture& pic, const H264EncPlanes& src, const H264EncWritablePlanes& rec, ByteBuffer* out, List<H264EncSlice>* slices,
                                   H264EncStatistics* stats, const H264EncOutputs& outputs) {
  pic_ = &pic;
  mbw_ = pic.mb_width;
  mbh_ = pic.mb_height;
  const uint64_t mbs = uint64_t{mbw_} * mbh_;
  const uint32_t W = mbw_ * 16, H = mbh_ * 16;
  // (what the picture says must be what the slice syntax can say)
  if (mbw_ == 0 || mbh_ == 0 || mbw_ > 1024 || mbh_ > 1024 || pic.disable_deblocking_filter_idc > 2 || pic.log2_max_frame_num < 4 || pic.log2_max_frame_num > 16 ||
      (pic.pic_order_cnt_type != 0 && pic.pic_order_cnt_type != 2) || pic.log2_max_poc_lsb < 4 || pic.log2_max_poc_lsb > 16 || pic.qp > 51 ||
      static_cast<uint8_t>(pic.type) > 3 || pic.pic_parameter_set_id > 255 || pic.idr_pic_id > 65535 || static_cast<uint8_t>(pic.entropy) > 1 ||
      static_cast<uint8_t>(pic.direct) > 2 || static_cast<uint8_t>(pic.hint_mode) > 2)
    return H264EncResult::PictureControlNotSupported;
  const bool inter = pic.type == H264EncFrameType::P || pic.type == H264EncFrameType::B;
  if ((inter && pic.list[0].empty()) || (pic.type == H264EncFrameType::B && pic.list[1].empty()) || pic.list[0].size() > 16 || pic.list[1].size() > 16 ||
      (!inter && (!pic.list[0].empty() || !pic.list[1].empty())) || (pic.type == H264EncFrameType::P && !pic.list[1].empty()))
    return H264EncResult::PictureControlNotSupported;
  if (pic.type == H264EncFrameType::B && pic.pic_order_cnt_type == 2) return H264EncResult::PictureControlNotSupported;   // (output order is decoding order there)
  if (!pic.qp_map.empty() && pic.qp_map.size() != mbs) return H264EncResult::PictureControlNotSupported;
  for (const int8_t q : pic.qp_map)
    if (pic.qp_map_absolute ? (q < 0 || q > 51) : (q < -51 || q > 51)) return H264EncResult::PictureControlNotSupported;
  for (const H264EncMarking& m : pic.marking)
    if (m.operation > 6) return H264EncResult::PictureControlNotSupported;
  for (int l = 0; l < 2; ++l)
    for (const H264EncListModification& m : pic.modification[l])
      if (m.idc > 3) return H264EncResult::PictureControlNotSupported;
  if (pic.refresh && (pic.refresh_begin >= pic.refresh_end || pic.refresh_end > mbh_)) return H264EncResult::PictureControlNotSupported;
  if ((!pic.unchanged.empty() && pic.unchanged.size() != mbs) || (!pic.hints.empty() && pic.hints.size() != mbs)) return H264EncResult::PictureControlNotSupported;
  if ((!outputs.motion.empty() && outputs.motion.size() != mbs) || (!outputs.report.empty() && outputs.report.size() != mbs)) return H264EncResult::PictureControlNotSupported;
  auto planes_fit = [&](uint64_t y, uint64_t u, uint64_t v, uint32_t w, uint32_t h) {
    return w == W && h == H && y >= uint64_t{W} * H && u >= uint64_t{W} * H / 4 && v >= uint64_t{W} * H / 4;
  };
  if (!planes_fit(src.y.size(), src.u.size(), src.v.size(), src.width, src.height) || !planes_fit(rec.y.size(), rec.u.size(), rec.v.size(), rec.width, rec.height))
    return H264EncResult::InvalidReferences;
  for (int l = 0; l < 2; ++l)
    for (const H264EncReference& r : pic.list[l])
      if (!planes_fit(r.picture.y.size(), r.picture.u.size(), r.picture.v.size(), r.picture.width, r.picture.height) || (!r.motion.empty() && r.motion.size() != mbs))
        return H264EncResult::InvalidReferences;
  const bool by_bytes = pic.slice_mode == H264EncSliceMode::BytesPerSlice;
  const uint32_t count = H264PictureEncoder::slice_count(pic.slice_mode, pic.slice_value, mbw_, mbh_);
  if (count == 0) return H264EncResult::SubregionLayoutNotSupported;
  starts_.clear();
  for (uint32_t s = 0; s <= count; ++s) {
    uint64_t start;
    switch (pic.slice_mode) {
      case H264EncSliceMode::MacroblocksPerSlice: start = uint64_t{s} * pic.slice_value; break;
      case H264EncSliceMode::RowsPerSlice: start = uint64_t{s} * pic.slice_value * mbw_; break;
      case H264EncSliceMode::SlicesPerFrame: start = uint64_t{s} * mbs / count; break;
      default: start = s == 0 ? 0 : mbs;
    }
    if (!starts_.push_back(static_cast<uint32_t>(start < mbs ? start : mbs))) return H264EncResult::NoMemory;
  }
  mbs_.clear();
  if (!mbs_.reserve(mbs)) return H264EncResult::NoMemory;
  for (uint64_t i = 0; i < mbs; ++i)
    if (!mbs_.push_back(MbData())) return H264EncResult::NoMemory;
  src_ = src;
  rec_ = rec;
  outputs_ = outputs;
  stats_ = H264EncStatistics();
  col_known_ = pic.type == H264EncFrameType::B && pic.direct != H264EncDirect::None && pic.list[1][0].motion.size() == mbs;
  slices->clear();
  const uint64_t mark = out->size();
  const bool idr = pic.type == H264EncFrameType::Idr;
  const int ref_idc = !(pic.reference || idr) ? 0 : (idr || pic.type == H264EncFrameType::I) ? 3 : 2;
  uint64_t bins = 0, vcl_bytes = 0;
  uint32_t first = 0;
  for (uint32_t s = 0; first < mbs; ++s) {
    const uint32_t end = by_bytes ? static_cast<uint32_t>(mbs) : starts_[s + 1];
    slice_ = static_cast<int32_t>(s);
    slice_first_ = first;
    bw_.reset();
    write_slice_header(first);
    const uint64_t header_bits = bw_.bits();
    if (cabac()) {
      bw_.align_with_ones();   // cabac_alignment_one_bit
      cw_.start(pic.type == H264EncFrameType::I || pic.type == H264EncFrameType::Idr ? 0 : 1, pic.qp);
    }
    qp_prev_ = pic.qp;
    dqp_nonzero_ = false;
    skip_run_ = 0;
    epb_scanned_ = epb_count_ = 0;
    epb_zeros_ = 0;
    uint32_t a = first;
    for (; a < end; ++a) {
      Resume resume;
      if (by_bytes && a > first) resume = save();
      if (cabac() && a > first) cw_.terminate(bw_, 0);   // end_of_slice_flag of the macroblock before
      code_mb(static_cast<int>(a % mbw_), static_cast<int>(a / mbw_));
      if (by_bytes && a > first && projected_slice_bytes() > pic.slice_value) {
        restore(resume);
        break;
      }
    }
    finish_slice_data();
    if (cabac()) bins += cw_.bins();
    if (!bw_.ok()) {
      out->truncate(mark);
      return H264EncResult::NoMemory;
    }
    // the NAL unit: start code, header, the RBSP with emulation prevention bytes
    H264EncSlice slice;
    slice.offset = out->size();
    slice.first_mb = first;
    slice.mb_count = a - first;
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    bool ok = out->append(ConstByteView(kStart, 4)) && out->put8(static_cast<uint8_t>((ref_idc << 5) | (idr ? 5 : 1)));
    int zeros = 0;
    uint64_t index = 0, inserted_in_header = 0;
    const uint64_t header_bytes = (header_bits + 7) / 8;
    for (const uint8_t b : bw_.bytes()) {
      if (zeros >= 2 && b <= 3) {
        ok = out->put8(3) && ok;
        zeros = 0;
        if (index < header_bytes) ++inserted_in_header;
      }
      ok = out->put8(b) && ok;
      zeros = b == 0 ? zeros + 1 : 0;
      ++index;
    }
    slice.size = out->size() - slice.offset;
    slice.header_bits = 40 + header_bits + 8 * inserted_in_header;
    vcl_bytes += slice.size - 4;
    if (!ok || !slices->push_back(slice)) {
      out->truncate(mark);
      return H264EncResult::NoMemory;
    }
    first = a;
  }
  // 7.4.2.10 / 9.3.4.6: CABAC's bins may not outnumber 32 / 3 per byte (plus RawMbBits / 32 a macroblock): cabac_zero_words after the last slice make up for it
  if (cabac()) {
    const uint64_t allowance = 32 * vcl_bytes + 3 * (3072 * mbs / 32);
    if (3 * bins > allowance) {
      const uint64_t words = (3 * bins - allowance + 95) / 96;
      static const uint8_t kZeroWord[3] = {0, 0, 3};
      for (uint64_t i = 0; i < words; ++i)
        if (!out->append(ConstByteView(kZeroWord, 3))) {
          out->truncate(mark);
          return H264EncResult::NoMemory;
        }
      (*slices)[slices->count() - 1].size += 3 * words;
    }
  }
  for (uint32_t y = 0; y < mbh_; ++y)
    for (uint32_t x = 0; x < mbw_; ++x) deblock_mb(static_cast<int>(x), static_cast<int>(y));
  // the rows clean of what an intra refresh has not reached yet
  uint32_t clean = 0xFFFFFFFFu;
  if (inter)
    for (int l = 0; l < 2; ++l)
      for (const H264EncReference& r : pic.list[l]) clean = r.clean_rows < clean ? r.clean_rows : clean;
  if (pic.refresh && inter && clean < pic.refresh_end) clean = pic.refresh_end;
  if (clean >= mbh_) clean = 0xFFFFFFFFu;
  stats_.clean_rows = clean;
  if (stats) *stats = stats_;
  return H264EncResult::Ok;
}

}  // namespace h264

H264PictureEncoder::H264PictureEncoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
H264PictureEncoder::~H264PictureEncoder() {}

uint32_t H264PictureEncoder::slice_count(H264EncSliceMode mode, uint32_t value, uint32_t mb_width, uint32_t mb_height) {
  const uint64_t mbs = uint64_t{mb_width} * mb_height;
  if (mbs == 0) return 0;
  switch (mode) {
    case H264EncSliceMode::FullFrame: return 1;
    case H264EncSliceMode::BytesPerSlice: return value == 0 ? 0 : 1;
    case H264EncSliceMode::MacroblocksPerSlice: return value == 0 ? 0 : static_cast<uint32_t>((mbs + value - 1) / value);
    case H264EncSliceMode::RowsPerSlice: return value == 0 ? 0 : (mb_height + value - 1) / value;
    case H264EncSliceMode::SlicesPerFrame: return value == 0 || value > mbs ? 0 : value;
    default: return 0;
  }
}

H264EncResult H264PictureEncoder::encode(const H264EncPicture& picture, const H264EncPlanes& source, const H264EncWritablePlanes& reconstruction, ByteBuffer* out,
                                         List<H264EncSlice>* slices, H264EncStatistics* statistics, const H264EncOutputs& outputs) {
  if (!core_) {
    core_ = UniquePointer<h264::PictureCoder>::make(allocator_, allocator_);
    if (!core_) return H264EncResult::NoMemory;
  }
  return core_->encode(picture, source, reconstruction, out, slices, statistics, outputs);
}

}  // namespace hardware
