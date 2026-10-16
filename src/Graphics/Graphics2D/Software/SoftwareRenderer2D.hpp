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
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"

namespace graphics {

// CPU rasterizer over a Surface. present() copies the surface to the display
// GPU (if one was given) and asks it to show the frame. Never fails: this is
// the renderer every machine can run.
class SoftwareRenderer2D final : public IRenderer2D {
 public:
  explicit SoftwareRenderer2D(const Surface& target, hardware::IGraphicsProcessingUnit* display = nullptr)
      : target_(target), display_(display) {
    reset_clip();
  }

  const char* name() const override { return "software 2D renderer"; }
  bool accelerated() const override { return false; }
  Extent2D extent() const override { return {target_.width, target_.height}; }

  void set_clip(const Rect2D& clip) override;
  void reset_clip() override { clip_ = {0, 0, target_.width, target_.height}; }
  void set_blend(BlendMode mode) override { blend_ = mode; }

  void clear(uint32_t color) override;
  void put_pixel(int32_t x, int32_t y, uint32_t color) override;
  void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) override;
  void fill_rect(const Rect2D& rect, uint32_t color) override;
  void draw_rect(const Rect2D& rect, uint32_t color) override;
  void fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) override;
  void fill_triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t color) override;
  void draw_image(const Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) override;
  void draw_text(int32_t x, int32_t y, const char* text, uint32_t color) override;
  void present() override;

  const Surface& target() const { return target_; }

 private:
  void plot(int32_t x, int32_t y, uint32_t color);  // clipped, blended
  void span(int32_t y, int32_t x0, int32_t x1, uint32_t color);  // [x0, x1) on row y

  Surface target_;
  hardware::IGraphicsProcessingUnit* display_;
  Rect2D clip_;
  BlendMode blend_ = BlendMode::Alpha;
};

// Source-over of a straight-alpha source onto a destination, both 0xAARRGGBB.
uint32_t blend_over(uint32_t source, uint32_t destination);

}  // namespace graphics
