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
#include "Allocators/IAllocator.hpp"
#include "Font/FontTypes.hpp"
#include "Font/TrueType.hpp"
#include "Utility/AllocatedView.hpp"

namespace font {

// An anti-aliased glyph image: 8-bit coverage (0 = outside, 255 = fully inside), one byte per pixel, rows from
// top to bottom with no padding (the pitch is `width`). A blit composites `colour * coverage` over the page.
//
// Placement: with the pen (baseline origin) at pixel (px, py), column 0 of row 0 is the pixel at
// (px + left, py - top). `left` and `top` are usually 0..2 and about the ascent respectively; either can be
// negative. A blank glyph (space) has width == height == 0.
class GlyphBitmap {
 public:
  explicit GlyphBitmap(allocators::IAllocator& allocator) : allocator_(&allocator) {}
  GlyphBitmap(GlyphBitmap&& other) { take(other); }
  GlyphBitmap& operator=(GlyphBitmap&& other) {
    if (this != &other) {
      release();
      take(other);
    }
    return *this;
  }
  GlyphBitmap(const GlyphBitmap&) = delete;
  GlyphBitmap& operator=(const GlyphBitmap&) = delete;
  ~GlyphBitmap() { release(); }

  int32_t left = 0;
  int32_t top = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  ConstByteView pixels() const { return ConstByteView(storage_); }
  uint32_t pitch() const { return width; }
  // Bytes of memory held (what the glyph cache charges).
  uint64_t memory_bytes() const { return storage_.size(); }
  bool empty() const { return width == 0 || height == 0; }

  // Allocates width*height zeroed bytes (replacing any pixels). False if memory ran out or the size is absurd.
  bool allocate(uint32_t w, uint32_t h);
  ByteView writable_pixels() { return storage_; }

 private:
  void release() {
    if (allocator_) release_view(*allocator_, storage_);
    width = height = 0;
  }
  void take(GlyphBitmap& other) {
    allocator_ = other.allocator_;
    storage_ = other.storage_;
    left = other.left;
    top = other.top;
    width = other.width;
    height = other.height;
    other.storage_ = ByteView();
    other.width = other.height = 0;
  }
  allocators::IAllocator* allocator_;
  ByteView storage_;
};

// Limits on what is rasterised: a glyph whose bitmap would be wider/taller than kMaxDimension or hold more than
// kMaxPixels pixels (a font size over roughly 700 px) comes out blank rather than exhausting memory.
constexpr uint32_t kMaxGlyphDimension = 1024;
constexpr uint32_t kMaxGlyphPixels = 1u << 18;

// Draws `outline` (font units) at `size` pixels (26.6) into `out` by signed-area accumulation: every outline
// edge adds the exact area it covers to the pixels it crosses, and a running sum along each row turns that
// into coverage (non-zero winding). `subpixel` (0..3) moves the glyph right by that many quarter pixels so text
// can be positioned between pixels. False only if memory ran out.
bool rasterize_outline(const Outline& outline, uint32_t units_per_em, Fixed size, uint32_t subpixel, allocators::IAllocator& allocator,
                       GlyphBitmap* out);

// Loads the outline of `glyph` and rasterises it. False if the glyph is missing or corrupt, or memory ran out.
bool rasterize_glyph(const TrueTypeFont& font, uint32_t glyph, Fixed size, uint32_t subpixel, allocators::IAllocator& allocator, GlyphBitmap* out);

}  // namespace font
