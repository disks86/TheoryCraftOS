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

#include "Hardware/VideoCodec/Software/Av1Mc.hpp"
#include "Hardware/VideoCodec/Software/Av1Tables.inc"

// Port of dav1d src/mc_tmpl.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors), see Av1Mc.hpp.

namespace hardware {
namespace av1 {

namespace {

constexpr int kMidStride = 128;

int iclip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int imin(int a, int b) { return a < b ? a : b; }
int iabs(int v) { return v < 0 ? -v : v; }

// dav1d's bitdepth_from_max(): the 8bpc build has 8 bits; the 16bpc build derives the depth.
int bitdepth_from_max(int bitdepth_max) { return 32 - __builtin_clz(static_cast<unsigned>(bitdepth_max)); }

// 4 for 8 bits (8bpc build), 4 for 10 bits and 2 for 12 bits (16bpc build).
int intermediate_bits_of(int bitdepth_max) {
  const int bitdepth = bitdepth_from_max(bitdepth_max);
  return bitdepth == 8 ? 4 : 14 - bitdepth;
}
// The 16bpc build subtracts a bias so that the intermediates fit in int16_t.
int prep_bias_of(int bitdepth_max) { return bitdepth_from_max(bitdepth_max) == 8 ? 0 : 8192; }

// The first and the last index of the 8-tap filter type encoded as dav1d's `filter_type`: horizontal | (vertical << 2).
int filter_type_of(Filter2d f) {
  switch (f) {
    case FILTER_2D_8TAP_REGULAR: return FILTER_8TAP_REGULAR | (FILTER_8TAP_REGULAR << 2);
    case FILTER_2D_8TAP_REGULAR_SMOOTH: return FILTER_8TAP_REGULAR | (FILTER_8TAP_SMOOTH << 2);
    case FILTER_2D_8TAP_REGULAR_SHARP: return FILTER_8TAP_REGULAR | (FILTER_8TAP_SHARP << 2);
    case FILTER_2D_8TAP_SHARP_REGULAR: return FILTER_8TAP_SHARP | (FILTER_8TAP_REGULAR << 2);
    case FILTER_2D_8TAP_SHARP_SMOOTH: return FILTER_8TAP_SHARP | (FILTER_8TAP_SMOOTH << 2);
    case FILTER_2D_8TAP_SHARP: return FILTER_8TAP_SHARP | (FILTER_8TAP_SHARP << 2);
    case FILTER_2D_8TAP_SMOOTH_REGULAR: return FILTER_8TAP_SMOOTH | (FILTER_8TAP_REGULAR << 2);
    case FILTER_2D_8TAP_SMOOTH: return FILTER_8TAP_SMOOTH | (FILTER_8TAP_SMOOTH << 2);
    case FILTER_2D_8TAP_SMOOTH_SHARP: return FILTER_8TAP_SMOOTH | (FILTER_8TAP_SHARP << 2);
    default: return 0;
  }
}

// The 8 taps of a filter, or an empty view for "no filtering" (position 0).
using Taps = ArrayView<const int8_t>;

Taps filter_taps(int set, int position_minus_one) { return Taps(k_mc_subpel_filters[set][position_minus_one], 8); }

// dav1d GET_H_FILTER / GET_V_FILTER. `size` is the block width (horizontal) or height (vertical): blocks of 4 or less use the 4-tap sets.
Taps h_filter(int filter_type, int size, int mx) {
  if (!mx) return Taps();
  return size > 4 ? filter_taps(filter_type & 3, mx - 1) : filter_taps(3 + (filter_type & 1), mx - 1);
}
Taps v_filter(int filter_type, int size, int my) {
  if (!my) return Taps();
  return size > 4 ? filter_taps(filter_type >> 2, my - 1) : filter_taps(3 + ((filter_type >> 2) & 1), my - 1);
}

bool block_size_ok(int w, int h) { return w >= 1 && w <= 128 && h >= 1 && h <= 128; }

// n samples of row y of the view starting at column x (checked once for the whole run).
ArrayView<const Pixel> row_of(const PixelView& v, int x, int y, int n) {
  const int64_t index = v.origin + static_cast<int64_t>(y) * v.stride + x;
  if (index < 0) bounds_violation("Av1Mc row", 0, v.plane.size());
  return ArrayView<const Pixel>(v.plane).subview(static_cast<uint64_t>(index), static_cast<uint64_t>(n));
}
ArrayView<Pixel> row_of_mut(const PixelView& v, int x, int y, int n) {
  const int64_t index = v.origin + static_cast<int64_t>(y) * v.stride + x;
  if (index < 0) bounds_violation("Av1Mc row", 0, v.plane.size());
  return v.plane.subview(static_cast<uint64_t>(index), static_cast<uint64_t>(n));
}

int tap8(Taps f, const ArrayView<const Pixel>& r, int x) {
  return f[0] * r[x] + f[1] * r[x + 1] + f[2] * r[x + 2] + f[3] * r[x + 3] + f[4] * r[x + 4] + f[5] * r[x + 5] + f[6] * r[x + 6] +
         f[7] * r[x + 7];
}

// Vertical 8 taps over int16 rows of the intermediate buffer (row stride 128), rows y .. y + 7.
int tap8_mid(Taps f, ArrayView<const int16_t> mid, int base, int x) {
  return f[0] * mid[base + x] + f[1] * mid[base + kMidStride + x] + f[2] * mid[base + 2 * kMidStride + x] +
         f[3] * mid[base + 3 * kMidStride + x] + f[4] * mid[base + 4 * kMidStride + x] + f[5] * mid[base + 5 * kMidStride + x] +
         f[6] * mid[base + 6 * kMidStride + x] + f[7] * mid[base + 7 * kMidStride + x];
}

int rnd_shift(int v, int sh) { return (v + ((1 << sh) >> 1)) >> sh; }

void put_copy(PixelView dst, PixelView src, int w, int h) {
  for (int y = 0; y < h; y++) row_of_mut(dst, 0, y, w).copy_from(row_of(src, 0, y, w));
}

void prep_copy(ArrayView<int16_t> tmp, PixelView src, int w, int h, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int bias = prep_bias_of(bitdepth_max);
  for (int y = 0; y < h; y++) {
    const ArrayView<const Pixel> r = row_of(src, 0, y, w);
    for (int x = 0; x < w; x++) tmp[y * w + x] = static_cast<int16_t>((r[x] << intermediate_bits) - bias);
  }
}

void put_8tap(int filter_type, PixelView dst, PixelView src, int w, int h, int mx, int my, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int intermediate_rnd = 32 + ((1 << (6 - intermediate_bits)) >> 1);
  const Taps fh = h_filter(filter_type, w, mx);
  const Taps fv = v_filter(filter_type, h, my);

  if (!fh.empty()) {
    if (!fv.empty()) {
      int16_t mid_storage[kMidStride * 135];
      const ArrayView<int16_t> mid(mid_storage);
      for (int y = 0; y < h + 7; y++) {
        const ArrayView<const Pixel> r = row_of(src, -3, y - 3, w + 7);
        for (int x = 0; x < w; x++) mid[y * kMidStride + x] = static_cast<int16_t>(rnd_shift(tap8(fh, r, x), 6 - intermediate_bits));
      }
      for (int y = 0; y < h; y++) {
        const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
        for (int x = 0; x < w; x++)
          d[x] = static_cast<Pixel>(iclip(rnd_shift(tap8_mid(fv, mid, y * kMidStride, x), 6 + intermediate_bits), 0, bitdepth_max));
      }
    } else {
      for (int y = 0; y < h; y++) {
        const ArrayView<const Pixel> r = row_of(src, -3, y, w + 7);
        const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
        for (int x = 0; x < w; x++) d[x] = static_cast<Pixel>(iclip((tap8(fh, r, x) + intermediate_rnd) >> 6, 0, bitdepth_max));
      }
    }
  } else if (!fv.empty()) {
    for (int y = 0; y < h; y++) {
      ArrayView<const Pixel> rows[8];
      for (int k = 0; k < 8; k++) rows[k] = row_of(src, 0, y + k - 3, w);
      const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
      for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int k = 0; k < 8; k++) sum += fv[k] * rows[k][x];
        d[x] = static_cast<Pixel>(iclip(rnd_shift(sum, 6), 0, bitdepth_max));
      }
    }
  } else {
    put_copy(dst, src, w, h);
  }
}

void prep_8tap(int filter_type, ArrayView<int16_t> tmp, PixelView src, int w, int h, int mx, int my, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int bias = prep_bias_of(bitdepth_max);
  const Taps fh = h_filter(filter_type, w, mx);
  const Taps fv = v_filter(filter_type, h, my);

  if (!fh.empty()) {
    if (!fv.empty()) {
      int16_t mid_storage[kMidStride * 135];
      const ArrayView<int16_t> mid(mid_storage);
      for (int y = 0; y < h + 7; y++) {
        const ArrayView<const Pixel> r = row_of(src, -3, y - 3, w + 7);
        for (int x = 0; x < w; x++) mid[y * kMidStride + x] = static_cast<int16_t>(rnd_shift(tap8(fh, r, x), 6 - intermediate_bits));
      }
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          tmp[y * w + x] = static_cast<int16_t>(rnd_shift(tap8_mid(fv, mid, y * kMidStride, x), 6) - bias);
    } else {
      for (int y = 0; y < h; y++) {
        const ArrayView<const Pixel> r = row_of(src, -3, y, w + 7);
        for (int x = 0; x < w; x++) tmp[y * w + x] = static_cast<int16_t>(rnd_shift(tap8(fh, r, x), 6 - intermediate_bits) - bias);
      }
    }
  } else if (!fv.empty()) {
    for (int y = 0; y < h; y++) {
      ArrayView<const Pixel> rows[8];
      for (int k = 0; k < 8; k++) rows[k] = row_of(src, 0, y + k - 3, w);
      for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int k = 0; k < 8; k++) sum += fv[k] * rows[k][x];
        tmp[y * w + x] = static_cast<int16_t>(rnd_shift(sum, 6 - intermediate_bits) - bias);
      }
    }
  } else {
    prep_copy(tmp, src, w, h, bitdepth_max);
  }
}

