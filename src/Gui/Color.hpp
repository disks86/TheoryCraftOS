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

// Colour model conversions for the colour picker: opaque 0xAARRGGBB colours to and from
// hue / saturation / value, in integers (no floating point).
#pragma once
#include <stdint.h>

namespace gui {

struct Hsv {
  uint16_t hue = 0;        // 0 .. 359 degrees
  uint8_t saturation = 0;  // 0 .. 255
  uint8_t value = 0;       // 0 .. 255
};

constexpr uint32_t make_color(uint8_t r, uint8_t g, uint8_t b) {
  return 0xFF000000u | (uint32_t{r} << 16) | (uint32_t{g} << 8) | b;
}
constexpr uint8_t red_of(uint32_t argb) { return static_cast<uint8_t>(argb >> 16); }
constexpr uint8_t green_of(uint32_t argb) { return static_cast<uint8_t>(argb >> 8); }
constexpr uint8_t blue_of(uint32_t argb) { return static_cast<uint8_t>(argb); }

// The opaque colour for an HSV value.
constexpr uint32_t hsv_to_color(const Hsv& hsv) {
  const uint32_t h = hsv.hue % 360, s = hsv.saturation, v = hsv.value;
  const uint32_t region = h / 60, rest = (h % 60) * 255 / 60;  // 0..254 through the region
  const uint32_t p = v * (255 - s) / 255;
  const uint32_t q = v * (255 - s * rest / 255) / 255;
  const uint32_t t = v * (255 - s * (255 - rest) / 255) / 255;
  switch (region) {
    case 0: return make_color(static_cast<uint8_t>(v), static_cast<uint8_t>(t), static_cast<uint8_t>(p));
    case 1: return make_color(static_cast<uint8_t>(q), static_cast<uint8_t>(v), static_cast<uint8_t>(p));
    case 2: return make_color(static_cast<uint8_t>(p), static_cast<uint8_t>(v), static_cast<uint8_t>(t));
    case 3: return make_color(static_cast<uint8_t>(p), static_cast<uint8_t>(q), static_cast<uint8_t>(v));
    case 4: return make_color(static_cast<uint8_t>(t), static_cast<uint8_t>(p), static_cast<uint8_t>(v));
    default: return make_color(static_cast<uint8_t>(v), static_cast<uint8_t>(p), static_cast<uint8_t>(q));
  }
}

// The HSV of a colour (alpha ignored). The hue of a grey is 0.
constexpr Hsv color_to_hsv(uint32_t argb) {
  const int32_t r = red_of(argb), g = green_of(argb), b = blue_of(argb);
  const int32_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
  const int32_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
  const int32_t delta = max - min;
  Hsv hsv;
  hsv.value = static_cast<uint8_t>(max);
  if (max == 0 || delta == 0) return hsv;
  hsv.saturation = static_cast<uint8_t>((delta * 255 + max / 2) / max);
  int32_t hue;  // in degrees, may be negative before wrapping
  if (max == r) hue = 60 * (g - b) / delta;
  else if (max == g) hue = 120 + 60 * (b - r) / delta;
  else hue = 240 + 60 * (r - g) / delta;
  if (hue < 0) hue += 360;
  hsv.hue = static_cast<uint16_t>(hue % 360);
  return hsv;
}

// "#RRGGBB" and a NUL, into `out` (8 bytes).
inline void format_color(uint32_t argb, char* out) {
  static const char kDigits[] = "0123456789ABCDEF";
  out[0] = '#';
  const uint8_t parts[3] = {red_of(argb), green_of(argb), blue_of(argb)};
  for (int i = 0; i < 3; ++i) {
    out[1 + 2 * i] = kDigits[parts[i] >> 4];
    out[2 + 2 * i] = kDigits[parts[i] & 15];
  }
  out[7] = 0;
}

// Black or white, whichever reads better on top of `argb`.
constexpr uint32_t contrasting_text_color(uint32_t argb) {
  return (red_of(argb) * 299u + green_of(argb) * 587u + blue_of(argb) * 114u) / 1000 > 128 ? 0xFF000000u : 0xFFFFFFFFu;
}

}  // namespace gui
