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


#include "Hardware/VideoCodec/Software/Av1Cdef.hpp"

// Port of dav1d's cdef_tmpl.c and cdef_apply_tmpl.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors), 16-bit samples. The
// arithmetic is dav1d's, step for step; the line buffers that let dav1d filter in place are replaced by reading a separate source frame.

namespace hardware {
namespace av1 {

namespace {

#include "Hardware/VideoCodec/Software/Av1Tables.inc"

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iclip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int abs_int(int v) { return v < 0 ? -v : v; }
inline int umin(int a, int b) { return static_cast<unsigned>(a) < static_cast<unsigned>(b) ? a : b; }
inline int ulog2(unsigned v) { return 31 - __builtin_clz(v); }
inline int apply_sign(int v, int s) { return s < 0 ? -v : v; }

inline int constrain(int diff, int threshold, int shift) {
  const int adiff = abs_int(diff);
  return apply_sign(imin(adiff, imax(0, threshold - (adiff >> shift))), diff);
}

constexpr int kTmpStride = 12;

}  // namespace

void cdef_filter_block(PixelView dst, PixelView src, int w, int h, int pri_strength, int sec_strength, int dir, int damping, int edges,
                       int bitdepth_max) {
  if ((w != 4 && w != 8) || (h != 4 && h != 8) || dir < 0 || dir > 7 || (!pri_strength && !sec_strength)) return;
  const int bitdepth_min_8 = 32 - __builtin_clz(static_cast<unsigned>(bitdepth_max)) - 8;
  int16_t tmp_buf[144];  // 12*12 is the maximum value of tmp_stride * (h + 4)
  const ArrayView<int16_t> tmp(tmp_buf);
  const int base = 2 * kTmpStride + 2;  // index of the sample (0, 0)
  auto at = [&](int x, int y) -> int16_t& { return tmp[static_cast<uint64_t>(base + y * kTmpStride + x)]; };
  // the sample at (x, y) plus an offset of the direction table (a negative index wraps and stops at the bounds check)
  auto tap = [&](int x, int y, int off) -> int { return tmp[static_cast<uint64_t>(static_cast<int64_t>(base + y * kTmpStride + x + off))]; };
  auto dir_offset = [&](int row, int k) { return static_cast<int>(k_cdef_directions[dir + row][k]); };

  // padding(): fill the extended input buffer
  const int fill_value = INT16_MIN;  // a large positive number when interpreted as unsigned, a large negative number when signed
  int x_start = -2, x_end = w + 2, y_start = -2, y_end = h + 2;
  if (!(edges & CDEF_HAVE_TOP)) {
    for (int y = -2; y < 0; y++)
      for (int x = -2; x < w + 2; x++) at(x, y) = static_cast<int16_t>(fill_value);
    y_start = 0;
  }
  if (!(edges & CDEF_HAVE_BOTTOM)) {
    for (int y = h; y < h + 2; y++)
      for (int x = -2; x < w + 2; x++) at(x, y) = static_cast<int16_t>(fill_value);
    y_end -= 2;
  }
  if (!(edges & CDEF_HAVE_LEFT)) {
    for (int y = y_start; y < y_end; y++)
      for (int x = -2; x < 0; x++) at(x, y) = static_cast<int16_t>(fill_value);
    x_start = 0;
  }
  if (!(edges & CDEF_HAVE_RIGHT)) {
    for (int y = y_start; y < y_end; y++)
      for (int x = w; x < w + 2; x++) at(x, y) = static_cast<int16_t>(fill_value);
    x_end -= 2;
  }
  for (int y = y_start; y < 0; y++)
    for (int x = x_start; x < x_end; x++) at(x, y) = static_cast<int16_t>(src.at(x, y));
  for (int y = 0; y < h; y++)
    for (int x = x_start; x < x_end; x++) at(x, y) = static_cast<int16_t>(src.at(x, y));
  for (int y = h; y < y_end; y++)
    for (int x = x_start; x < x_end; x++) at(x, y) = static_cast<int16_t>(src.at(x, y));

  if (pri_strength) {
    const int pri_tap = 4 - ((pri_strength >> bitdepth_min_8) & 1);
    const int pri_shift = imax(0, damping - ulog2(static_cast<unsigned>(pri_strength)));
    if (sec_strength) {
      const int sec_shift = damping - ulog2(static_cast<unsigned>(sec_strength));
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          const int px = at(x, y);
          int sum = 0;
          int max = px, min = px;
          int pri_tap_k = pri_tap;
          for (int k = 0; k < 2; k++) {
            const int off1 = dir_offset(2, k);  // dir
            const int p0 = tap(x, y, off1);
            const int p1 = tap(x, y, -off1);
            sum += pri_tap_k * constrain(p0 - px, pri_strength, pri_shift);
            sum += pri_tap_k * constrain(p1 - px, pri_strength, pri_shift);
            // if pri_tap_k == 4 then it becomes 2 else it remains 3
            pri_tap_k = (pri_tap_k & 3) | 2;
            min = umin(p0, min);
            max = imax(p0, max);
            min = umin(p1, min);
            max = imax(p1, max);
            const int off2 = dir_offset(4, k);  // dir + 2
            const int off3 = dir_offset(0, k);  // dir - 2
            const int s0 = tap(x, y, off2);
            const int s1 = tap(x, y, -off2);
            const int s2 = tap(x, y, off3);
            const int s3 = tap(x, y, -off3);
            // sec_tap starts at 2 and becomes 1
            const int sec_tap = 2 - k;
            sum += sec_tap * constrain(s0 - px, sec_strength, sec_shift);
            sum += sec_tap * constrain(s1 - px, sec_strength, sec_shift);
            sum += sec_tap * constrain(s2 - px, sec_strength, sec_shift);
            sum += sec_tap * constrain(s3 - px, sec_strength, sec_shift);
            min = umin(s0, min);
            max = imax(s0, max);
            min = umin(s1, min);
            max = imax(s1, max);
            min = umin(s2, min);
            max = imax(s2, max);
            min = umin(s3, min);
            max = imax(s3, max);
          }
          dst.at(x, y) = static_cast<Pixel>(iclip(px + ((sum - (sum < 0) + 8) >> 4), min, max));
        }
      }
    } else {  // pri_strength only
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          const int px = at(x, y);
          int sum = 0;
          int pri_tap_k = pri_tap;
          for (int k = 0; k < 2; k++) {
            const int off = dir_offset(2, k);  // dir
            const int p0 = tap(x, y, off);
            const int p1 = tap(x, y, -off);
            sum += pri_tap_k * constrain(p0 - px, pri_strength, pri_shift);
            sum += pri_tap_k * constrain(p1 - px, pri_strength, pri_shift);
            pri_tap_k = (pri_tap_k & 3) | 2;
          }
          dst.at(x, y) = static_cast<Pixel>(px + ((sum - (sum < 0) + 8) >> 4));
        }
      }
    }
  } else {  // sec_strength only
    const int sec_shift = damping - ulog2(static_cast<unsigned>(sec_strength));
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const int px = at(x, y);
        int sum = 0;
        for (int k = 0; k < 2; k++) {
          const int off1 = dir_offset(4, k);  // dir + 2
          const int off2 = dir_offset(0, k);  // dir - 2
          const int s0 = tap(x, y, off1);
          const int s1 = tap(x, y, -off1);
          const int s2 = tap(x, y, off2);
          const int s3 = tap(x, y, -off2);
          const int sec_tap = 2 - k;
          sum += sec_tap * constrain(s0 - px, sec_strength, sec_shift);
          sum += sec_tap * constrain(s1 - px, sec_strength, sec_shift);
          sum += sec_tap * constrain(s2 - px, sec_strength, sec_shift);
          sum += sec_tap * constrain(s3 - px, sec_strength, sec_shift);
        }
        dst.at(x, y) = static_cast<Pixel>(px + ((sum - (sum < 0) + 8) >> 4));
      }
    }
  }
}

