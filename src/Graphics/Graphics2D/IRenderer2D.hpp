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

// 2D rendering: shapes, images and text onto one render target. Colors are
// 0xAARRGGBB with straight alpha; coordinates are pixels from the top left and
// everything is clipped to the target and the current clip rectangle.
// Implementations: SoftwareRenderer2D (CPU, works everywhere) and, when a GPU
// with a 3D engine is present, the accelerated one picked by SystemGraphics.
#pragma once
#include "Graphics/GraphicsTypes.hpp"

namespace graphics {

enum class BlendMode : uint8_t {
  Replace,  // write the source color as is
  Alpha,    // source over destination using the source alpha
};

class IRenderer2D {
 public:
  virtual const char* name() const = 0;
  // True if the drawing is done by a GPU rather than the CPU.
  virtual bool accelerated() const = 0;
  virtual Extent2D extent() const = 0;

  virtual void set_clip(const Rect2D& clip) = 0;
  virtual void reset_clip() = 0;
  virtual void set_blend(BlendMode mode) = 0;

  virtual void clear(uint32_t color) = 0;
  virtual void put_pixel(int32_t x, int32_t y, uint32_t color) = 0;
  virtual void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) = 0;
  virtual void fill_rect(const Rect2D& rect, uint32_t color) = 0;
  virtual void draw_rect(const Rect2D& rect, uint32_t color) = 0;  // 1 pixel outline
  virtual void fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) = 0;
  virtual void fill_triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t color) = 0;
  // Copies `source_rect` of `source` (0xAARRGGBB through source.decode) with its top left at (x, y).
  virtual void draw_image(const Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) = 0;
  // Fixed 8x16 font, no wrapping; '\n' is not interpreted. Background pixels are left alone.
  virtual void draw_text(int32_t x, int32_t y, const char* text, uint32_t color) = 0;

  // Makes everything drawn so far visible.
  virtual void present() = 0;

 protected:
  constexpr IRenderer2D() = default;
  ~IRenderer2D() = default;
};

}  // namespace graphics
