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
#include "Hardware/Printer/PrinterTypes.hpp"

namespace hardware {

// How grey levels become dots. A laser engine only makes toner or no toner, so tones are made from patterns of dots.
enum class Halftone : uint8_t {
  Threshold,       // black below mid grey: text and line art
  Ordered,         // an 8x8 Bayer matrix: stable patterns, no artefacts between pages
  ErrorDiffusion,  // Floyd-Steinberg: the best photographs
};
enum class Fit : uint8_t { Contain, Stretch };

// Draws `source` (0xAARRGGBB pixels, transparency over white) into `page`, scaled to fit it (keeping the proportions and
// centred, or stretched to fill) and halftoned. Every dot of the page is written; what the picture does not cover is white.
bool render_surface(const graphics::Surface& source, PageBitmap* page, Fit fit, Halftone method, allocators::IAllocator& allocator);

// TIFF PackBits compression of `count` bytes into `out` (which needs room for count + count / 128 + 1 bytes); returns the
// compressed size. PCL raster graphics "compression mode 2" is this.
uint32_t packbits(const uint8_t* data, uint32_t count, uint8_t* out);

}  // namespace hardware
