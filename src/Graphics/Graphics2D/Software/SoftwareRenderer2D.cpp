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
#include "Graphics/Graphics2D/Raster2D.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

using raster::div255;
using raster::intersect;

uint32_t blend_over(uint32_t s, uint32_t d) {
  const uint32_t sa = s >> 24;
  if (sa == 255) return s;
  if (sa == 0) return d;
  const uint32_t da = d >> 24;
  const uint32_t inv = 255 - sa;
  if (da == 255) {
    // Opaque destination (the common case): the result alpha is 255 and every channel reduces to
    // floor((sc * sa + dc * inv + 127) / 255).
    uint32_t out = 0xFF000000u;
    for (int shift = 16; shift >= 0; shift -= 8)
      out |= raster::div255_floor(((s >> shift) & 0xFF) * sa + ((d >> shift) & 0xFF) * inv + 127) << shift;
    return out;
  }
  if (da == 0) return (s & 0x00FFFFFFu) | (sa << 24);  // nothing underneath: the source as is
  // Result alpha = sa + da * (1 - sa); color weighted by the destination's own coverage.
  const uint32_t oa = sa + raster::div255_floor(da * inv + 127);
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
inline int64_t min_i64(int64_t a, int64_t b) { return a < b ? a : b; }
inline int64_t max_i64(int64_t a, int64_t b) { return a > b ? a : b; }
inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }
inline float absf(float v) { return v < 0 ? -v : v; }
inline bool rect_empty(const Rect2D& r) { return r.width == 0 || r.height == 0; }
inline int32_t rect_right(const Rect2D& r) { return raster::saturate_i32(int64_t{r.x} + r.width); }
inline int32_t rect_bottom(const Rect2D& r) { return raster::saturate_i32(int64_t{r.y} + r.height); }
}  // namespace

SoftwareRenderer2D::~SoftwareRenderer2D() {
  for (uint32_t i = 0; i < clip_depth_; ++i) release_clip_entry(clips_[i]);
  for (uint32_t i = 0; i < layer_depth_; ++i)
    if (layers_[i].real && allocator_) release_view(*allocator_, layers_[i].surface.pixels);
}

void SoftwareRenderer2D::update_clip() {
  Rect2D r = base_clip_;
  for (uint32_t i = clip_floor_; i < clip_depth_; ++i) r = intersect(r, clips_[i].bounds);
  if (clip_overflow_) r = {r.x, r.y, 0, 0};  // fail closed: pushes beyond the stack's depth clip everything
  clip_ = r;
  if (clip_depth_ > clip_floor_) {
    mask_ = clips_[clip_depth_ - 1].mask;
    mask_rect_ = clips_[clip_depth_ - 1].mask_rect;
  } else {
    mask_ = ArrayView<uint8_t>();
  }
}

void SoftwareRenderer2D::set_clip(const Rect2D& clip) {
  base_clip_ = intersect(clip, current_bounds());
  update_clip();
}

void SoftwareRenderer2D::reset_clip() {
  base_clip_ = current_bounds();
  update_clip();
}

uint32_t SoftwareRenderer2D::mask_cov(int32_t x, int32_t y) const {
  if (mask_.empty()) return 255;
  const int64_t mx = int64_t{x} - mask_rect_.x, my = int64_t{y} - mask_rect_.y;  // inside by construction
  return mask_[static_cast<uint64_t>(my) * mask_rect_.width + static_cast<uint64_t>(mx)];
}

ArrayView<uint32_t> SoftwareRenderer2D::row_span(int32_t y, int32_t x0, int32_t x1) const {
  return ArrayView<uint32_t>(cur_.row(static_cast<uint32_t>(int64_t{y} - oy_)), cur_.width)
      .subview(static_cast<uint64_t>(int64_t{x0} - ox_), static_cast<uint64_t>(int64_t{x1} - x0));
}

