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

#include "Hardware/VideoCodec/Software/Av1LoopRestoration.hpp"
#include "Utility/AllocatedView.hpp"

// Port of dav1d's looprestoration_tmpl.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors) and lr_apply_tmpl.c.
// dav1d filters in place with hand-rolled row pointer rotation and goto-based tails for the short stripes. Here the stripe is
// first copied into a padded buffer in which all edge handling is resolved (see the header), after which every kernel is the plain
// mathematical definition: output row j reads the padded rows j-3..j+3, replicated above/below as the padding says. The arithmetic of
// every step (types, rounding, clipping) is dav1d's 16-bit variant.

namespace hardware {
namespace av1 {

namespace {
#include "Hardware/VideoCodec/Software/Av1Tables.inc"

constexpr int RW = 400;          // ring-buffer row stride (w + 2 <= 386)
constexpr int FILTER_W = 384;    // width of the finish temporaries

int clip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int bitdepth_from_max(int bitdepth_max) { return 32 - __builtin_clz(static_cast<unsigned>(bitdepth_max)); }
int clip_pixel(int v, int bitdepth_max) { return clip(v, 0, bitdepth_max); }

// ---- Wiener -----------------------------------------------------------------------------------------------------------------------

// Horizontal pass of padded row r into one ring row.
void wiener_row_h(ArrayView<int32_t> dst, PixelView src, int r, int w, const int16_t* fh, int bitdepth) {
  const int round_bits_h = 3 + (bitdepth == 12) * 2;
  const int rounding_off_h = 1 << (round_bits_h - 1);
  const int clip_limit = 1 << (bitdepth + 1 + 7 - round_bits_h);
  for (int x = 0; x < w; x++) {
    int sum = 1 << (bitdepth + 6);
    for (int i = 0; i < 7; i++) sum += src.at(x + i - 3, r) * fh[i];
    dst[static_cast<uint64_t>(x)] = clip((sum + rounding_off_h) >> round_bits_h, 0, clip_limit - 1);
  }
}

// ---- Self-guided filter -----------------------------------------------------------------------------------------------------------

struct SgrScratch {
  ArrayView<int32_t> hq3, hs3, hq5, hs5;  // horizontal box sums (4 ring rows each)
  ArrayView<int32_t> a3, b3, a5, b5;      // A and B planes of the box filters
  ArrayView<int32_t> t5, t3;              // finish temporaries (2 rows each)
};

struct Sgr {
  PixelView src;
  int w, h, bitdepth_max;
  SgrScratch s;
  int tag_h3[4], tag_h5[8], tag_ab3[4], tag_ab5[4];  // which row a ring slot currently holds

  static int slot(int r) { return (r + 4) & 3; }  // r >= -3

  ArrayView<int32_t> row(ArrayView<int32_t> ring, int slot_index) const {
    return ring.subview(static_cast<uint64_t>(slot_index) * RW, RW);
  }

  // Horizontal sums over 3 / 5 samples, for x in [-1, w], stored at index x + 1 (dav1d sgr_box3_row_h / sgr_box5_row_h).
  ArrayView<int32_t> h3(int r, bool want_sq) {
    const int sl = slot(r);
    ArrayView<int32_t> q = row(s.hq3, sl), m = row(s.hs3, sl);
    if (tag_h3[sl] != r) {
      for (int x = -1; x < w + 1; x++) {
        const int a = src.at(x - 1, r), b = src.at(x, r), c = src.at(x + 1, r);
        m[static_cast<uint64_t>(x + 1)] = a + b + c;
        q[static_cast<uint64_t>(x + 1)] = a * a + b * b + c * c;
      }
      tag_h3[sl] = r;
    }
    return want_sq ? q : m;
  }
  ArrayView<int32_t> h5(int r, bool want_sq) {
    const int sl = (r + 8) & 7;  // the 5x5 pass keeps 5 rows alive
    ArrayView<int32_t> q = row(s.hq5, sl), m = row(s.hs5, sl);
    if (tag_h5[sl] != r) {
      for (int x = -1; x < w + 1; x++) {
        const int a = src.at(x - 2, r), b = src.at(x - 1, r), c = src.at(x, r), d = src.at(x + 1, r), e = src.at(x + 2, r);
        m[static_cast<uint64_t>(x + 1)] = a + b + c + d + e;
        q[static_cast<uint64_t>(x + 1)] = a * a + b * b + c * c + d * d + e * e;
      }
      tag_h5[sl] = r;
    }
    return want_sq ? q : m;
  }

