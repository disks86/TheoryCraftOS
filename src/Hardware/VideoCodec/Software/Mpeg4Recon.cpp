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


// MPEG-4 Part 2 and H.263 decoding, part 2: inverse quantisation (7.4.4), the inverse transform, motion compensation with half and quarter samples (7.6), and the
// concealment of damaged macroblocks. The arithmetic is FFmpeg's wherever the standard allows a choice (see Mpeg4.hpp).
#include "Hardware/VideoCodec/Software/Mpeg4Core.hpp"

namespace hardware {
namespace mpeg4 {

namespace {
#include "Hardware/VideoCodec/Software/Mpeg4Tables.inc"

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
uint8_t clip_pixel(int64_t v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// ---- the inverse transform ----
// FFmpeg's "simple" IDCT (simple_idct_template.c, 8-bit): rows with a DC-only shortcut, then columns, 14-bit cosines. The input is clipped to the range of the
// standard ([-2048, 2047]); the columns are summed in 64 bits so that no input can overflow.
constexpr int kW1 = 22725, kW2 = 21407, kW3 = 19266, kW4 = 16383, kW5 = 12873, kW6 = 8867, kW7 = 4520;
constexpr int kRowShift = 11, kColShift = 20;

void idct(ArrayView<const int16_t> in, int (&out)[64]) {
  int32_t t[64];
  for (int r = 0; r < 8; ++r) {
    int32_t x[8];
    for (int c = 0; c < 8; ++c) x[c] = clamp_int(in[static_cast<uint64_t>(r * 8 + c)], -2048, 2047);
    int32_t* row = &t[r * 8];
    if ((x[1] | x[2] | x[3] | x[4] | x[5] | x[6] | x[7]) == 0) {
      for (int c = 0; c < 8; ++c) row[c] = x[0] * 8;
      continue;
    }
    int32_t a0 = kW4 * x[0] + (1 << (kRowShift - 1)), a1 = a0, a2 = a0, a3 = a0;
    a0 += kW2 * x[2];
    a1 += kW6 * x[2];
    a2 -= kW6 * x[2];
    a3 -= kW2 * x[2];
    int32_t b0 = kW1 * x[1] + kW3 * x[3], b1 = kW3 * x[1] - kW7 * x[3], b2 = kW5 * x[1] - kW1 * x[3], b3 = kW7 * x[1] - kW5 * x[3];
    a0 += kW4 * x[4] + kW6 * x[6];
    a1 += -kW4 * x[4] - kW2 * x[6];
    a2 += -kW4 * x[4] + kW2 * x[6];
    a3 += kW4 * x[4] - kW6 * x[6];
    b0 += kW5 * x[5] + kW7 * x[7];
    b1 += -kW1 * x[5] - kW5 * x[7];
    b2 += kW7 * x[5] + kW3 * x[7];
    b3 += kW3 * x[5] - kW1 * x[7];
    row[0] = (a0 + b0) >> kRowShift;
    row[7] = (a0 - b0) >> kRowShift;
    row[1] = (a1 + b1) >> kRowShift;
    row[6] = (a1 - b1) >> kRowShift;
    row[2] = (a2 + b2) >> kRowShift;
    row[5] = (a2 - b2) >> kRowShift;
    row[3] = (a3 + b3) >> kRowShift;
    row[4] = (a3 - b3) >> kRowShift;
  }
  for (int c = 0; c < 8; ++c) {
    const int64_t c0 = t[c], c1 = t[8 + c], c2 = t[16 + c], c3 = t[24 + c], c4 = t[32 + c], c5 = t[40 + c], c6 = t[48 + c], c7 = t[56 + c];
    int64_t a0 = kW4 * (c0 + ((1 << (kColShift - 1)) / kW4)), a1 = a0, a2 = a0, a3 = a0;
    a0 += kW2 * c2;
    a1 += kW6 * c2;
    a2 -= kW6 * c2;
    a3 -= kW2 * c2;
    int64_t b0 = kW1 * c1 + kW3 * c3, b1 = kW3 * c1 - kW7 * c3, b2 = kW5 * c1 - kW1 * c3, b3 = kW7 * c1 - kW5 * c3;
    a0 += kW4 * c4 + kW6 * c6;
    a1 += -kW4 * c4 - kW2 * c6;
    a2 += -kW4 * c4 + kW2 * c6;
    a3 += kW4 * c4 - kW6 * c6;
    b0 += kW5 * c5 + kW7 * c7;
    b1 += -kW1 * c5 - kW5 * c7;
    b2 += kW7 * c5 + kW3 * c7;
    b3 += kW3 * c5 - kW1 * c7;
    out[0 * 8 + c] = static_cast<int>((a0 + b0) >> kColShift);
    out[7 * 8 + c] = static_cast<int>((a0 - b0) >> kColShift);
    out[1 * 8 + c] = static_cast<int>((a1 + b1) >> kColShift);
    out[6 * 8 + c] = static_cast<int>((a1 - b1) >> kColShift);
    out[2 * 8 + c] = static_cast<int>((a2 + b2) >> kColShift);
    out[5 * 8 + c] = static_cast<int>((a2 - b2) >> kColShift);
    out[3 * 8 + c] = static_cast<int>((a3 + b3) >> kColShift);
    out[4 * 8 + c] = static_cast<int>((a3 - b3) >> kColShift);
  }
}

// Xvid's integer IDCT (its C reference, as FFmpeg's xvididct.c has it, and with the same error profile as its SIMD versions): Xvid's encoder reconstructs
// with it, so an Xvid stream (user data "XviD<build>") is decoded with it too, as FFmpeg does. The rows are kept in 16 bits as Xvid keeps them, including
// the rows it leaves alone when they round to zero.
namespace xvid {
constexpr int kRowShift = 11, kColShift = 6;
constexpr int kTab04[7] = {22725, 21407, 19266, 16384, 12873, 8867, 4520};
constexpr int kTab17[7] = {31521, 29692, 26722, 22725, 17855, 12299, 6270};
constexpr int kTab26[7] = {29692, 27969, 25172, 21407, 16819, 11585, 5906};
constexpr int kTab35[7] = {26722, 25172, 22654, 19266, 15137, 10426, 5315};
constexpr int kTan1 = 0x32EC, kTan2 = 0x6A0A, kTan3 = 0xAB0E, kSqrt2 = 0x5A82;

int16_t narrow(int64_t v) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint64_t>(v))); }
int32_t wrap32(int64_t v) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v))); }
int mult(int c, int x) { return wrap32(static_cast<int64_t>(c) * x) >> 16; }