int cdef_find_dir(PixelView img, unsigned& var, int bitdepth_max) {
  const int bitdepth_min_8 = 32 - __builtin_clz(static_cast<unsigned>(bitdepth_max)) - 8;
  int partial_sum_hv[2][8] = {{0}};
  int partial_sum_diag[2][15] = {{0}};
  int partial_sum_alt[4][11] = {{0}};

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      const int px = (img.at(x, y) >> bitdepth_min_8) - 128;

      partial_sum_diag[0][y + x] += px;
      partial_sum_alt[0][y + (x >> 1)] += px;
      partial_sum_hv[0][y] += px;
      partial_sum_alt[1][3 + y - (x >> 1)] += px;
      partial_sum_diag[1][7 + y - x] += px;
      partial_sum_alt[2][3 - (y >> 1) + x] += px;
      partial_sum_hv[1][x] += px;
      partial_sum_alt[3][(y >> 1) + x] += px;
    }
  }

  unsigned cost[8] = {0};
  for (int n = 0; n < 8; n++) {
    cost[2] += partial_sum_hv[0][n] * partial_sum_hv[0][n];
    cost[6] += partial_sum_hv[1][n] * partial_sum_hv[1][n];
  }
  cost[2] *= 105;
  cost[6] *= 105;

  static const uint16_t div_table[7] = {840, 420, 280, 210, 168, 140, 120};
  for (int n = 0; n < 7; n++) {
    const int d = div_table[n];
    cost[0] += (partial_sum_diag[0][n] * partial_sum_diag[0][n] + partial_sum_diag[0][14 - n] * partial_sum_diag[0][14 - n]) * d;
    cost[4] += (partial_sum_diag[1][n] * partial_sum_diag[1][n] + partial_sum_diag[1][14 - n] * partial_sum_diag[1][14 - n]) * d;
  }
  cost[0] += partial_sum_diag[0][7] * partial_sum_diag[0][7] * 105;
  cost[4] += partial_sum_diag[1][7] * partial_sum_diag[1][7] * 105;

  for (int n = 0; n < 4; n++) {
    unsigned& cost_ref = cost[n * 2 + 1];
    for (int m = 0; m < 5; m++) cost_ref += partial_sum_alt[n][3 + m] * partial_sum_alt[n][3 + m];
    cost_ref *= 105;
    for (int m = 0; m < 3; m++) {
      const int d = div_table[2 * m + 1];
      cost_ref += (partial_sum_alt[n][m] * partial_sum_alt[n][m] + partial_sum_alt[n][10 - m] * partial_sum_alt[n][10 - m]) * d;
    }
  }

  int best_dir = 0;
  unsigned best_cost = cost[0];
  for (int n = 1; n < 8; n++) {
    if (cost[n] > best_cost) {
      best_cost = cost[n];
      best_dir = n;
    }
  }

  var = (best_cost - (cost[best_dir ^ 4])) >> 10;
  return best_dir;
}

