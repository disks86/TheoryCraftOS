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

#include "Hardware/VideoCodec/Intel/IntelTiling.hpp"

namespace hardware {
namespace intel_mfx {

bool detile_nv12_to_i420(ConstByteView tiled, const Nv12TiledLayout& layout, uint32_t width, uint32_t height, VideoFrameBuffer& out) {
  const VideoFormat& f = out.format();
  if (!out.valid() || f.pixel_format != PixelFormat::I420 || f.width != width || f.height != height) return false;
  if (width == 0 || height == 0 || width > layout.pitch || height > layout.aligned_height || tiled.size() < layout.size) return false;
  const uint32_t chroma_w = (width + 1) / 2, chroma_h = (height + 1) / 2;
  if (uint64_t{layout.chroma_offset_rows} + chroma_h > layout.size / layout.pitch) return false;

  ByteView y_plane = out.plane(0), u_plane = out.plane(1), v_plane = out.plane(2);
  // Luma: spans of up to 16 bytes, each contiguous in the tile.
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t x = 0; x < width; x += 16) {
      const uint32_t n = width - x < 16 ? width - x : 16;
      const ByteView dst = y_plane.subview(uint64_t{row} * width + x, n);
      if (!dst.copy_from(tiled.subview(y_tile_offset(layout.pitch, x, row), n))) return false;
    }
  }
  // Chroma: interleaved Cb Cr pairs, a span of 8 pairs at a time.
  for (uint32_t row = 0; row < chroma_h; ++row) {
    for (uint32_t x = 0; x < chroma_w; x += 8) {
      const uint32_t pairs = chroma_w - x < 8 ? chroma_w - x : 8;
      const ConstByteView src = tiled.subview(y_tile_offset(layout.pitch, x * 2, layout.chroma_offset_rows + row), uint64_t{pairs} * 2);
      for (uint32_t k = 0; k < pairs; ++k) {
        u_plane[uint64_t{row} * chroma_w + x + k] = src[uint64_t{k} * 2];
        v_plane[uint64_t{row} * chroma_w + x + k] = src[uint64_t{k} * 2 + 1];
      }
    }
  }
  return true;
}

}  // namespace intel_mfx
}  // namespace hardware