  // dav1d sgr_calc_row_ab: turns the box sums (AA = sum of squares, BB = sum) into the A and B planes in place.
  void calc_ab(ArrayView<int32_t> aa, ArrayView<int32_t> bb, int s_strength, int n, int sgr_one_by_x) const {
    const int bitdepth_min_8 = bitdepth_from_max(bitdepth_max) - 8;
    for (int i = 0; i < w + 2; i++) {
      const uint64_t u = static_cast<uint64_t>(i);
      const int a = (aa[u] + ((1 << (2 * bitdepth_min_8)) >> 1)) >> (2 * bitdepth_min_8);
      const int b = (bb[u] + ((1 << bitdepth_min_8) >> 1)) >> bitdepth_min_8;
      const int pv = a * n - b * b;
      const unsigned p = static_cast<unsigned>(pv > 0 ? pv : 0);
      const unsigned z = (p * static_cast<unsigned>(s_strength) + (1u << 19)) >> 20;
      const unsigned x = k_sgr_x_by_x[z < 255 ? z : 255];
      // This is where A and B are inverted, so that B is of size coef.
      aa[u] = static_cast<int32_t>((x * static_cast<unsigned>(bb[u]) * static_cast<unsigned>(sgr_one_by_x) + (1u << 11)) >> 12);
      bb[u] = static_cast<int32_t>(x);
    }
  }

  // A/B of the 3x3 pass centred on row y (y in [-1, h]): box over the padded rows y-1..y+1.
  void ab3(int y, ArrayView<int32_t>* a, ArrayView<int32_t>* b, int strength) {
    const int sl = slot(y);
    *a = row(s.a3, sl);
    *b = row(s.b3, sl);
    if (tag_ab3[sl] == y) return;
    ArrayView<int32_t> q0 = h3(y - 1, true), q1 = h3(y, true), q2 = h3(y + 1, true);
    ArrayView<int32_t> m0 = h3(y - 1, false), m1 = h3(y, false), m2 = h3(y + 1, false);
    for (int i = 0; i < w + 2; i++) {
      const uint64_t u = static_cast<uint64_t>(i);
      (*a)[u] = q0[u] + q1[u] + q2[u];
      (*b)[u] = m0[u] + m1[u] + m2[u];
    }
    calc_ab(*a, *b, strength, 9, 455);
    tag_ab3[sl] = y;
  }
  // The same for the 5x5 pass (y odd in [-1, h]): box over y-2..y+2.
  void ab5(int y, ArrayView<int32_t>* a, ArrayView<int32_t>* b, int strength) {
    const int sl = ((y + 4) >> 1) & 3;
    *a = row(s.a5, sl);
    *b = row(s.b5, sl);
    if (tag_ab5[sl] == y) return;
    ArrayView<int32_t> q[5], m[5];
    for (int k = 0; k < 5; k++) {
      q[k] = h5(y - 2 + k, true);
      m[k] = h5(y - 2 + k, false);
    }
    for (int i = 0; i < w + 2; i++) {
      const uint64_t u = static_cast<uint64_t>(i);
      (*a)[u] = q[0][u] + q[1][u] + q[2][u] + q[3][u] + q[4][u];
      (*b)[u] = m[0][u] + m[1][u] + m[2][u] + m[3][u] + m[4][u];
    }
    calc_ab(*a, *b, strength, 25, 164);
    tag_ab5[sl] = y;
  }

