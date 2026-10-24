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

#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/Software/Av1IntraEdge.hpp"
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Utility/ArrayView.hpp"

// AV1 intra prediction (port of dav1d's ipred_tmpl.c, ipred_prepare_tmpl.c and pal.c, BSD-2-Clause, Copyright (c) VideoLAN and dav1d
// authors). One instantiation, 16-bit samples, `bitdepth_max = (1 << bitdepth) - 1` is a parameter. Strides are in samples.
//
// How the block reconstruction (dav1d's dav1d_recon_b_intra / interintra code) uses this:
//
//   IntraEdge edge;                                      // scratch for the neighbouring samples, one per decoder is enough
//   // luma transform block at (bx, by) (4-sample units), size tw x th (4-sample units), dst = PixelView at its top left sample:
//   int angle = b.y_angle;                               // angle delta [-3, 3]
//   IntraPredMode m = prepare_intra_edges(bx, bx > tile_col_start, by, by > tile_row_start, tile_col_end, tile_row_end, edge_flags, dst,
//                                         top_sb_edge, b.y_mode, angle, tw, th, seq_hdr.intra_edge_filter, edge, bitdepth_max);
//   intra_pred(m, dst, edge, tw * 4, th * 4, angle | intra_flags, 4 * frame_bw - 4 * bx, 4 * frame_bh - 4 * by, bitdepth_max);
//   // intra_flags = sm_flag(above, bx4) | sm_flag(left, by4) | (seq_hdr.intra_edge_filter ? ANGLE_USE_EDGE_FILTER_FLAG : 0)
//   // top_sb_edge: empty view, or (when `by` is the first row of a superblock) the saved row of the superblock above, i.e. dav1d's
//   //   `f->ipred_edge[plane] + sb128w * 128 * (sby - 1)`, as a view that starts there and extends to the end of that row (>= 4 * w samples).
//   // Chroma (plane transform block): x, y, w, h, tile bounds divided by the subsampling; edge_flags 0 and filter_edge 0 for CfL.
//   // Interintra and the sub8x8 chroma: angle = 0, edge_flags = 0, filter_edge = 0, angle argument to intra_pred 0, max_width/height 0.
//
//   // CfL: build the luma AC once per block, then predict U and V:
//   cfl_ac(layout, ac, y_src, cbw4 * 4, cbh4 * 4, cbw4 - (furthest_r >> ss_hor), cbh4 - (furthest_b >> ss_ver));
//   m = prepare_intra_edges(..., DC_PRED, angle = 0, ..., filter_edge = 0, edge, ...);       // m is DC / LEFT_DC / TOP_DC / DC_128
//   cfl_pred(m, uv_dst, edge, uv_tw * 4, uv_th * 4, ac, b.cfl_alpha[pl], bitdepth_max);
//
// The predictors never read outside what prepare_intra_edges filled for the returned mode.

namespace hardware {
namespace av1 {

// These flags are OR'd with the angle argument of intra_pred. ANGLE_USE_EDGE_FILTER_FLAG: filter / upsample the edges
// (seq_hdr.intra_edge_filter). ANGLE_SMOOTH_EDGE_FLAG: a neighbour block uses a smooth mode, which reduces the filter strength.
constexpr int ANGLE_USE_EDGE_FILTER_FLAG = 1024;
constexpr int ANGLE_SMOOTH_EDGE_FLAG = 512;

// dav1d's sm_flag / sm_uv_flag for one neighbour: pass its (luma or chroma) prediction mode; for a luma neighbour that is not intra pass
// any non-smooth mode (e.g. DC_PRED).
inline int smooth_edge_flag(int mode) {
    return (mode == SMOOTH_PRED || mode == SMOOTH_H_PRED || mode == SMOOTH_V_PRED) ? ANGLE_SMOOTH_EDGE_FLAG : 0;
}

// The neighbouring samples of a block, as dav1d's `topleft` pointer: index 0 is the top left sample, 1..w the top row, w+1..2w the top
// right, -1..-h the left column (-1 is the topmost), -h-1..-2h the bottom left. Blocks are at most 64 samples wide / high.
struct IntraEdge {
    static constexpr int kTopLeft = 128;
    static constexpr int kSize = 2 * kTopLeft + 1;
    Pixel samples[kSize] = {};

