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

namespace font {

// Lengths in pixels as 26.6 fixed point (1/64 px), the same unit as the layout engine's LayoutUnit. Font sizes,
// advances, kerning, ascent and descent are all Fixed.
using Fixed = int32_t;
constexpr Fixed kFixedOne = 64;

constexpr Fixed fixed_from_px(int32_t px) { return px * kFixedOne; }
// Rounds to the nearest whole pixel (halves away from zero).
constexpr int32_t fixed_round(Fixed v) { return v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64); }

// value (in font units) * size / units_per_em, rounded to nearest, saturating to the Fixed range. `units_per_em`
// must not be 0 (TrueTypeFont guarantees it).
inline Fixed scale_units(int32_t value, Fixed size, uint32_t units_per_em) {
  const int64_t n = int64_t{value} * size;
  const int64_t half = units_per_em / 2;
  int64_t r = n >= 0 ? (n + half) / units_per_em : -((-n + half) / units_per_em);
  if (r > INT32_MAX) r = INT32_MAX;
  if (r < INT32_MIN) r = INT32_MIN;
  return static_cast<Fixed>(r);
}

enum class FontStyle : uint8_t { Normal, Italic, Oblique };

}  // namespace font