// Fills the 8-row ring `mid` (row r at mid_rows[r]) with the horizontally filtered source rows needed up to row `src_y`.
// Shared by the scaled put and prep. `src_row` is the next source row to filter (relative to the block's first row - 3 taps).
struct ScaledState {
  int16_t mid_storage[kMidStride * 8];
  int mid_rows[8];
  int in_y = -8;
  int src_row = -3;
};

void scaled_advance_8tap(ScaledState& st, PixelView src, int filter_type, int w, int mx, int dx, int src_y, int intermediate_bits) {
  const ArrayView<int16_t> mid(st.mid_storage);
  while (st.in_y < src_y) {
    int imx = mx, ioff = 0;
    const int row = st.mid_rows[0];
    for (int i = 0; i < 7; i++) st.mid_rows[i] = st.mid_rows[i + 1];
    st.mid_rows[7] = row;

    for (int x = 0; x < w; x++) {
      const Taps fh = h_filter(filter_type, w, imx >> 6);
      int value;
      if (!fh.empty()) {
        const ArrayView<const Pixel> r = row_of(src, ioff - 3, st.src_row, 8);
        value = rnd_shift(tap8(fh, r, 0), 6 - intermediate_bits);
      } else {
        value = src.at(ioff, st.src_row) << intermediate_bits;
      }
      mid[row + x] = static_cast<int16_t>(value);
      imx += dx;
      ioff += imx >> 10;
      imx &= 0x3ff;
    }
    st.src_row++;
    st.in_y++;
  }
}