void SoftwareRenderer2D::blend_stored(uint32_t& d, uint32_t argb) const {
  if (blend_ == BlendMode::Replace) {
    d = cur_.encode(argb);
    return;
  }
  const uint32_t a = argb >> 24;
  if (a == 255) d = cur_.encode(argb);
  else if (a != 0) d = cur_.encode(blend_over(argb, cur_.decode(d)));
}

void SoftwareRenderer2D::store_px(uint32_t& d, int32_t x, int32_t y, uint32_t argb) const {
  if (!mask_.empty()) {
    const uint32_t m = mask_cov(x, y);
    if (blend_ == BlendMode::Replace) {
      if (m < 128) return;
    } else {
      if (m == 0) return;
      if (m != 255) argb = raster::scale_alpha(argb, m);
    }
  }
  blend_stored(d, argb);
}

void SoftwareRenderer2D::put_px(int32_t x, int32_t y, uint32_t argb, uint32_t cov) {
  if (cov != 255) {
    if (blend_ == BlendMode::Replace) {
      if (cov < 128) return;
    } else {
      if (cov == 0) return;
      argb = raster::scale_alpha(argb, cov);
    }
  }
  store_px(row_span(y, x, x + 1)[0], x, y, argb);
}

void SoftwareRenderer2D::plot(int32_t x, int32_t y, uint32_t color) {
  if (!cur_.valid()) return;
  if (x < clip_.x || y < clip_.y || x >= rect_right(clip_) || y >= rect_bottom(clip_)) return;
  put_px(x, y, color, 255);
}

void SoftwareRenderer2D::span(int32_t y, int32_t x0, int32_t x1, uint32_t color) {
  if (!cur_.valid() || y < clip_.y || y >= rect_bottom(clip_)) return;
  x0 = max32(x0, clip_.x);
  x1 = min32(x1, rect_right(clip_));
  if (x0 >= x1) return;
  const ArrayView<uint32_t> run = row_span(y, x0, x1);
  if (!mask_.empty()) {
    int32_t x = x0;
    for (uint32_t& p : run) store_px(p, x++, y, color);
  } else if (blend_ == BlendMode::Replace || (color >> 24) == 255) {
    run.fill(cur_.encode(color));
  } else if ((color >> 24) != 0) {
    for (uint32_t& p : run) p = cur_.encode(blend_over(color, cur_.decode(p)));
  }
}

