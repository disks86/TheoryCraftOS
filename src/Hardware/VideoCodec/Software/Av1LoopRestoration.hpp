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
#include "Utility/ArrayView.hpp"

// AV1 loop restoration (Wiener and self-guided filters), a port of dav1d's looprestoration_tmpl.c and lr_apply_tmpl.c
// (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors) in whole-frame form: the decoder restores a plane after the
// deblocking, CDEF and super-resolution passes have run on the whole frame. The result is bit-exact with dav1d.
//
// ---- How restoration units are indexed (what the block decoder has to fill) ------------------------------------------------------
// Every plane that has frame_hdr.restoration.type[plane] != RESTORATION_NONE has its own grid of units, stored row-major:
// units[unit_row * unit_cols + unit_col]. With
//     unit_size_log2 = frame_hdr.restoration.unit_size[plane != 0]          (luma 6..8, chroma 5..8: the unit is 32 to 256 samples)
//     plane width/height = Round2(UpscaledWidth, ss_hor) / Round2(FrameHeight, ss_ver)   (the size AFTER super-resolution)
//     unit_rows = lr_unit_count(plane_height, unit_size_log2), unit_cols = lr_unit_count(plane_width, unit_size_log2)
// the units tile the plane in unit_size steps; the last row and column absorb the remainder (so they are up to 1.5 units
// wide/tall) and there is always at least one unit. Unit (ur, uc) covers x in [uc * unit_size, next), y in [ur * unit_size, next)
// in plane samples, but the rows are shifted up by 8 luma rows (the stripe offset of the AV1 spec): a stripe starting at
// plane row y belongs to unit row min(unit_rows - 1, (64 * stripe_index >> ss_ver) >> unit_size_log2).
// The bitstream order (spec read_lr) is given by lr_unit_read_range(): for each superblock and plane it names the units to read.
// A unit that is not signalled (type NONE) keeps type = RESTORATION_NONE. Wiener and SGR parameters are stored the way dav1d does
// after read_restoration_info (filter_h/filter_v are the 3 transmitted taps, sgr_idx the 4-bit set index).
// Reference values for the "previous unit" used by the delta coding (spec RefLrWiener / RefSgrXqd), reset per tile and plane:
//     filter_h = filter_v = {3, -7, 15}, sgr_weights = {-32, 31}
//
// ---- Frame-level use -----------------------------------------------------------------------------------------------------------
// loop_restoration_plane() needs, besides the CDEF output ("src"), the deblocked but not CDEF-filtered (and, with super-resolution,
// already upscaled) picture ("deblocked"): the two rows above and below every stripe boundary are read from it. If CDEF is off
// both are the same picture; pass a separate `dst` then (or the filter would read rows it already restored).

namespace hardware {
namespace av1 {

// Parameters of one restoration unit (dav1d: Av1RestorationUnit, with the SGR set index stored apart from the type).
struct RestorationUnit {
  uint8_t type = RESTORATION_NONE;  // RESTORATION_NONE, RESTORATION_WIENER or RESTORATION_SGRPROJ (never SWITCHABLE)
  int8_t filter_h[3] = {3, -7, 15};
  int8_t filter_v[3] = {3, -7, 15};
  uint8_t sgr_idx = 0;                // index into k_sgr_params (0..15)
  int8_t sgr_weights[2] = {-32, 31};  // w0, w1 as transmitted
};

// Number of units along one dimension: Max((size + unit / 2) / unit, 1), spec count_units_in_frame.
constexpr int lr_unit_count(int size, int unit_size_log2) {
  const int unit = 1 << unit_size_log2;
  const int n = (size + (unit >> 1)) >> unit_size_log2;
  return n > 0 ? n : 1;
}

// The units a superblock signals (spec read_lr), as half-open ranges. (mi_row, mi_col) is the superblock position in 4x4 units,
// bw4/bh4 its size in 4x4 units (16 or 32), ss_hor/ss_ver the plane subsampling, super_res_denom frame_hdr.super_res.width_scale_denominator
// (8 without super-resolution), plane_unit_rows/cols from lr_unit_count().
struct LrUnitRange {
  int row_start, row_end, col_start, col_end;
};
LrUnitRange lr_unit_read_range(int mi_row, int mi_col, int bh4, int bw4, int ss_hor, int ss_ver, int unit_size_log2,
                               bool super_res, int super_res_denom, int plane_unit_rows, int plane_unit_cols);

// ---- Filter kernels (differentially tested against dav1d's C code) ----------------------------------------------------------------
constexpr int LR_MAX_UNIT_W = 384;  // 256 * 1.5: the widest unit a kernel is called with
constexpr int LR_SCRATCH_INTS = 16768;  // 40 ring rows of 400 + two 384-sample temporaries

// Filter taps prepared from a unit (dav1d: LooprestorationParams).
struct LrFilterParams {
  int16_t filter[2][8] = {};  // Wiener: [0] horizontal, [1] vertical, symmetric 7 taps with the centre tap holding the +128
  uint32_t s0 = 0, s1 = 0;    // SGR strengths of the 5x5 and 3x3 passes (k_sgr_params)
  int16_t w0 = 0, w1 = 0;     // SGR weights
};

// Prepares the taps. Returns false if the unit has no filter (type NONE, an SGR set without any pass).
bool lr_make_params(const RestorationUnit& unit, LrFilterParams* out);

// The kernels read a source that is already padded: `src.at(x, y)` is valid for x in [-3, w + 3) and y in [-3, h + 3) around the
// w x h block, with the edge handling of dav1d's LR_HAVE_LEFT/RIGHT/TOP/BOTTOM already resolved (replicated frame edge samples, the
// pre-restoration neighbours of the adjacent unit, the pre-CDEF rows beyond a stripe boundary). `dst` receives the w x h result and
// may not overlap `src`. `scratch` needs LR_SCRATCH_INTS elements, w in 1..LR_MAX_UNIT_W, h in 1..64 (a stripe).
void wiener_filter(PixelView dst, PixelView src, int w, int h, const LrFilterParams& params, int bitdepth_max,
                   ArrayView<int32_t> scratch);
// variant: 0 = 5x5 only (dav1d sgr[0]), 1 = 3x3 only, 2 = mix of both.
void sgr_filter(PixelView dst, PixelView src, int w, int h, int variant, const LrFilterParams& params, int bitdepth_max,
                ArrayView<int32_t> scratch);
// Picks the kernel from the unit type: dst receives the restored block. Returns false (dst untouched) if the unit has no filter.
bool restore_unit(PixelView dst, PixelView src, int w, int h, const RestorationUnit& unit, int bitdepth_max,
                  ArrayView<int32_t> scratch);

// ---- Whole plane --------------------------------------------------------------------------------------------------------------------
struct LrPlane {
  int width = 0, height = 0;  // plane size after super-resolution
  int ss_ver = 0;             // 1 only for 4:2:0 chroma (it shortens the stripes), else 0
  int unit_size_log2 = 6;  // 5..8
  int bitdepth_max = 255;
  ArrayView<const RestorationUnit> units;  // lr_unit_count(height) * lr_unit_count(width) units, row-major
};

// Restores one plane stripe by stripe. `src` is the CDEF output, `deblocked` the pre-CDEF picture (see above), both of the plane
// size; `dst` receives the result and may be the same view as `src` (in place). Samples of units without a filter are copied to
// dst when it is a different picture. Returns false on bad arguments or when the allocator is out of memory.
bool loop_restoration_plane(allocators::IAllocator& allocator, PixelView dst, PixelView src, PixelView deblocked, const LrPlane& plane);

}  // namespace av1
}  // namespace hardware
