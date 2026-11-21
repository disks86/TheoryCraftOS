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

// The block-compressed texture formats BC1, BC2 and BC3 (DXT1, DXT3 and DXT5 of S3TC): each 4 x 4 pixels are one block of 8 (BC1) or 16 bytes, rows of blocks top to bottom.
// A device with graphics::kFeatureCompressedFormats samples them as they are (virgl); for the others the memory manager decodes them into 32-bit pixels when it uploads them
// (decode_blocks). The decoding is the one the Direct3D 10 specification gives: the two 5:6:5 endpoints expanded by bit replication, the interpolated colours rounded
// as (2a + b + 1) / 3 and (a + b + 1) / 2, BC1's three-colour mode with transparent black when the first endpoint is not greater, BC2's 4-bit alphas times 17, BC3's
// eight- and six-value alpha ramps.
#pragma once

#include "Graphics/GraphicsTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace graphics {

namespace bc_detail {

// The bytes of a block are read through this: a view that is too short is refused before (decode_blocks checks the size), so every index is inside.
inline uint32_t le16(ConstByteView b, uint64_t at) { return uint32_t{b[at]} | (uint32_t{b[at + 1]} << 8); }
inline uint32_t le32(ConstByteView b, uint64_t at) { return le16(b, at) | (le16(b, at + 2) << 16); }

// A 5:6:5 colour as 8-bit channels (the high bits repeated in the low ones).
struct Rgb { uint32_t r, g, b; };
inline Rgb expand565(uint32_t c) {
  const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}
inline uint32_t pack(uint32_t a, Rgb c) { return (a << 24) | (c.r << 16) | (c.g << 8) | c.b; }

// The four colours of a colour block (BC1's, or the colour half of BC2 / BC3, which always has four colours) as 0xAARRGGBB with alpha 255; `three` gets BC1's
// three-colour mode when the first endpoint is not greater than the second (the fourth colour is then transparent black).
inline void palette(uint32_t c0, uint32_t c1, bool allow_three, uint32_t out[4]) {
  const Rgb a = expand565(c0), b = expand565(c1);
  out[0] = pack(255, a);
  out[1] = pack(255, b);
  if (c0 > c1 || !allow_three) {
    out[2] = pack(255, {(2 * a.r + b.r + 1) / 3, (2 * a.g + b.g + 1) / 3, (2 * a.b + b.b + 1) / 3});
    out[3] = pack(255, {(a.r + 2 * b.r + 1) / 3, (a.g + 2 * b.g + 1) / 3, (a.b + 2 * b.b + 1) / 3});
  } else {
    out[2] = pack(255, {(a.r + b.r + 1) / 2, (a.g + b.g + 1) / 2, (a.b + b.b + 1) / 2});
    out[3] = 0;
  }
}

// BC3's alpha ramp: eight values from two endpoints, or six and 0 and 255.
inline void alpha_ramp(uint32_t a0, uint32_t a1, uint32_t out[8]) {
  out[0] = a0;
  out[1] = a1;
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) out[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
  } else {
    for (uint32_t i = 1; i < 5; ++i) out[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
    out[6] = 0;
    out[7] = 255;
  }
}

}  // namespace bc_detail

constexpr bool is_compressed_format(Format f) {
  return f == Format::Bc1Unorm || f == Format::Bc2Unorm || f == Format::Bc3Unorm || f == Format::Bc1Srgb || f == Format::Bc2Srgb || f == Format::Bc3Srgb;
}
// The bytes of one 4 x 4 block (0 for a format that is not block-compressed).
constexpr uint32_t compressed_block_bytes(Format f) {
  return f == Format::Bc1Unorm || f == Format::Bc1Srgb ? 8 : is_compressed_format(f) ? 16 : 0;
}
// The bytes of the blocks of a w x h image, and of one row of blocks.
constexpr uint64_t compressed_row_bytes(Format f, uint32_t w) { return uint64_t{(w + 3) / 4} * compressed_block_bytes(f); }
constexpr uint64_t compressed_image_bytes(Format f, uint32_t w, uint32_t h) { return compressed_row_bytes(f, w) * ((h + 3) / 4); }
// The 32-bit format the pixels of a compressed format are decoded into.
constexpr Format decoded_format(Format f) { return f == Format::Bc1Srgb || f == Format::Bc2Srgb || f == Format::Bc3Srgb ? Format::B8G8R8A8Srgb : Format::B8G8R8A8Unorm; }

// Decodes the blocks of a w x h image (`blocks`: compressed_image_bytes of them, rows of blocks top to bottom) into w * h pixels, 0xAARRGGBB, rows top to bottom (the
// blocks of the right and bottom edges are cut to the image). False when a view has the wrong size or the format is not a compressed one; `out` is then unchanged.
inline bool decode_blocks(Format f, ConstByteView blocks, uint32_t w, uint32_t h, ArrayView<uint32_t> out) {
  using namespace bc_detail;
  const uint32_t block = compressed_block_bytes(f);
  if (block == 0 || w == 0 || h == 0 || blocks.size() != compressed_image_bytes(f, w, h) || out.size() != uint64_t{w} * h) return false;
  const uint32_t across = (w + 3) / 4, down = (h + 3) / 4;
  const bool bc1 = block == 8, bc2 = f == Format::Bc2Unorm || f == Format::Bc2Srgb;
  for (uint32_t by = 0; by < down; ++by) {
    for (uint32_t bx = 0; bx < across; ++bx) {
      const ConstByteView b = blocks.subview((uint64_t{by} * across + bx) * block, block);
      const uint64_t colour_at = bc1 ? 0 : 8;
      uint32_t colours[4];
      palette(le16(b, colour_at), le16(b, colour_at + 2), bc1, colours);
      const uint32_t indices = le32(b, colour_at + 4);
      uint32_t ramp[8] = {};
      uint64_t alpha_bits = 0;
      if (!bc1 && !bc2) {
        alpha_ramp(b[0], b[1], ramp);
        for (uint32_t i = 0; i < 6; ++i) alpha_bits |= uint64_t{b[2 + i]} << (8 * i);
      }
      for (uint32_t py = 0; py < 4; ++py) {
        const uint32_t y = by * 4 + py;
        if (y >= h) break;
        for (uint32_t px = 0; px < 4; ++px) {
          const uint32_t x = bx * 4 + px;
          if (x >= w) break;
          const uint32_t n = py * 4 + px;
          uint32_t c = colours[(indices >> (2 * n)) & 3];
          if (bc2) {
            const uint32_t a = (b[n / 2] >> (4 * (n & 1))) & 15;
            c = (c & 0xFFFFFFu) | ((a * 17) << 24);
          } else if (!bc1) {
            c = (c & 0xFFFFFFu) | (ramp[(alpha_bits >> (3 * n)) & 7] << 24);
          }
          out[uint64_t{y} * w + x] = c;
        }
      }
    }
  }
  return true;
}

}  // namespace graphics
