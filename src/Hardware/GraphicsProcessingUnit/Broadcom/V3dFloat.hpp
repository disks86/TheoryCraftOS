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
#include <stdint.h>

namespace hardware {

// Conversions from floats to the bit patterns the V3D code takes. They use floating point, so only code that may (the 3D device, the host tests) includes this;
// the V3D list, draw and shader code does not, and builds in the AArch64 image (which has no floating point).
inline uint32_t v3d_float_bits(float value) { return __builtin_bit_cast(uint32_t, value); }

// 0..1 r, g, b, a to the tile buffer's 8-bit pixel with R in the lowest byte (values outside 0..1 are clamped).
inline uint32_t v3d_pack_rgba8(const float rgba[4]) {
  uint32_t word = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    float v = rgba[i];
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    word |= (static_cast<uint32_t>(v * 255.0f + 0.5f) & 0xFF) << (8 * i);
  }
  return word;
}

}  // namespace hardware