bool row(int16_t* in, const int (&tab)[7], int rnd) {
  const int64_t c1 = tab[0], c2 = tab[1], c3 = tab[2], c4 = tab[3], c5 = tab[4], c6 = tab[5], c7 = tab[6];
  const int right = in[5] | in[6] | in[7], left = in[1] | in[2] | in[3];
  if (!(right | in[4])) {
    const int64_t k = c4 * in[0] + rnd;
    if (left) {
      const int64_t a0 = k + c2 * in[2], a1 = k + c6 * in[2], a2 = k - c6 * in[2], a3 = k - c2 * in[2];
      const int64_t b0 = c1 * in[1] + c3 * in[3], b1 = c3 * in[1] - c7 * in[3], b2 = c5 * in[1] - c1 * in[3], b3 = c7 * in[1] - c5 * in[3];
      in[0] = narrow(wrap32(a0 + b0) >> kRowShift);
      in[1] = narrow(wrap32(a1 + b1) >> kRowShift);
      in[2] = narrow(wrap32(a2 + b2) >> kRowShift);
      in[3] = narrow(wrap32(a3 + b3) >> kRowShift);
      in[4] = narrow(wrap32(a3 - b3) >> kRowShift);
      in[5] = narrow(wrap32(a2 - b2) >> kRowShift);
      in[6] = narrow(wrap32(a1 - b1) >> kRowShift);
      in[7] = narrow(wrap32(a0 - b0) >> kRowShift);
    } else {
      const int a0 = wrap32(k) >> kRowShift;
      if (!a0) return false;
      for (int i = 0; i < 8; ++i) in[i] = narrow(a0);
    }
  } else if (!(left | right)) {
    const int a0 = wrap32(rnd + c4 * (in[0] + in[4])) >> kRowShift, a1 = wrap32(rnd + c4 * (in[0] - in[4])) >> kRowShift;
    in[0] = in[3] = in[4] = in[7] = narrow(a0);
    in[1] = in[2] = in[5] = in[6] = narrow(a1);
  } else {
    const int64_t k = c4 * in[0] + rnd;
    const int64_t a0 = k + c2 * in[2] + c4 * in[4] + c6 * in[6], a1 = k + c6 * in[2] - c4 * in[4] - c2 * in[6];
    const int64_t a2 = k - c6 * in[2] - c4 * in[4] + c2 * in[6], a3 = k - c2 * in[2] + c4 * in[4] - c6 * in[6];
    const int64_t b0 = c1 * in[1] + c3 * in[3] + c5 * in[5] + c7 * in[7], b1 = c3 * in[1] - c7 * in[3] - c1 * in[5] - c5 * in[7];
    const int64_t b2 = c5 * in[1] - c1 * in[3] + c7 * in[5] + c3 * in[7], b3 = c7 * in[1] - c5 * in[3] + c3 * in[5] - c1 * in[7];
    in[0] = narrow(wrap32(a0 + b0) >> kRowShift);
    in[1] = narrow(wrap32(a1 + b1) >> kRowShift);
    in[2] = narrow(wrap32(a2 + b2) >> kRowShift);
    in[3] = narrow(wrap32(a3 + b3) >> kRowShift);
    in[4] = narrow(wrap32(a3 - b3) >> kRowShift);
    in[5] = narrow(wrap32(a2 - b2) >> kRowShift);
    in[6] = narrow(wrap32(a1 - b1) >> kRowShift);
    in[7] = narrow(wrap32(a0 - b0) >> kRowShift);
  }
  return true;
}

// One column; `rows`: how many of the first rows can be non-zero (3, 4 or 8), which picks the shortcut Xvid takes (the same arithmetic on fewer inputs).
void column(int16_t* in, int rows) {
  auto at = [&](int r) { return r < rows ? static_cast<int>(in[r * 8]) : 0; };
  int mm0, mm1, mm2, mm3, mm4, mm5, mm6, mm7, t;
  if (rows == 8) {
    mm4 = at(7);
    mm5 = at(5);
    mm6 = at(3);
    mm7 = at(1);
    mm0 = mult(kTan1, mm4) + mm7;
    mm1 = mult(kTan1, mm7) - mm4;
    mm2 = mult(kTan3, mm5) + mm6;
    mm3 = mult(kTan3, mm6) - mm5;
    mm7 = mm0 + mm2;
    mm4 = mm1 - mm3;
    mm0 = mm0 - mm2;
    mm1 = mm1 + mm3;
    mm6 = mm0 + mm1;
    mm5 = mm0 - mm1;
    mm5 = 2 * mult(kSqrt2, mm5);
    mm6 = 2 * mult(kSqrt2, mm6);
    mm1 = at(2);
    mm2 = at(6);
    mm3 = mult(kTan2, mm2) + mm1;
    mm2 = mult(kTan2, mm1) - mm2;
    mm0 = at(0) + at(4);
    mm1 = at(0) - at(4);
  } else if (rows == 4) {
    mm0 = at(1);
    mm2 = at(3);
    mm1 = mult(kTan1, mm0);
    mm3 = mult(kTan3, mm2);
    mm7 = mm0 + mm2;
    mm4 = mm1 - mm3;
    mm0 = mm0 - mm2;
    mm1 = mm1 + mm3;
    mm6 = mm0 + mm1;
    mm5 = mm0 - mm1;
    mm6 = 2 * mult(kSqrt2, mm6);
    mm5 = 2 * mult(kSqrt2, mm5);
    mm0 = mm1 = at(0);
    mm3 = at(2);
    mm2 = mult(kTan2, mm3);
  } else {
    mm7 = at(1);
    mm4 = mult(kTan1, mm7);
    mm6 = mm7 + mm4;
    mm5 = mm7 - mm4;
    mm6 = 2 * mult(kSqrt2, mm6);
    mm5 = 2 * mult(kSqrt2, mm5);
    mm0 = mm1 = at(0);
    mm3 = at(2);
    mm2 = mult(kTan2, mm3);
  }
  t = mm0 + mm3; mm3 = mm0 - mm3; mm0 = t;
  t = mm0 + mm7; mm7 = mm0 - mm7; mm0 = t;
  in[0] = narrow(mm0 >> kColShift);
  in[56] = narrow(mm7 >> kColShift);
  t = mm3 + mm4; mm4 = mm3 - mm4; mm3 = t;
  in[24] = narrow(mm3 >> kColShift);
  in[32] = narrow(mm4 >> kColShift);
  t = mm1 + mm2; mm2 = mm1 - mm2; mm1 = t;
  t = mm1 + mm6; mm6 = mm1 - mm6; mm1 = t;
  in[8] = narrow(mm1 >> kColShift);
  in[48] = narrow(mm6 >> kColShift);
  t = mm2 + mm5; mm5 = mm2 - mm5; mm2 = t;
  in[16] = narrow(mm2 >> kColShift);
  in[40] = narrow(mm5 >> kColShift);
}
}  // namespace xvid

void xvid_idct(ArrayView<const int16_t> input, int (&out)[64]) {
  int16_t in[64];
  for (int i = 0; i < 64; ++i) in[i] = static_cast<int16_t>(clamp_int(input[static_cast<uint64_t>(i)], -2048, 2047));
  int rows = 0x07;
  xvid::row(&in[0], xvid::kTab04, 65536);
  xvid::row(&in[8], xvid::kTab17, 3597);
  xvid::row(&in[16], xvid::kTab26, 2260);
  if (xvid::row(&in[24], xvid::kTab35, 1203)) rows |= 0x08;
  if (xvid::row(&in[32], xvid::kTab04, 0)) rows |= 0x10;
  if (xvid::row(&in[40], xvid::kTab35, 120)) rows |= 0x20;
  if (xvid::row(&in[48], xvid::kTab26, 512)) rows |= 0x40;
  if (xvid::row(&in[56], xvid::kTab17, 512)) rows |= 0x80;
  const int depth = (rows & 0xF0) ? 8 : (rows & 0x08) ? 4 : 3;
  for (int c = 0; c < 8; ++c) xvid::column(&in[c], depth);
  for (int i = 0; i < 64; ++i) out[i] = in[i];
}

// ---- motion compensation ----
// A plane of a reference picture: its samples and its edges (whole macroblocks).
struct PlaneRef {
  ConstByteView data;
  int stride = 0, width = 0, height = 0;
};
PlaneRef plane_of(const Picture& p, int c) {
  PlaneRef r;
  r.data = p.view(c);
  r.stride = p.stride[c];
  r.width = c == 0 ? p.width : p.width / 2;
  r.height = c == 0 ? p.height : p.height / 2;
  return r;
}

