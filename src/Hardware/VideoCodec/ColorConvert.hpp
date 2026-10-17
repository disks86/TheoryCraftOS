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
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"

namespace hardware {

// Converts a picture between the pixel formats (BT.601 matrix, integer arithmetic): `out` must be allocated for
// the target format and the same width and height as `in`. Going to YUV produces full-range pictures; going from
// YUV honours VideoFrame::limited_range. Chroma is averaged over 2x2 pixels going to I420/NV12 and
// repeated going back. False if the sizes differ or either side has no memory.
bool convert_frame(const VideoFrame& in, const VideoFrameBuffer& out);

// The same into a Bgra8888 picture that is not a VideoFrameBuffer (a graphics::Surface's pixels, say):
// `pitch` is in pixels. `dest` needs width, height and pitch big enough.
bool convert_to_bgra(const VideoFrame& in, uint32_t* dest, uint32_t pitch);

}  // namespace hardware
