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
#include "Hardware/VideoCodec/Intel/IntelMfxCommands.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"

namespace hardware {
namespace intel_mfx {

// Byte offset, inside a Y-tiled surface, of the byte at (`x_byte`, `row`). Y tiles are 128 bytes wide and 32 rows high (4 KiB); inside a tile the
// bytes sit in 16-byte spans, a column of 32 spans after another; tiles go left to right, then down. (Gen8 and later use no bit-6 swizzling.)
constexpr uint64_t y_tile_offset(uint32_t pitch, uint32_t x_byte, uint32_t row) {
  return (uint64_t{row} / 32) * pitch * 32 + (uint64_t{row} % 32) * 16 + (uint64_t{x_byte} / 16) * 512 + (x_byte % 16);
}

// Converts the engine's decoded NV12 surface to a linear I420 frame of `width` x `height` (the cropped size), which must already be allocated in `out`. The
// frame starts at (`left`, `top`) in the surface (even, as the chroma planes are half size). False if the sizes do not fit the layout or `tiled` is smaller than
// the surface.
bool detile_nv12_to_i420(ConstByteView tiled, const Nv12TiledLayout& layout, uint32_t width, uint32_t height, VideoFrameBuffer& out, uint32_t left = 0, uint32_t top = 0);

}  // namespace intel_mfx
}  // namespace hardware