// Samples fetched from a reference: up to 17 columns and rows. Coordinates outside the plane take the nearest edge sample, which is what FFmpeg's edge emulation
// does (for field motion it emulates on frame lines: row k is frame line y + k * step, clamped).
struct Samples {
  uint8_t v[17][17];
};
void fetch(const PlaneRef& p, int x, int y, int step, int w, int h, Samples& out) {
  for (int r = 0; r < h; ++r) {
    const int yy = clamp_int(y + r * step, 0, p.height - 1);
    const ConstByteView row = p.data.subview(static_cast<uint64_t>(yy) * static_cast<uint64_t>(p.stride), static_cast<uint64_t>(p.width));
    for (int c = 0; c < w; ++c) out.v[r][c] = row[static_cast<uint64_t>(clamp_int(x + c, 0, p.width - 1))];
  }
}

// A prediction of up to 16 x 16 samples.
struct Prediction {
  uint8_t v[16][16];
};

int average2(int a, int b, bool rnd) { return (a + b + (rnd ? 1 : 0)) >> 1; }

// Half-sample interpolation (FFmpeg's put_pixels / put_no_rnd_pixels): `dxy` bit 0 horizontal, bit 1 vertical. The output goes to columns [ox, ox + w).
void half_sample(const Samples& s, int w, int h, int dxy, bool rnd, Prediction& out, int ox = 0) {
  for (int r = 0; r < h; ++r)
    for (int c = 0; c < w; ++c) {
      int v;
      switch (dxy) {
        case 0: v = s.v[r][c]; break;
        case 1: v = average2(s.v[r][c], s.v[r][c + 1], rnd); break;
        case 2: v = average2(s.v[r][c], s.v[r + 1][c], rnd); break;
        default: v = (s.v[r][c] + s.v[r][c + 1] + s.v[r + 1][c] + s.v[r + 1][c + 1] + (rnd ? 2 : 1)) >> 2; break;
      }
      out.v[r][ox + c] = static_cast<uint8_t>(v);
    }
}

// 7.6.2.2: the quarter-sample filter of MPEG-4 on n + 1 samples (n = 8 or 16), mirrored at the ends of the block, giving the n half-sample positions.
void lowpass(const int* s, int n, bool rnd, int* out) {
  auto at = [&](int j) { return s[j < 0 ? -1 - j : j > n ? 2 * n + 1 - j : j]; };
  for (int i = 0; i < n; ++i) {
    const int v = 20 * (at(i) + at(i + 1)) - 6 * (at(i - 1) + at(i + 2)) + 3 * (at(i - 2) + at(i + 3)) - (at(i - 3) + at(i + 4));
    out[i] = clamp_int((v + (rnd ? 16 : 15)) >> 5, 0, 255);
  }
}

// Quarter-sample interpolation of an n x n block from (n + 1) x (n + 1) samples starting at column `sx` of `s`, as FFmpeg's qpeldsp composes it: the horizontal
// half sample (averaged with the full sample to its left or right for a quarter position), then the vertical one on that (averaged likewise).
void quarter_sample(const Samples& s, int sx, int n, int dxy, bool rnd, Prediction& out, int ox) {
  const int fx = dxy & 3, fy = dxy >> 2;
  const int rows = fy ? n + 1 : n;
  int hs[17][16];
  for (int r = 0; r < rows; ++r) {
    int line[17], filtered[16];
    for (int c = 0; c <= n; ++c) line[c] = s.v[r][sx + c];
    if (fx) lowpass(line, n, rnd, filtered);
    for (int c = 0; c < n; ++c) {
      switch (fx) {
        case 0: hs[r][c] = line[c]; break;
        case 1: hs[r][c] = average2(filtered[c], line[c], rnd); break;
        case 2: hs[r][c] = filtered[c]; break;
        default: hs[r][c] = average2(filtered[c], line[c + 1], rnd); break;
      }
    }
  }
  for (int c = 0; c < n; ++c) {
    int column[17], filtered[16];
    if (fy) {
      for (int r = 0; r <= n; ++r) column[r] = hs[r][c];
      lowpass(column, n, rnd, filtered);
    }
    for (int r = 0; r < n; ++r) {
      int v;
      switch (fy) {
        case 0: v = hs[r][c]; break;
        case 1: v = average2(hs[r][c], filtered[r], rnd); break;
        case 2: v = filtered[r]; break;
        default: v = average2(hs[r + 1][c], filtered[r], rnd); break;
      }
      out.v[r][ox + c] = static_cast<uint8_t>(v);
    }
  }
}

// Writes a prediction into the picture being decoded at (x, y), row k on line y + k * step; `average`: the mean with what is there (the second direction of a B
// macroblock, rounded up).
// average: 0 replaces what is there, 1 takes the mean with it rounded up (MPEG-4 B planes), 2 the mean truncated (H.263 B pictures and B parts).
void store(const Picture& p, int c, int x, int y, int step, int w, int h, const Prediction& pred, int average) {
  const ByteView plane = p.view(c);
  const int stride = p.stride[c];
  for (int r = 0; r < h; ++r) {
    const ByteView row = plane.subview(static_cast<uint64_t>(y + r * step) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(x), static_cast<uint64_t>(w));
    for (int i = 0; i < w; ++i) {
      const int v = pred.v[r][i];
      row[static_cast<uint64_t>(i)] = static_cast<uint8_t>(average ? (row[static_cast<uint64_t>(i)] + v + (average == 1 ? 1 : 0)) >> 1 : v);
    }
  }
}

// FFmpeg's ff_h263_round_chroma: the chroma vector (in half samples) of a macroblock with four luma vectors, from their sum.
int round_chroma(int x) {
  static const uint8_t kRound[16] = {0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1};
  return kRound[x & 15] + (x >> 3);
}

}  // namespace

