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

#include "Hardware/VideoCodec/Software/Av1Types.hpp"

namespace hardware {
namespace av1 {

// AV1 film grain synthesis: a port of dav1d's filmgrain_tmpl.c and fg_apply_tmpl.c (bit-exact).
//
// The decoded reference picture is never modified: the grain is applied while copying `src` to `dst`.
// The caller supplies both pictures:
//   * `plane[0]` is luma, `plane[1]`/`plane[2]` are Cb/Cr (left empty for PIXEL_LAYOUT_I400).
//   * `width`/`height` are the luma dimensions of the picture; chroma planes are ((w + ssx) >> ssx) x ((h + ssy) >> ssy)
//     (ssx = layout != I444, ssy = layout == I420). Every PixelView must be able to address those samples
//     (checked accesses, so a too small plane is a bounds violation, not a silent overrun).
//   * `src` and `dst` must not share sample storage. Strides are in samples and may differ between src and dst.
//   * Samples must be below 1 << bitdepth. `bitdepth` is 8, 10 or 12.
//   * Planes that the grain does not touch are copied (width x height samples only, not any stride padding).
struct FilmGrainPicture {
  PixelView plane[3];
  int width = 0;
  int height = 0;
  PixelLayout layout = PIXEL_LAYOUT_I420;
  int bitdepth = 8;
};

// `data` is frame_hdr->film_grain.data of the frame being output. `matrix_is_identity` is (seq_hdr->mtrx == MC_IDENTITY).
// Returns false (and writes nothing) if the arguments are inconsistent (bad bit depth/size/layout or invalid point tables).
bool apply_film_grain(const FilmGrainData& data, bool matrix_is_identity, const FilmGrainPicture& src,
                      const FilmGrainPicture& dst);

}  // namespace av1
}  // namespace hardware
