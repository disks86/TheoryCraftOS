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

// Layout of the 2x2 colour filter tile on the sensor, listed top-left first.
enum class BayerPattern : uint8_t { Rggb, Bggr, Grbg, Gbrg };

// One frame of raw sensor data: a single sample per pixel, each behind a colour
// filter of the Bayer pattern.
struct RawFrame {
  const uint16_t* pixels;
  uint32_t width, height;
  uint32_t stride;      // samples per row (>= width)
  uint8_t bit_depth;    // significant bits per sample, 8-16
  BayerPattern pattern;
};

// A finished image, one 0x00RRGGBB pixel each.
struct ImageBuffer {
  uint32_t* pixels;
  uint32_t width, height;
  uint32_t stride;  // pixels per row (>= width)
};

// Tuning applied by process(). Gains are Q8.8 fixed point (256 = 1.0).
struct IspSettings {
  uint16_t black_level = 0;     // sensor offset in raw units, subtracted first
  uint16_t gain_red = 256;
  uint16_t gain_green = 256;
  uint16_t gain_blue = 256;
  uint16_t gamma_x100 = 220;    // display gamma times 100 (220 = 2.2)
};

// Cheap whole-frame measurements for auto exposure / auto white balance.
struct IspStatistics {
  uint32_t mean_red, mean_green, mean_blue;  // average raw value per colour site
  uint8_t mean_luma;                         // overall brightness, 0-255
};

}  // namespace hardware