  // dav1d sgr_finish_filter_row1: the 3x3 pass of output row j into tmp[0..w).
  void finish3(ArrayView<int32_t> tmp, int j, const LrFilterParams& p) {
    ArrayView<int32_t> a[3], b[3];
    for (int k = 0; k < 3; k++) ab3(j - 1 + k, &a[k], &b[k], static_cast<int>(p.s1));
    for (int i = 0; i < w; i++) {
      const uint64_t u = static_cast<uint64_t>(i + 1);
      auto eight = [&](ArrayView<int32_t>* P) {
        return (P[1][u] + P[1][u - 1] + P[1][u + 1] + P[0][u] + P[2][u]) * 4 + (P[0][u - 1] + P[2][u - 1] + P[0][u + 1] + P[2][u + 1]) * 3;
      };
      const int av = eight(b);
      const int bv = eight(a);
      tmp[static_cast<uint64_t>(i)] = (bv - av * src.at(i, j) + (1 << 8)) >> 9;
    }
  }
  // dav1d sgr_finish_filter2: the 5x5 pass of output row j into tmp[0..w).
  void finish5(ArrayView<int32_t> tmp, int j, const LrFilterParams& p) {
    if ((j & 1) == 0) {
      ArrayView<int32_t> a[2], b[2];
      ab5(j - 1, &a[0], &b[0], static_cast<int>(p.s0));
      ab5(j + 1, &a[1], &b[1], static_cast<int>(p.s0));
      for (int i = 0; i < w; i++) {
        const uint64_t u = static_cast<uint64_t>(i + 1);
        auto six = [&](ArrayView<int32_t>* P) { return (P[0][u] + P[1][u]) * 6 + (P[0][u - 1] + P[1][u - 1] + P[0][u + 1] + P[1][u + 1]) * 5; };
        const int av = six(b);
        const int bv = six(a);
        tmp[static_cast<uint64_t>(i)] = (bv - av * src.at(i, j) + (1 << 8)) >> 9;
      }
    } else {
      ArrayView<int32_t> a, b;
      ab5(j, &a, &b, static_cast<int>(p.s0));
      for (int i = 0; i < w; i++) {
        const uint64_t u = static_cast<uint64_t>(i + 1);
        const int av = b[u] * 6 + (b[u - 1] + b[u + 1]) * 5;
        const int bv = a[u] * 6 + (a[u - 1] + a[u + 1]) * 5;
        tmp[static_cast<uint64_t>(i)] = (bv - av * src.at(i, j) + (1 << 7)) >> 8;
      }
    }
  }
};

void parse_scratch(ArrayView<int32_t> scratch, SgrScratch* s) {
  uint64_t off = 0;
  auto take = [&](uint64_t n) {
    ArrayView<int32_t> v = scratch.subview(off, n);
    off += n;
    return v;
  };
  s->hq3 = take(4 * RW);
  s->hs3 = take(4 * RW);
  s->hq5 = take(8 * RW);
  s->hs5 = take(8 * RW);
  s->a3 = take(4 * RW);
  s->b3 = take(4 * RW);
  s->a5 = take(4 * RW);
  s->b5 = take(4 * RW);
  s->t5 = take(FILTER_W);
  s->t3 = take(FILTER_W);
}

}  // namespace

// ---- Public kernels ---------------------------------------------------------------------------------------------------------------

bool lr_make_params(const RestorationUnit& unit, LrFilterParams* out) {
  *out = LrFilterParams();
  if (unit.type == RESTORATION_WIENER) {
    int16_t(*filter)[8] = out->filter;
    filter[0][0] = filter[0][6] = unit.filter_h[0];
    filter[0][1] = filter[0][5] = unit.filter_h[1];
    filter[0][2] = filter[0][4] = unit.filter_h[2];
    filter[0][3] = static_cast<int16_t>(-(filter[0][0] + filter[0][1] + filter[0][2]) * 2);
    // The 16-bit variant of dav1d folds the +128 of the centre tap into the horizontal taps.
    filter[0][3] = static_cast<int16_t>(filter[0][3] + 128);

    filter[1][0] = filter[1][6] = unit.filter_v[0];
    filter[1][1] = filter[1][5] = unit.filter_v[1];
    filter[1][2] = filter[1][4] = unit.filter_v[2];
    filter[1][3] = static_cast<int16_t>(128 - (filter[1][0] + filter[1][1] + filter[1][2]) * 2);
    return true;
  }
  if (unit.type == RESTORATION_SGRPROJ) {
    if (unit.sgr_idx > 15) return false;
    const uint16_t* sgr = k_sgr_params[unit.sgr_idx];
    out->s0 = sgr[0];
    out->s1 = sgr[1];
    out->w0 = unit.sgr_weights[0];
    out->w1 = static_cast<int16_t>(128 - (unit.sgr_weights[0] + unit.sgr_weights[1]));
    return sgr[0] != 0 || sgr[1] != 0;
  }
  return false;
}

void wiener_filter(PixelView dst, PixelView src, int w, int h, const LrFilterParams& params, int bitdepth_max,
                   ArrayView<int32_t> scratch) {
  if (w < 1 || w > LR_MAX_UNIT_W || h < 1) bounds_violation("wiener_filter", static_cast<uint64_t>(w), LR_MAX_UNIT_W);
  const int bitdepth = bitdepth_from_max(bitdepth_max);
  const int round_bits_v = 11 - (bitdepth == 12) * 2;
  const int rounding_off_v = 1 << (round_bits_v - 1);
  const int round_offset = 1 << (bitdepth + (round_bits_v - 1));
  const int16_t* fh = params.filter[0];
  const int16_t* fv = params.filter[1];
  ArrayView<int32_t> ring = scratch.subview(0, 7 * RW);

  auto slot_of = [&](int r) { return ring.subview(static_cast<uint64_t>((r + 3) % 7) * RW, RW); };
  for (int r = -3; r < 3; r++) wiener_row_h(slot_of(r), src, r, w, fh, bitdepth);
  for (int j = 0; j < h; j++) {
    wiener_row_h(slot_of(j + 3), src, j + 3, w, fh, bitdepth);
    ArrayView<int32_t> rows[7];
    for (int k = 0; k < 7; k++) rows[k] = slot_of(j - 3 + k);
    for (int i = 0; i < w; i++) {
      int sum = -round_offset;
      for (int k = 0; k < 7; k++) sum += rows[k][static_cast<uint64_t>(i)] * fv[k];
      dst.at(i, j) = static_cast<Pixel>(clip_pixel((sum + rounding_off_v) >> round_bits_v, bitdepth_max));
    }
  }
}

void sgr_filter(PixelView dst, PixelView src, int w, int h, int variant, const LrFilterParams& params, int bitdepth_max,
                ArrayView<int32_t> scratch) {
  if (w < 1 || w > LR_MAX_UNIT_W || h < 1 || variant < 0 || variant > 2) bounds_violation("sgr_filter", static_cast<uint64_t>(w), LR_MAX_UNIT_W);
  Sgr sgr{src, w, h, bitdepth_max, {}, {}, {}, {}, {}};
  parse_scratch(scratch, &sgr.s);
  for (int i = 0; i < 4; i++) sgr.tag_h3[i] = sgr.tag_ab3[i] = sgr.tag_ab5[i] = -1000;
  for (int i = 0; i < 8; i++) sgr.tag_h5[i] = -1000;
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      const uint64_t u = static_cast<uint64_t>(i);
      int v;
      if (variant == 0) {
        if (i == 0) sgr.finish5(sgr.s.t5, j, params);
        v = params.w0 * sgr.s.t5[u];
      } else if (variant == 1) {
        if (i == 0) sgr.finish3(sgr.s.t3, j, params);
        v = params.w1 * sgr.s.t3[u];
      } else {
        if (i == 0) {
          sgr.finish5(sgr.s.t5, j, params);
          sgr.finish3(sgr.s.t3, j, params);
        }
        v = params.w0 * sgr.s.t5[u] + params.w1 * sgr.s.t3[u];
      }
      dst.at(i, j) = static_cast<Pixel>(clip_pixel(src.at(i, j) + ((v + (1 << 10)) >> 11), bitdepth_max));
    }
  }
}

