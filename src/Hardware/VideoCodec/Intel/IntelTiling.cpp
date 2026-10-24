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

bool detile_nv12_to_i420(ConstByteView tiled, const Nv12TiledLayout& layout, uint32_t width, uint32_t height, VideoFrameBuffer& out, uint32_t left, uint32_t top) {
  const VideoFormat& f = out.format();
  if (!out.valid() || f.pixel_format != PixelFormat::I420 || f.width != width || f.height != height) return false;
  if (width == 0 || height == 0 || (left & 1) || (top & 1) || tiled.size() < layout.size) return false;
  // Everything is checked in 64 bits: the numbers come from a stream.
  if (uint64_t{left} + width > layout.pitch || uint64_t{top} + height > layout.aligned_height) return false;
  const uint32_t chroma_w = (width + 1) / 2, chroma_h = (height + 1) / 2;
  const uint32_t chroma_left = left / 2, chroma_top = top / 2;
  if (uint64_t{layout.chroma_offset_rows} + chroma_top + chroma_h > layout.size / layout.pitch) return false;

  ByteView y_plane = out.plane(0), u_plane = out.plane(1), v_plane = out.plane(2);
  // Luma: pieces of up to 16 bytes, each contiguous in its tile (a piece ends where the 16-byte span of the tile ends).
  for (uint32_t row = 0; row < height; ++row) {
    uint32_t x = 0;
    while (x < width) {
      const uint32_t sx = left + x;
      uint32_t n = 16 - (sx & 15);
      if (n > width - x) n = width - x;
      const ByteView dst = y_plane.subview(uint64_t{row} * width + x, n);
      if (!dst.copy_from(tiled.subview(y_tile_offset(layout.pitch, sx, top + row), n))) return false;
      x += n;
    }
  }
  // Chroma: interleaved Cb Cr pairs, a span (8 pairs) at a time.
  for (uint32_t row = 0; row < chroma_h; ++row) {
    uint32_t x = 0;
    while (x < chroma_w) {
      const uint32_t sx = chroma_left + x;
      uint32_t pairs = 8 - (sx & 7);
      if (pairs > chroma_w - x) pairs = chroma_w - x;
      const ConstByteView src = tiled.subview(y_tile_offset(layout.pitch, sx * 2, layout.chroma_offset_rows + chroma_top + row), uint64_t{pairs} * 2);
      for (uint32_t k = 0; k < pairs; ++k) {
        u_plane[uint64_t{row} * chroma_w + x + k] = src[uint64_t{k} * 2];
        v_plane[uint64_t{row} * chroma_w + x + k] = src[uint64_t{k} * 2 + 1];
      }
      x += pairs;
    }
  }
  return true;
}

bool detile_nv12_to_planes(ConstByteView tiled, const Nv12TiledLayout& layout, uint32_t width, uint32_t height, ByteView y, uint32_t y_stride, ByteView u, uint32_t u_stride, ByteView v,
                           uint32_t v_stride, uint32_t first, uint32_t step) {
  if (width == 0 || height == 0 || (width & 1) || (height & 1) || step == 0 || step > 2 || first >= step || tiled.size() < layout.size) return false;
  if (width > layout.pitch || height > layout.aligned_height || y_stride < width || u_stride < width / 2 || v_stride < width / 2) return false;
  const uint32_t chroma_w = width / 2, chroma_h = height / 2;
  if (uint64_t{layout.chroma_offset_rows} + chroma_h > layout.size / layout.pitch) return false;
  for (uint32_t row = first; row < height; row += step) {
    for (uint32_t x = 0; x < width;) {
      uint32_t n = 16 - (x & 15);
      if (n > width - x) n = width - x;
      if (!y.subview(uint64_t{row} * y_stride + x, n).copy_from(tiled.subview(y_tile_offset(layout.pitch, x, row), n))) return false;
      x += n;
    }
  }
  for (uint32_t row = first; row < chroma_h; row += step) {
    for (uint32_t x = 0; x < chroma_w;) {
      uint32_t pairs = 8 - (x & 7);
      if (pairs > chroma_w - x) pairs = chroma_w - x;
      const ConstByteView src = tiled.subview(y_tile_offset(layout.pitch, x * 2, layout.chroma_offset_rows + row), uint64_t{pairs} * 2);
      ByteView ud = u.subview(uint64_t{row} * u_stride + x, pairs), vd = v.subview(uint64_t{row} * v_stride + x, pairs);
      for (uint32_t k = 0; k < pairs; ++k) {
        ud[k] = src[uint64_t{k} * 2];
        vd[k] = src[uint64_t{k} * 2 + 1];
      }
      x += pairs;
    }
  }
  return true;
}

}  // namespace intel_mfx
}  // namespace hardware