// Predicts the macroblock (mb_x_, mb_y_) from `ref` with the vectors of direction `dir` (0 forward, 1 backward).
void Core::motion(const Mb& mb, int dir, const Picture& ref, bool average_with_previous, bool truncate) {
  const int average = average_with_previous ? (truncate ? 2 : 1) : 0;
  const bool rnd = !pic_.no_rounding;
  const bool qpel = vol_.quarter_sample && !pic_.short_header;
  const PlaneRef luma = plane_of(ref, 0), cb = plane_of(ref, 1), cr = plane_of(ref, 2);
  const int x0 = mb_x_ * 16, y0 = mb_y_ * 16, cx0 = mb_x_ * 8, cy0 = mb_y_ * 8;
  Samples s;
  Prediction pred;
  auto chroma = [&](int ux, int uy, int ystep, int yrow_offset, int h, int uvdxy, int dest_y, int dest_step) {
    for (int c = 1; c <= 2; ++c) {
      fetch(c == 1 ? cb : cr, ux, uy * (ystep == 2 ? 2 : 1) + yrow_offset, ystep, 9, h + 1, s);
      half_sample(s, 8, h, uvdxy, rnd, pred);
      store(*cur_, c, cx0, dest_y, dest_step, 8, h, pred, average);
    }
  };
  if (mb.mv_type == kMv16x16) {
    const int mx = mb.mv[dir][0][0], my = mb.mv[dir][0][1];
    if (qpel) {
      fetch(luma, x0 + (mx >> 2), y0 + (my >> 2), 1, 17, 17, s);
      quarter_sample(s, 0, 16, ((my & 3) << 2) | (mx & 3), rnd, pred, 0);
      store(*cur_, 0, x0, y0, 1, 16, 16, pred, average);
      int cmx = mx / 2, cmy = my / 2;
      cmx = (cmx >> 1) | (cmx & 1);
      cmy = (cmy >> 1) | (cmy & 1);
      chroma(cx0 + (cmx >> 1), cy0 + (cmy >> 1), 1, 0, 8, (cmx & 1) | ((cmy & 1) << 1), cy0, 1);
    } else {
      const int dxy = ((my & 1) << 1) | (mx & 1);
      const int sx = x0 + (mx >> 1), sy = y0 + (my >> 1);
      fetch(luma, sx, sy, 1, 17, 17, s);
      half_sample(s, 16, 16, dxy, rnd, pred);
      store(*cur_, 0, x0, y0, 1, 16, 16, pred, average);
      chroma(sx >> 1, sy >> 1, 1, 0, 8, dxy | (my & 2) | ((mx & 2) >> 1), cy0, 1);
    }
    return;
  }
  if (mb.mv_type == kMv8x8) {
    int sum_x = 0, sum_y = 0;
    for (int i = 0; i < 4; ++i) {
      const int mx = mb.mv[dir][i][0], my = mb.mv[dir][i][1];
      const int bx = x0 + (i & 1) * 8, by = y0 + (i >> 1) * 8;
      // (The clipping to the coded size, and the half or quarter positions dropped there, are FFmpeg's.)
      if (qpel) {
        int dxy = ((my & 3) << 2) | (mx & 3);
        int sx = clamp_int(bx + (mx >> 2), -16, width_), sy = clamp_int(by + (my >> 2), -16, height_);
        if (sx == width_) dxy &= ~3;
        if (sy == height_) dxy &= ~12;
        fetch(luma, sx, sy, 1, 9, 9, s);
        quarter_sample(s, 0, 8, dxy, rnd, pred, 0);
        sum_x += mx / 2;
        sum_y += my / 2;
      } else {
        int dxy = 0;
        int sx = clamp_int(bx + (mx >> 1), -16, width_), sy = clamp_int(by + (my >> 1), -16, height_);
        if (sx != width_) dxy |= mx & 1;
        if (sy != height_) dxy |= (my & 1) << 1;
        fetch(luma, sx, sy, 1, 9, 9, s);
        half_sample(s, 8, 8, dxy, rnd, pred);
        sum_x += mx;
        sum_y += my;
      }
      store(*cur_, 0, bx, by, 1, 8, 8, pred, average);
    }
    const int cmx = round_chroma(sum_x), cmy = round_chroma(sum_y);
    int dxy = ((cmy & 1) << 1) | (cmx & 1);
    int ux = clamp_int(cx0 + (cmx >> 1), -8, width_ >> 1), uy = clamp_int(cy0 + (cmy >> 1), -8, height_ >> 1);
    if (ux == (width_ >> 1)) dxy &= ~1;
    if (uy == (height_ >> 1)) dxy &= ~2;
    chroma(ux, uy, 1, 0, 8, dxy, cy0, 1);
    return;
  }
  // Field motion: each field of the macroblock from the field `field_select` of the reference, 16 x 8 luma and 8 x 4 chroma samples.
  for (int i = 0; i < 2; ++i) {
    const int fs = mb.field_select[dir][i];
    const int mx = mb.mv[dir][i][0], my = mb.mv[dir][i][1];
    if (qpel) {
      const int dxy = ((my & 3) << 2) | (mx & 3);
      const int sx = x0 + (mx >> 2), sy = mb_y_ * 8 + (my >> 2);
      fetch(luma, sx, 2 * sy + fs, 2, 17, 9, s);
      quarter_sample(s, 0, 8, dxy, rnd, pred, 0);
      quarter_sample(s, 8, 8, dxy, rnd, pred, 8);
      store(*cur_, 0, x0, y0 + i, 2, 16, 8, pred, average);
      int cmx = mx / 2, cmy = my >> 1;
      cmx = (cmx >> 1) | (cmx & 1);
      cmy = (cmy >> 1) | (cmy & 1);
      chroma(cx0 + (cmx >> 1), mb_y_ * 4 + (cmy >> 1), 2, fs, 4, (cmx & 1) | ((cmy & 1) << 1), cy0 + i, 2);
    } else {
      const int dxy = ((my & 1) << 1) | (mx & 1);
      const int sx = x0 + (mx >> 1), sy = mb_y_ * 8 + (my >> 1);
      fetch(luma, sx, 2 * sy + fs, 2, 17, 9, s);
      half_sample(s, 16, 8, dxy, rnd, pred);
      store(*cur_, 0, x0, y0 + i, 2, 16, 8, pred, average);
      chroma(sx >> 1, sy >> 1, 2, fs, 4, dxy | (my & 2) | ((mx & 2) >> 1), cy0 + i, 2);
    }
  }
}

// 7.8.7: the prediction of a macroblock of an S plane with the global motion. A translation (one warping point) is bilinear in 1/16 samples on a 17 x 17 block
// (FFmpeg's gmc1, or its half-sample copy when the position is on a half sample); a warp is bilinear per sample at the warped position (FFmpeg's ff_gmc_c,
// with its edge handling).
void Core::global_motion(const Picture& ref) {
  const int x0 = mb_x_ * 16, y0 = mb_y_ * 16, cx0 = mb_x_ * 8, cy0 = mb_y_ * 8;
  const int acc = pic_.accuracy;
  const bool no_rounding = pic_.no_rounding;
  Samples s;
  Prediction pred;
  if (pic_.warping_points == 1) {
    for (int c = 0; c < 3; ++c) {
      const PlaneRef p = plane_of(ref, c);
      const int size = c == 0 ? 16 : 8;
      int mx = pic_.sprite_offset[c == 0 ? 0 : 1][0], my = pic_.sprite_offset[c == 0 ? 0 : 1][1];
      const int limit_x = c == 0 ? width_ : width_ >> 1, limit_y = c == 0 ? height_ : height_ >> 1;
      int sx = (c == 0 ? x0 : cx0) + (mx >> (acc + 1)), sy = (c == 0 ? y0 : cy0) + (my >> (acc + 1));
      mx *= 1 << (3 - acc);
      my *= 1 << (3 - acc);
      sx = clamp_int(sx, -size, limit_x);
      if (sx == limit_x) mx = 0;
      sy = clamp_int(sy, -size, limit_y);
      if (sy == limit_y) my = 0;
      fetch(p, sx, sy, 1, size + 1, size + 1, s);
      if (c == 0 && ((mx | my) & 7) == 0) {
        half_sample(s, 16, 16, ((mx >> 3) & 1) | ((my >> 2) & 2), !no_rounding, pred);
      } else {
        const int fx = mx & 15, fy = my & 15;
        const int ka = (16 - fx) * (16 - fy), kb = fx * (16 - fy), kc = (16 - fx) * fy, kd = fx * fy;
        const int rounder = 128 - (no_rounding ? 1 : 0);
        for (int r = 0; r < size; ++r)
          for (int i = 0; i < size; ++i)
            pred.v[r][i] = static_cast<uint8_t>((ka * s.v[r][i] + kb * s.v[r][i + 1] + kc * s.v[r + 1][i] + kd * s.v[r + 1][i + 1] + rounder) >> 8);
      }
      store(*cur_, c, c == 0 ? x0 : cx0, c == 0 ? y0 : cy0, 1, size, size, pred, false);
    }
    return;
  }
  const int shift = acc + 1, unit = 1 << shift;
  const int rounder = (1 << (2 * acc + 1)) - (no_rounding ? 1 : 0);
  const int dxx = pic_.sprite_delta[0][0], dxy = pic_.sprite_delta[0][1], dyx = pic_.sprite_delta[1][0], dyy = pic_.sprite_delta[1][1];
  for (int c = 0; c < 3; ++c) {
    const PlaneRef p = plane_of(ref, c);
    const int size = c == 0 ? 16 : 8, bx = c == 0 ? x0 : cx0, by = c == 0 ? y0 : cy0;
    const int k = c == 0 ? 0 : 1;
    const int width = (c == 0 ? p.width : (ref.width + 1) >> 1) - 1, height = (c == 0 ? p.height : (ref.height + 1) >> 1) - 1;
    auto at = [&](int x, int y) -> int { return p.data[static_cast<uint64_t>(y) * static_cast<uint64_t>(p.stride) + static_cast<uint64_t>(x)]; };
    // (32-bit wrapping arithmetic, as FFmpeg's; setup_sprite keeps it in range.)
    auto wrap = [](int64_t v) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v))); };
    const int64_t ox0 = static_cast<int64_t>(pic_.sprite_offset[k][0]) + static_cast<int64_t>(dxx) * bx + static_cast<int64_t>(dxy) * by;
    const int64_t oy0 = static_cast<int64_t>(pic_.sprite_offset[k][1]) + static_cast<int64_t>(dyx) * bx + static_cast<int64_t>(dyy) * by;
    for (int r = 0; r < size; ++r)
      for (int i = 0; i < size; ++i) {
        const int vx = wrap(ox0 + static_cast<int64_t>(dxx) * i + static_cast<int64_t>(dxy) * r);
        const int vy = wrap(oy0 + static_cast<int64_t>(dyx) * i + static_cast<int64_t>(dyy) * r);
        int src_x = vx >> 16, src_y = vy >> 16;
        const int frac_x = src_x & (unit - 1), frac_y = src_y & (unit - 1);
        src_x >>= shift;
        src_y >>= shift;
        int v;
        const bool in_x = static_cast<unsigned>(src_x) < static_cast<unsigned>(width), in_y = static_cast<unsigned>(src_y) < static_cast<unsigned>(height);
        if (in_x && in_y) {
          v = ((at(src_x, src_y) * (unit - frac_x) + at(src_x + 1, src_y) * frac_x) * (unit - frac_y) +
               (at(src_x, src_y + 1) * (unit - frac_x) + at(src_x + 1, src_y + 1) * frac_x) * frac_y + rounder) >>
              (shift * 2);
        } else if (in_x) {
          const int yy = clamp_int(src_y, 0, height);
          v = ((at(src_x, yy) * (unit - frac_x) + at(src_x + 1, yy) * frac_x) * unit + rounder) >> (shift * 2);
        } else if (in_y) {
          const int xx = clamp_int(src_x, 0, width);
          v = ((at(xx, src_y) * (unit - frac_y) + at(xx, src_y + 1) * frac_y) * unit + rounder) >> (shift * 2);
        } else {
          v = at(clamp_int(src_x, 0, width), clamp_int(src_y, 0, height));
        }
        pred.v[r][i] = static_cast<uint8_t>(v);
      }
    store(*cur_, c, bx, by, 1, size, size, pred, false);
  }
}