bool restore_unit(PixelView dst, PixelView src, int w, int h, const RestorationUnit& unit, int bitdepth_max,
                  ArrayView<int32_t> scratch) {
  LrFilterParams params;
  if (!lr_make_params(unit, &params)) return false;
  if (unit.type == RESTORATION_WIENER) {
    wiener_filter(dst, src, w, h, params, bitdepth_max, scratch);
  } else {
    sgr_filter(dst, src, w, h, (params.s0 != 0) + (params.s1 != 0) * 2 - 1, params, bitdepth_max, scratch);
  }
  return true;
}

// ---- Units and stripes ------------------------------------------------------------------------------------------------------------

LrUnitRange lr_unit_read_range(int mi_row, int mi_col, int bh4, int bw4, int ss_hor, int ss_ver, int unit_size_log2,
                               bool super_res, int super_res_denom, int plane_unit_rows, int plane_unit_cols) {
  const int64_t unit = int64_t{1} << unit_size_log2;
  LrUnitRange r;
  r.row_start = static_cast<int>((static_cast<int64_t>(mi_row) * (4 >> ss_ver) + unit - 1) / unit);
  int64_t row_end = (static_cast<int64_t>(mi_row + bh4) * (4 >> ss_ver) + unit - 1) / unit;
  r.row_end = static_cast<int>(row_end < plane_unit_rows ? row_end : plane_unit_rows);
  const int64_t numerator = super_res ? static_cast<int64_t>(4 >> ss_hor) * super_res_denom : (4 >> ss_hor);
  const int64_t denominator = super_res ? unit * 8 : unit;
  r.col_start = static_cast<int>((mi_col * numerator + denominator - 1) / denominator);
  int64_t col_end = ((static_cast<int64_t>(mi_col) + bw4) * numerator + denominator - 1) / denominator;
  r.col_end = static_cast<int>(col_end < plane_unit_cols ? col_end : plane_unit_cols);
  return r;
}

