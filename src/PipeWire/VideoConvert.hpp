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

// Video frames from one raw format to another of the same size, for a watcher that cannot take its producer's format (what PipeWire's videoconvert
// does in the program; here the server does it). The 8-bit formats: packed RGB (RGBx, BGRx, xRGB, xBGR, RGBA, BGRA, ARGB, ABGR, RGB, BGR),
// planar YUV 4:2:0 (I420, YV12), semi-planar (NV12, NV21), packed 4:2:2 (YUY2, UYVY, YVYU) and GRAY8. YUV is BT.601 limited range (as cameras
// send it), GRAY8 full range; chroma is averaged over the pixels it covers. The frames are VideoChannel's: planes with no padding, one after another.
#pragma once
#include <stdint.h>
#include "PipeWire/VideoChannel.hpp"
#include "Utility/ByteBuffer.hpp"

namespace pipewire {

// Whether raw frames of SPA format `from` can be turned into `to` (both of the formats above; the same format too).
bool can_convert_video(uint32_t from, uint32_t to);
// `in`: a whole frame of `from` (VideoChannel's layout); *out becomes the frame in `to` (`to`'s size must be `from`'s). False when the formats
// cannot be converted, the sizes differ, `in` is short, or there is no memory.
bool convert_video(const VideoFormat& from, ConstByteView in, const VideoFormat& to, ByteBuffer* out);

}  // namespace pipewire
