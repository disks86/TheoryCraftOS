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

#include "Graphics/Logo/Logo.hpp"
#include "Media/ImageCodecs.hpp"

namespace graphics {

bool Logo::load(ConstByteView png) { return media::decode_png(png, &image_) && image_.width() && image_.height(); }

bool Logo::render(uint32_t size, uint32_t background, ArrayView<uint32_t> out) const {
  if (!loaded() || size == 0 || out.size() < uint64_t{size} * size) return false;
  const uint32_t sw = image_.width(), sh = image_.height();
  const ArrayView<uint32_t> source = image_.frame(0);
  for (uint32_t y = 0; y < size; ++y) {
    // The source rows and columns this pixel covers (at least one, even when enlarging).
    const uint32_t y0 = static_cast<uint32_t>(uint64_t{y} * sh / size);
    const uint32_t y1 = static_cast<uint32_t>(uint64_t{y + 1} * sh / size);
    for (uint32_t x = 0; x < size; ++x) {
      const uint32_t x0 = static_cast<uint32_t>(uint64_t{x} * sw / size);
      const uint32_t x1 = static_cast<uint32_t>(uint64_t{x + 1} * sw / size);
      // Alpha-weighted sums, so that transparent pixels' colors do not bleed in.
      uint64_t a = 0, r = 0, g = 0, b = 0, count = 0;
      for (uint32_t sy = y0; sy < (y1 > y0 ? y1 : y0 + 1) && sy < sh; ++sy) {
        for (uint32_t sx = x0; sx < (x1 > x0 ? x1 : x0 + 1) && sx < sw; ++sx) {
          const uint32_t p = source[uint64_t{sy} * sw + sx];
          const uint64_t pa = p >> 24;
          a += pa;
          r += pa * ((p >> 16) & 0xFF);
          g += pa * ((p >> 8) & 0xFF);
          b += pa * (p & 0xFF);
          ++count;
        }
      }
      uint32_t color = background | 0xFF000000u;
      if (count && a) {
        const uint64_t opacity = a / count;   // 0..255 (a is the sum of the alphas)
        const uint64_t cr = r / a, cg = g / a, cb = b / a;
        const uint64_t br = (background >> 16) & 0xFF, bg = (background >> 8) & 0xFF, bb = background & 0xFF;
        color = 0xFF000000u | static_cast<uint32_t>(((cr * opacity + br * (255 - opacity)) / 255) << 16) |
                static_cast<uint32_t>(((cg * opacity + bg * (255 - opacity)) / 255) << 8) | static_cast<uint32_t>((cb * opacity + bb * (255 - opacity)) / 255);
      }
      out[uint64_t{y} * size + x] = color;
    }
  }
  return true;
}

}  // namespace graphics