int tap8_rows(Taps f, ArrayView<const int16_t> mid, const int* rows, int x) {
  int sum = 0;
  for (int k = 0; k < 8; k++) sum += f[k] * mid[rows[k] + x];
  return sum;
}

void put_8tap_scaled(int filter_type, PixelView dst, PixelView src, int w, int h, int mx, int my, int dx, int dy, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int intermediate_rnd = (1 << intermediate_bits) >> 1;
  ScaledState st;
  for (int i = 0; i < 8; i++) st.mid_rows[i] = kMidStride * i;
  const ArrayView<const int16_t> mid(st.mid_storage);

  for (int y = 0; y < h; y++) {
    const int src_y = my >> 10;
    const Taps fv = v_filter(filter_type, h, (my & 0x3ff) >> 6);
    scaled_advance_8tap(st, src, filter_type, w, mx, dx, src_y, intermediate_bits);

    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
    for (int x = 0; x < w; x++) {
      if (!fv.empty())
        d[x] = static_cast<Pixel>(iclip(rnd_shift(tap8_rows(fv, mid, st.mid_rows, x), 6 + intermediate_bits), 0, bitdepth_max));
      else
        d[x] = static_cast<Pixel>(iclip((mid[st.mid_rows[3] + x] + intermediate_rnd) >> intermediate_bits, 0, bitdepth_max));
    }
    my += dy;
  }
}