namespace {
// FFmpeg's hpel_motion: an 8 x 8 luma block at (x, y) moved by a half-sample vector, clipped to the coded size as FFmpeg clips it.
void block_half_sample(const PlaneRef& luma, int x, int y, int mx, int my, int width, int height, bool rnd, Prediction& out) {
  int dxy = 0;
  const int sx = clamp_int(x + (mx >> 1), -16, width), sy = clamp_int(y + (my >> 1), -16, height);
  if (sx != width) dxy |= mx & 1;
  if (sy != height) dxy |= (my & 1) << 1;
  Samples s;
  fetch(luma, sx, sy, 1, 9, 9, s);
  half_sample(s, 8, 8, dxy, rnd, out);
}

// The weights of Annex F.3 (FFmpeg's put_obmc): [row][column] = (top, left, middle, right, bottom), in eighths.
constexpr uint8_t kObmcWeights[8][8][5] = {
    {{2, 2, 4, 0, 0}, {2, 1, 5, 0, 0}, {2, 1, 5, 0, 0}, {2, 1, 5, 0, 0}, {2, 0, 5, 1, 0}, {2, 0, 5, 1, 0}, {2, 0, 5, 1, 0}, {2, 0, 4, 2, 0}},
    {{1, 2, 5, 0, 0}, {1, 2, 5, 0, 0}, {2, 1, 5, 0, 0}, {2, 1, 5, 0, 0}, {2, 0, 5, 1, 0}, {2, 0, 5, 1, 0}, {1, 0, 5, 2, 0}, {1, 0, 5, 2, 0}},
    {{1, 2, 5, 0, 0}, {1, 2, 5, 0, 0}, {1, 1, 6, 0, 0}, {1, 1, 6, 0, 0}, {1, 0, 6, 1, 0}, {1, 0, 6, 1, 0}, {1, 0, 5, 2, 0}, {1, 0, 5, 2, 0}},
    {{1, 2, 5, 0, 0}, {1, 2, 5, 0, 0}, {1, 1, 6, 0, 0}, {1, 1, 6, 0, 0}, {1, 0, 6, 1, 0}, {1, 0, 6, 1, 0}, {1, 0, 5, 2, 0}, {1, 0, 5, 2, 0}},
    {{0, 2, 5, 0, 1}, {0, 2, 5, 0, 1}, {0, 1, 6, 0, 1}, {0, 1, 6, 0, 1}, {0, 0, 6, 1, 1}, {0, 0, 6, 1, 1}, {0, 0, 5, 2, 1}, {0, 0, 5, 2, 1}},
    {{0, 2, 5, 0, 1}, {0, 2, 5, 0, 1}, {0, 1, 6, 0, 1}, {0, 1, 6, 0, 1}, {0, 0, 6, 1, 1}, {0, 0, 6, 1, 1}, {0, 0, 5, 2, 1}, {0, 0, 5, 2, 1}},
    {{0, 2, 5, 0, 1}, {0, 2, 5, 0, 1}, {0, 1, 5, 0, 2}, {0, 1, 5, 0, 2}, {0, 0, 5, 1, 2}, {0, 0, 5, 1, 2}, {0, 0, 5, 2, 1}, {0, 0, 5, 2, 1}},
    {{0, 2, 4, 0, 2}, {0, 1, 5, 0, 2}, {0, 1, 5, 0, 2}, {0, 1, 5, 0, 2}, {0, 0, 5, 1, 2}, {0, 0, 5, 1, 2}, {0, 0, 5, 1, 2}, {0, 0, 4, 2, 2}}};

// Annex J.3 (FFmpeg's h263_{h,v}_loop_filter): 8 samples across an edge; `across` steps over the edge, `along` along it.
void filter_edge(ByteView plane, uint64_t at, int64_t across, int64_t along, int q) {
  const int strength = kDeblockStrength[q & 31];
  for (int k = 0; k < 8; ++k) {
    const uint64_t o = static_cast<uint64_t>(static_cast<int64_t>(at) + along * k);
    const uint64_t i0 = static_cast<uint64_t>(static_cast<int64_t>(o) - 2 * across), i1 = static_cast<uint64_t>(static_cast<int64_t>(o) - across),
                   i3 = static_cast<uint64_t>(static_cast<int64_t>(o) + across);
    const int p0 = plane[i0], p3 = plane[i3];
    int p1 = plane[i1], p2 = plane[o];
    const int d = (p0 - p3 + 4 * (p2 - p1)) / 8;
    int d1;
    if (d < -2 * strength) d1 = 0;
    else if (d < -strength) d1 = -2 * strength - d;
    else if (d < strength) d1 = d;
    else if (d < 2 * strength) d1 = 2 * strength - d;
    else d1 = 0;
    p1 = clamp_int(p1 + d1, 0, 255);
    p2 = clamp_int(p2 - d1, 0, 255);
    plane[i1] = static_cast<uint8_t>(p1);
    plane[o] = static_cast<uint8_t>(p2);
    const int ad1 = (d1 < 0 ? -d1 : d1) >> 1;
    const int d2 = clamp_int((p0 - p3) / 4, -ad1, ad1);
    plane[i0] = static_cast<uint8_t>(p0 - d2);
    plane[i3] = static_cast<uint8_t>(p3 + d2);
  }
}
}  // namespace