    // Checked access by index relative to the top left sample.
    Pixel& at(int relative) { return ArrayView<Pixel>(samples)[static_cast<uint64_t>(static_cast<int64_t>(kTopLeft) + relative)]; }
    Pixel at(int relative) const {
        return ArrayView<const Pixel>(samples)[static_cast<uint64_t>(static_cast<int64_t>(kTopLeft) + relative)];
    }
};

// Edge setup (dav1d_prepare_intra_edges). x, y: absolute block position and w, h: tile end / frame end, all in 4-sample units of the plane
// (for a chroma plane: luma units shifted by the subsampling); have_left / have_top: the neighbour is inside the tile.
// edge_flags: EdgeFlags for this transform block (see Av1IntraEdge.hpp). dst: window at the block's top left sample (its left column and
// row above are read). prefilter_toplevel_sb_edge: empty, or the unfiltered row above the superblock (see the header comment).
// mode: IntraPredMode as coded (DC_PRED .. PAETH_PRED, or FILTER_PRED which is passed through); angle: angle delta [-3, 3] on input,
// absolute angle on output. tw, th: transform block size in 4-sample units. filter_edge: seq_hdr.intra_edge_filter.
// Returns the mode to hand to intra_pred / cfl_pred (DC may become LEFT_DC_PRED, TOP_DC_PRED or DC_128_PRED, PAETH may become VERT_PRED /
// HOR_PRED / DC_128_PRED, the directional modes become Z1_PRED, Z2_PRED, Z3_PRED, VERT_PRED or HOR_PRED).
IntraPredMode prepare_intra_edges(int x, bool have_left, int y, bool have_top, int w, int h, int edge_flags, PixelView dst,
                                  ArrayView<const Pixel> prefilter_toplevel_sb_edge, IntraPredMode mode, int& angle, int tw, int th,
                                  bool filter_edge, IntraEdge& topleft_out, int bitdepth_max);

// intra_pred[mode] of dav1d: predicts width x height samples into dst. `mode` is what prepare_intra_edges returned (DC_PRED, DC_128_PRED,
// TOP_DC_PRED, LEFT_DC_PRED, HOR_PRED, VERT_PRED, PAETH_PRED, SMOOTH_PRED, SMOOTH_V_PRED, SMOOTH_H_PRED, Z1_PRED, Z2_PRED, Z3_PRED,
// FILTER_PRED). `angle`: for Z1..Z3 the absolute angle | ANGLE flags, for FILTER_PRED the filter index (0..4), ignored otherwise.
// max_width / max_height: in samples, the distance from the block to the frame's right / bottom edge (used by Z2's edge filter only).
// Returns false (and writes nothing) for a mode that is not a predictor.
bool intra_pred(IntraPredMode mode, PixelView dst, const IntraEdge& edge, int width, int height, int angle, int max_width, int max_height,
                int bitdepth_max);

// The individual predictors (same arguments as intra_pred without the mode).
void ipred_dc(PixelView dst, const IntraEdge& edge, int width, int height, int bitdepth_max);
void ipred_dc_128(PixelView dst, int width, int height, int bitdepth_max);
void ipred_dc_top(PixelView dst, const IntraEdge& edge, int width, int height, int bitdepth_max);
void ipred_dc_left(PixelView dst, const IntraEdge& edge, int width, int height, int bitdepth_max);
void ipred_v(PixelView dst, const IntraEdge& edge, int width, int height);
void ipred_h(PixelView dst, const IntraEdge& edge, int width, int height);
void ipred_paeth(PixelView dst, const IntraEdge& edge, int width, int height);
void ipred_smooth(PixelView dst, const IntraEdge& edge, int width, int height);
void ipred_smooth_v(PixelView dst, const IntraEdge& edge, int width, int height);
void ipred_smooth_h(PixelView dst, const IntraEdge& edge, int width, int height);
void ipred_z1(PixelView dst, const IntraEdge& edge, int width, int height, int angle, int bitdepth_max);
void ipred_z2(PixelView dst, const IntraEdge& edge, int width, int height, int angle, int max_width, int max_height, int bitdepth_max);
void ipred_z3(PixelView dst, const IntraEdge& edge, int width, int height, int angle, int bitdepth_max);
// Filter intra: width and height at most 32.
void ipred_filter(PixelView dst, const IntraEdge& edge, int width, int height, int filter_index, int bitdepth_max);

// Chroma from luma. cfl_ac builds the subsampled luma of the block with its mean removed: `layout` is I420, I422 or I444, y is the window
// at the top left luma sample of the block, cw / ch the chroma block size in samples, w_pad / h_pad the right / bottom part of the block
// that lies outside the frame in 4-sample units. `ac` holds cw * ch values (row stride cw). Returns false for another layout or if `ac`
// is smaller than cw * ch.
bool cfl_ac(PixelLayout layout, ArrayView<int16_t> ac, PixelView y, int w_pad, int h_pad, int cw, int ch);
// cfl_pred[mode] of dav1d: mode is DC_PRED, DC_128_PRED, TOP_DC_PRED or LEFT_DC_PRED (as returned by prepare_intra_edges); alpha is
// the signed q3 scale [-16, 16]; `ac` as produced by cfl_ac for this block (width * height values). False for another mode.
bool cfl_pred(IntraPredMode mode, PixelView dst, const IntraEdge& edge, int width, int height, ArrayView<const int16_t> ac, int alpha,
              int bitdepth_max);

// Palette: dst[x, y] = pal[idx[x, y]]. `pal` has 8 entries; `idx` holds two 4-bit indices per byte (low nibble first), w / 2 bytes per row
// (w is even, a multiple of 4).
void pal_pred(PixelView dst, ArrayView<const Pixel> pal, ArrayView<const uint8_t> idx, int w, int h);
// dav1d's pal_idx_finish: packs the w x h visible indices of `src` (row stride bw, one index per byte) into 4-bit pairs in `dst` (row
// stride bw / 2) and replicates the last column / row over the invisible part of the bw x bh block. bw, bh are 4..64 powers of two;
// w, h are multiples of 4 and at most bw, bh. False if a size is out of range or a buffer is too small.
bool pal_idx_finish(ArrayView<uint8_t> dst, ArrayView<const uint8_t> src, int bw, int bh, int w, int h);

}  // namespace av1
}  // namespace hardware