void prep_8tap_scaled(int filter_type, ArrayView<int16_t> tmp, PixelView src, int w, int h, int mx, int my, int dx, int dy, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int bias = prep_bias_of(bitdepth_max);
  ScaledState st;
  for (int i = 0; i < 8; i++) st.mid_rows[i] = kMidStride * i;
  const ArrayView<const int16_t> mid(st.mid_storage);

  for (int y = 0; y < h; y++) {
    const int src_y = my >> 10;
    const Taps fv = v_filter(filter_type, h, (my & 0x3ff) >> 6);
    scaled_advance_8tap(st, src, filter_type, w, mx, dx, src_y, intermediate_bits);

    for (int x = 0; x < w; x++)
      tmp[y * w + x] = static_cast<int16_t>((!fv.empty() ? rnd_shift(tap8_rows(fv, mid, st.mid_rows, x), 6) : mid[st.mid_rows[3] + x]) - bias);
    my += dy;
  }
}

// ---- bilinear ----------------------------------------------------------------------------------------------------------------------
int bilin(int a, int b, int mxy) { return 16 * a + mxy * (b - a); }

void put_bilin(PixelView dst, PixelView src, int w, int h, int mx, int my, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int intermediate_rnd = (1 << intermediate_bits) >> 1;

  if (mx) {
    if (my) {
      int16_t mid_storage[kMidStride * 129];
      const ArrayView<int16_t> mid(mid_storage);
      for (int y = 0; y < h + 1; y++) {
        const ArrayView<const Pixel> r = row_of(src, 0, y, w + 1);
        for (int x = 0; x < w; x++) mid[y * kMidStride + x] = static_cast<int16_t>(rnd_shift(bilin(r[x], r[x + 1], mx), 4 - intermediate_bits));
      }
      for (int y = 0; y < h; y++) {
        const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
        for (int x = 0; x < w; x++)
          d[x] = static_cast<Pixel>(
              iclip(rnd_shift(bilin(mid[y * kMidStride + x], mid[(y + 1) * kMidStride + x], my), 4 + intermediate_bits), 0, bitdepth_max));
      }
    } else {
      for (int y = 0; y < h; y++) {
        const ArrayView<const Pixel> r = row_of(src, 0, y, w + 1);
        const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
        for (int x = 0; x < w; x++) {
          const int px = rnd_shift(bilin(r[x], r[x + 1], mx), 4 - intermediate_bits);
          d[x] = static_cast<Pixel>(iclip((px + intermediate_rnd) >> intermediate_bits, 0, bitdepth_max));
        }
      }
    }
  } else if (my) {
    for (int y = 0; y < h; y++) {
      const ArrayView<const Pixel> r0 = row_of(src, 0, y, w);
      const ArrayView<const Pixel> r1 = row_of(src, 0, y + 1, w);
      const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
      for (int x = 0; x < w; x++) d[x] = static_cast<Pixel>(iclip(rnd_shift(bilin(r0[x], r1[x], my), 4), 0, bitdepth_max));
    }
  } else {
    put_copy(dst, src, w, h);
  }
}

void prep_bilin(ArrayView<int16_t> tmp, PixelView src, int w, int h, int mx, int my, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int bias = prep_bias_of(bitdepth_max);

  if (mx) {
    if (my) {
      int16_t mid_storage[kMidStride * 129];
      const ArrayView<int16_t> mid(mid_storage);
      for (int y = 0; y < h + 1; y++) {
        const ArrayView<const Pixel> r = row_of(src, 0, y, w + 1);
        for (int x = 0; x < w; x++) mid[y * kMidStride + x] = static_cast<int16_t>(rnd_shift(bilin(r[x], r[x + 1], mx), 4 - intermediate_bits));
      }
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          tmp[y * w + x] = static_cast<int16_t>(rnd_shift(bilin(mid[y * kMidStride + x], mid[(y + 1) * kMidStride + x], my), 4) - bias);
    } else {
      for (int y = 0; y < h; y++) {
        const ArrayView<const Pixel> r = row_of(src, 0, y, w + 1);
        for (int x = 0; x < w; x++) tmp[y * w + x] = static_cast<int16_t>(rnd_shift(bilin(r[x], r[x + 1], mx), 4 - intermediate_bits) - bias);
      }
    }
  } else if (my) {
    for (int y = 0; y < h; y++) {
      const ArrayView<const Pixel> r0 = row_of(src, 0, y, w);
      const ArrayView<const Pixel> r1 = row_of(src, 0, y + 1, w);
      for (int x = 0; x < w; x++) tmp[y * w + x] = static_cast<int16_t>(rnd_shift(bilin(r0[x], r1[x], my), 4 - intermediate_bits) - bias);
    }
  } else {
    prep_copy(tmp, src, w, h, bitdepth_max);
  }
}

