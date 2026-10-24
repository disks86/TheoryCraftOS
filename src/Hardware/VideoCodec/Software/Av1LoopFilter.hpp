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
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ArrayView.hpp"

// AV1 deblocking loop filter, a port of dav1d's lf_mask.c, loopfilter_tmpl.c and lf_apply_tmpl.c (BSD-2-Clause, Copyright (c) VideoLAN
// and dav1d authors). dav1d filters one superblock row at a time while it decodes; here the block decoder only records, per block, what
// the filter will need, and loop_filter_frame() then filters the whole decoded frame (all vertical edges, then all horizontal edges,
// which is what the AV1 specification says and what dav1d's row-by-row order amounts to). The result is bit-exact with dav1d.
//
// ---- What the block decoder has to do (dav1d: decode_b / decode_tile_sbrow) ----------------------------------------------------------
// All positions are in units of 4 luma samples (dav1d's t->bx / t->by), frame absolute.
//   * LoopFilterMasks::init(seq, hdr) once per frame, before the first block.
//   * At the first block of every superblock (when (bx | by) & (sb128 ? 31 : 15) == 0), after the delta-q / delta-lf syntax has been read
//     (that is where dav1d refreshes ts->lflvl): set_delta_lf(last_delta_lf), with the running delta_lf[0..3] of the tile (all 0 when
//     delta_lf is not present; the tile state starts at 0 at every tile, as ts->last_delta_lf does).
//   * For every block with skip == 0, right where dav1d reads "cdef index": if (cdef_idx_unset(bx, by)) set_cdef_idx(bx, by, bs,
//     read_literal(hdr.cdef.n_bits)). With n_bits == 0 the index is simply 0 (a block that is never skip makes the 64x64 "set").
//   * After the block has been reconstructed (so its final b->tx / b->uvtx / tx_split are known): create_lf_mask_intra() or
//     create_lf_mask_inter(). They return at once when the frame has no luma deblocking, so they can be called unconditionally.
//   * At the end of decode_b, for every block with skip == 0: mark_coded(bx, by, bs) (dav1d: the noskip_mask update).
//   * Tiles must be decoded in the order of the bitstream (raster order of the tiles). The "left" and "above" transform-size contexts the
//     filter needs across tile edges are frame wide here, so unlike dav1d no fix-up pass over tile edges (tx_lpf_right_edge,
//     start_of_tile_row) is needed. Left neighbours in the first tile column keep the initial value, as in dav1d.
// Afterwards: loop_filter_frame() on the reconstructed planes, then cdef_frame() (Av1Cdef.hpp) which reads the masks too.
//
// Plane requirements: luma must be at least (bw4 * 4) x (bh4 * 4) samples (the frame size rounded up to 8), chroma that shifted by the
// subsampling. The deblocking filters touch rows and columns up to the 4-sample aligned frame size (w4 / h4), CDEF up to the 8 aligned one.

