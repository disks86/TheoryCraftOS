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

#include "Hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"

namespace hardware {

bool FramebufferGpu::video_memory(uint64_t* total_bytes, uint64_t* used_bytes) const {
  if (memory_.empty()) return false;
  const uint64_t used = memory_.size();
  if (total_bytes) *total_bytes = video_total_ > used ? video_total_ : used;
  if (used_bytes) *used_bytes = used;
  return true;
}

void FramebufferGpu::set_framebuffer(ByteView memory, uint32_t width, uint32_t height, uint32_t pitch_bytes,
                                     bool bgr) {
  const bool fits = pitch_bytes >= uint64_t{width} * 4 && pitch_bytes % 4 == 0 &&
                    memory.size() >= uint64_t{height} * pitch_bytes &&
                    reinterpret_cast<uint64_t>(memory.data()) % 4 == 0;
  memory_ = fits ? memory.first(uint64_t{height} * pitch_bytes) : ByteView();
  allocation_ = fits ? memory : ByteView();
  width_ = fits ? width : 0;
  height_ = fits ? height : 0;
  pitch_ = pitch_bytes;
  bgr_ = bgr;
  dirty_x0_ = dirty_y0_ = dirty_x1_ = dirty_y1_ = 0;
}

void FramebufferGpu::mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  if (!w || !h) return;
  if (dirty_x0_ >= dirty_x1_) {
    dirty_x0_ = x, dirty_y0_ = y, dirty_x1_ = x + w, dirty_y1_ = y + h;
    return;
  }
  if (x < dirty_x0_) dirty_x0_ = x;
  if (y < dirty_y0_) dirty_y0_ = y;
  if (x + w > dirty_x1_) dirty_x1_ = x + w;
  if (y + h > dirty_y1_) dirty_y1_ = y + h;
}

bool FramebufferGpu::take_dirty(uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) {
  if (dirty_x0_ >= dirty_x1_ || memory_.empty()) return false;
  *x = dirty_x0_, *y = dirty_y0_, *w = dirty_x1_ - dirty_x0_, *h = dirty_y1_ - dirty_y0_;
  dirty_x0_ = dirty_y0_ = dirty_x1_ = dirty_y1_ = 0;
  return true;
}

uint32_t FramebufferGpu::native(uint32_t color) const {
  if (bgr_) return color & 0x00FFFFFF;  // little-endian 0x00RRGGBB is B, G, R, x in memory
  return ((color & 0xFF) << 16) | (color & 0xFF00) | ((color >> 16) & 0xFF);
}

ArrayView<uint32_t> FramebufferGpu::row(uint32_t y) const {
  return memory_.subview(static_cast<uint64_t>(y) * pitch_, uint64_t{width_} * 4).cast<uint32_t>();
}

void FramebufferGpu::put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (memory_.empty() || x >= width_ || y >= height_) return;
  row(y)[x] = native(color);
  mark_dirty(x, y, 1, 1);
}

void FramebufferGpu::fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  if (memory_.empty() || x >= width_ || y >= height_) return;
  if (w > width_ - x) w = width_ - x;
  if (h > height_ - y) h = height_ - y;
  const uint32_t value = native(color);
  for (uint32_t j = 0; j < h; ++j) row(y + j).subview(x, w).fill(value);
  mark_dirty(x, y, w, h);
}

void FramebufferGpu::copy_rect(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
                               uint32_t w, uint32_t h) {
  if (memory_.empty()) return;
  if (src_x >= width_ || src_y >= height_ || dst_x >= width_ || dst_y >= height_) return;
  uint32_t max_w = width_ - (src_x > dst_x ? src_x : dst_x);
  uint32_t max_h = height_ - (src_y > dst_y ? src_y : dst_y);
  if (w > max_w) w = max_w;
  if (h > max_h) h = max_h;
  mark_dirty(dst_x, dst_y, w, h);
  // Walk rows in the direction that never overwrites unread source rows.
  for (uint32_t j = 0; j < h; ++j) {
    uint32_t r = dst_y > src_y ? h - 1 - j : j;
    row(dst_y + r).subview(dst_x, w).copy_from(row(src_y + r).subview(src_x, w));
  }
}

void FramebufferGpu::blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels,
                          uint32_t pitch_pixels) {
  if (memory_.empty() || x >= width_ || y >= height_) return;
  if (w > width_ - x) w = width_ - x;
  if (h > height_ - y) h = height_ - y;
  for (uint32_t j = 0; j < h; ++j) {
    const ArrayView<uint32_t> out = row(y + j).subview(x, w);
    const uint32_t* in = pixels + uint64_t{j} * pitch_pixels;
    if (bgr_) {
      for (uint32_t i = 0; i < w; ++i) out[i] = in[i] & 0x00FFFFFF;
    } else {
      for (uint32_t i = 0; i < w; ++i) out[i] = native(in[i]);
    }
  }
  mark_dirty(x, y, w, h);
}

uint32_t FramebufferGpu::glyph_width() const { return Font8x16::kWidth; }
uint32_t FramebufferGpu::glyph_height() const { return Font8x16::kHeight; }

void FramebufferGpu::draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
  if (memory_.empty() || x >= width_ || y >= height_) return;
  const ConstByteView glyph = Font8x16::glyph(c);  // 16 rows, one byte each
  const uint32_t fg_n = native(fg), bg_n = native(bg);
  const uint32_t cols = (width_ - x < Font8x16::kWidth) ? width_ - x : Font8x16::kWidth;
  const uint32_t rows = (height_ - y < Font8x16::kHeight) ? height_ - y : Font8x16::kHeight;
  for (uint32_t j = 0; j < rows; ++j) {
    const ArrayView<uint32_t> pixels = row(y + j).subview(x, cols);
    for (uint32_t i = 0; i < cols; ++i) pixels[i] = (glyph[j] & (0x80 >> i)) ? fg_n : bg_n;
  }
  mark_dirty(x, y, cols, rows);
}

void FramebufferGpu::draw_text(uint32_t x, uint32_t y, const char* text, uint32_t fg,
                               uint32_t bg) {
  for (; *text; ++text, x += Font8x16::kWidth) draw_char(x, y, *text, fg, bg);
}

}  // namespace hardware