// The scaled bilinear functions share their row producer.
void bilin_scaled_advance(int16_t (&mid_storage)[kMidStride * 2], int& in_y, int& src_row, PixelView src, int w, int mx, int dx, int y,
                          int intermediate_bits) {
  const ArrayView<int16_t> mid(mid_storage);
  while (in_y < y) {
    int imx = mx, ioff = 0;
    const int base = (in_y & 1) * kMidStride;
    for (int x = 0; x < w; x++) {
      const int a = src.at(ioff, src_row);
      const int b = src.at(ioff + 1, src_row);
      mid[base + x] = static_cast<int16_t>(rnd_shift(bilin(a, b, imx >> 6), 4 - intermediate_bits));
      imx += dx;
      ioff += imx >> 10;
      imx &= 0x3ff;
    }
    src_row++;
    in_y++;
  }
}

void put_bilin_scaled(PixelView dst, PixelView src, int w, int h, int mx, int my, int dx, int dy, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  int16_t mid_storage[kMidStride * 2];
  const ArrayView<const int16_t> mid(mid_storage);
  int in_y = -2, src_row = 0;

  for (int row = 0; row < h; row++) {
    const int y = my >> 10;
    const int mid1 = (y & 1) * kMidStride;
    const int mid2 = ((y + 1) & 1) * kMidStride;
    const int dmy = my & 0x3ff;
    bilin_scaled_advance(mid_storage, in_y, src_row, src, w, mx, dx, y, intermediate_bits);

    const ArrayView<Pixel> d = row_of_mut(dst, 0, row, w);
    for (int x = 0; x < w; x++)
      d[x] = static_cast<Pixel>(iclip(rnd_shift(bilin(mid[mid1 + x], mid[mid2 + x], dmy >> 6), 4 + intermediate_bits), 0, bitdepth_max));
    my += dy;
  }
}

void prep_bilin_scaled(ArrayView<int16_t> tmp, PixelView src, int w, int h, int mx, int my, int dx, int dy, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int bias = prep_bias_of(bitdepth_max);
  int16_t mid_storage[kMidStride * 2];
  const ArrayView<const int16_t> mid(mid_storage);
  int in_y = -2, src_row = 0;

  for (int row = 0; row < h; row++) {
    const int y = my >> 10;
    const int mid1 = (y & 1) * kMidStride;
    const int mid2 = ((y + 1) & 1) * kMidStride;
    const int dmy = my & 0x3ff;
    bilin_scaled_advance(mid_storage, in_y, src_row, src, w, mx, dx, y, intermediate_bits);

    for (int x = 0; x < w; x++) tmp[row * w + x] = static_cast<int16_t>(rnd_shift(bilin(mid[mid1 + x], mid[mid2 + x], dmy >> 6), 4) - bias);
    my += dy;
  }
}

int blend_px(int a, int b, int m) { return (a * (64 - m) + b * m + 32) >> 6; }

// ---- warp --------------------------------------------------------------------------------------------------------------------------
// The warp filter for a position in 1/65536 units; positions of legal warp parameters are inside the table (the index is clamped for others).
Taps warp_filter(int position) { return Taps(k_mc_warp_filter[iclip(64 + ((position + 512) >> 10), 0, 192)], 8); }

// The 15 horizontally filtered rows of an 8x8 warp block (dav1d warp_affine_8x8_c, first pass), into mid[15 * 8].
void warp_horizontal(ArrayView<int16_t> mid, PixelView src, ArrayView<const int16_t> abcd, int mx, int intermediate_bits) {
  for (int y = 0; y < 15; y++, mx += abcd[1]) {
    const ArrayView<const Pixel> r = row_of(src, -3, y - 3, 8 + 7);
    for (int x = 0, tmx = mx; x < 8; x++, tmx += abcd[0]) {
      const Taps filter = warp_filter(tmx);
      mid[y * 8 + x] = static_cast<int16_t>(rnd_shift(tap8(filter, r, x), 7 - intermediate_bits));
    }
  }
}

