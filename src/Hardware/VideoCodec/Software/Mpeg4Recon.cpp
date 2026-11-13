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
void store(const Picture& p, int c, int x, int y, int step, int w, int h, const Prediction& pred, bool average) {
  const ByteView plane = p.view(c);
  const int stride = p.stride[c];
  for (int r = 0; r < h; ++r) {
    const ByteView row = plane.subview(static_cast<uint64_t>(y + r * step) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(x), static_cast<uint64_t>(w));
    for (int i = 0; i < w; ++i) {
      const int v = pred.v[r][i];
      row[static_cast<uint64_t>(i)] = static_cast<uint8_t>(average ? (row[static_cast<uint64_t>(i)] + v + 1) >> 1 : v);
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
void Core::motion(const Mb& mb, int dir, const Picture& ref, bool average) {
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

// 7.4.4: inverse quantisation of block `n` (intra blocks: always; inter blocks: under the MPEG quantiser - those under the H.263 one were dequantised when read),
// saturation to [-2048, 2047] and, for the MPEG quantiser, mismatch control (the sum of the coefficients made odd through the last one).
void Core::dequantize(Mb& mb, int n) {
  const ArrayView<int16_t> block(mb.block[n]);
  const int q = qscale_;
  const bool mpeg = !pic_.short_header && vol_.quant_type_mpeg;
  int sum = 0;
  auto saturate = [](int v) { return v > 2047 ? 2047 : v < -2048 ? -2048 : v; };
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
      if (mb.dir & kBackward) motion(mb, 1, *anchor_[1], (mb.dir & kForward) != 0);
    } else {
      motion(mb, 0, *anchor_[1], false);
    }
  }
  for (int n = 0; n < 6; ++n) {
    if (!mb.intra && mb.last[n] < 0) continue;
    if (mb.intra || (!pic_.short_header && vol_.quant_type_mpeg)) dequantize(mb, n);
    int residual[64];
    idct(ArrayView<const int16_t>(mb.block[n]), residual);
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
