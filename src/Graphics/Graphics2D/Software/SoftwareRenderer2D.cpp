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

#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"

namespace graphics {

uint32_t blend_over(uint32_t s, uint32_t d) {
  const uint32_t sa = s >> 24;
  if (sa == 255) return s;
  if (sa == 0) return d;
  const uint32_t da = d >> 24;
  const uint32_t inv = 255 - sa;
  // Result alpha = sa + da * (1 - sa); color weighted by the destination's own coverage.
  const uint32_t oa = sa + (da * inv + 127) / 255;
  if (oa == 0) return 0;
  uint32_t out = oa << 24;
  for (int shift = 16; shift >= 0; shift -= 8) {
    const uint32_t sc = (s >> shift) & 0xFF, dc = (d >> shift) & 0xFF;
    const uint32_t num = sc * sa * 255 + dc * da * inv;
    uint32_t c = (num + oa * 127) / (oa * 255);
    if (c > 255) c = 255;
    out |= c << shift;
  }
  return out;
}

namespace {
inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }
inline float absf(float v) { return v < 0 ? -v : v; }
}  // namespace

void SoftwareRenderer2D::set_clip(const Rect2D& clip) {
  const int64_t x0 = clip.x > 0 ? clip.x : 0, y0 = clip.y > 0 ? clip.y : 0;
  int64_t x1 = static_cast<int64_t>(clip.x) + clip.width, y1 = static_cast<int64_t>(clip.y) + clip.height;
  if (x1 > target_.width) x1 = target_.width;
  if (y1 > target_.height) y1 = target_.height;
  clip_ = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0,
           y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
}

void SoftwareRenderer2D::plot(int32_t x, int32_t y, uint32_t color) {
  if (!target_.valid()) return;
  if (x < clip_.x || y < clip_.y || x >= clip_.x + static_cast<int32_t>(clip_.width) ||
      y >= clip_.y + static_cast<int32_t>(clip_.height))
    return;
  uint32_t* p = target_.row(static_cast<uint32_t>(y)) + x;
  *p = target_.encode(blend_ == BlendMode::Alpha ? blend_over(color, target_.decode(*p)) : color);
}

void SoftwareRenderer2D::span(int32_t y, int32_t x0, int32_t x1, uint32_t color) {
  if (!target_.valid() || y < clip_.y || y >= clip_.y + static_cast<int32_t>(clip_.height)) return;
  x0 = max32(x0, clip_.x);
  x1 = min32(x1, clip_.x + static_cast<int32_t>(clip_.width));
  if (x0 >= x1) return;
  uint32_t* p = target_.row(static_cast<uint32_t>(y));
  if (blend_ == BlendMode::Replace || (color >> 24) == 255) {
    const uint32_t v = target_.encode(color);
    for (int32_t x = x0; x < x1; ++x) p[x] = v;
  } else {
    for (int32_t x = x0; x < x1; ++x) p[x] = target_.encode(blend_over(color, target_.decode(p[x])));
  }
}

void SoftwareRenderer2D::clear(uint32_t color) {
  if (!target_.valid()) return;
  const uint32_t v = target_.encode(color);
  for (uint32_t y = 0; y < target_.height; ++y) {
    uint32_t* p = target_.row(y);
    for (uint32_t x = 0; x < target_.width; ++x) p[x] = v;
  }
}

void SoftwareRenderer2D::put_pixel(int32_t x, int32_t y, uint32_t color) { plot(x, y, color); }