int warp_vertical_sum(ArrayView<const int16_t> mid, int y, int x, int tmy) {
  const Taps f = warp_filter(tmy);
  int sum = 0;
  for (int k = 0; k < 8; k++) sum += f[k] * mid[(y + k) * 8 + x];
  return sum;
}

}  // namespace

// ---- public entry points -----------------------------------------------------------------------------------------------------------
void mc(Filter2d filter_2d, PixelView dst, PixelView src, int w, int h, int mx, int my, int bitdepth_max) {
  if (!block_size_ok(w, h)) return;
  if (filter_2d == FILTER_2D_BILINEAR)
    put_bilin(dst, src, w, h, mx, my, bitdepth_max);
  else
    put_8tap(filter_type_of(filter_2d), dst, src, w, h, mx, my, bitdepth_max);
}

void mct(Filter2d filter_2d, ArrayView<int16_t> tmp, PixelView src, int w, int h, int mx, int my, int bitdepth_max) {
  if (!block_size_ok(w, h)) return;
  if (filter_2d == FILTER_2D_BILINEAR)
    prep_bilin(tmp, src, w, h, mx, my, bitdepth_max);
  else
    prep_8tap(filter_type_of(filter_2d), tmp, src, w, h, mx, my, bitdepth_max);
}

void mc_scaled(Filter2d filter_2d, PixelView dst, PixelView src, int w, int h, int mx, int my, int dx, int dy, int bitdepth_max) {
  if (!block_size_ok(w, h)) return;
  if (filter_2d == FILTER_2D_BILINEAR)
    put_bilin_scaled(dst, src, w, h, mx, my, dx, dy, bitdepth_max);
  else
    put_8tap_scaled(filter_type_of(filter_2d), dst, src, w, h, mx, my, dx, dy, bitdepth_max);
}

void mct_scaled(Filter2d filter_2d, ArrayView<int16_t> tmp, PixelView src, int w, int h, int mx, int my, int dx, int dy, int bitdepth_max) {
  if (!block_size_ok(w, h)) return;
  if (filter_2d == FILTER_2D_BILINEAR)
    prep_bilin_scaled(tmp, src, w, h, mx, my, dx, dy, bitdepth_max);
  else
    prep_8tap_scaled(filter_type_of(filter_2d), tmp, src, w, h, mx, my, dx, dy, bitdepth_max);
}

void avg(PixelView dst, ArrayView<const int16_t> tmp1, ArrayView<const int16_t> tmp2, int w, int h, int bitdepth_max) {
  if (!block_size_ok(w, h)) return;
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int sh = intermediate_bits + 1;
  const int rnd = (1 << intermediate_bits) + prep_bias_of(bitdepth_max) * 2;
  for (int y = 0; y < h; y++) {
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
    for (int x = 0; x < w; x++) d[x] = static_cast<Pixel>(iclip((tmp1[y * w + x] + tmp2[y * w + x] + rnd) >> sh, 0, bitdepth_max));
  }
}

void w_avg(PixelView dst, ArrayView<const int16_t> tmp1, ArrayView<const int16_t> tmp2, int w, int h, int weight, int bitdepth_max) {
  if (!block_size_ok(w, h)) return;
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int sh = intermediate_bits + 4;
  const int rnd = (8 << intermediate_bits) + prep_bias_of(bitdepth_max) * 16;
  for (int y = 0; y < h; y++) {
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
    for (int x = 0; x < w; x++)
      d[x] = static_cast<Pixel>(iclip((tmp1[y * w + x] * weight + tmp2[y * w + x] * (16 - weight) + rnd) >> sh, 0, bitdepth_max));
  }
}

