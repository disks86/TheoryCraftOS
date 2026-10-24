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

// Warped motion parameter derivation of the AV1 decoder: a port of dav1d's src/warpmv.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d
// authors). Everything works on WarpedMotionParams::matrix (the six 16.16-style affine parameters, matrix[2..5] around 0x10000) and
// WarpedMotionParams::u.p (alpha, beta, gamma, delta). The 8x8 warp filters themselves are warp8x8()/warp8x8t() in Av1Mc.hpp.
namespace hardware {
namespace av1 {

// One neighbouring block sample for find_affine_int (dav1d's pts[i][2][2]): the position in the current frame (src) and in the
// reference frame (dst), both as {x, y} in 1/8 sample units.
struct WarpSample {
  int src[2];
  int dst[2];
};

// Computes alpha, beta, gamma and delta from wm->matrix. Returns 1 if the matrix is invalid (no shear is possible), 0 on success.
// Used for global motion and for local warps (dav1d_get_shear_params).
int get_shear_params(WarpedMotionParams* wm);

// Fits the affine model to the np neighbour samples (np <= 8 in practice) for the block of bw4 x bh4 4x4 units at (bx4, by4) whose
// motion vector is mv, and stores it in wm->matrix. Returns 1 if the fit is degenerate (singular), else 0. The caller then calls
// get_shear_params() (dav1d_find_affine_int).
int find_affine_int(ArrayView<const WarpSample> pts, int bw4, int bh4, Mv mv, WarpedMotionParams* wm, int bx4, int by4);

// Sets matrix[0] and matrix[1] (the translation) from mv for a block whose matrix[2..5] are already set (a warped block whose matrix
// was coded explicitly, dav1d_set_affine_mv2d). Call get_shear_params() afterwards.
void set_affine_mv2d(int bw4, int bh4, Mv mv, WarpedMotionParams* wm, int bx4, int by4);

}  // namespace av1
}  // namespace hardware
