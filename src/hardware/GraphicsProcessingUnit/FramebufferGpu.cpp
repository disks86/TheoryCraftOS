#include "hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "hardware/GraphicsProcessingUnit/Font8x16.hpp"

namespace hardware {

void FramebufferGpu::set_framebuffer(void* base, uint32_t width, uint32_t height,
                                     uint32_t pitch_bytes, bool bgr) {
  base_ = static_cast<uint8_t*>(base);
  width_ = width;
  height_ = height;
  pitch_ = pitch_bytes;
  bgr_ = bgr;
}

uint32_t FramebufferGpu::native(uint32_t color) const {
  if (bgr_) return color & 0x00FFFFFF;  // little-endian 0x00RRGGBB is B, G, R, x in memory
  return ((color & 0xFF) << 16) | (color & 0xFF00) | ((color >> 16) & 0xFF);
}

uint32_t* FramebufferGpu::row(uint32_t y) const {
  return reinterpret_cast<uint32_t*>(base_ + static_cast<uint64_t>(y) * pitch_);
}

void FramebufferGpu::put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (!base_ || x >= width_ || y >= height_) return;
  row(y)[x] = native(color);
}

void FramebufferGpu::fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  if (!base_ || x >= width_ || y >= height_) return;
  if (w > width_ - x) w = width_ - x;
  if (h > height_ - y) h = height_ - y;
  const uint32_t value = native(color);
  for (uint32_t j = 0; j < h; ++j) {
    uint32_t* p = row(y + j) + x;
    for (uint32_t i = 0; i < w; ++i) p[i] = value;
  }
}

void FramebufferGpu::copy_rect(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
                               uint32_t w, uint32_t h) {
  if (!base_) return;
  if (src_x >= width_ || src_y >= height_ || dst_x >= width_ || dst_y >= height_) return;
  uint32_t max_w = width_ - (src_x > dst_x ? src_x : dst_x);
  uint32_t max_h = height_ - (src_y > dst_y ? src_y : dst_y);
  if (w > max_w) w = max_w;
  if (h > max_h) h = max_h;
  // Walk rows in the direction that never overwrites unread source rows.
  for (uint32_t j = 0; j < h; ++j) {
    uint32_t r = dst_y > src_y ? h - 1 - j : j;
    __builtin_memmove(row(dst_y + r) + dst_x, row(src_y + r) + src_x, uint64_t{w} * 4);
  }
}

uint32_t FramebufferGpu::glyph_width() const { return Font8x16::kWidth; }
uint32_t FramebufferGpu::glyph_height() const { return Font8x16::kHeight; }

void FramebufferGpu::draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
  if (!base_ || x >= width_ || y >= height_) return;
  const uint8_t* glyph = Font8x16::glyph(c);
  const uint32_t fg_n = native(fg), bg_n = native(bg);
  const uint32_t cols = (width_ - x < Font8x16::kWidth) ? width_ - x : Font8x16::kWidth;
  const uint32_t rows = (height_ - y < Font8x16::kHeight) ? height_ - y : Font8x16::kHeight;
  for (uint32_t j = 0; j < rows; ++j) {
    uint32_t* p = row(y + j) + x;
    for (uint32_t i = 0; i < cols; ++i) p[i] = (glyph[j] & (0x80 >> i)) ? fg_n : bg_n;
  }
}

void FramebufferGpu::draw_text(uint32_t x, uint32_t y, const char* text, uint32_t fg,
                               uint32_t bg) {
  for (; *text; ++text, x += Font8x16::kWidth) draw_char(x, y, *text, fg, bg);
}

}  // namespace hardware
