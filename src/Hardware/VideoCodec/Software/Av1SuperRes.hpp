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

// AV1 super-resolution: the horizontal upscaling of a decoded (deblocked, CDEF-filtered) frame to its final width. A port of the
// frame-level setup in dav1d's decode.c (resize_step / resize_start) and of the use of the `resize` kernel in recon_tmpl.c
// (dav1d_filter_sbrow_resize) and lf_apply_tmpl.c (backup_lpf), in whole-frame form. Bit-exact with dav1d.
//
// Frame-level recipe when frame_hdr.super_res.enabled (width[0] != width[1]):
//   per plane (luma, then chroma with its subsampled widths)
//     in_w  = plane width of the coded (downscaled) frame = Round2(width[0], ss_hor)
//     out_w = plane width of the upscaled frame            = Round2(width[1], ss_hor)
//     src_w = super_res_src_width(width[0], ss_hor)
//     SuperResStep s = super_res_step(in_w, out_w);
//     upscale_plane(dst, src, out_w, src_w, plane_height, s, bitdepth_max);
// Upscale the CDEF output into the final picture and, if loop restoration runs on the plane, the deblocked (pre-CDEF) picture into
// the second picture that loop_restoration_plane() reads at the stripe boundaries.
// The reference frames for motion compensation are the upscaled pictures.

namespace hardware {
namespace av1 {

// Step and start phase of the horizontal scaler in 1/16384 sample units (dav1d: f->resize_step[], f->resize_start[]).
struct SuperResStep {
  int step = 0;
  int start = 0;
};

// in_w/out_w are the plane widths before and after upscaling (out_w > in_w).
SuperResStep super_res_step(int in_w, int out_w);

// Width of the source area the scaler may read (it clamps to src_w - 1): the decoded width rounded up to a multiple of 8 luma
// samples, subsampled (dav1d: (4 * f->bw + ss_hor) >> ss_hor). `frame_width` is the luma width before upscaling (width[0]).
inline int super_res_src_width(int frame_width, int ss_hor) { return ((((frame_width + 7) >> 3) << 3) + ss_hor) >> ss_hor; }

// Upscales `rows` rows of src (src_w readable samples per row) to dst_w samples per row. dst and src must be different pictures.
// This is dav1d's mc.resize (the same eight-tap kernel as the `resize` of the motion compensation package, prototype
// resize(PixelView dst, PixelView src, int dst_w, int src_w, int h, int dx, int mx0, int bitdepth_max)); a private copy is used here.
// Returns false (nothing written) on invalid arguments.
bool upscale_plane(PixelView dst, PixelView src, int dst_w, int src_w, int rows, SuperResStep step, int bitdepth_max);

}  // namespace av1
}  // namespace hardware
