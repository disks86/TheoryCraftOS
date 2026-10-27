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
#include "Graphics/GraphicsTypes.hpp"

namespace graphics {

// Mipmaps as the graphics devices share them: the sizes of the levels, and the box filter that makes a level from the one above it. Level k of an image w x h is
// max(1, w >> k) x max(1, h >> k); an image has at most mip_level_limit() levels (the last is 1 x 1 in its larger side's terms: the chain stops when both sides are 1).
constexpr uint32_t kMaxMipLevels = 13;   // 4096 x 4096 down to 1 x 1

constexpr uint32_t mip_extent(uint32_t size, uint32_t level) {
  const uint32_t s = level >= 32 ? 0 : size >> level;
  return s ? s : 1;
}

// How many levels an image of this size can have: until its larger side is 1.
inline uint32_t mip_level_limit(Extent2D extent) {
  uint32_t larger = extent.width > extent.height ? extent.width : extent.height;
  uint32_t levels = 1;
  while (larger > 1) {
    larger >>= 1;
    ++levels;
  }
  return levels;
}

// The 2 x 2 box filter: every destination pixel is the per-channel average (rounded half up) of the four source pixels at twice its position (the last row and column are
// repeated where the source has an odd size or is 1 wide). 0xAARRGGBB, straight alpha; `dst` is dw x dh, `src` sw x sh.
inline void mip_box_downsample(ArrayView<const uint32_t> src, uint32_t sw, uint32_t sh, ArrayView<uint32_t> dst, uint32_t dw, uint32_t dh) {
  for (uint32_t y = 0; y < dh; ++y) {
    const uint32_t y0 = 2 * y < sh ? 2 * y : sh - 1, y1 = 2 * y + 1 < sh ? 2 * y + 1 : sh - 1;
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = 2 * x < sw ? 2 * x : sw - 1, x1 = 2 * x + 1 < sw ? 2 * x + 1 : sw - 1;
      const uint32_t p[4] = {src[uint64_t{y0} * sw + x0], src[uint64_t{y0} * sw + x1], src[uint64_t{y1} * sw + x0], src[uint64_t{y1} * sw + x1]};
      uint32_t out = 0;
      for (uint32_t shift = 0; shift < 32; shift += 8) {
        const uint32_t sum = ((p[0] >> shift) & 0xFF) + ((p[1] >> shift) & 0xFF) + ((p[2] >> shift) & 0xFF) + ((p[3] >> shift) & 0xFF);
        out |= ((sum + 2) >> 2) << shift;
      }
      dst[uint64_t{y} * dw + x] = out;
    }
  }
}

// log2 of a positive finite float to about 1e-4: the exponent and a polynomial of the mantissa (no math library here). The level of detail of a texture lookup is
// computed from it; non-positive and non-finite values give a very small number (the base level).
inline float mip_log2(float x) {
  if (!(x > 1e-30f) || x > 1e30f) return -100.0f;
  uint32_t bits = __builtin_bit_cast(uint32_t, x);
  const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127;
  bits = (bits & 0x007FFFFFu) | 0x3F800000u;
  const float m = __builtin_bit_cast(float, bits);   // [1, 2)
  // log2(m) on [1, 2): the series of ln(1 + t) / ln 2 as a start (rough near t = 1), then two Newton steps on 2^r = m with 2^r as a degree 5 series.
  const float t = m - 1.0f;
  float r = t * (1.4426950f + t * (-0.7213475f + t * (0.4808983f + t * (-0.3606738f + t * (0.2885390f + t * -0.2404492f)))));
  for (int i = 0; i < 2; ++i) {
    const float e = 1.0f + r * (0.6931472f + r * (0.2402265f + r * (0.0555041f + r * (0.0096181f + r * 0.0013333f))));   // 2^r on [0, 1]
    r += (m - e) / (e * 0.6931472f);
  }
  return static_cast<float>(exponent) + r;
}

}  // namespace graphics
