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


#include "Hardware/VideoCodec/Software/Av1LoopFilter.hpp"

// Port of dav1d's lf_mask.c, loopfilter_tmpl.c and lf_apply_tmpl.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors), 16-bit
// samples. The arithmetic is dav1d's, step for step; the superblock-row pipelining and the tile edge fix-ups are gone (see the header).

namespace hardware {
namespace av1 {

namespace {

#include "Hardware/VideoCodec/Software/Av1Tables.inc"

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iclip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int abs_int(int v) { return v < 0 ? -v : v; }

// Checks that a block derived index is inside an array of N entries (the mask arrays are indexed relative to the superblock).
template <int N>
inline int chk(int index) {
  if (static_cast<unsigned>(index) >= static_cast<unsigned>(N)) bounds_violation("av1 loop filter mask index", static_cast<uint64_t>(static_cast<int64_t>(index)), N);
  return index;
}

// dav1d's dav1d_txfm_dimensions, the columns the loop filter needs: size in 4x4 units, log2 of it, and the next smaller transform.
struct TxInfo {
  uint8_t w, h, lw, lh, sub;
};
constexpr TxInfo k_txfm_dimensions[N_RECT_TX_SIZES] = {
    {1, 1, 0, 0, TX_4X4},    // TX_4X4
    {2, 2, 1, 1, TX_4X4},    // TX_8X8
    {4, 4, 2, 2, TX_8X8},    // TX_16X16
    {8, 8, 3, 3, TX_16X16},  // TX_32X32
    {16, 16, 4, 4, TX_32X32},  // TX_64X64
    {1, 2, 0, 1, TX_4X4},    // RTX_4X8
    {2, 1, 1, 0, TX_4X4},    // RTX_8X4
    {2, 4, 1, 2, TX_8X8},    // RTX_8X16
    {4, 2, 2, 1, TX_8X8},    // RTX_16X8
    {4, 8, 2, 3, TX_16X16},  // RTX_16X32
    {8, 4, 3, 2, TX_16X16},  // RTX_32X16
    {8, 16, 3, 4, TX_32X32},  // RTX_32X64
    {16, 8, 4, 3, TX_32X32},  // RTX_64X32
    {1, 4, 0, 2, RTX_4X8},   // RTX_4X16
    {4, 1, 2, 0, RTX_8X4},   // RTX_16X4
    {2, 8, 1, 3, RTX_8X16},  // RTX_8X32
    {8, 2, 3, 1, RTX_16X8},  // RTX_32X8
    {4, 16, 2, 4, RTX_16X32},  // RTX_16X64
    {16, 4, 4, 2, RTX_32X16},  // RTX_64X16
};

// ---- the edge filter (loopfilter_tmpl.c) ---------------------------------------------------------------------------------------------

// Filters 4 positions along an edge. Position i, tap k (negative = before the edge) is at (x + i * along_x + k * across_x,
// y + i * along_y + k * across_y). wd is the filter size: 4, 6, 8 or 16.
void loop_filter(PixelView dst, int x, int y, int along_x, int along_y, int across_x, int across_y, int E, int I, int H, int wd,
                 int bitdepth_max) {
  const int bitdepth_min_8 = 32 - __builtin_clz(static_cast<unsigned>(bitdepth_max)) - 8;  // bitdepth_from_max(bitdepth_max) - 8
  const int F = 1 << bitdepth_min_8;
  E <<= bitdepth_min_8;
  I <<= bitdepth_min_8;
  H <<= bitdepth_min_8;

  for (int i = 0; i < 4; i++) {
    const int bx = x + i * along_x, by = y + i * along_y;
    auto px = [&](int k) -> Pixel& { return dst.at(bx + k * across_x, by + k * across_y); };
    int p6 = 0, p5 = 0, p4 = 0, p3 = 0, p2 = 0;
    int p1 = px(-2), p0 = px(-1);
    int q0 = px(0), q1 = px(1);
    int q2 = 0, q3 = 0, q4 = 0, q5 = 0, q6 = 0;
    int fm, flat8out = 0, flat8in = 0;

    fm = abs_int(p1 - p0) <= I && abs_int(q1 - q0) <= I && abs_int(p0 - q0) * 2 + (abs_int(p1 - q1) >> 1) <= E;

    if (wd > 4) {
      p2 = px(-3);
      q2 = px(2);

      fm &= abs_int(p2 - p1) <= I && abs_int(q2 - q1) <= I;

      if (wd > 6) {
        p3 = px(-4);
        q3 = px(3);

        fm &= abs_int(p3 - p2) <= I && abs_int(q3 - q2) <= I;
      }
    }
    if (!fm) continue;

    if (wd >= 16) {
      p6 = px(-7);
      p5 = px(-6);
      p4 = px(-5);
      q4 = px(4);
      q5 = px(5);
      q6 = px(6);

      flat8out = abs_int(p6 - p0) <= F && abs_int(p5 - p0) <= F && abs_int(p4 - p0) <= F && abs_int(q4 - q0) <= F &&
                 abs_int(q5 - q0) <= F && abs_int(q6 - q0) <= F;
    }

    if (wd >= 6)
      flat8in = abs_int(p2 - p0) <= F && abs_int(p1 - p0) <= F && abs_int(q1 - q0) <= F && abs_int(q2 - q0) <= F;

    if (wd >= 8) flat8in &= abs_int(p3 - p0) <= F && abs_int(q3 - q0) <= F;

    if (wd >= 16 && (flat8out & flat8in)) {
      px(-6) = static_cast<Pixel>((p6 + p6 + p6 + p6 + p6 + p6 * 2 + p5 * 2 + p4 * 2 + p3 + p2 + p1 + p0 + q0 + 8) >> 4);
      px(-5) = static_cast<Pixel>((p6 + p6 + p6 + p6 + p6 + p5 * 2 + p4 * 2 + p3 * 2 + p2 + p1 + p0 + q0 + q1 + 8) >> 4);
      px(-4) = static_cast<Pixel>((p6 + p6 + p6 + p6 + p5 + p4 * 2 + p3 * 2 + p2 * 2 + p1 + p0 + q0 + q1 + q2 + 8) >> 4);
      px(-3) = static_cast<Pixel>((p6 + p6 + p6 + p5 + p4 + p3 * 2 + p2 * 2 + p1 * 2 + p0 + q0 + q1 + q2 + q3 + 8) >> 4);
      px(-2) = static_cast<Pixel>((p6 + p6 + p5 + p4 + p3 + p2 * 2 + p1 * 2 + p0 * 2 + q0 + q1 + q2 + q3 + q4 + 8) >> 4);
      px(-1) = static_cast<Pixel>((p6 + p5 + p4 + p3 + p2 + p1 * 2 + p0 * 2 + q0 * 2 + q1 + q2 + q3 + q4 + q5 + 8) >> 4);
      px(0) = static_cast<Pixel>((p5 + p4 + p3 + p2 + p1 + p0 * 2 + q0 * 2 + q1 * 2 + q2 + q3 + q4 + q5 + q6 + 8) >> 4);
      px(1) = static_cast<Pixel>((p4 + p3 + p2 + p1 + p0 + q0 * 2 + q1 * 2 + q2 * 2 + q3 + q4 + q5 + q6 + q6 + 8) >> 4);
      px(2) = static_cast<Pixel>((p3 + p2 + p1 + p0 + q0 + q1 * 2 + q2 * 2 + q3 * 2 + q4 + q5 + q6 + q6 + q6 + 8) >> 4);
      px(3) = static_cast<Pixel>((p2 + p1 + p0 + q0 + q1 + q2 * 2 + q3 * 2 + q4 * 2 + q5 + q6 + q6 + q6 + q6 + 8) >> 4);
      px(4) = static_cast<Pixel>((p1 + p0 + q0 + q1 + q2 + q3 * 2 + q4 * 2 + q5 * 2 + q6 + q6 + q6 + q6 + q6 + 8) >> 4);
      px(5) = static_cast<Pixel>((p0 + q0 + q1 + q2 + q3 + q4 * 2 + q5 * 2 + q6 * 2 + q6 + q6 + q6 + q6 + q6 + 8) >> 4);
    } else if (wd >= 8 && flat8in) {
      px(-3) = static_cast<Pixel>((p3 + p3 + p3 + 2 * p2 + p1 + p0 + q0 + 4) >> 3);
      px(-2) = static_cast<Pixel>((p3 + p3 + p2 + 2 * p1 + p0 + q0 + q1 + 4) >> 3);
      px(-1) = static_cast<Pixel>((p3 + p2 + p1 + 2 * p0 + q0 + q1 + q2 + 4) >> 3);
      px(0) = static_cast<Pixel>((p2 + p1 + p0 + 2 * q0 + q1 + q2 + q3 + 4) >> 3);
      px(1) = static_cast<Pixel>((p1 + p0 + q0 + 2 * q1 + q2 + q3 + q3 + 4) >> 3);
      px(2) = static_cast<Pixel>((p0 + q0 + q1 + 2 * q2 + q3 + q3 + q3 + 4) >> 3);
    } else if (wd == 6 && flat8in) {
      px(-2) = static_cast<Pixel>((p2 + 2 * p2 + 2 * p1 + 2 * p0 + q0 + 4) >> 3);
      px(-1) = static_cast<Pixel>((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
      px(0) = static_cast<Pixel>((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
      px(1) = static_cast<Pixel>((p0 + 2 * q0 + 2 * q1 + 2 * q2 + q2 + 4) >> 3);
    } else {
      const int hev = abs_int(p1 - p0) > H || abs_int(q1 - q0) > H;
      const int lo = -128 * (1 << bitdepth_min_8), hi = 128 * (1 << bitdepth_min_8) - 1;

      if (hev) {
        int f = iclip(p1 - q1, lo, hi), f1, f2;
        f = iclip(3 * (q0 - p0) + f, lo, hi);

        f1 = imin(f + 4, (128 << bitdepth_min_8) - 1) >> 3;
        f2 = imin(f + 3, (128 << bitdepth_min_8) - 1) >> 3;

        px(-1) = static_cast<Pixel>(iclip(p0 + f2, 0, bitdepth_max));
        px(0) = static_cast<Pixel>(iclip(q0 - f1, 0, bitdepth_max));
      } else {
        int f = iclip(3 * (q0 - p0), lo, hi);

        const int f1 = imin(f + 4, (128 << bitdepth_min_8) - 1) >> 3;
        const int f2 = imin(f + 3, (128 << bitdepth_min_8) - 1) >> 3;

        px(-1) = static_cast<Pixel>(iclip(p0 + f2, 0, bitdepth_max));
        px(0) = static_cast<Pixel>(iclip(q0 - f1, 0, bitdepth_max));

        f = (f1 + 1) >> 1;
        px(-2) = static_cast<Pixel>(iclip(p1 + f, 0, bitdepth_max));
        px(1) = static_cast<Pixel>(iclip(q1 - f, 0, bitdepth_max));
      }
    }
  }
}

}  // namespace

void loop_filter_h_sb128y(PixelView dst, const uint32_t vmask[3], const LevelView& l, const FilterLut& lut, int bitdepth_max) {
  const unsigned vm = vmask[0] | vmask[1] | vmask[2];
  for (int n = 0; n < 32; n++) {
    const unsigned y = 1u << n;
    if (!(vm & ~(y - 1))) break;
    if (vm & y) {
      const int L = l.at(0, n) ? l.at(0, n) : l.at(-1, n);
      if (!L) continue;
      const int H = L >> 4;
      const int E = lut.e[L], I = lut.i[L];
      const int idx = (vmask[2] & y) ? 2 : !!(vmask[1] & y);
      loop_filter(dst, 0, 4 * n, 0, 1, 1, 0, E, I, H, 4 << idx, bitdepth_max);
    }
  }
}

void loop_filter_v_sb128y(PixelView dst, const uint32_t vmask[3], const LevelView& l, const FilterLut& lut, int bitdepth_max) {
  const unsigned vm = vmask[0] | vmask[1] | vmask[2];
  for (int n = 0; n < 32; n++) {
    const unsigned x = 1u << n;
    if (!(vm & ~(x - 1))) break;
    if (vm & x) {
      const int L = l.at(n, 0) ? l.at(n, 0) : l.at(n, -1);
      if (!L) continue;
      const int H = L >> 4;
      const int E = lut.e[L], I = lut.i[L];
      const int idx = (vmask[2] & x) ? 2 : !!(vmask[1] & x);
      loop_filter(dst, 4 * n, 0, 1, 0, 0, 1, E, I, H, 4 << idx, bitdepth_max);
    }
  }
}

void loop_filter_h_sb128uv(PixelView dst, const uint32_t vmask[2], const LevelView& l, const FilterLut& lut, int bitdepth_max) {
  const unsigned vm = vmask[0] | vmask[1];
  for (int n = 0; n < 32; n++) {
    const unsigned y = 1u << n;
    if (!(vm & ~(y - 1))) break;
    if (vm & y) {
      const int L = l.at(0, n) ? l.at(0, n) : l.at(-1, n);
      if (!L) continue;
      const int H = L >> 4;
      const int E = lut.e[L], I = lut.i[L];
      const int idx = !!(vmask[1] & y);
      loop_filter(dst, 0, 4 * n, 0, 1, 1, 0, E, I, H, 4 + 2 * idx, bitdepth_max);
    }
  }
}

void loop_filter_v_sb128uv(PixelView dst, const uint32_t vmask[2], const LevelView& l, const FilterLut& lut, int bitdepth_max) {
  const unsigned vm = vmask[0] | vmask[1];
  for (int n = 0; n < 32; n++) {
    const unsigned x = 1u << n;
    if (!(vm & ~(x - 1))) break;
    if (vm & x) {
      const int L = l.at(n, 0) ? l.at(n, 0) : l.at(n, -1);
      if (!L) continue;
      const int H = L >> 4;
      const int E = lut.e[L], I = lut.i[L];
      const int idx = !!(vmask[1] & x);
      loop_filter(dst, 4 * n, 0, 1, 0, 0, 1, E, I, H, 4 + 2 * idx, bitdepth_max);
    }
  }
}

// ---- level tables (lf_mask.c) --------------------------------------------------------------------------------------------------------

void calc_eih(FilterLut& lut, int filter_sharpness) {
  // set E/I/H values from loopfilter level
  const int sharp = filter_sharpness;
  for (int level = 0; level < 64; level++) {
    int limit = level;

    if (sharp > 0) {
      limit >>= (sharp + 3) >> 2;
      limit = imin(limit, 9 - sharp);
    }
    limit = imax(limit, 1);

    lut.i[level] = static_cast<uint8_t>(limit);
    lut.e[level] = static_cast<uint8_t>(2 * (level + 2) + limit);
  }
}

namespace {

void calc_lf_value(uint8_t (&lflvl_values)[8][2], int base_lvl, int lf_delta, int seg_delta, const LoopfilterModeRefDeltas* mr_delta) {
  const int base = iclip(iclip(base_lvl + lf_delta, 0, 63) + seg_delta, 0, 63);

  if (!mr_delta) {
    for (auto& entry : lflvl_values) entry[0] = entry[1] = static_cast<uint8_t>(base);
  } else {
    const int sh = base >= 32;
    lflvl_values[0][0] = lflvl_values[0][1] = static_cast<uint8_t>(iclip(base + (mr_delta->ref_delta[0] * (1 << sh)), 0, 63));
    for (int r = 1; r < 8; r++) {
      for (int m = 0; m < 2; m++) {
        const int delta = mr_delta->mode_delta[m] + mr_delta->ref_delta[r];
        lflvl_values[r][m] = static_cast<uint8_t>(iclip(base + (delta * (1 << sh)), 0, 63));
      }
    }
  }
}

void calc_lf_value_chroma(uint8_t (&lflvl_values)[8][2], int base_lvl, int lf_delta, int seg_delta, const LoopfilterModeRefDeltas* mr_delta) {
  if (!base_lvl) {
    for (auto& entry : lflvl_values) entry[0] = entry[1] = 0;
  } else {
    calc_lf_value(lflvl_values, base_lvl, lf_delta, seg_delta, mr_delta);
  }
}

}  // namespace

void calc_lf_values(LfValues& values, const FrameHeader& hdr, const int8_t lf_delta[4]) {
  const int n_seg = hdr.segmentation.enabled ? 8 : 1;

  if (!hdr.loopfilter.level_y[0] && !hdr.loopfilter.level_y[1]) {
    for (int s = 0; s < n_seg; s++)
      for (auto& component : values.level[s])
        for (auto& entry : component) entry[0] = entry[1] = 0;
    return;
  }

  const LoopfilterModeRefDeltas* const mr_deltas = hdr.loopfilter.mode_ref_delta_enabled ? &hdr.loopfilter.mode_ref_deltas : nullptr;
  const int multi = hdr.delta.lf.multi ? 1 : 0;
  for (int s = 0; s < n_seg; s++) {
    const SegmentationData* const segd = hdr.segmentation.enabled ? &hdr.segmentation.seg_data.d[s] : nullptr;

    calc_lf_value(values.level[s][0], hdr.loopfilter.level_y[0], lf_delta[0], segd ? segd->delta_lf_y_v : 0, mr_deltas);
    calc_lf_value(values.level[s][1], hdr.loopfilter.level_y[1], lf_delta[multi ? 1 : 0], segd ? segd->delta_lf_y_h : 0, mr_deltas);
    calc_lf_value_chroma(values.level[s][2], hdr.loopfilter.level_u, lf_delta[multi ? 2 : 0], segd ? segd->delta_lf_u : 0, mr_deltas);
    calc_lf_value_chroma(values.level[s][3], hdr.loopfilter.level_v, lf_delta[multi ? 3 : 0], segd ? segd->delta_lf_v : 0, mr_deltas);
  }
}

// ---- edge masks of one block (lf_mask.c) -------------------------------------------------------------------------------------------

namespace {

using YMasks = uint16_t[2][32][3][2];
using UvMasks = uint16_t[2][32][2][2];

// The transform size (log2, capped at 2) and step of every 4x4 of the block, for both edge directions.
struct TxGrid {
  uint8_t v[2 /* edge: 0 = left edges, 1 = top edges */][2 /* txsz, step */][32 /* y */][32 /* x */];
};

void decomp_tx(TxGrid& txa, int base_y, int base_x, RectTxfmSize from, int depth, int y_off, int x_off, const uint16_t tx_masks[2]) {
  const TxInfo& t_dim = k_txfm_dimensions[from];
  const int is_split = (from == static_cast<RectTxfmSize>(TX_4X4) || depth > 1) ? 0 : (tx_masks[depth] >> (y_off * 4 + x_off)) & 1;

  if (is_split) {
    const RectTxfmSize sub = static_cast<RectTxfmSize>(t_dim.sub);
    const int htw4 = t_dim.w >> 1, hth4 = t_dim.h >> 1;

    decomp_tx(txa, base_y, base_x, sub, depth + 1, y_off * 2 + 0, x_off * 2 + 0, tx_masks);
    if (t_dim.w >= t_dim.h) decomp_tx(txa, base_y, base_x + htw4, sub, depth + 1, y_off * 2 + 0, x_off * 2 + 1, tx_masks);
    if (t_dim.h >= t_dim.w) {
      decomp_tx(txa, base_y + hth4, base_x, sub, depth + 1, y_off * 2 + 1, x_off * 2 + 0, tx_masks);
      if (t_dim.w >= t_dim.h) decomp_tx(txa, base_y + hth4, base_x + htw4, sub, depth + 1, y_off * 2 + 1, x_off * 2 + 1, tx_masks);
    }
  } else {
    const int lw = imin(2, t_dim.lw), lh = imin(2, t_dim.lh);
    for (int y = 0; y < t_dim.h; y++) {
      const int row = chk<32>(base_y + y);
      for (int x = 0; x < t_dim.w; x++) {
        const int col = chk<32>(base_x + x);
        txa.v[0][0][row][col] = static_cast<uint8_t>(lw);
        txa.v[1][0][row][col] = static_cast<uint8_t>(lh);
      }
      txa.v[0][1][row][chk<32>(base_x)] = t_dim.w;
    }
    for (int x = 0; x < t_dim.w; x++) txa.v[1][1][chk<32>(base_y)][chk<32>(base_x + x)] = t_dim.h;
  }
}

void mask_edges_inter(YMasks& masks, int by4, int bx4, int w4, int h4, int skip, RectTxfmSize max_tx, const uint16_t tx_masks[2],
                      ArrayView<uint8_t> a, ArrayView<uint8_t> l) {
  const TxInfo& t_dim = k_txfm_dimensions[max_tx];
  int y, x;

  TxGrid txa = {};
  for (int y_off = 0, yy = 0; yy < h4; yy += t_dim.h, y_off++)
    for (int x_off = 0, xx = 0; xx < w4; xx += t_dim.w, x_off++) decomp_tx(txa, yy, xx, max_tx, 0, y_off, x_off, tx_masks);

  // left block edge
  unsigned mask = 1U << by4;
  for (y = 0; y < h4; y++, mask <<= 1) {
    const int sidx = mask >= 0x10000;
    const unsigned smask = mask >> (sidx << 4);
    masks[0][chk<32>(bx4)][chk<3>(imin(txa.v[0][0][chk<32>(y)][0], l[y]))][sidx] |= static_cast<uint16_t>(smask);
  }

  // top block edge
  for (x = 0, mask = 1U << bx4; x < w4; x++, mask <<= 1) {
    const int sidx = mask >= 0x10000;
    const unsigned smask = mask >> (sidx << 4);
    masks[1][chk<32>(by4)][chk<3>(imin(txa.v[1][0][0][chk<32>(x)], a[x]))][sidx] |= static_cast<uint16_t>(smask);
  }

  if (!skip) {
    // inner (tx) left|right edges
    for (y = 0, mask = 1U << by4; y < h4; y++, mask <<= 1) {
      const int sidx = mask >= 0x10000U;
      const unsigned smask = mask >> (sidx << 4);
      int ltx = txa.v[0][0][chk<32>(y)][0];
      int step = txa.v[0][1][y][0];
      for (x = step; x < w4; x += step) {
        const int rtx = txa.v[0][0][y][chk<32>(x)];
        masks[0][chk<32>(bx4 + x)][chk<3>(imin(rtx, ltx))][sidx] |= static_cast<uint16_t>(smask);
        ltx = rtx;
        step = txa.v[0][1][y][x];
        if (step == 0) break;  // cannot happen for a decomposed block; keeps a broken grid from looping forever
      }
    }

    //            top
    // inner (tx) --- edges
    //           bottom
    for (x = 0, mask = 1U << bx4; x < w4; x++, mask <<= 1) {
      const int sidx = mask >= 0x10000U;
      const unsigned smask = mask >> (sidx << 4);
      int ttx = txa.v[1][0][0][chk<32>(x)];
      int step = txa.v[1][1][0][x];
      for (y = step; y < h4; y += step) {
        const int btx = txa.v[1][0][chk<32>(y)][x];
        masks[1][chk<32>(by4 + y)][chk<3>(imin(ttx, btx))][sidx] |= static_cast<uint16_t>(smask);
        ttx = btx;
        step = txa.v[1][1][y][x];
        if (step == 0) break;
      }
    }
  }

  for (y = 0; y < h4; y++) l[y] = txa.v[0][0][chk<32>(y)][chk<32>(w4 - 1)];
  for (x = 0; x < w4; x++) a[x] = txa.v[1][0][chk<32>(h4 - 1)][chk<32>(x)];
}

void mask_edges_intra(YMasks& masks, int by4, int bx4, int w4, int h4, RectTxfmSize tx, ArrayView<uint8_t> a, ArrayView<uint8_t> l) {
  const TxInfo& t_dim = k_txfm_dimensions[tx];
  const int twl4 = t_dim.lw, thl4 = t_dim.lh;
  const int twl4c = imin(2, twl4), thl4c = imin(2, thl4);
  int y, x;

  // left block edge
  unsigned mask = 1U << by4;
  for (y = 0; y < h4; y++, mask <<= 1) {
    const int sidx = mask >= 0x10000;
    const unsigned smask = mask >> (sidx << 4);
    masks[0][chk<32>(bx4)][chk<3>(imin(twl4c, l[y]))][sidx] |= static_cast<uint16_t>(smask);
  }

  // top block edge
  for (x = 0, mask = 1U << bx4; x < w4; x++, mask <<= 1) {
    const int sidx = mask >= 0x10000;
    const unsigned smask = mask >> (sidx << 4);
    masks[1][chk<32>(by4)][chk<3>(imin(thl4c, a[x]))][sidx] |= static_cast<uint16_t>(smask);
  }

  // inner (tx) left|right edges
  const int hstep = t_dim.w;
  unsigned t = 1U << by4;
  unsigned inner = static_cast<unsigned>((static_cast<uint64_t>(t) << h4) - t);
  unsigned inner1 = inner & 0xffff, inner2 = inner >> 16;
  for (x = hstep; x < w4; x += hstep) {
    if (inner1) masks[0][chk<32>(bx4 + x)][twl4c][0] |= static_cast<uint16_t>(inner1);
    if (inner2) masks[0][chk<32>(bx4 + x)][twl4c][1] |= static_cast<uint16_t>(inner2);
  }

  //            top
  // inner (tx) --- edges
  //           bottom
  const int vstep = t_dim.h;
  t = 1U << bx4;
  inner = static_cast<unsigned>((static_cast<uint64_t>(t) << w4) - t);
  inner1 = inner & 0xffff;
  inner2 = inner >> 16;
  for (y = vstep; y < h4; y += vstep) {
    if (inner1) masks[1][chk<32>(by4 + y)][thl4c][0] |= static_cast<uint16_t>(inner1);
    if (inner2) masks[1][chk<32>(by4 + y)][thl4c][1] |= static_cast<uint16_t>(inner2);
  }

  a.first(static_cast<uint64_t>(w4)).fill(static_cast<uint8_t>(thl4c));
  l.first(static_cast<uint64_t>(h4)).fill(static_cast<uint8_t>(twl4c));
}

void mask_edges_chroma(UvMasks& masks, int cby4, int cbx4, int cw4, int ch4, int skip_inter, RectTxfmSize tx, ArrayView<uint8_t> a,
                       ArrayView<uint8_t> l, int ss_hor, int ss_ver) {
  const TxInfo& t_dim = k_txfm_dimensions[tx];
  const int twl4 = t_dim.lw, thl4 = t_dim.lh;
  const int twl4c = !!twl4, thl4c = !!thl4;
  int y, x;
  const int vbits = 4 - ss_ver, hbits = 4 - ss_hor;
  const int vmask = 16 >> ss_ver, hmask = 16 >> ss_hor;
  const unsigned vmax = 1u << vmask, hmax = 1u << hmask;

  // left block edge
  unsigned mask = 1U << cby4;
  for (y = 0; y < ch4; y++, mask <<= 1) {
    const int sidx = mask >= vmax;
    const unsigned smask = mask >> (sidx << vbits);
    masks[0][chk<32>(cbx4)][chk<2>(imin(twl4c, l[y]))][sidx] |= static_cast<uint16_t>(smask);
  }

  // top block edge
  for (x = 0, mask = 1U << cbx4; x < cw4; x++, mask <<= 1) {
    const int sidx = mask >= hmax;
    const unsigned smask = mask >> (sidx << hbits);
    masks[1][chk<32>(cby4)][chk<2>(imin(thl4c, a[x]))][sidx] |= static_cast<uint16_t>(smask);
  }

  if (!skip_inter) {
    // inner (tx) left|right edges
    const int hstep = t_dim.w;
    unsigned t = 1U << cby4;
    unsigned inner = static_cast<unsigned>((static_cast<uint64_t>(t) << ch4) - t);
    unsigned inner1 = inner & ((1u << vmask) - 1), inner2 = inner >> vmask;
    for (x = hstep; x < cw4; x += hstep) {
      if (inner1) masks[0][chk<32>(cbx4 + x)][twl4c][0] |= static_cast<uint16_t>(inner1);
      if (inner2) masks[0][chk<32>(cbx4 + x)][twl4c][1] |= static_cast<uint16_t>(inner2);
    }

    //            top
    // inner (tx) --- edges
    //           bottom
    const int vstep = t_dim.h;
    t = 1U << cbx4;
    inner = static_cast<unsigned>((static_cast<uint64_t>(t) << cw4) - t);
    inner1 = inner & ((1u << hmask) - 1);
    inner2 = inner >> hmask;
    for (y = vstep; y < ch4; y += vstep) {
      if (inner1) masks[1][chk<32>(cby4 + y)][thl4c][0] |= static_cast<uint16_t>(inner1);
      if (inner2) masks[1][chk<32>(cby4 + y)][thl4c][1] |= static_cast<uint16_t>(inner2);
    }
  }

  a.first(static_cast<uint64_t>(cw4)).fill(static_cast<uint8_t>(thl4c));
  l.first(static_cast<uint64_t>(ch4)).fill(static_cast<uint8_t>(twl4c));
}

}  // namespace

// ---- LoopFilterMasks ---------------------------------------------------------------------------------------------------------------

namespace {

// Gives `view` exactly `count` elements, reusing the allocation if it already has that size. False if out of memory.
template <class T>
bool resize_view(allocators::IAllocator& allocator, ArrayView<T>& view, uint64_t count) {
  if (view.size() == count) return true;
  release_view(allocator, view);
  view = allocate_view<T>(allocator, count);
  return !view.empty();
}

}  // namespace

void LoopFilterMasks::release() {
  release_view(allocator_, filters_);
  release_view(allocator_, level_);
  release_view(allocator_, above_y_);
  release_view(allocator_, above_uv_);
  release_view(allocator_, left_y_);
  release_view(allocator_, left_uv_);
}

bool LoopFilterMasks::init(const SequenceHeader& seq, const FrameHeader& hdr) {
  if (hdr.width[0] <= 0 || hdr.height <= 0 || hdr.width[0] > 65536 || hdr.height > 65536) return false;
  hdr_ = hdr;
  sb128_ = seq.sb128 != 0;
  layout_ = seq.layout;
  ss_ver_ = layout_ == PIXEL_LAYOUT_I420;
  ss_hor_ = layout_ != PIXEL_LAYOUT_I444;
  w4_ = (hdr.width[0] + 3) >> 2;
  h4_ = (hdr.height + 3) >> 2;
  bw4_ = ((hdr.width[0] + 7) >> 3) << 1;
  bh4_ = ((hdr.height + 7) >> 3) << 1;
  sb128w_ = (bw4_ + 31) >> 5;
  sb128h_ = (bh4_ + 31) >> 5;
  level_stride_ = static_cast<int64_t>(sb128w_) * 32;
  const uint64_t columns = static_cast<uint64_t>(sb128w_) * 32, rows = static_cast<uint64_t>(sb128h_) * 32;

  if (!resize_view(allocator_, filters_, static_cast<uint64_t>(sb128w_) * static_cast<uint64_t>(sb128h_)) ||
      !resize_view(allocator_, level_, columns * rows * 4) || !resize_view(allocator_, above_y_, columns) ||
      !resize_view(allocator_, above_uv_, columns) || !resize_view(allocator_, left_y_, rows) || !resize_view(allocator_, left_uv_, rows)) {
    release();
    return false;
  }
  Av1Filter blank = {};
  for (auto& idx : blank.cdef_idx) idx = -1;
  filters_.fill(blank);
  level_.fill(0);
  // reset_context(): above and left transform size contexts start at the largest class (luma) / the 8x8 class (chroma)
  above_y_.fill(2);
  left_y_.fill(2);
  above_uv_.fill(1);
  left_uv_.fill(1);

  enabled_ = hdr.loopfilter.level_y[0] || hdr.loopfilter.level_y[1];
  chroma_filtered_ = (hdr.loopfilter.level_u || hdr.loopfilter.level_v) && layout_ != PIXEL_LAYOUT_I400;
  for (int s = 0; s < MAX_SEGMENTS; s++) lossless_[s] = hdr.segmentation.lossless[s] != 0;
  calc_eih(lut_, hdr.loopfilter.sharpness);
  const int8_t zero[4] = {0, 0, 0, 0};
  frame_lvl_ = {};
  delta_lvl_ = {};
  calc_lf_values(frame_lvl_, hdr, zero);
  lvl_ = &frame_lvl_;
  for (int8_t& d : delta_lf_) d = 0;
  return true;
}

void LoopFilterMasks::set_delta_lf(const int8_t delta_lf[4]) {
  bool changed = false, nonzero = false;
  for (int i = 0; i < 4; i++) {
    changed |= delta_lf_[i] != delta_lf[i];
    nonzero |= delta_lf[i] != 0;
    delta_lf_[i] = delta_lf[i];
  }
  if (!nonzero) {
    lvl_ = &frame_lvl_;
  } else {
    if (changed || lvl_ != &delta_lvl_) calc_lf_values(delta_lvl_, hdr_, delta_lf_);
    lvl_ = &delta_lvl_;
  }
}

bool LoopFilterMasks::cdef_idx_unset(int bx, int by) const {
  const Av1Filter& f = filters_[static_cast<uint64_t>(chk<65536>(by) >> 5) * static_cast<uint64_t>(sb128w_) + static_cast<uint64_t>(chk<65536>(bx) >> 5)];
  return f.cdef_idx[((bx & 16) >> 4) + ((by & 16) >> 3)] == -1;
}

void LoopFilterMasks::set_cdef_idx(int bx, int by, BlockSize bs, int idx) {
  Av1Filter& f = filter_at(bx, by);
  const int bw4 = k_block_dimensions[bs][0], bh4 = k_block_dimensions[bs][1];
  const int slot = ((bx & 16) >> 4) + ((by & 16) >> 3);
  const int8_t v = static_cast<int8_t>(idx);
  f.cdef_idx[slot] = v;
  if (bw4 > 16) f.cdef_idx[chk<4>(slot + 1)] = v;
  if (bh4 > 16) f.cdef_idx[chk<4>(slot + 2)] = v;
  if (bw4 == 32 && bh4 == 32) f.cdef_idx[chk<4>(slot + 3)] = v;
}

void LoopFilterMasks::mark_coded(int bx, int by, BlockSize bs) {
  Av1Filter& f = filter_at(bx, by);
  const int bw4 = k_block_dimensions[bs][0], bh4 = k_block_dimensions[bs][1];
  const int bx4 = bx & 31, by4 = by & 31;
  const unsigned mask = (~0U >> (32 - bw4)) << (bx4 & 15);
  const int bx_idx = (bx4 & 16) >> 4;
  for (int y = 0; y < bh4; y += 2) {
    uint16_t(&entry)[2] = f.noskip_mask[chk<16>((by4 >> 1) + (y >> 1))];
    entry[bx_idx] |= static_cast<uint16_t>(mask);
    if (bw4 == 32) entry[1] |= static_cast<uint16_t>(mask);  // this should be mask >> 16, but it's 0xffffffff anyway
  }
}

namespace {

// dav1d's fill_levelcache_block: the two levels of a luma block (components 0, 1) or of a chroma block (components 2, 3).
void fill_levelcache_block(ArrayView<uint8_t> level, int64_t stride, int x0, int y0, int bw, int bh, int component, unsigned v0, unsigned v1) {
  for (int y = 0; y < bh; y++) {
    for (int x = 0; x < bw; x++) {
      const uint64_t base = static_cast<uint64_t>((static_cast<int64_t>(y0 + y) * stride + x0 + x) * 4 + component);
      level[base] = static_cast<uint8_t>(v0);
      level[base + 1] = static_cast<uint8_t>(v1);
    }
  }
}

}  // namespace

void LoopFilterMasks::create_lf_mask_intra(int bx, int by, BlockSize bs, RectTxfmSize ytx, RectTxfmSize uvtx, int seg_id, bool has_chroma) {
  if (!enabled_) return;
  Av1Filter& lflvl = filter_at(bx, by);
  const uint8_t* const b_dim = k_block_dimensions[bs];
  const int bw4 = imin(w4_ - bx, b_dim[0]);
  const int bh4 = imin(h4_ - by, b_dim[1]);
  const int bx4 = bx & 31;
  const int by4 = by & 31;
  const uint8_t(*const filter_level)[8][2] = lvl_->level[chk<MAX_SEGMENTS>(seg_id)];

  if (bw4 > 0 && bh4 > 0) {
    fill_levelcache_block(level_, level_stride_, bx, by, bw4, bh4, 0, filter_level[0][0][0], filter_level[1][0][0]);
    mask_edges_intra(lflvl.filter_y, by4, bx4, bw4, bh4, ytx, above_y_.subview(static_cast<uint64_t>(bx), static_cast<uint64_t>(bw4)),
                     left_y_.subview(static_cast<uint64_t>(by), static_cast<uint64_t>(bh4)));
  }

  if (!has_chroma) return;

  const int ss_ver = ss_ver_, ss_hor = ss_hor_;
  const int cbw4 = imin(((w4_ + ss_hor) >> ss_hor) - (bx >> ss_hor), (b_dim[0] + ss_hor) >> ss_hor);
  const int cbh4 = imin(((h4_ + ss_ver) >> ss_ver) - (by >> ss_ver), (b_dim[1] + ss_ver) >> ss_ver);
  if (cbw4 <= 0 || cbh4 <= 0) return;

  const int cbx4 = bx4 >> ss_hor;
  const int cby4 = by4 >> ss_ver;

  fill_levelcache_block(level_, level_stride_, bx >> ss_hor, by >> ss_ver, cbw4, cbh4, 2, filter_level[2][0][0], filter_level[3][0][0]);
  mask_edges_chroma(lflvl.filter_uv, cby4, cbx4, cbw4, cbh4, 0, uvtx,
                    above_uv_.subview(static_cast<uint64_t>(bx >> ss_hor), static_cast<uint64_t>(cbw4)),
                    left_uv_.subview(static_cast<uint64_t>(by >> ss_ver), static_cast<uint64_t>(cbh4)), ss_hor, ss_ver);
}

void LoopFilterMasks::create_lf_mask_inter(int bx, int by, BlockSize bs, bool skip, RectTxfmSize max_ytx, uint16_t tx_split0, uint16_t tx_split1,
                                           RectTxfmSize uvtx, int seg_id, int ref0, bool is_globalmv, bool has_chroma) {
  if (!enabled_) return;
  RectTxfmSize ytx = max_ytx;
  if (lossless_[chk<MAX_SEGMENTS>(seg_id)]) {
    ytx = static_cast<RectTxfmSize>(TX_4X4);
    uvtx = static_cast<RectTxfmSize>(TX_4X4);
  }
  const uint16_t tx_masks[2] = {tx_split0, tx_split1};
  Av1Filter& lflvl = filter_at(bx, by);
  const uint8_t* const b_dim = k_block_dimensions[bs];
  const int bw4 = imin(w4_ - bx, b_dim[0]);
  const int bh4 = imin(h4_ - by, b_dim[1]);
  const int bx4 = bx & 31;
  const int by4 = by & 31;
  // &lflvl[seg_id][0][ref0 + 1][!is_globalmv] seen as an array of components
  const int r = chk<8>(ref0 + 1), m = is_globalmv ? 0 : 1;
  const uint8_t(*const filter_level)[8][2] = lvl_->level[chk<MAX_SEGMENTS>(seg_id)];
  auto level_of = [&](int component) { return filter_level[component][r][m]; };

  if (bw4 > 0 && bh4 > 0) {
    fill_levelcache_block(level_, level_stride_, bx, by, bw4, bh4, 0, level_of(0), level_of(1));
    mask_edges_inter(lflvl.filter_y, by4, bx4, bw4, bh4, skip, ytx, tx_masks,
                     above_y_.subview(static_cast<uint64_t>(bx), static_cast<uint64_t>(bw4)),
                     left_y_.subview(static_cast<uint64_t>(by), static_cast<uint64_t>(bh4)));
  }

  if (!has_chroma) return;

  const int ss_ver = ss_ver_, ss_hor = ss_hor_;
  const int cbw4 = imin(((w4_ + ss_hor) >> ss_hor) - (bx >> ss_hor), (b_dim[0] + ss_hor) >> ss_hor);
  const int cbh4 = imin(((h4_ + ss_ver) >> ss_ver) - (by >> ss_ver), (b_dim[1] + ss_ver) >> ss_ver);
  if (cbw4 <= 0 || cbh4 <= 0) return;

  const int cbx4 = bx4 >> ss_hor;
  const int cby4 = by4 >> ss_ver;

  fill_levelcache_block(level_, level_stride_, bx >> ss_hor, by >> ss_ver, cbw4, cbh4, 2, level_of(2), level_of(3));
  mask_edges_chroma(lflvl.filter_uv, cby4, cbx4, cbw4, cbh4, skip, uvtx,
                    above_uv_.subview(static_cast<uint64_t>(bx >> ss_hor), static_cast<uint64_t>(cbw4)),
                    left_uv_.subview(static_cast<uint64_t>(by >> ss_ver), static_cast<uint64_t>(cbh4)), ss_hor, ss_ver);
}

// ---- applying the filter to the frame (lf_apply_tmpl.c) ----------------------------------------------------------------------------

LevelView LoopFilterMasks::level_view(int component, int x4, int y4) const {
  LevelView view;
  view.levels = ArrayView<const uint8_t>(level_);
  view.origin = static_cast<int64_t>(y4) * level_stride_ + x4;
  view.stride = level_stride_;
  view.component = component;
  return view;
}

void LoopFilterMasks::loop_filter_sbrow_cols(PixelView p[3], int sby, int bitdepth_max) const {
  // Don't filter outside the frame
  const int is_sb64 = !sb128_;
  const int starty4 = (sby & is_sb64) << 4;
  const int sbsz = 32 >> is_sb64;
  const int ss_ver = ss_ver_, ss_hor = ss_hor_;
  const int endy4 = starty4 + imin(h4_ - sby * sbsz, sbsz);
  const int uv_endy4 = (endy4 + ss_ver) >> ss_ver;
  const uint64_t row_of_areas = static_cast<uint64_t>(sby >> is_sb64) * static_cast<uint64_t>(sb128w_);
  const int y_px = sby * sbsz * 4;  // first luma row of this superblock row
  const int y4 = sby * sbsz;        // ... in 4x4 units

  for (int x = 0; x < sb128w_; x++) {
    const bool have_left = x > 0;
    const Av1Filter& lflvl = filters_[row_of_areas + static_cast<uint64_t>(x)];
    const int w = imin(32, w4_ - x * 32);
    // filter edges between columns (e.g. block1 | block2)
    for (int xx = 0; xx < w; xx++) {
      if (!have_left && !xx) continue;
      uint32_t hmask[3];
      for (int k = 0; k < 3; k++) {
        if (!starty4) {
          hmask[k] = lflvl.filter_y[0][xx][k][0];
          if (endy4 > 16) hmask[k] |= static_cast<unsigned>(lflvl.filter_y[0][xx][k][1]) << 16;
        } else {
          hmask[k] = lflvl.filter_y[0][xx][k][1];
        }
      }
      loop_filter_h_sb128y(p[0].offset(x * 128 + xx * 4, y_px), hmask, level_view(0, x * 32 + xx, y4), lut_, bitdepth_max);
    }
  }

  if (!chroma_filtered_) return;

  const int cy_px = y_px >> ss_ver;
  const int cy4 = (sby * sbsz) >> ss_ver;
  for (int x = 0; x < sb128w_; x++) {
    const bool have_left = x > 0;
    const Av1Filter& lflvl = filters_[row_of_areas + static_cast<uint64_t>(x)];
    const int w = (imin(32, w4_ - x * 32) + ss_hor) >> ss_hor;
    const int cstarty4 = starty4 >> ss_ver;
    for (int xx = 0; xx < w; xx++) {
      if (!have_left && !xx) continue;
      uint32_t hmask[2];
      for (int k = 0; k < 2; k++) {
        if (!cstarty4) {
          hmask[k] = lflvl.filter_uv[0][xx][k][0];
          if (uv_endy4 > (16 >> ss_ver)) hmask[k] |= static_cast<unsigned>(lflvl.filter_uv[0][xx][k][1]) << (16 >> ss_ver);
        } else {
          hmask[k] = lflvl.filter_uv[0][xx][k][1];
        }
      }
      const int cx4 = x * (32 >> ss_hor) + xx;
      const int px = x * (128 >> ss_hor) + xx * 4;
      loop_filter_h_sb128uv(p[1].offset(px, cy_px), hmask, level_view(2, cx4, cy4), lut_, bitdepth_max);
      loop_filter_h_sb128uv(p[2].offset(px, cy_px), hmask, level_view(3, cx4, cy4), lut_, bitdepth_max);
    }
  }
}

void LoopFilterMasks::loop_filter_sbrow_rows(PixelView p[3], int sby, int bitdepth_max) const {
  const int have_top = sby > 0;
  const int is_sb64 = !sb128_;
  const int starty4 = (sby & is_sb64) << 4;
  const int sbsz = 32 >> is_sb64;
  const int ss_ver = ss_ver_, ss_hor = ss_hor_;
  const int endy4 = starty4 + imin(h4_ - sby * sbsz, sbsz);
  const int uv_endy4 = (endy4 + ss_ver) >> ss_ver;
  const uint64_t row_of_areas = static_cast<uint64_t>(sby >> is_sb64) * static_cast<uint64_t>(sb128w_);
  const int y_px = sby * sbsz * 4;
  const int y4 = sby * sbsz;

  for (int x = 0; x < sb128w_; x++) {
    const Av1Filter& lflvl = filters_[row_of_areas + static_cast<uint64_t>(x)];
    //                                 block1
    // filter edges between rows (e.g. ------)
    //                                 block2
    for (int y = starty4; y < endy4; y++) {
      if (!have_top && !y) continue;
      uint32_t vmask[3];
      for (int k = 0; k < 3; k++) vmask[k] = lflvl.filter_y[1][y][k][0] | (static_cast<unsigned>(lflvl.filter_y[1][y][k][1]) << 16);
      loop_filter_v_sb128y(p[0].offset(x * 128, y_px + (y - starty4) * 4), vmask, level_view(1, x * 32, y4 + (y - starty4)), lut_, bitdepth_max);
    }
  }

  if (!chroma_filtered_) return;

  const int cy_px = y_px >> ss_ver;
  const int cy4 = (sby * sbsz) >> ss_ver;
  const int cstarty4 = starty4 >> ss_ver;
  for (int x = 0; x < sb128w_; x++) {
    const Av1Filter& lflvl = filters_[row_of_areas + static_cast<uint64_t>(x)];
    for (int y = cstarty4; y < uv_endy4; y++) {
      if (!have_top && !y) continue;
      uint32_t vmask[2];
      for (int k = 0; k < 2; k++) vmask[k] = lflvl.filter_uv[1][y][k][0] | (static_cast<unsigned>(lflvl.filter_uv[1][y][k][1]) << (16 >> ss_hor));
      const int cx4 = x * (32 >> ss_hor);
      const int px = x * (128 >> ss_hor);
      loop_filter_v_sb128uv(p[1].offset(px, cy_px + (y - cstarty4) * 4), vmask, level_view(2, cx4, cy4 + (y - cstarty4)), lut_, bitdepth_max);
      loop_filter_v_sb128uv(p[2].offset(px, cy_px + (y - cstarty4) * 4), vmask, level_view(3, cx4, cy4 + (y - cstarty4)), lut_, bitdepth_max);
    }
  }
}

void LoopFilterMasks::loop_filter_frame(PixelView y, PixelView u, PixelView v, int bitdepth) const {
  if (!enabled_ || filters_.empty()) return;
  PixelView p[3] = {y, u, v};
  const int bitdepth_max = (1 << bitdepth) - 1;
  const int sb_shift = 4 + (sb128_ ? 1 : 0), sb_step = 16 << (sb128_ ? 1 : 0);
  const int sbh = (bh4_ + sb_step - 1) >> sb_shift;
  for (int sby = 0; sby < sbh; sby++) loop_filter_sbrow_cols(p, sby, bitdepth_max);
  for (int sby = 0; sby < sbh; sby++) loop_filter_sbrow_rows(p, sby, bitdepth_max);
}

}  // namespace av1
}  // namespace hardware