void SoftwareRenderer2D::draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
  const int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;  // dy <= 0
  const int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int32_t err = dx + dy;
  for (;;) {
    plot(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    const int32_t e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void SoftwareRenderer2D::fill_rect(const Rect2D& r, uint32_t color) {
  const int64_t x1 = static_cast<int64_t>(r.x) + r.width, y1 = static_cast<int64_t>(r.y) + r.height;
  const int32_t ye = static_cast<int32_t>(y1 > 0x7FFFFFFF ? 0x7FFFFFFF : y1);
  const int32_t xe = static_cast<int32_t>(x1 > 0x7FFFFFFF ? 0x7FFFFFFF : x1);
  for (int32_t y = max32(r.y, clip_.y); y < min32(ye, clip_.y + static_cast<int32_t>(clip_.height)); ++y)
    span(y, r.x, xe, color);
}

void SoftwareRenderer2D::draw_rect(const Rect2D& r, uint32_t color) {
  if (!r.width || !r.height) return;
  const int32_t x1 = r.x + static_cast<int32_t>(r.width) - 1, y1 = r.y + static_cast<int32_t>(r.height) - 1;
  span(r.y, r.x, x1 + 1, color);
  if (y1 != r.y) span(y1, r.x, x1 + 1, color);
  for (int32_t y = r.y + 1; y < y1; ++y) {
    plot(r.x, y, color);
    if (x1 != r.x) plot(x1, y, color);
  }
}

void SoftwareRenderer2D::fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) {
  const int64_t r = radius, r2 = r * r;
  for (int64_t dy = -r; dy <= r; ++dy) {
    int64_t dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r2) ++dx;
    span(cy + static_cast<int32_t>(dy), cx - static_cast<int32_t>(dx), cx + static_cast<int32_t>(dx) + 1, color);
  }
}

void SoftwareRenderer2D::fill_triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t color) {
  const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (absf(area) < 1e-6f) return;
  float min_x = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
  float max_x = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
  float min_y = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
  float max_y = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
  int32_t y0 = max32(static_cast<int32_t>(min_y), clip_.y), y1 = min32(static_cast<int32_t>(max_y) + 1, clip_.y + static_cast<int32_t>(clip_.height));
  int32_t x0 = max32(static_cast<int32_t>(min_x), clip_.x), x1 = min32(static_cast<int32_t>(max_x) + 1, clip_.x + static_cast<int32_t>(clip_.width));
  const float sign = area < 0 ? -1.0f : 1.0f;
  for (int32_t y = y0; y < y1; ++y) {
    // Run of covered pixel centers on this row.
    int32_t run_start = -1;
    for (int32_t x = x0; x < x1; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float w0 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * sign;
      const float w1 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * sign;
      const float w2 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * sign;
      const bool inside = w0 >= 0 && w1 >= 0 && w2 >= 0;
      if (inside && run_start < 0) run_start = x;
      if (!inside && run_start >= 0) {
        span(y, run_start, x, color);
        run_start = -1;
      }
    }
    if (run_start >= 0) span(y, run_start, x1, color);
  }
}

void SoftwareRenderer2D::draw_image(const Surface& source, const Rect2D& rect, int32_t x, int32_t y) {
  if (!source.valid() || !target_.valid()) return;
  for (uint32_t j = 0; j < rect.height; ++j) {
    const int64_t sy = static_cast<int64_t>(rect.y) + j;
    if (sy < 0 || sy >= source.height) continue;
    for (uint32_t i = 0; i < rect.width; ++i) {
      const int64_t sx = static_cast<int64_t>(rect.x) + i;
      if (sx < 0 || sx >= source.width) continue;
      plot(x + static_cast<int32_t>(i), y + static_cast<int32_t>(j), source.decode(source.row(static_cast<uint32_t>(sy))[sx]));
    }
  }
}

void SoftwareRenderer2D::draw_text(int32_t x, int32_t y, const char* text, uint32_t color) {
  for (; *text; ++text, x += static_cast<int32_t>(hardware::Font8x16::kWidth)) {
    const ConstByteView glyph = hardware::Font8x16::glyph(*text);
    for (uint32_t j = 0; j < hardware::Font8x16::kHeight; ++j)
      for (uint32_t i = 0; i < hardware::Font8x16::kWidth; ++i)
        if (glyph[j] & (0x80 >> i)) plot(x + static_cast<int32_t>(i), y + static_cast<int32_t>(j), color);
  }
}

void SoftwareRenderer2D::present() {
  if (display_ && target_.valid()) {
    display_->blit(0, 0, target_.width, target_.height, target_.pixels.data(), target_.pitch);
    display_->present();
  }
}

}  // namespace graphics