// Annex F.3: overlapped block motion compensation of a P macroblock: each 8 x 8 luma block is the weighted mean of its prediction with its own vector and
// with the vectors of the blocks above, to the left and to the right (below: its own), a neighbour that is intra or outside the picture lending this
// block's vector (FFmpeg's apply_obmc, whose vectors of the macroblock to the right come from preview_next). Chroma as with four vectors.
void Core::overlapped_motion(const Picture& ref) {
  const bool rnd = !pic_.no_rounding;
  const PlaneRef luma = plane_of(ref, 0), cb = plane_of(ref, 1), cr = plane_of(ref, 2);
  const MbInfo& cur = cur_->mbs[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
  int cache[4][4][2];   // [row][column]: rows above, this macroblock (2), below (= its own); columns left, this (2), right
  for (int r = 0; r < 2; ++r)
    for (int c = 0; c < 2; ++c) {
      cache[1 + r][1 + c][0] = cur.mv[r * 2 + c][0];
      cache[1 + r][1 + c][1] = cur.mv[r * 2 + c][1];
    }
  // A neighbour's vectors, or false when it is outside the picture or intra.
  auto neighbour = [&](int dx, int dy, int16_t (&mv)[4][2]) {
    const int x = mb_x_ + dx, y = mb_y_ + dy;
    if (x < 0 || y < 0 || x >= mb_w_) return false;
    const MbInfo& info = cur_->mbs[static_cast<uint64_t>(y * mb_w_ + x)];
    if (info.flags & kMbIntra) return false;
    for (int b = 0; b < 4; ++b) {
      mv[b][0] = info.mv[b][0];
      mv[b][1] = info.mv[b][1];
    }
    return true;
  };
  int16_t above[4][2], left[4][2], right[4][2];
  const bool has_above = neighbour(0, -1, above), has_left = neighbour(-1, 0, left), has_right = neighbour(1, 0, right);
  for (int c = 0; c < 2; ++c)
    for (int k = 0; k < 2; ++k) {
      cache[3][1 + c][k] = cache[2][1 + c][k];
      cache[0][1 + c][k] = has_above ? above[2 + c][k] : cache[1][1 + c][k];
    }
  for (int r = 0; r < 2; ++r)
    for (int k = 0; k < 2; ++k) {
      cache[1 + r][0][k] = has_left ? left[r * 2 + 1][k] : cache[1 + r][1][k];
      cache[1 + r][3][k] = has_right ? right[r * 2][k] : cache[1 + r][2][k];
    }
  int sum_x = 0, sum_y = 0;
  for (int i = 0; i < 4; ++i) {
    const int x = (i & 1) + 1, y = (i >> 1) + 1;
    // middle, top, left, right, bottom
    const int mv[5][2] = {{cache[y][x][0], cache[y][x][1]}, {cache[y - 1][x][0], cache[y - 1][x][1]}, {cache[y][x - 1][0], cache[y][x - 1][1]},
                          {cache[y][x + 1][0], cache[y][x + 1][1]}, {cache[y + 1][x][0], cache[y + 1][x][1]}};
    const int bx = mb_x_ * 16 + (i & 1) * 8, by = mb_y_ * 16 + (i >> 1) * 8;
    Prediction pred[5];
    for (int j = 0; j < 5; ++j) {
      if (j && mv[j][0] == mv[0][0] && mv[j][1] == mv[0][1]) pred[j] = pred[0];
      else block_half_sample(luma, bx, by, mv[j][0], mv[j][1], width_, height_, rnd, pred[j]);
    }
    Prediction out;
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) {
        const uint8_t* w = kObmcWeights[r][c];
        out.v[r][c] = static_cast<uint8_t>((w[0] * pred[1].v[r][c] + w[1] * pred[2].v[r][c] + w[2] * pred[0].v[r][c] + w[3] * pred[3].v[r][c] +
                                            w[4] * pred[4].v[r][c] + 4) >> 3);
      }
    store(*cur_, 0, bx, by, 1, 8, 8, out, false);
    sum_x += mv[0][0];
    sum_y += mv[0][1];
  }
  const int cmx = round_chroma(sum_x), cmy = round_chroma(sum_y);
  int dxy = ((cmy & 1) << 1) | (cmx & 1);
  const int ux = clamp_int(mb_x_ * 8 + (cmx >> 1), -8, width_ >> 1), uy = clamp_int(mb_y_ * 8 + (cmy >> 1), -8, height_ >> 1);
  if (ux == (width_ >> 1)) dxy &= ~1;
  if (uy == (height_ >> 1)) dxy &= ~2;
  Samples s;
  Prediction pred;
  for (int c = 1; c <= 2; ++c) {
    fetch(c == 1 ? cb : cr, ux, uy, 1, 9, 9, s);
    half_sample(s, 8, 8, dxy, rnd, pred);
    store(*cur_, c, mb_x_ * 8, mb_y_ * 8, 1, 8, 8, pred, false);
  }
}

// Annex J: the deblocking filter on the edges of this macroblock that are decoded, as FFmpeg's ff_h263_loop_filter runs it after each macroblock (the
// quantisers of not coded macroblocks count as zero: their edges are left alone unless the other side is coded).
void Core::deblock_macroblock(const Mb& mb) {
  const ByteView y_plane = cur_->view(0), u_plane = cur_->view(1), v_plane = cur_->view(2);
  const int64_t ls = cur_->stride[0], uvs = cur_->stride[1];
  const uint64_t xy = static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_);
  const uint64_t dy = static_cast<uint64_t>(mb_y_ * 16) * static_cast<uint64_t>(ls) + static_cast<uint64_t>(mb_x_ * 16);
  const uint64_t dc = static_cast<uint64_t>(mb_y_ * 8) * static_cast<uint64_t>(uvs) + static_cast<uint64_t>(mb_x_ * 8);
  auto skipped = [&](uint64_t index) { return (cur_->mbs[index].flags & kMbSkipped) != 0 && !(cur_->mbs[index].flags & kMbIntra); };
  auto v_filter = [&](ByteView plane, uint64_t at, int64_t stride, int q) { filter_edge(plane, at, stride, 1, q); };   // a horizontal edge
  auto h_filter = [&](ByteView plane, uint64_t at, int64_t stride, int q) { filter_edge(plane, at, 1, stride, q); };   // a vertical edge
  const bool is_skipped = mb.skipped && !mb.intra;
  int qp_c = 0;
  if (!is_skipped) {
    qp_c = qscale_;
    v_filter(y_plane, dy + 8 * ls, ls, qp_c);
    v_filter(y_plane, dy + 8 * ls + 8, ls, qp_c);
  }
  if (mb_y_) {
    const uint64_t top = xy - static_cast<uint64_t>(mb_w_);
    const int qp_tt = skipped(top) ? 0 : mb_qscale_[top];
    const int qp_tc = qp_c ? qp_c : qp_tt;
    if (qp_tc) {
      const int cq = chroma_quant(qp_tc);
      v_filter(y_plane, dy, ls, qp_tc);
      v_filter(y_plane, dy + 8, ls, qp_tc);
      v_filter(u_plane, dc, uvs, cq);
      v_filter(v_plane, dc, uvs, cq);
    }
    if (qp_tt) h_filter(y_plane, dy - 8 * ls + 8, ls, qp_tt);
    if (mb_x_) {
      const int qp_dt = (qp_tt || skipped(top - 1)) ? qp_tt : mb_qscale_[top - 1];
      if (qp_dt) {
        const int cq = chroma_quant(qp_dt);
        h_filter(y_plane, dy - 8 * ls, ls, qp_dt);
        h_filter(u_plane, dc - 8 * uvs, uvs, cq);
        h_filter(v_plane, dc - 8 * uvs, uvs, cq);
      }
    }
  }
  if (qp_c) {
    h_filter(y_plane, dy + 8, ls, qp_c);
    if (mb_y_ + 1 == mb_h_) h_filter(y_plane, dy + 8 * ls + 8, ls, qp_c);
  }
  if (mb_x_) {
    const int qp_lc = (qp_c || skipped(xy - 1)) ? qp_c : mb_qscale_[xy - 1];
    if (qp_lc) {
      h_filter(y_plane, dy, ls, qp_lc);
      if (mb_y_ + 1 == mb_h_) {
        const int cq = chroma_quant(qp_lc);
        h_filter(y_plane, dy + 8 * ls, ls, qp_lc);
        h_filter(u_plane, dc, uvs, cq);
        h_filter(v_plane, dc, uvs, cq);
      }
    }
  }
}