void mask(PixelView dst, ArrayView<const int16_t> tmp1, ArrayView<const int16_t> tmp2, int w, int h, ArrayView<const uint8_t> mask,
          int bitdepth_max) {
  if (!block_size_ok(w, h)) return;
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int sh = intermediate_bits + 6;
  const int rnd = (32 << intermediate_bits) + prep_bias_of(bitdepth_max) * 64;
  for (int y = 0; y < h; y++) {
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
    for (int x = 0; x < w; x++) {
      const int i = y * w + x;
      d[x] = static_cast<Pixel>(iclip((tmp1[i] * mask[i] + tmp2[i] * (64 - mask[i]) + rnd) >> sh, 0, bitdepth_max));
    }
  }
}

void w_mask(int ss_hor, int ss_ver, PixelView dst, ArrayView<const int16_t> tmp1, ArrayView<const int16_t> tmp2, int w, int h,
            ArrayView<uint8_t> mask, int sign, int bitdepth_max) {
  if (!block_size_ok(w, h)) return;
  // store mask at 2x2 resolution, i.e. store 2x1 sum for even rows, and then load this intermediate to calculate final value for odd rows
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int bitdepth = bitdepth_from_max(bitdepth_max);
  const int sh = intermediate_bits + 6;
  const int rnd = (32 << intermediate_bits) + prep_bias_of(bitdepth_max) * 64;
  const int mask_sh = bitdepth + intermediate_bits - 4;
  const int mask_rnd = 1 << (mask_sh - 5);
  int mask_off = 0;
  int rows_left = h;  // dav1d's decrementing `h`, whose parity selects the mask row handling
  for (int y = 0; y < h; y++, rows_left--) {
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
    for (int x = 0; x < w; x++) {
      const int i = y * w + x;
      const int tmpdiff = tmp1[i] - tmp2[i];
      const int m = imin(38 + ((iabs(tmpdiff) + mask_rnd) >> mask_sh), 64);
      d[x] = static_cast<Pixel>(iclip((tmpdiff * m + tmp2[i] * 64 + rnd) >> sh, 0, bitdepth_max));

      if (ss_hor) {
        x++;
        const int j = y * w + x;
        const int tmpdiff2 = tmp1[j] - tmp2[j];
        const int n = imin(38 + ((iabs(tmpdiff2) + mask_rnd) >> mask_sh), 64);
        d[x] = static_cast<Pixel>(iclip((tmpdiff2 * n + tmp2[j] * 64 + rnd) >> sh, 0, bitdepth_max));

        if (rows_left & ss_ver)
          mask[mask_off + (x >> 1)] = static_cast<uint8_t>((m + n + mask[mask_off + (x >> 1)] + 2 - sign) >> 2);
        else if (ss_ver)
          mask[mask_off + (x >> 1)] = static_cast<uint8_t>(m + n);
        else
          mask[mask_off + (x >> 1)] = static_cast<uint8_t>((m + n + 1 - sign) >> 1);
      } else {
        mask[mask_off + x] = static_cast<uint8_t>(m);
      }
    }
    if (!ss_ver || (rows_left & 1)) mask_off += w >> ss_hor;
  }
}

void blend(PixelView dst, ArrayView<const Pixel> tmp, int w, int h, ArrayView<const uint8_t> mask) {
  if (!block_size_ok(w, h)) return;
  for (int y = 0; y < h; y++) {
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
    for (int x = 0; x < w; x++) d[x] = static_cast<Pixel>(blend_px(d[x], tmp[y * w + x], mask[y * w + x]));
  }
}

void blend_v(PixelView dst, ArrayView<const Pixel> tmp, int w, int h) {
  if (!block_size_ok(w, h)) return;
  for (int y = 0; y < h; y++) {
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
    for (int x = 0; x < (w * 3) >> 2; x++) d[x] = static_cast<Pixel>(blend_px(d[x], tmp[y * w + x], k_obmc_masks[w + x]));
  }
}

void blend_h(PixelView dst, ArrayView<const Pixel> tmp, int w, int h) {
  if (!block_size_ok(w, h)) return;
  const int rows = (h * 3) >> 2;
  for (int y = 0; y < rows; y++) {
    const int m = k_obmc_masks[h + y];
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, w);
    for (int x = 0; x < w; x++) d[x] = static_cast<Pixel>(blend_px(d[x], tmp[y * w + x], m));
  }
}