namespace {

int adjust_strength(int strength, unsigned var) {
  if (!var) return 0;
  const int i = var >> 6 ? imin(ulog2(var >> 6), 12) : 0;
  return (strength * (4 + i) + 8) >> 4;
}

}  // namespace

void cdef_frame(const LoopFilterMasks& masks, const SequenceHeader& seq, const FrameHeader& hdr, const PixelView src[3], const PixelView dst[3],
                int bitdepth) {
  if (!seq.cdef) return;
  const int bitdepth_max = (1 << bitdepth) - 1;
  const int bitdepth_min_8 = bitdepth - 8;
  const int bw = masks.bw4(), bh = masks.bh4();
  const PixelLayout layout = seq.layout;
  const int sb64w = masks.sb128w() << 1;
  const int damping = hdr.cdef.damping + bitdepth_min_8;
  const int ss_ver = layout == PIXEL_LAYOUT_I420;
  const int ss_hor = layout != PIXEL_LAYOUT_I444;
  const bool have_chroma = layout != PIXEL_LAYOUT_I400;
  static const uint8_t uv_dirs[2][8] = {{0, 1, 2, 3, 4, 5, 6, 7}, {7, 0, 2, 4, 5, 6, 6, 6}};
  const uint8_t* const uv_dir = uv_dirs[layout == PIXEL_LAYOUT_I422];
  const int uv_w = 8 >> ss_hor, uv_h = 8 >> ss_ver;
  constexpr int sbsz = 16;

  for (int by = 0; by < bh; by += 2) {
    const int by_idx = (by & 30) >> 1;
    for (int sbx = 0; sbx < sb64w; sbx++) {
      const int sb128x = sbx >> 1;
      const int sb64_idx = ((by & sbsz) >> 3) + (sbx & 1);
      const Av1Filter& lflvl = masks.filter(sb128x, by >> 5);
      const int cdef_idx = lflvl.cdef_idx[sb64_idx];
      if (cdef_idx < 0 || cdef_idx >= MAX_CDEF_STRENGTHS) continue;  // -1: every block of the 64x64 was skipped
      if (!hdr.cdef.y_strength[cdef_idx] && !hdr.cdef.uv_strength[cdef_idx]) continue;

      // Create a complete 32-bit mask for the sb row ahead of time.
      const uint16_t(&noskip_row)[2] = lflvl.noskip_mask[by_idx];
      const unsigned noskip_mask = static_cast<unsigned>(noskip_row[1]) << 16 | noskip_row[0];

      const int y_lvl = hdr.cdef.y_strength[cdef_idx];
      const int uv_lvl = hdr.cdef.uv_strength[cdef_idx];

      const int y_pri_lvl = (y_lvl >> 2) << bitdepth_min_8;
      int y_sec_lvl = y_lvl & 3;
      y_sec_lvl += y_sec_lvl == 3;
      y_sec_lvl <<= bitdepth_min_8;

      const int uv_pri_lvl = (uv_lvl >> 2) << bitdepth_min_8;
      int uv_sec_lvl = uv_lvl & 3;
      uv_sec_lvl += uv_sec_lvl == 3;
      uv_sec_lvl <<= bitdepth_min_8;

      for (int bx = sbx * sbsz; bx < imin((sbx + 1) * sbsz, bw); bx += 2) {
        // check if this 8x8 block had any coded coefficients; if not, go to the next block
        const uint32_t bx_mask = 3U << (bx & 30);
        if (!(noskip_mask & bx_mask)) continue;
        int edges = 0;
        if (by > 0) edges |= CDEF_HAVE_TOP;
        if (by + 2 < bh) edges |= CDEF_HAVE_BOTTOM;
        if (bx > 0) edges |= CDEF_HAVE_LEFT;
        if (bx + 2 < bw) edges |= CDEF_HAVE_RIGHT;

        int dir = 0;
        unsigned variance = 0;
        const int x = bx * 4, y = by * 4;
        if (y_pri_lvl || uv_pri_lvl) dir = cdef_find_dir(src[0].offset(x, y), variance, bitdepth_max);

        if (y_pri_lvl) {
          const int adj_y_pri_lvl = adjust_strength(y_pri_lvl, variance);
          if (adj_y_pri_lvl || y_sec_lvl)
            cdef_filter_block(dst[0].offset(x, y), src[0].offset(x, y), 8, 8, adj_y_pri_lvl, y_sec_lvl, dir, damping, edges, bitdepth_max);
        } else if (y_sec_lvl) {
          cdef_filter_block(dst[0].offset(x, y), src[0].offset(x, y), 8, 8, 0, y_sec_lvl, 0, damping, edges, bitdepth_max);
        }

        if (!uv_lvl || !have_chroma) continue;

        const int uvdir = uv_pri_lvl ? uv_dir[dir] : 0;
        const int cx = x >> ss_hor, cy = y >> ss_ver;
        for (int pl = 1; pl <= 2; pl++)
          cdef_filter_block(dst[pl].offset(cx, cy), src[pl].offset(cx, cy), uv_w, uv_h, uv_pri_lvl, uv_sec_lvl, uvdir, damping - 1, edges,
                            bitdepth_max);
      }
    }
  }
}

}  // namespace av1
}  // namespace hardware
