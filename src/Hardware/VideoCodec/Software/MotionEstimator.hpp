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

// Block motion estimation on the CPU (what Direct3D 12's ID3D12VideoMotionEstimator does here) and the luma interpolation it and the H.264 picture encoder share.
//
// Sub-sample positions are H.264's (8.4.2.2.1): half samples from the six-tap filter (1, -5, 20, 20, -5, 1) / 32, the centre half sample from the six-tap filter of the
// unrounded horizontal halves / 1024, quarter samples the rounded average of the two nearest whole or half samples. Samples outside the picture are the nearest edge sample.
//
// The search, for each block (8x8 or 16x16) of the current picture, finds the vector into the reference picture with the least sum of absolute differences (SAD) of luma:
//   1. candidates: (0, 0), the hint vector of the block (if a hint is given), and the vectors already found for the left, top and top-right blocks;
//   2. a full search of whole-sample vectors within +-kWholeSearchRange samples of the best candidate (and of (0, 0), if that is not the best);
//   3. refinement: the eight half-sample neighbours of the best vector, then the eight quarter-sample neighbours of the best of those.
// Ties keep the earlier (and so the smaller, closer to the predictors) vector. Vectors are in quarter samples, horizontal within +-2048 samples, vertical within +-512:
// `block at (x, y) of the current picture ~ block at (x + mv.x / 4, y + mv.y / 4) of the reference`.
#pragma once
#include "Utility/ArrayView.hpp"

namespace hardware {

// One 8-bit plane, `pitch` bytes a row.
struct LumaPlane {
  ConstByteView samples;
  int width = 0, height = 0;
  int pitch = 0;
};

// The prediction of a w x h block (at most 16 x 16) whose top-left sample is at (x + fx / 4, y + fy / 4) in `plane` (fx, fy 0 .. 3), into `out` (w * h, rows of w).
void h264_luma_prediction(const LumaPlane& plane, int x, int y, int fx, int fy, int w, int h, ByteView out);

struct MotionVector {
  int16_t x = 0, y = 0;   // quarter samples
};
struct MotionBlockResult {
  MotionVector vector;
  uint32_t sad = 0;
};

constexpr int kWholeSearchRange = 8;

// The vectors of every block of `current` into `reference` (same size), row by row: ceil(width / block) x ceil(height / block) of them in `out`. A block that reaches
// past the picture's edge is matched over the samples inside it. `hints` (empty, or one per block) are candidate vectors. False if the sizes do not fit.
[[nodiscard]] bool estimate_motion(const LumaPlane& current, const LumaPlane& reference, int block, ArrayView<const MotionVector> hints, ArrayView<MotionBlockResult> out);

}  // namespace hardware
