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

// The OS logo (assets/logo.png, 256x256 RGBA, drawn by scripts/gen_logo.py), built into the image. It is shown on the boot screen and on the
// faces of the 3D test's cube. Logo decodes it and scales it; the data itself is behind embedded_logo_png() so host tests can load the file
// from disk instead and use the same code.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Media/Image.hpp"
#include "Utility/ArrayView.hpp"

namespace graphics {

class Logo {
 public:
  explicit Logo(allocators::IAllocator& allocator) : image_(allocator) {}

  // Decodes a PNG; false (and nothing loaded) if it is not one, or damaged.
  bool load(ConstByteView png);
  bool loaded() const { return !image_.empty(); }
  uint32_t width() const { return image_.width(); }
  uint32_t height() const { return image_.height(); }

  // The logo scaled to size x size (a box filter: every source pixel counts, so shrinking does not alias) and laid over the opaque color
  // `background` (0x00RRGGBB), as 0xFFRRGGBB into `out`, which needs size * size pixels. False if nothing is loaded or `out` is too small.
  bool render(uint32_t size, uint32_t background, ArrayView<uint32_t> out) const;

 private:
  media::Image image_;
};

// The built-in PNG (empty only if the image was built without it).
ConstByteView embedded_logo_png();

}  // namespace graphics