// 7.4.4: inverse quantisation of block `n` (intra blocks: always; inter blocks: under the MPEG quantiser - those under the H.263 one were dequantised when read),
// saturation to [-2048, 2047] and, for the MPEG quantiser, mismatch control (the sum of the coefficients made odd through the last one).
void Core::dequantize(Mb& mb, int n) {
  const ArrayView<int16_t> block(mb.block[n]);
  const int q = n < 4 ? qscale_ : chroma_quant(qscale_);
  const bool mpeg = !pic_.short_header && vol_.quant_type_mpeg;
  int sum = 0;
  auto saturate = [](int v) { return v > 2047 ? 2047 : v < -2048 ? -2048 : v; };
  if (mb.intra && pic_.short_header && pic_.aic) {
    // Annex I: the DC is reconstructed already; the AC levels are 2 * Q * level (no rounding term).
    for (uint64_t i = 1; i < 64; ++i)
      if (block[i]) block[i] = static_cast<int16_t>(saturate(block[i] * 2 * q));
    return;
  }
  if (mb.intra) {
    const int dc_scale = pic_.short_header ? 8 : n < 4 ? (q < 5 ? 8 : q < 9 ? 2 * q : q < 25 ? q + 8 : 2 * q - 16) : (q < 5 ? 8 : q < 25 ? (q + 13) / 2 : q - 6);
    block[0] = static_cast<int16_t>(saturate(block[0] * dc_scale));
    sum = block[0];
    for (uint64_t i = 1; i < 64; ++i) {
      int level = block[i];
      if (!level) continue;
      if (mpeg) {
        const int magnitude = ((level < 0 ? -level : level) * 2 * q * intra_matrix_[i]) >> 4;
        level = level < 0 ? -magnitude : magnitude;
      } else {
        level = level < 0 ? level * 2 * q - ((q - 1) | 1) : level * 2 * q + ((q - 1) | 1);
      }
      block[i] = static_cast<int16_t>(saturate(level));
      sum += block[i];
    }
  } else {
    for (uint64_t i = 0; i < 64; ++i) {
      int level = block[i];
      if (!level) continue;
      const int magnitude = (((level < 0 ? -level : level) * 2 + 1) * 2 * q * inter_matrix_[i]) >> 5;
      block[i] = static_cast<int16_t>(saturate(level < 0 ? -magnitude : magnitude));
      sum += block[i];
    }
  }
  if (mpeg && (sum & 1) == 0) block[63] = static_cast<int16_t>(block[63] ^ 1);
}

void Core::reconstruct(Mb& mb) {
  const Picture& cur = *cur_;
  const int x0 = mb_x_ * 16, y0 = mb_y_ * 16;
  if (!mb.intra) {
    if (pic_.type == kPictureB) {
      if (mb.dir & kForward) motion(mb, 0, *anchor_[0], false);
      if (mb.dir & kBackward) motion(mb, 1, *anchor_[1], (mb.dir & kForward) != 0, pic_.short_header);
    } else if (mb.mcsel) {
      global_motion(*anchor_[1]);
    } else if (pic_.short_header && pic_.obmc) {
      overlapped_motion(*anchor_[1]);
    } else {
      motion(mb, 0, *anchor_[1], false);
    }
  }
  for (int n = 0; n < 6; ++n) {
    if (!mb.intra && mb.last[n] < 0) continue;
    if (mb.intra || (!pic_.short_header && vol_.quant_type_mpeg)) dequantize(mb, n);
    int residual[64];
    if (xvid_idct_) xvid_idct(ArrayView<const int16_t>(mb.block[n]), residual);
    else idct(ArrayView<const int16_t>(mb.block[n]), residual);
    int c = 0, x, y, step = 1;
    if (n < 4) {
      x = x0 + (n & 1) * 8;
      if (mb.field_dct) {
        y = y0 + (n >> 1);
        step = 2;
      } else {
        y = y0 + (n >> 1) * 8;
      }
    } else {
      c = n - 3;
      x = mb_x_ * 8;
      y = mb_y_ * 8;
    }
    const ByteView plane = cur.view(c);
    for (int r = 0; r < 8; ++r) {
      const ByteView row = plane.subview(static_cast<uint64_t>(y + r * step) * static_cast<uint64_t>(cur.stride[c]) + static_cast<uint64_t>(x), 8);
      for (uint64_t i = 0; i < 8; ++i) {
        const int v = residual[r * 8 + static_cast<int>(i)];
        row[i] = clip_pixel(mb.intra ? v : row[i] + v);
      }
    }
  }
  // A PB-frame's B part is predicted from this macroblock as reconstructed and clipped, before the deblocking filter (Annex J.3).
  if (cur_b_ && pic_.short_header) reconstruct_b_part(mb);
  if (pic_.short_header && pic_.deblocking) deblock_macroblock(mb);
}

namespace {
// A sample at half-sample position (x2, y2) of a plane (6.1.2 with RCONTROL 0), coordinates clamped to the plane.
int half_sample_at(const PlaneRef& p, int x2, int y2) {
  const int x = x2 >> 1, y = y2 >> 1, fx = x2 & 1, fy = y2 & 1;
  auto at = [&](int xx, int yy) -> int {
    xx = clamp_int(xx, 0, p.width - 1);
    yy = clamp_int(yy, 0, p.height - 1);
    return p.data[static_cast<uint64_t>(yy) * static_cast<uint64_t>(p.stride) + static_cast<uint64_t>(xx)];
  };
  const int a = at(x, y);
  if (!fx && !fy) return a;
  if (fx && !fy) return (a + at(x + 1, y) + 1) >> 1;
  if (!fx) return (a + at(x, y + 1) + 1) >> 1;
  return (a + at(x + 1, y) + at(x, y + 1) + at(x + 1, y + 1) + 2) >> 2;
}
}  // namespace

