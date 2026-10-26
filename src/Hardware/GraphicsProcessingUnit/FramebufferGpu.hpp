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
  void blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels,
            uint32_t pitch_pixels) override;

  // The framebuffer is all the video memory known about unless a subclass says there is more (set_video_memory_total).
  bool video_memory(uint64_t* total_bytes, uint64_t* used_bytes) const override;

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
  uint32_t pitch_bytes() const { return pitch_; }
  // Memory order B, G, R, x (the usual layout) rather than R, G, B, x.
  bool bgr() const { return bgr_; }
  // The size of the device's video memory when it is more than the visible framebuffer.
  void set_video_memory_total(uint64_t bytes) override { video_total_ = bytes; }
  bool framebuffer_range(uint64_t* base, uint64_t* bytes) const override {
    if (allocation_.empty() || !base || !bytes) return false;
    *base = reinterpret_cast<uint64_t>(allocation_.data());   // the firmware identity-maps its framebuffer: the virtual address is the physical one
    *bytes = allocation_.size();
    return true;
  }
  // The rectangle drawn on since the last call (and clears it); false if nothing was. For GPUs
  // whose screen lives in separate memory and needs the changes copied over.
  bool take_dirty(uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h);
  void mark_all_dirty() { mark_dirty(0, 0, width_, height_); }

 private:
  uint32_t native(uint32_t color) const;
  void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
  // Row y as a view of `width_` pixels (checked against the framebuffer memory).
  ArrayView<uint32_t> row(uint32_t y) const;

  ByteView memory_;
  ByteView allocation_;   // the whole buffer the firmware gave, of which memory_ is the part the mode shows
  uint32_t width_ = 0, height_ = 0, pitch_ = 0;
  uint64_t video_total_ = 0;
  bool bgr_ = true;
  uint32_t dirty_x0_ = 0, dirty_y0_ = 0, dirty_x1_ = 0, dirty_y1_ = 0;  // empty when x0 >= x1
};

}  // namespace hardware