void SoftwareRenderer2D::clear(uint32_t color) {
  if (!cur_.valid()) return;
  const uint32_t v = cur_.encode(color);
  for (uint32_t y = 0; y < cur_.height; ++y) ArrayView<uint32_t>(cur_.row(y), cur_.width).fill(v);
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
  const int32_t xe = rect_right(r), ye = rect_bottom(r);
  for (int32_t y = max32(r.y, clip_.y); y < min32(ye, rect_bottom(clip_)); ++y) span(y, r.x, xe, color);
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
  if (!source.valid() || !cur_.valid()) return;
  // Source pixels that exist, then the destination columns and rows inside the clip.
  const int64_t sx0 = rect.x > 0 ? rect.x : 0, sy0 = rect.y > 0 ? rect.y : 0;
  const int64_t sx1 = min32(raster::saturate_i32(int64_t{rect.x} + rect.width), static_cast<int32_t>(source.width > 0x7FFFFFFF ? 0x7FFFFFFF : source.width));
  const int64_t sy1 = min32(raster::saturate_i32(int64_t{rect.y} + rect.height), static_cast<int32_t>(source.height > 0x7FFFFFFF ? 0x7FFFFFFF : source.height));
  const int64_t ddx = int64_t{x} - rect.x, ddy = int64_t{y} - rect.y;  // destination = source + delta
  const int64_t dx0 = max_i64(sx0 + ddx, clip_.x), dx1 = min_i64(sx1 + ddx, rect_right(clip_));
  const int64_t dy0 = max_i64(sy0 + ddy, clip_.y), dy1 = min_i64(sy1 + ddy, rect_bottom(clip_));
  if (dx0 >= dx1 || dy0 >= dy1) return;
  const uint64_t n = static_cast<uint64_t>(dx1 - dx0);
  const bool same_format = source.format == cur_.format;
  for (int64_t dy = dy0; dy < dy1; ++dy) {
    const ArrayView<const uint32_t> src = ArrayView<const uint32_t>(source.row(static_cast<uint32_t>(dy - ddy)), source.width)
                                              .subview(static_cast<uint64_t>(dx0 - ddx), n);
    const ArrayView<uint32_t> dst = row_span(static_cast<int32_t>(dy), static_cast<int32_t>(dx0), static_cast<int32_t>(dx1));
    if (!mask_.empty()) {
      for (uint64_t i = 0; i < n; ++i) store_px(dst[i], static_cast<int32_t>(dx0 + static_cast<int64_t>(i)), static_cast<int32_t>(dy), source.decode(src[i]));
    } else if (blend_ == BlendMode::Replace) {
      if (same_format) dst.copy_from(src);
      else for (uint64_t i = 0; i < n; ++i) dst[i] = cur_.encode(source.decode(src[i]));
    } else {
      for (uint64_t i = 0; i < n;) {
        const uint32_t s = src[i];
        const uint32_t a = s >> 24;
        if (a == 255) {
          uint64_t j = i + 1;  // a run of opaque pixels is one row copy
          while (j < n && (src[j] >> 24) == 255) ++j;
          if (same_format) dst.subview(i, j - i).copy_from(src.subview(i, j - i));
          else for (uint64_t k = i; k < j; ++k) dst[k] = cur_.encode(source.decode(src[k]));
          i = j;
        } else {
          if (a != 0) dst[i] = cur_.encode(blend_over(source.decode(s), cur_.decode(dst[i])));
          ++i;
        }
      }
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

// ---- Rounded rectangles --------------------------------------------------------------------------------

void SoftwareRenderer2D::fill_rounded_rect(const Rect2D& rect, const CornerRadii& radii, uint32_t color) {
  if (!cur_.valid() || rect_empty(rect) || (blend_ == BlendMode::Alpha && (color >> 24) == 0)) return;
  const raster::RRect rr = raster::RRect::make(rect, radii);
  struct Sink {
    SoftwareRenderer2D& r;
    uint32_t color;
    void solid(int32_t y, int32_t xa, int32_t xb) { r.span(y, xa, xb, color); }
    void pixel(int32_t x, int32_t y, uint32_t cov) {
      if (x >= r.clip_.x && x < rect_right(r.clip_)) r.put_px(x, y, color, cov);
    }
  } sink{*this, color};
  raster::rrect_rows(rr, nullptr, clip_.y, rect_bottom(clip_), sink);
}

void SoftwareRenderer2D::stroke_rounded_rect(const Rect2D& outer, const CornerRadii& radii, const EdgeWidths& widths, uint32_t color) {
  if (!cur_.valid() || rect_empty(outer) || (blend_ == BlendMode::Alpha && (color >> 24) == 0)) return;
  const raster::RRect rr = raster::RRect::make(outer, radii);
  const raster::RRect hole = raster::RRect::inset(rr, widths);
  struct Sink {
    SoftwareRenderer2D& r;
    uint32_t color;
    void solid(int32_t y, int32_t xa, int32_t xb) { r.span(y, xa, xb, color); }
    void pixel(int32_t x, int32_t y, uint32_t cov) {
      if (x >= r.clip_.x && x < rect_right(r.clip_)) r.put_px(x, y, color, cov);
    }
  } sink{*this, color};
  raster::rrect_rows(rr, &hole, clip_.y, rect_bottom(clip_), sink);
}

// ---- Masks and scaled images -----------------------------------------------------------------------------

void SoftwareRenderer2D::draw_mask(const MaskView& mask, const Rect2D& rect, int32_t x, int32_t y, uint32_t color) {
  if (!mask.valid() || !cur_.valid() || (blend_ == BlendMode::Alpha && (color >> 24) == 0)) return;
  const int64_t sx0 = rect.x > 0 ? rect.x : 0, sy0 = rect.y > 0 ? rect.y : 0;
  const int64_t sx1 = min_i64(int64_t{rect.x} + rect.width, mask.width), sy1 = min_i64(int64_t{rect.y} + rect.height, mask.height);
  const int64_t ddx = int64_t{x} - rect.x, ddy = int64_t{y} - rect.y;
  const int64_t dx0 = max_i64(sx0 + ddx, clip_.x), dx1 = min_i64(sx1 + ddx, rect_right(clip_));
  const int64_t dy0 = max_i64(sy0 + ddy, clip_.y), dy1 = min_i64(sy1 + ddy, rect_bottom(clip_));
  if (dx0 >= dx1 || dy0 >= dy1) return;
  const uint64_t n = static_cast<uint64_t>(dx1 - dx0);
  const bool fast = mask_.empty() && blend_ == BlendMode::Alpha && (color >> 24) == 255;
  const uint32_t solid = cur_.encode(color);
  for (int64_t dy = dy0; dy < dy1; ++dy) {
    const ConstByteView cov = mask.coverage.subview(static_cast<uint64_t>(dy - ddy) * mask.pitch + static_cast<uint64_t>(dx0 - ddx), n);
    const ArrayView<uint32_t> dst = row_span(static_cast<int32_t>(dy), static_cast<int32_t>(dx0), static_cast<int32_t>(dx1));
    if (fast) {
      for (uint64_t i = 0; i < n; ++i) {
        const uint32_t c = cov[i];
        if (c == 255) dst[i] = solid;
        else if (c != 0) dst[i] = cur_.encode(blend_over((color & 0x00FFFFFFu) | (c << 24), cur_.decode(dst[i])));
      }
    } else {
      for (uint64_t i = 0; i < n; ++i)
        if (cov[i]) put_px(static_cast<int32_t>(dx0 + static_cast<int64_t>(i)), static_cast<int32_t>(dy), color, cov[i]);
    }
  }
}

void SoftwareRenderer2D::draw_image_scaled(const Surface& source, const RectF& src, const Rect2D& dest, ImageFilter filter) {
  if (!cur_.valid() || rect_empty(dest)) return;
  const raster::SampleBounds bounds = raster::sample_bounds(source, src);
  if (bounds.empty()) return;
  const Rect2D area = intersect(dest, clip_);
  if (rect_empty(area)) return;
  const float step_x = src.width / static_cast<float>(dest.width), step_y = src.height / static_cast<float>(dest.height);
  const int64_t col0 = int64_t{area.x} - dest.x, row0 = int64_t{area.y} - dest.y;
  for (uint32_t j = 0; j < area.height; ++j) {
    const int32_t y = area.y + static_cast<int32_t>(j);
    const float sy = src.y + (static_cast<float>(row0 + j) + 0.5f) * step_y;
    const ArrayView<uint32_t> dst = row_span(y, area.x, area.x + static_cast<int32_t>(area.width));
    for (uint32_t i = 0; i < area.width; ++i) {
      const float sx = src.x + (static_cast<float>(col0 + i) + 0.5f) * step_x;
      const uint32_t argb = filter == ImageFilter::Nearest ? raster::sample_nearest(source, bounds, sx, sy)
                                                           : raster::sample_bilinear(source, bounds, sx, sy);
      store_px(dst[i], area.x + static_cast<int32_t>(i), y, argb);
    }
  }
}

bool SoftwareRenderer2D::draw_image_transformed(const Surface& source, const Rect2D& source_rect, const Matrix2D& m, ImageFilter filter) {
  const float det = m.a * m.d - m.b * m.c;
  if (!cur_.valid() || !(det > 1e-12f || det < -1e-12f)) return false;
  const RectF rect{static_cast<float>(source_rect.x), static_cast<float>(source_rect.y), static_cast<float>(source_rect.width), static_cast<float>(source_rect.height)};
  const raster::SampleBounds bounds = raster::sample_bounds(source, rect);
  if (bounds.empty()) return true;
  const float w = static_cast<float>(source_rect.width), h = static_cast<float>(source_rect.height);
  // the target pixels the transformed rectangle can touch
  const float xs[4] = {m.e, m.a * w + m.e, m.c * h + m.e, m.a * w + m.c * h + m.e};
  const float ys[4] = {m.f, m.b * w + m.f, m.d * h + m.f, m.b * w + m.d * h + m.f};
  float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
  for (int i = 1; i < 4; ++i) {
    if (xs[i] < x0) x0 = xs[i];
    if (xs[i] > x1) x1 = xs[i];
    if (ys[i] < y0) y0 = ys[i];
    if (ys[i] > y1) y1 = ys[i];
  }
  const int64_t bx0 = raster::floor_i(x0), by0 = raster::floor_i(y0), bx1 = raster::ceil_i(x1), by1 = raster::ceil_i(y1);
  const Rect2D area = intersect({raster::saturate_i32(bx0), raster::saturate_i32(by0), static_cast<uint32_t>(bx1 > bx0 ? bx1 - bx0 : 0), static_cast<uint32_t>(by1 > by0 ? by1 - by0 : 0)}, clip_);
  if (rect_empty(area)) return true;
  const float ia = m.d / det, ib = -m.b / det, ic = -m.c / det, id = m.a / det;
  for (uint32_t j = 0; j < area.height; ++j) {
    const int32_t y = area.y + static_cast<int32_t>(j);
    const float py = static_cast<float>(y) + 0.5f - m.f;
    const ArrayView<uint32_t> dst = row_span(y, area.x, area.x + static_cast<int32_t>(area.width));
    for (uint32_t i = 0; i < area.width; ++i) {
      const float px = static_cast<float>(area.x + static_cast<int32_t>(i)) + 0.5f - m.e;
      const float u = ia * px + ic * py, v = ib * px + id * py;   // (inside the rectangle: 0..w, 0..h)
      if (!(u >= 0.0f && u < w && v >= 0.0f && v < h)) continue;
      const float sx = rect.x + u, sy = rect.y + v;
      const uint32_t argb = filter == ImageFilter::Nearest ? raster::sample_nearest(source, bounds, sx, sy) : raster::sample_bilinear(source, bounds, sx, sy);
      store_px(dst[i], area.x + static_cast<int32_t>(i), y, argb);
    }
  }
  return true;
}

// ---- Gradients -------------------------------------------------------------------------------------------

void SoftwareRenderer2D::fill_linear_gradient(const Rect2D& rect, Vec2 from, Vec2 to, ArrayView<const GradientStop> stops) {
  raster::GradientPrep g;
  if (!cur_.valid() || !g.build(stops)) return;
  const Rect2D area = intersect(rect, clip_);
  if (rect_empty(area)) return;
  const float dx = to.x - from.x, dy = to.y - from.y, len2 = dx * dx + dy * dy;
  if (!(len2 > 1e-12f) || g.count == 1) {
    fill_rect(area, g.color[g.count - 1]);
    return;
  }
  const float inv = 1.0f / len2, tx = dx * inv, ty = dy * inv;
  for (uint32_t j = 0; j < area.height; ++j) {
    const int32_t y = area.y + static_cast<int32_t>(j);
    const float t_row = (static_cast<float>(y) + 0.5f - from.y) * ty + (static_cast<float>(area.x) + 0.5f - from.x) * tx;
    if (tx == 0) {
      span(y, area.x, area.x + static_cast<int32_t>(area.width), g.color_at(t_row));
      continue;
    }
    const ArrayView<uint32_t> dst = row_span(y, area.x, area.x + static_cast<int32_t>(area.width));
    for (uint32_t i = 0; i < area.width; ++i)
      store_px(dst[i], area.x + static_cast<int32_t>(i), y, g.color_at(t_row + static_cast<float>(i) * tx));
  }
}

void SoftwareRenderer2D::fill_radial_gradient(const Rect2D& rect, Vec2 center, Vec2 radii, ArrayView<const GradientStop> stops) {
  raster::GradientPrep g;
  if (!cur_.valid() || !g.build(stops)) return;
  const Rect2D area = intersect(rect, clip_);
  if (rect_empty(area)) return;
  if (!(radii.x > 0) || !(radii.y > 0) || g.count == 1) {
    fill_rect(area, g.color[g.count - 1]);
    return;
  }
  const float irx = 1.0f / radii.x, iry = 1.0f / radii.y;
  for (uint32_t j = 0; j < area.height; ++j) {
    const int32_t y = area.y + static_cast<int32_t>(j);
    const float v = (static_cast<float>(y) + 0.5f - center.y) * iry;
    const ArrayView<uint32_t> dst = row_span(y, area.x, area.x + static_cast<int32_t>(area.width));
    for (uint32_t i = 0; i < area.width; ++i) {
      const float u = (static_cast<float>(area.x + static_cast<int32_t>(i)) + 0.5f - center.x) * irx;
      store_px(dst[i], area.x + static_cast<int32_t>(i), y, g.color_at(raster::sqrt_f(u * u + v * v)));
    }
  }
}

// ---- Clip stack ------------------------------------------------------------------------------------------

void SoftwareRenderer2D::release_clip_entry(ClipEntry& e) {
  if (e.owns_mask && allocator_) release_view(*allocator_, e.mask);
  e.mask = ArrayView<uint8_t>();
  e.owns_mask = false;
}

bool SoftwareRenderer2D::push_clip_entry(const ClipEntry& entry) {
  if (clip_depth_ >= kMaxClipDepth) {
    ++clip_overflow_;
    update_clip();
    return false;
  }
  clips_[clip_depth_++] = entry;
  update_clip();
  return true;
}

bool SoftwareRenderer2D::push_clip_rect(const Rect2D& rect) {
  ClipEntry e;
  e.bounds = intersect(rect, current_bounds());
  if (clip_depth_ > clip_floor_) {  // keep the mask in force
    e.mask = mask_;
    e.mask_rect = mask_rect_;
  }
  return push_clip_entry(e);
}

bool SoftwareRenderer2D::push_clip_rounded_rect(const Rect2D& rect, const CornerRadii& radii) {
  const raster::RRect rr = raster::RRect::make(rect, radii);
  bool square = true;
  for (int i = 0; i < 4; ++i) square = square && rr.rx[i] == 0;
  if (square) return push_clip_rect(rect);
  const Rect2D area = intersect(intersect(rect, clip_), current_bounds());
  if (rect_empty(area)) return push_clip_rect(area);
  ArrayView<uint8_t> mask;
  if (allocator_ && clip_depth_ < kMaxClipDepth && area.width <= ~uint64_t{0} / area.height)
    mask = allocate_view<uint8_t>(*allocator_, uint64_t{area.width} * area.height);
  if (mask.empty()) {
    push_clip_rect(rect);
    return false;
  }
  mask.fill(0);
  struct Sink {
    ArrayView<uint8_t> mask;
    Rect2D area;
    void solid(int32_t y, int32_t xa, int32_t xb) {
      xa = max32(xa, area.x);
      xb = min32(xb, rect_right(area));
      if (xa < xb) mask.subview(static_cast<uint64_t>(y - area.y) * area.width + static_cast<uint64_t>(xa - area.x), static_cast<uint64_t>(xb - xa)).fill(255);
    }
    void pixel(int32_t x, int32_t y, uint32_t cov) {
      if (x >= area.x && x < rect_right(area)) mask[static_cast<uint64_t>(y - area.y) * area.width + static_cast<uint64_t>(x - area.x)] = static_cast<uint8_t>(cov);
    }
  } sink{mask, area};
  raster::rrect_rows(rr, nullptr, area.y, rect_bottom(area), sink);
  if (!mask_.empty()) {  // combine with the clip already in force
    for (uint32_t j = 0; j < area.height; ++j)
      for (uint32_t i = 0; i < area.width; ++i) {
        uint8_t& m = mask[uint64_t{j} * area.width + i];
        if (m) m = static_cast<uint8_t>(div255(m * mask_cov(area.x + static_cast<int32_t>(i), area.y + static_cast<int32_t>(j))));
      }
  }
  ClipEntry e;
  e.bounds = area;
  e.mask = mask;
  e.mask_rect = area;
  e.owns_mask = true;
  return push_clip_entry(e);
}

bool SoftwareRenderer2D::push_clip_mask(const MaskView& mask, int32_t x, int32_t y) {
  if (!mask.valid()) {  // fail closed: nothing may be drawn
    push_clip_rect({0, 0, 0, 0});
    return false;
  }
  const Rect2D area = intersect(intersect({x, y, mask.width, mask.height}, clip_), current_bounds());
  if (rect_empty(area)) return push_clip_rect(area);
  ArrayView<uint8_t> copy;
  if (allocator_ && clip_depth_ < kMaxClipDepth) copy = allocate_view<uint8_t>(*allocator_, uint64_t{area.width} * area.height);
  if (copy.empty()) {
    push_clip_rect({x, y, mask.width, mask.height});
    return false;
  }
  for (uint32_t j = 0; j < area.height; ++j) {
    const uint64_t row = static_cast<uint64_t>(int64_t{area.y} + j - y) * mask.pitch + static_cast<uint64_t>(int64_t{area.x} - x);
    const ConstByteView src = mask.coverage.subview(row, area.width);
    for (uint32_t i = 0; i < area.width; ++i) {
      uint32_t c = src[i];
      if (!mask_.empty()) c = div255(c * mask_cov(area.x + static_cast<int32_t>(i), area.y + static_cast<int32_t>(j)));
      copy[uint64_t{j} * area.width + i] = static_cast<uint8_t>(c);
    }
  }
  ClipEntry e;
  e.bounds = area;
  e.mask = copy;
  e.mask_rect = area;
  e.owns_mask = true;
  return push_clip_entry(e);
}

void SoftwareRenderer2D::pop_clip() {
  if (clip_overflow_) {
    --clip_overflow_;
  } else if (clip_depth_ > clip_floor_) {
    release_clip_entry(clips_[--clip_depth_]);
  }
  update_clip();
}

// ---- Layers ----------------------------------------------------------------------------------------------

bool SoftwareRenderer2D::push_layer(const Rect2D& bounds, uint8_t opacity) {
  if (layer_depth_ >= kMaxLayerDepth) {
    ++layer_overflow_;
    return false;
  }
  LayerEntry& l = layers_[layer_depth_++];
  l = LayerEntry();
  l.opacity = opacity;
  l.saved_surface = cur_;
  l.saved_ox = ox_, l.saved_oy = oy_;
  l.saved_base_clip = base_clip_;
  l.saved_floor = clip_floor_, l.saved_overflow = clip_overflow_;
  const Rect2D area = intersect(intersect(bounds, clip_), current_bounds());
  l.bounds = area;
  bool exact = true;
  if (!rect_empty(area)) {
    ArrayView<uint32_t> pixels;
    if (allocator_ && area.width <= ~uint64_t{0} / area.height) pixels = allocate_view<uint32_t>(*allocator_, uint64_t{area.width} * area.height);
    if (!pixels.empty()) {
      pixels.fill(0);
      l.surface.pixels = pixels;
      l.surface.width = l.surface.pitch = area.width;
      l.surface.height = area.height;
      l.surface.format = cur_.format;
      l.real = true;
      cur_ = l.surface;
      ox_ = area.x, oy_ = area.y;
    } else {
      exact = false;  // no memory: draw straight onto the parent, without group opacity
    }
  }
  base_clip_ = area;
  clip_floor_ = clip_depth_;
  clip_overflow_ = 0;
  update_clip();
  return exact;
}

void SoftwareRenderer2D::pop_layer() {
  if (layer_overflow_) {
    --layer_overflow_;
    return;
  }
  if (layer_depth_ == 0) return;
  LayerEntry& l = layers_[--layer_depth_];
  while (clip_depth_ > clip_floor_) release_clip_entry(clips_[--clip_depth_]);
  base_clip_ = l.saved_base_clip;
  clip_floor_ = l.saved_floor, clip_overflow_ = l.saved_overflow;
  cur_ = l.saved_surface;
  ox_ = l.saved_ox, oy_ = l.saved_oy;
  update_clip();
  if (!l.real) return;
  const Surface layer = l.surface;
  const Rect2D area = intersect(l.bounds, clip_);
  if (!rect_empty(area) && l.opacity != 0) {
    const BlendMode saved = blend_;
    blend_ = BlendMode::Alpha;  // a layer is always composited source-over
    for (uint32_t j = 0; j < area.height; ++j) {
      const int32_t y = area.y + static_cast<int32_t>(j);
      const ArrayView<const uint32_t> src = ArrayView<const uint32_t>(layer.row(static_cast<uint32_t>(int64_t{y} - l.bounds.y)), layer.width)
                                                .subview(static_cast<uint64_t>(int64_t{area.x} - l.bounds.x), area.width);
      const ArrayView<uint32_t> dst = row_span(y, area.x, area.x + static_cast<int32_t>(area.width));
      for (uint32_t i = 0; i < area.width; ++i) {
        uint32_t argb = layer.decode(src[i]);
        if ((argb >> 24) == 0) continue;
        if (l.opacity != 255) argb = raster::scale_alpha(argb, l.opacity);
        store_px(dst[i], area.x + static_cast<int32_t>(i), y, argb);
      }
    }
    blend_ = saved;
  }
  if (allocator_) release_view(*allocator_, l.surface.pixels);
}

// ---- Scroll blit -----------------------------------------------------------------------------------------

bool SoftwareRenderer2D::copy_rect(const Rect2D& source, int32_t dest_x, int32_t dest_y) {
  if (!cur_.valid()) return false;
  const Rect2D src = intersect(source, current_bounds());
  if (rect_empty(src)) return true;
  // The destination of the part of the source that exists, inside the clip rectangle.
  const int64_t ddx = int64_t{dest_x} - source.x, ddy = int64_t{dest_y} - source.y;
  const Rect2D want = {raster::saturate_i32(int64_t{src.x} + ddx), raster::saturate_i32(int64_t{src.y} + ddy), src.width, src.height};
  const Rect2D dst = intersect(want, clip_);
  if (rect_empty(dst)) return true;
  const bool down = ddy > 0;  // copy bottom rows first when moving down, so unread rows are not overwritten
  for (uint32_t k = 0; k < dst.height; ++k) {
    const uint32_t j = down ? dst.height - 1 - k : k;
    const int32_t y = dst.y + static_cast<int32_t>(j);
    const int32_t sy = raster::saturate_i32(int64_t{y} - ddy);
    row_span(y, dst.x, dst.x + static_cast<int32_t>(dst.width))
        .copy_from(ArrayView<const uint32_t>(row_span(sy, raster::saturate_i32(int64_t{dst.x} - ddx), raster::saturate_i32(int64_t{dst.x} - ddx + dst.width))));
  }
  return true;
}

void SoftwareRenderer2D::present() {
  if (display_ && target_.valid()) {
    display_->blit(0, 0, target_.width, target_.height, target_.pixels.data(), target_.pitch);
    display_->present();
  }
}

}  // namespace graphics