void warp8x8(PixelView dst, PixelView src, ArrayView<const int16_t> abcd, int mx, int my, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  int16_t mid_storage[15 * 8];
  const ArrayView<int16_t> mid(mid_storage);
  warp_horizontal(mid, src, abcd, mx, intermediate_bits);

  for (int y = 0; y < 8; y++, my += abcd[3]) {
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, 8);
    for (int x = 0, tmy = my; x < 8; x++, tmy += abcd[2]) {
      const int sum = warp_vertical_sum(mid, y, x, tmy);
      d[x] = static_cast<Pixel>(iclip(rnd_shift(sum, 7 + intermediate_bits), 0, bitdepth_max));
    }
  }
}

void warp8x8t(ArrayView<int16_t> tmp, int64_t tmp_stride, PixelView src, ArrayView<const int16_t> abcd, int mx, int my, int bitdepth_max) {
  const int intermediate_bits = intermediate_bits_of(bitdepth_max);
  const int bias = prep_bias_of(bitdepth_max);
  int16_t mid_storage[15 * 8];
  const ArrayView<int16_t> mid(mid_storage);
  warp_horizontal(mid, src, abcd, mx, intermediate_bits);

  for (int y = 0; y < 8; y++, my += abcd[3]) {
    for (int x = 0, tmy = my; x < 8; x++, tmy += abcd[2]) {
      const int sum = warp_vertical_sum(mid, y, x, tmy);
      tmp[static_cast<uint64_t>(y * tmp_stride + x)] = static_cast<int16_t>(rnd_shift(sum, 7) - bias);
    }
  }
}

void emu_edge(int bw, int bh, int iw, int ih, int x, int y, PixelView dst, PixelView ref) {
  if (bw < 1 || bh < 1 || iw < 1 || ih < 1) return;
  // find offset in reference of visible block to copy
  const int ref_x = iclip(x, 0, iw - 1);
  const int ref_y = iclip(y, 0, ih - 1);

  // number of pixels to extend (left, right, top, bottom)
  const int left_ext = iclip(-x, 0, bw - 1);
  const int right_ext = iclip(x + bw - iw, 0, bw - 1);
  const int top_ext = iclip(-y, 0, bh - 1);
  const int bottom_ext = iclip(y + bh - ih, 0, bh - 1);
  if (left_ext + right_ext >= bw || top_ext + bottom_ext >= bh) return;  // cannot happen for iw, ih >= 1; kept as a guard

  // copy visible portion first
  const int center_w = bw - left_ext - right_ext;
  const int center_h = bh - top_ext - bottom_ext;
  for (int row = 0; row < center_h; row++) {
    const ArrayView<Pixel> blk = row_of_mut(dst, 0, top_ext + row, bw);
    blk.subview(left_ext, center_w).copy_from(row_of(ref, ref_x, ref_y + row, center_w));
    // extend left edge for this line
    if (left_ext) blk.first(left_ext).fill(blk[left_ext]);
    // extend right edge for this line
    if (right_ext) blk.subview(left_ext + center_w, right_ext).fill(blk[left_ext + center_w - 1]);
  }

  // copy top
  const ArrayView<const Pixel> first_row = row_of_mut(dst, 0, top_ext, bw);
  for (int row = 0; row < top_ext; row++) row_of_mut(dst, 0, row, bw).copy_from(first_row);

  // copy bottom
  const ArrayView<const Pixel> last_row = row_of_mut(dst, 0, top_ext + center_h - 1, bw);
  for (int row = 0; row < bottom_ext; row++) row_of_mut(dst, 0, top_ext + center_h + row, bw).copy_from(last_row);
}

void resize(PixelView dst, PixelView src, int dst_w, int h, int src_w, int dx, int mx0, int bitdepth_max) {
  if (dst_w < 1 || src_w < 1 || h < 1) return;
  for (int y = 0; y < h; y++) {
    const ArrayView<Pixel> d = row_of_mut(dst, 0, y, dst_w);
    const ArrayView<const Pixel> s = row_of(src, 0, y, src_w);
    int mx = mx0, src_x = -1;
    for (int x = 0; x < dst_w; x++) {
      const Taps F(k_resize_filter[(mx >> 8) & 63], 8);
      int sum = 0;
      for (int k = 0; k < 8; k++) sum += F[k] * s[iclip(src_x - 3 + k, 0, src_w - 1)];
      d[x] = static_cast<Pixel>(iclip((-sum + 64) >> 7, 0, bitdepth_max));
      mx += dx;
      src_x += mx >> 14;
      mx &= 0x3fff;
    }
  }
}

}  // namespace av1
}  // namespace hardware