namespace hardware {
namespace av1 {

// ---- parameter tables --------------------------------------------------------------------------------------------------------------

// dav1d: Av1FilterLUT (without the "sharp" words, which only the assembly versions read). e is the edge limit and i the interior limit,
// indexed by the filter level (0..63); the high-edge-variance threshold is level >> 4.
struct FilterLut {
  uint8_t e[64];
  uint8_t i[64];
};
void calc_eih(FilterLut& lut, int filter_sharpness);

// The filter level of every (segment, component, reference, mode): component 0 = luma vertical edges (loopfilter.level_y[0]), 1 = luma
// horizontal edges (level_y[1]), 2 = u, 3 = v; reference index 0 is intra, 1.. is ref + 1; mode 1 = not a global-motion block.
struct LfValues {
  uint8_t level[MAX_SEGMENTS][4][8][2];
};
// lf_delta: the four running delta_lf values of the superblock (zeros for the frame level table). Only segments that exist are written
// (1 without segmentation, else 8), the rest is left untouched.
void calc_lf_values(LfValues& values, const FrameHeader& hdr, const int8_t lf_delta[4]);

// ---- per-superblock data -----------------------------------------------------------------------------------------------------------

// dav1d: Av1Filter. One of these describes 128x128 luma samples (one 128 superblock or four 64 ones).
// filter_y[dir][4x4 row or column 0..31][filter size class 0..2 (4, 8, 16 taps)][half]: bit n of half h is 4x4 row (dir 0) or column
// (dir 1) 16 * h + n of the area, which has an edge of that class at its left (dir 0) or top (dir 1). filter_uv likewise with two classes
// (4 and 6 taps) and halves of 16 >> subsampling bits.
struct Av1Filter {
  uint16_t filter_y[2 /* 0 = vertical edges (between columns), 1 = horizontal edges */][32][3][2];
  uint16_t filter_uv[2][32][2][2];
  int8_t cdef_idx[4];           // per 64x64: -1 means not set (all blocks skipped)
  uint16_t noskip_mask[16][2];  // for 8x8 blocks, but stored on a 4x8 basis: bit n of [row / 2][half] is the 4x4 column 16 * half + n
};

// ---- kernels -----------------------------------------------------------------------------------------------------------------------

// The per-4x4 level cache (dav1d: f->lf.level): 4 bytes per 4x4 block, in rows of `stride` blocks. A LevelView looks at one of the four
// components starting at block `origin`; at(dx, dy) is checked against the cache.
struct LevelView {
  ArrayView<const uint8_t> levels;
  int64_t origin = 0;  // in blocks
  int64_t stride = 0;  // in blocks
  int component = 0;
  uint8_t at(int dx, int dy) const {
    const int64_t index = (origin + static_cast<int64_t>(dy) * stride + dx) * 4 + component;
    if (index < 0) bounds_violation("LevelView::at", 0, levels.size());
    return levels[static_cast<uint64_t>(index)];
  }
};

// dav1d: dsp->lf.loop_filter_sb[plane][direction]. `dst` is the first sample of the superblock-wide strip: the first 4x4 row (h) or
// column (v) of vmask bit 0. vmask[k] holds one bit per 4x4 unit along the strip for filter size class k (luma: 3 classes, chroma: 2).
//   h: filters the edges between columns (vertical edges) along 4x4 rows, bit n = the edge at the left of row n; the level comes from
//      l.at(0, n) with l.at(-1, n) as the fallback when that is 0.
//   v: filters the edges between rows (horizontal edges) along 4x4 columns, level from l.at(n, 0) with l.at(n, -1) as the fallback.
void loop_filter_h_sb128y(PixelView dst, const uint32_t vmask[3], const LevelView& l, const FilterLut& lut, int bitdepth_max);
void loop_filter_v_sb128y(PixelView dst, const uint32_t vmask[3], const LevelView& l, const FilterLut& lut, int bitdepth_max);
void loop_filter_h_sb128uv(PixelView dst, const uint32_t vmask[2], const LevelView& l, const FilterLut& lut, int bitdepth_max);
void loop_filter_v_sb128uv(PixelView dst, const uint32_t vmask[2], const LevelView& l, const FilterLut& lut, int bitdepth_max);

// ---- the state the block decoder fills and the frame filter reads -------------------------------------------------------------------

class LoopFilterMasks {
 public:
  explicit LoopFilterMasks(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~LoopFilterMasks() { release(); }
  LoopFilterMasks(const LoopFilterMasks&) = delete;
  LoopFilterMasks& operator=(const LoopFilterMasks&) = delete;

  // Sizes everything for the frame and clears it. False if the memory cannot be had. Keeps a copy of what it needs from the headers.
  bool init(const SequenceHeader& seq, const FrameHeader& hdr);

  // ---- geometry (dav1d: f->w4, h4, bw, bh, sb128w, sb128h)
  int w4() const { return w4_; }          // (width + 3) >> 2
  int h4() const { return h4_; }
  int bw4() const { return bw4_; }        // ((width + 7) >> 3) << 1: the width in 4x4 units rounded up to an even number
  int bh4() const { return bh4_; }
  int sb128w() const { return sb128w_; }  // 128x128 areas across / down
  int sb128h() const { return sb128h_; }
  // True if the frame is deblocked at all (loopfilter.level_y[0] || level_y[1]).
  bool deblock_enabled() const { return enabled_; }

  // ---- called by the block decoder (see the top of this file)
  void set_delta_lf(const int8_t delta_lf[4]);
  bool cdef_idx_unset(int bx, int by) const;
  void set_cdef_idx(int bx, int by, BlockSize bs, int idx);
  void mark_coded(int bx, int by, BlockSize bs);
  // ytx / uvtx are b->tx and b->uvtx. has_chroma is dav1d's has_chroma of decode_b (false for monochrome and for the luma-only 4xN
  // blocks of subsampled formats).
  void create_lf_mask_intra(int bx, int by, BlockSize bs, RectTxfmSize ytx, RectTxfmSize uvtx, int seg_id, bool has_chroma);
  // max_ytx = b->max_ytx, tx_split0 / tx_split1 = b->tx_split0 / tx_split1, ref0 = b->ref[0] (>= 0), is_globalmv = b->inter_mode ==
  // (compound ? GLOBALMV_GLOBALMV : GLOBALMV). Lossless segments are handled here (4x4 transforms).
  void create_lf_mask_inter(int bx, int by, BlockSize bs, bool skip, RectTxfmSize max_ytx, uint16_t tx_split0, uint16_t tx_split1,
                            RectTxfmSize uvtx, int seg_id, int ref0, bool is_globalmv, bool has_chroma);

  // ---- the frame passes
  // Deblocks the planes in place. `u` and `v` are ignored (may be empty views) for monochrome. bitdepth is 8, 10 or 12.
  void loop_filter_frame(PixelView y, PixelView u, PixelView v, int bitdepth) const;

  // ---- read access for the CDEF pass
  const Av1Filter& filter(int sb128x, int sb128y) const { return filters_[static_cast<uint64_t>(sb128y) * static_cast<uint64_t>(sb128w_) + static_cast<uint64_t>(sb128x)]; }

 private:
  void release();
  void loop_filter_sbrow_cols(PixelView p[3], int sby, int bitdepth_max) const;
  void loop_filter_sbrow_rows(PixelView p[3], int sby, int bitdepth_max) const;
  Av1Filter& filter_at(int bx, int by) { return filters_[static_cast<uint64_t>(by >> 5) * static_cast<uint64_t>(sb128w_) + static_cast<uint64_t>(bx >> 5)]; }
  LevelView level_view(int component, int x4, int y4) const;

  allocators::IAllocator& allocator_;
  ArrayView<Av1Filter> filters_;
  ArrayView<uint8_t> level_;       // 4 bytes per 4x4 block, level_stride_ blocks per row
  ArrayView<uint8_t> above_y_, above_uv_, left_y_, left_uv_;  // frame wide transform-size contexts (dav1d: BlockContext::tx_lpf_*)
  int64_t level_stride_ = 0;
  int w4_ = 0, h4_ = 0, bw4_ = 0, bh4_ = 0, sb128w_ = 0, sb128h_ = 0;
  bool sb128_ = false, enabled_ = false;
  PixelLayout layout_ = PIXEL_LAYOUT_I420;
  int ss_hor_ = 1, ss_ver_ = 1;
  bool lossless_[MAX_SEGMENTS] = {};
  bool chroma_filtered_ = false;   // level_u || level_v
  FilterLut lut_ = {};
  LfValues frame_lvl_ = {};
  LfValues delta_lvl_ = {};
  const LfValues* lvl_ = nullptr;  // frame_lvl_ or delta_lvl_: the table of the current superblock
  int8_t delta_lf_[4] = {};
  FrameHeader hdr_{};
};

}  // namespace av1
}  // namespace hardware
