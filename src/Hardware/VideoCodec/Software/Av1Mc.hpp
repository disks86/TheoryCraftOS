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
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Utility/ArrayView.hpp"

// Inter prediction primitives of the AV1 decoder: a port of dav1d's src/mc_tmpl.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d
// authors). dav1d's function-pointer table (Dav1dMCDSPContext) becomes plain functions; names match its members (mc, mct, mc_scaled,
// mct_scaled, avg, w_avg, mask, w_mask, blend, blend_v, blend_h, warp8x8, warp8x8t, emu_edge, resize).
//
// Conventions
//  - Samples are 16-bit (Pixel). `bitdepth_max` is (1 << bitdepth) - 1 for bitdepth 8, 10 or 12.
//  - dav1d's two builds are reproduced: for bitdepth 8 the 8bpc arithmetic (4 intermediate bits, no prep bias) is used, for 10 and 12 the
//    16bpc arithmetic (14 - bitdepth intermediate bits, prep bias 8192). The 16-bit intermediates ("tmp" buffers: mct, avg, w_avg, mask,
//    w_mask, warp8x8t) are therefore only meaningful to the functions of this file, with the same bitdepth_max.
//  - PixelView arguments: a source view points at the sample that dav1d's `src` pointer points at (the block's top-left sample, not the
//    sample minus the filter taps); the filters read up to 3 samples before and 4 after it in each direction. Strides are in samples.
//    A view is checked on every access: reaching outside the plane stops the OS, so use emu_edge() to build a padded copy of a
//    reference block that is not completely inside its picture (see "calling pattern" below).
//  - "tmp" buffers and masks are contiguous rows of `w` entries (stride w), as in dav1d. They must hold at least w * h entries.
//  - w must be in 1..128 and h in 1..128 (blocks are at most 128x128); a call with other sizes does nothing.
//  - mx, my are the 1/16 sample positions of the unscaled functions (0..15), for chroma already shifted left by one if the plane is
//    subsampled (mc() in dav1d's recon_tmpl.c passes `mx << !ss_hor`). For the scaled functions mx, my are positions in 1/1024 units
//    inside the first sample (0..1023) and dx, dy are the steps in 1/1024 units.
//
// Calling pattern (what recon_tmpl.c's mc() does)
//  - unscaled: if the block, grown by 3 samples before and 4 after for each non-zero fraction, is inside the reference picture, call
//    mc()/mct() with a view whose origin is at (dx, dy). Otherwise call emu_edge(bw + !!mx * 7, bh + !!my * 7, w, h, dx - !!mx * 3,
//    dy - !!my * 3, scratch, ref_plane) with a scratch view of stride 192 and pass scratch.offset(!!mx * 3, !!my * 3).
//  - scaled: the same with the block's reference area (left/top/right/bottom), a scratch stride of 320 and an offset of (3, 3).
//  - warp: emu_edge(15, 15, width, height, dx - 3, dy - 3, scratch (stride 32), ref_plane), then warp8x8()/warp8x8t() on
//    scratch.offset(3, 3).
namespace hardware {
namespace av1 {

// The 1/16 sample filters. `filter_2d` is dav1d's Filter2d (the horizontal and the vertical filter type, or bilinear).
// Unscaled prediction written to dst as pixels (dav1d: dsp->mc.mc[filter_2d]).
void mc(Filter2d filter_2d, PixelView dst, PixelView src, int w, int h, int mx, int my, int bitdepth_max);
// The same, written to the 16-bit intermediate buffer tmp (w * h entries) for compound prediction (dsp->mc.mct[filter_2d]).
void mct(Filter2d filter_2d, ArrayView<int16_t> tmp, PixelView src, int w, int h, int mx, int my, int bitdepth_max);
// Scaled references (dsp->mc.mc_scaled / mct_scaled).
void mc_scaled(Filter2d filter_2d, PixelView dst, PixelView src, int w, int h, int mx, int my, int dx, int dy, int bitdepth_max);
void mct_scaled(Filter2d filter_2d, ArrayView<int16_t> tmp, PixelView src, int w, int h, int mx, int my, int dx, int dy, int bitdepth_max);

// Compound prediction: dst = (tmp1 + tmp2) / 2 and the weighted forms. weight is 0..16 (weight for tmp1). mask is 0..64 per sample.
void avg(PixelView dst, ArrayView<const int16_t> tmp1, ArrayView<const int16_t> tmp2, int w, int h, int bitdepth_max);
void w_avg(PixelView dst, ArrayView<const int16_t> tmp1, ArrayView<const int16_t> tmp2, int w, int h, int weight, int bitdepth_max);
void mask(PixelView dst, ArrayView<const int16_t> tmp1, ArrayView<const int16_t> tmp2, int w, int h, ArrayView<const uint8_t> mask, int bitdepth_max);
// Difference-weighted compound (dsp->mc.w_mask[layout]): also writes the mask for the chroma planes, subsampled according to
// ss_hor/ss_ver (444: 0/0, 422: 1/0, 420: 1/1; dav1d indexes w_mask[] by 0, 1, 2 = 444, 422, 420). `mask` receives
// (w >> ss_hor) * (h >> ss_ver) entries. w must be even if ss_hor is set, h even if ss_ver is set. sign is the stream's mask_sign.
void w_mask(int ss_hor, int ss_ver, PixelView dst, ArrayView<const int16_t> tmp1, ArrayView<const int16_t> tmp2, int w, int h, ArrayView<uint8_t> mask,
            int sign, int bitdepth_max);

// dst = blend(dst, tmp, mask) with a w * h mask (inter-intra and wedge inter-intra). tmp has stride w.
void blend(PixelView dst, ArrayView<const Pixel> tmp, int w, int h, ArrayView<const uint8_t> mask);
// Overlapped block motion compensation: blend_v for the left neighbour (the left 3/4 of the w columns), blend_h for the top neighbour
// (the top 3/4 of the h rows). tmp has stride w. w, h at most 64.
void blend_v(PixelView dst, ArrayView<const Pixel> tmp, int w, int h);
void blend_h(PixelView dst, ArrayView<const Pixel> tmp, int w, int h);

// Warped motion for one 8x8 block. abcd is WarpedMotionParams::u.abcd (alpha, beta, gamma, delta); mx, my as computed by
// recon_tmpl.c's warp_affine(). warp8x8t writes to tmp with stride tmp_stride entries.
void warp8x8(PixelView dst, PixelView src, ArrayView<const int16_t> abcd, int mx, int my, int bitdepth_max);
void warp8x8t(ArrayView<int16_t> tmp, int64_t tmp_stride, PixelView src, ArrayView<const int16_t> abcd, int mx, int my, int bitdepth_max);

// Copies the bw x bh block whose top-left corner is (x, y) in the plane `ref` (origin at sample (0, 0), size iw x ih) to dst, replacing
// samples outside the plane by the nearest sample inside it. bw may be up to 320 and bh up to 320 depending on the caller's scratch
// size (dst is checked). Needs bw >= 1 and bh >= 1.
void emu_edge(int bw, int bh, int iw, int ih, int x, int y, PixelView dst, PixelView ref);

// Super-resolution horizontal upscaling of h rows from src_w to dst_w samples (dsp->mc.resize). dx and mx0 are
// FrameContext::resize_step / resize_start.
void resize(PixelView dst, PixelView src, int dst_w, int h, int src_w, int dx, int mx0, int bitdepth_max);

}  // namespace av1
}  // namespace hardware