// G.5 and M.2: the B part of a PB-frame macroblock. Bidirectional: forward with MVF from the previous reference picture, averaged (truncated) with backward
// with MVB from this frame's P macroblock (PREC) where MVB stays inside it, forward only elsewhere; Annex M adds forward prediction with its own vector and
// backward prediction (PREC itself). Then the B blocks' residual.
void Core::reconstruct_b_part(Mb& mb) {
  const Picture& out = *cur_b_;
  const PlaneRef prev[3] = {plane_of(*anchor_[1], 0), plane_of(*anchor_[1], 1), plane_of(*anchor_[1], 2)};
  const PlaneRef prec[3] = {plane_of(*cur_, 0), plane_of(*cur_, 1), plane_of(*cur_, 2)};
  const int x0 = mb_x_ * 16, y0 = mb_y_ * 16, cx0 = mb_x_ * 8, cy0 = mb_y_ * 8;
  const MbInfo& info = cur_->mbs[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
  Prediction luma, chroma[2];
  Samples s;
  if (mb.b_mode == kBBackward) {
    for (int r = 0; r < 16; ++r)
      for (int i = 0; i < 16; ++i) luma.v[r][i] = static_cast<uint8_t>(half_sample_at(prec[0], (x0 + i) * 2, (y0 + r) * 2));
    for (int c = 0; c < 2; ++c)
      for (int r = 0; r < 8; ++r)
        for (int i = 0; i < 8; ++i) chroma[c].v[r][i] = static_cast<uint8_t>(half_sample_at(prec[1 + c], (cx0 + i) * 2, (cy0 + r) * 2));
  } else if (mb.b_mode == kBForward) {
    const int mx = mb.mvd[0], my = mb.mvd[1];
    const int sx = x0 + (mx >> 1), sy = y0 + (my >> 1);
    fetch(prev[0], sx, sy, 1, 17, 17, s);
    half_sample(s, 16, 16, ((my & 1) << 1) | (mx & 1), true, luma);
    const int uvdxy = (((my & 1) << 1) | (mx & 1)) | (my & 2) | ((mx & 2) >> 1);
    for (int c = 0; c < 2; ++c) {
      fetch(prev[1 + c], sx >> 1, sy >> 1, 1, 9, 9, s);
      half_sample(s, 8, 8, uvdxy, true, chroma[c]);
    }
  } else {
    int mvf[4][2], mvb[4][2], sum_f[2] = {0, 0}, sum_b[2] = {0, 0};
    for (int b = 0; b < 4; ++b)
      for (int c = 0; c < 2; ++c) {
        const int mv = info.mv[b][c];
        const int mvd = pb_mode_ == 1 ? mb.mvd[c] : 0;
        mvf[b][c] = trb_ * mv / trd_ + mvd;
        mvb[b][c] = mvd == 0 ? (trb_ - trd_) * mv / trd_ : mvf[b][c] - mv;
        sum_f[c] += mvf[b][c];
        sum_b[c] += mvb[b][c];
      }
    for (int b = 0; b < 4; ++b) {
      const int nh = b & 1, nv = b >> 1;
      const int bx = x0 + nh * 8, by = y0 + nv * 8;
      Prediction f;
      fetch(prev[0], bx + (mvf[b][0] >> 1), by + (mvf[b][1] >> 1), 1, 9, 9, s);
      half_sample(s, 8, 8, ((mvf[b][1] & 1) << 1) | (mvf[b][0] & 1), true, f);
      for (int r = 0; r < 8; ++r)
        for (int i = 0; i < 8; ++i) luma.v[nv * 8 + r][nh * 8 + i] = f.v[r][i];
      // G.5's procedure for luminance: the pixels whose backward sample lies inside PREC (C division, truncating).
      const int mh = mvb[b][0], mv = mvb[b][1];
      const int i_lo = nh * 8 + (0 > (-mh + 1) / 2 - nh * 8 ? 0 : (-mh + 1) / 2 - nh * 8);
      const int i_hi = nh * 8 + (7 < 15 - (mh + 1) / 2 - nh * 8 ? 7 : 15 - (mh + 1) / 2 - nh * 8);
      const int j_lo = nv * 8 + (0 > (-mv + 1) / 2 - nv * 8 ? 0 : (-mv + 1) / 2 - nv * 8);
      const int j_hi = nv * 8 + (7 < 15 - (mv + 1) / 2 - nv * 8 ? 7 : 15 - (mv + 1) / 2 - nv * 8);
      for (int i = i_lo; i <= i_hi; ++i)
        for (int j = j_lo; j <= j_hi; ++j) {
          const int back = half_sample_at(prec[0], (x0 + i) * 2 + mh, (y0 + j) * 2 + mv);
          luma.v[j][i] = static_cast<uint8_t>((luma.v[j][i] + back) / 2);
        }
    }
    const int mfx = round_chroma(sum_f[0]), mfy = round_chroma(sum_f[1]), mbx = round_chroma(sum_b[0]), mby = round_chroma(sum_b[1]);
    for (int c = 0; c < 2; ++c) {
      fetch(prev[1 + c], cx0 + (mfx >> 1), cy0 + (mfy >> 1), 1, 9, 9, s);
      half_sample(s, 8, 8, ((mfy & 1) << 1) | (mfx & 1), true, chroma[c]);
      const int i_lo = 0 > (-mbx + 1) / 2 ? 0 : (-mbx + 1) / 2, i_hi = 7 < 7 - (mbx + 1) / 2 ? 7 : 7 - (mbx + 1) / 2;
      const int j_lo = 0 > (-mby + 1) / 2 ? 0 : (-mby + 1) / 2, j_hi = 7 < 7 - (mby + 1) / 2 ? 7 : 7 - (mby + 1) / 2;
      for (int i = i_lo; i <= i_hi; ++i)
        for (int j = j_lo; j <= j_hi; ++j) {
          const int back = half_sample_at(prec[1 + c], (cx0 + i) * 2 + mbx, (cy0 + j) * 2 + mby);
          chroma[c].v[j][i] = static_cast<uint8_t>((chroma[c].v[j][i] + back) / 2);
        }
    }
  }
  store(out, 0, x0, y0, 1, 16, 16, luma, 0);
  store(out, 1, cx0, cy0, 1, 8, 8, chroma[0], 0);
  store(out, 2, cx0, cy0, 1, 8, 8, chroma[1], 0);
  for (int n = 0; n < 6; ++n) {
    if (mb.b_last[n] < 0) continue;
    int residual[64];
    idct(ArrayView<const int16_t>(mb.b_block[n]), residual);
    const int c = n < 4 ? 0 : n - 3;
    const int x = n < 4 ? x0 + (n & 1) * 8 : cx0, y = n < 4 ? y0 + (n >> 1) * 8 : cy0;
    const ByteView plane = out.view(c);
    for (int r = 0; r < 8; ++r) {
      const ByteView row = plane.subview(static_cast<uint64_t>(y + r) * static_cast<uint64_t>(out.stride[c]) + static_cast<uint64_t>(x), 8);
      for (uint64_t i = 0; i < 8; ++i) row[i] = clip_pixel(row[i] + residual[r * 8 + static_cast<int>(i)]);
    }
  }
}

// Macroblocks [from, to) could not be decoded: they are copied from the reference (an intra picture without one is left grey), and count as not coded for the
// planes that refer to this one.
void Core::conceal(int from, int to) {
  const Picture* ref = pic_.type == kPictureB ? anchor_[0] : anchor_[1];
  for (int index = from; index < to && index < mb_count_; ++index) {
    mb_x_ = index % mb_w_;
    mb_y_ = index / mb_w_;
    mb_packet_[static_cast<uint64_t>(index)] = -1;
    if (pic_.type != kPictureB) {
      MbInfo& info = cur_->mbs[static_cast<uint64_t>(index)];
      info.flags = kMbSkipped;
      for (auto& v : info.mv) v[0] = v[1] = 0;
    }
    if (ref) {
      Mb copy;
      copy.mv_type = kMv16x16;
      motion(copy, 0, *ref, false);
      if (cur_b_) {   // (a PB-frame's B part: the same)
        for (int c = 0; c < 3; ++c) {
          const int size = c == 0 ? 16 : 8;
          for (int r = 0; r < size; ++r) {
            const uint64_t at = static_cast<uint64_t>(mb_y_ * size + r) * static_cast<uint64_t>(cur_->stride[c]) + static_cast<uint64_t>(mb_x_ * size);
            cur_b_->view(c).subview(at, static_cast<uint64_t>(size)).copy_from(ConstByteView(cur_->view(c).subview(at, static_cast<uint64_t>(size))));
          }
        }
      }
      continue;
    }
    for (int c = 0; c < 3; ++c) {
      const int size = c == 0 ? 16 : 8;
      const ByteView plane = cur_->view(c);
      for (int r = 0; r < size; ++r)
        plane.subview(static_cast<uint64_t>(mb_y_ * size + r) * static_cast<uint64_t>(cur_->stride[c]) + static_cast<uint64_t>(mb_x_ * size), static_cast<uint64_t>(size))
            .fill(128);
    }
  }
}

}  // namespace mpeg4
}  // namespace hardware
