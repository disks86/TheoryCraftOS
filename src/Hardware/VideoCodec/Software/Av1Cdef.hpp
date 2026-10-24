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
#include "Hardware/VideoCodec/Software/Av1LoopFilter.hpp"
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Utility/ArrayView.hpp"

// AV1 constrained directional enhancement filter (CDEF), a port of dav1d's cdef_tmpl.c and cdef_apply_tmpl.c (BSD-2-Clause, Copyright
// (c) VideoLAN and dav1d authors) in whole-frame form. The result is bit-exact with dav1d.
//
// CDEF reads only deblocked, not yet CDEF-filtered samples (dav1d keeps line buffers of them while it filters in place). Here the filter
// reads `src`, the deblocked frame, and writes `dst`, a second frame that must contain a copy of `src` when cdef_frame() is called (8x8
// blocks that are not filtered are simply left as they are). `dst` is also what the loop restoration pass wants as its "CDEF output",
// while it wants `src` as the "deblocked" picture (Av1LoopRestoration.hpp). The two frames must be distinct (filtering in place would read
// already filtered neighbours).
//
// The block decoder feeds the filter through LoopFilterMasks (Av1LoopFilter.hpp): cdef_idx per 64x64 and the noskip mask. The sequence
// header's `cdef` flag and the frame header's cdef fields (damping as dav1d stores it, i.e. already + 3; n_bits; y_strength and
// uv_strength, each 6 bits: primary strength << 2 | secondary strength) are used as dav1d does.
// Plane requirements: luma at least (bw4 * 4) x (bh4 * 4) samples (the frame size rounded up to a multiple of 8), chroma that shifted by
// the subsampling.

namespace hardware {
namespace av1 {

enum CdefEdgeFlags {
  CDEF_HAVE_LEFT = 1 << 0,
  CDEF_HAVE_RIGHT = 1 << 1,
  CDEF_HAVE_TOP = 1 << 2,
  CDEF_HAVE_BOTTOM = 1 << 3,
};

// dav1d: dsp->cdef.dir. Returns the direction (0..7) of the 8x8 block at the origin of `img` and its variance in `var`.
int cdef_find_dir(PixelView img, unsigned& var, int bitdepth_max);

// dav1d: dsp->cdef.fb[]. Filters the w x h block (8x8, 4x8 or 4x4) at the origin of `src` into the same place of `dst`. `src` is read
// up to two samples beyond the block on each side for which the matching edge flag is set (so src.at(-2, y) is only touched with
// CDEF_HAVE_LEFT and so on); the missing neighbours count as unavailable. pri_strength / sec_strength are the adjusted strengths
// (at least one non-zero), damping already includes the bit depth offset (and is one less for chroma, as dav1d passes it).
// dst may alias src.
void cdef_filter_block(PixelView dst, PixelView src, int w, int h, int pri_strength, int sec_strength, int dir, int damping, int edges,
                       int bitdepth_max);

// Runs CDEF over the whole frame. src[3] is the deblocked frame (read only), dst[3] a copy of it that receives the result; chroma views
// are ignored for monochrome. Does nothing if seq.cdef is 0. bitdepth is 8, 10 or 12.
void cdef_frame(const LoopFilterMasks& masks, const SequenceHeader& seq, const FrameHeader& hdr, const PixelView src[3], const PixelView dst[3],
                int bitdepth);

}  // namespace av1
}  // namespace hardware