namespace {

constexpr int MAX_STRIPE_H = 64;

// Copies the padded rows -3 .. h+2 of the stripe [y0, y0 + h) into buf (row stride w + 6, sample (x, r) at (r + 3) * stride + x + 3).
// This is the whole edge handling of dav1d's lr_stripe (LR_HAVE_*): replicate the first/last row of the frame, read the pre-CDEF
// rows (two above, two below) at the stripe boundaries and replicate the frame's left and right columns.
void build_padded_stripe(ArrayView<Pixel> buf, PixelView src, PixelView deblocked, int w, int height, int y0, int h) {
  const int stride = w + 6;
  const bool have_top = y0 > 0;
  const bool have_bottom = y0 + h < height;
  for (int r = -3; r < h + 3; r++) {
    PixelView from = src;
    int y;
    if (r < 0) {
      if (have_top) {
        from = deblocked;
        y = y0 + (r < -2 ? -2 : r);
      } else {
        y = y0;
      }
    } else if (r >= h) {
      if (have_bottom) {
        from = deblocked;
        y = y0 + h + (r - h > 1 ? 1 : r - h);
        if (y > height - 1) y = height - 1;
      } else {
        y = y0 + h - 1;
      }
    } else {
      y = y0 + r;
    }
    ArrayView<Pixel> out = buf.subview(static_cast<uint64_t>(r + 3) * static_cast<uint64_t>(stride), static_cast<uint64_t>(stride));
    for (int x = -3; x < w + 3; x++) out[static_cast<uint64_t>(x + 3)] = from.at(clip(x, 0, w - 1), y);
  }
}

}  // namespace

bool loop_restoration_plane(allocators::IAllocator& allocator, PixelView dst, PixelView src, PixelView deblocked, const LrPlane& plane) {
  const int w = plane.width, height = plane.height;
  if (w < 1 || height < 1 || w > 65536 || height > 65536 || (plane.ss_ver != 0 && plane.ss_ver != 1)) return false;
  if (plane.unit_size_log2 < 5 || plane.unit_size_log2 > 8) return false;
  if (plane.bitdepth_max != 255 && plane.bitdepth_max != 1023 && plane.bitdepth_max != 4095) return false;
  const int unit_rows = lr_unit_count(height, plane.unit_size_log2);
  const int unit_cols = lr_unit_count(w, plane.unit_size_log2);
  if (plane.units.size() < static_cast<uint64_t>(unit_rows) * static_cast<uint64_t>(unit_cols)) return false;
  const int unit_size = 1 << plane.unit_size_log2;
  const bool in_place = dst.plane.data() == src.plane.data() && dst.origin == src.origin && dst.stride == src.stride;

  const uint64_t stride = static_cast<uint64_t>(w) + 6;
  ArrayView<Pixel> buf = allocate_view<Pixel>(allocator, stride * (MAX_STRIPE_H + 6));
  ArrayView<int32_t> scratch = allocate_view<int32_t>(allocator, LR_SCRATCH_INTS);
  bool ok = !buf.empty() && !scratch.empty();

  // The first stripe of the frame is shorter by 8 luma rows.
  const int full_h = 64 >> plane.ss_ver;
  int y0 = 0;
  for (int stripe = 0; ok && y0 < height; stripe++) {
    int h = (stripe == 0 ? 56 >> plane.ss_ver : full_h);
    if (h > height - y0) h = height - y0;
    const int unit_row = clip(((64 * stripe) >> plane.ss_ver) >> plane.unit_size_log2, 0, unit_rows - 1);

    build_padded_stripe(buf, src, deblocked, w, height, y0, h);
    PixelView padded{buf, static_cast<int64_t>(3 * stride + 3), static_cast<int64_t>(stride)};

    for (int uc = 0; uc < unit_cols; uc++) {
      const int x0 = uc * unit_size;
      const int uw = (uc == unit_cols - 1) ? w - x0 : unit_size;
      const RestorationUnit& unit = plane.units[static_cast<uint64_t>(unit_row) * static_cast<uint64_t>(unit_cols) + static_cast<uint64_t>(uc)];
      PixelView out = dst.offset(x0, y0);
      if (!restore_unit(out, padded.offset(x0, 0), uw, h, unit, plane.bitdepth_max, scratch) && !in_place) {
        for (int y = 0; y < h; y++)
          for (int x = 0; x < uw; x++) out.at(x, y) = padded.at(x0 + x, y);
      }
    }
    y0 += h;
  }
  release_view(allocator, buf);
  release_view(allocator, scratch);
  return ok;
}

}  // namespace av1
}  // namespace hardware
