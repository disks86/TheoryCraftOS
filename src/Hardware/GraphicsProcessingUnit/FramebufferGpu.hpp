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
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// Software drawing and text rendering on a linear 32-bit framebuffer. Concrete
// GPUs implement mode enumeration/switching and call set_framebuffer() whenever
// the visible buffer changes.
class FramebufferGpu : public IGraphicsProcessingUnit {
 public:
  uint32_t width() const override { return width_; }
  uint32_t height() const override { return height_; }

  void put_pixel(uint32_t x, uint32_t y, uint32_t color) override;
  void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) override;
  void copy_rect(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w,
                 uint32_t h) override;
  void clear(uint32_t color) override { fill_rect(0, 0, width_, height_, color); }

  uint32_t glyph_width() const override;
  uint32_t glyph_height() const override;
  void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) override;
  void draw_text(uint32_t x, uint32_t y, const char* text, uint32_t fg, uint32_t bg) override;

 protected:
  constexpr FramebufferGpu() = default;
  ~FramebufferGpu() = default;

  // bgr: memory order is B, G, R, x (the usual layout); otherwise R, G, B, x.
  // `memory` is the whole framebuffer; it must cover height * pitch_bytes bytes or no drawing happens.
  void set_framebuffer(ByteView memory, uint32_t width, uint32_t height, uint32_t pitch_bytes, bool bgr);

 private:
  uint32_t native(uint32_t color) const;
  // Row y as a view of `width_` pixels (checked against the framebuffer memory).
  ArrayView<uint32_t> row(uint32_t y) const;

  ByteView memory_;
  uint32_t width_ = 0, height_ = 0, pitch_ = 0;
  bool bgr_ = true;
};

}  // namespace hardware
