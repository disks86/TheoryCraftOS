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

#include "Graphics/Graphics2D/ScaledRenderer2D.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"

namespace graphics {

Extent2D ScaledRenderer2D::extent() const {
  const Extent2D e = device_->extent();
  return {static_cast<uint32_t>(device_to_units(static_cast<int32_t>(e.width), percent_)), static_cast<uint32_t>(device_to_units(static_cast<int32_t>(e.height), percent_))};
}

Rect2D ScaledRenderer2D::up(const Rect2D& r) const {
  if (percent_ == 100) return r;
  return {units_to_device(r.x, percent_), units_to_device(r.y, percent_), units_span_to_device(r.x, r.width, percent_), units_span_to_device(r.y, r.height, percent_)};
}

CornerRadii ScaledRenderer2D::up(const CornerRadii& r) const {
  const float k = factor();
  return {r.tl_x * k, r.tl_y * k, r.tr_x * k, r.tr_y * k, r.br_x * k, r.br_y * k, r.bl_x * k, r.bl_y * k};
}

Matrix2D ScaledRenderer2D::up(const Matrix2D& m) const {
  const float k = factor();
  return {m.a * k, m.b * k, m.c * k, m.d * k, m.e * k, m.f * k};
}

Matrix2D ScaledRenderer2D::conjugate(const Matrix2D& m) const {
  const float k = factor();
  return {m.a, m.b, m.c, m.d, m.e * k, m.f * k};      // (S m S^-1: the linear part is the same, the translation is in device pixels)
}

void ScaledRenderer2D::put_pixel(int32_t x, int32_t y, uint32_t color) {
  if (percent_ == 100) { device_->put_pixel(x, y, color); return; }
  device_->fill_rect(up(Rect2D{x, y, 1, 1}), color);
}

// A line one unit thick: an upright one is a rectangle; any other is drawn once per device pixel of a unit's thickness.
void ScaledRenderer2D::draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
  if (percent_ == 100) { device_->draw_line(x0, y0, x1, y1, color); return; }
  if (x0 == x1 || y0 == y1) {
    const int32_t lx = x0 < x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1;
    const uint32_t w = static_cast<uint32_t>((x0 < x1 ? x1 - x0 : x0 - x1)) + 1, h = static_cast<uint32_t>((y0 < y1 ? y1 - y0 : y0 - y1)) + 1;
    device_->fill_rect(up(Rect2D{lx, ly, w, h}), color);
    return;
  }
  const int32_t t = static_cast<int32_t>((percent_ + 99) / 100);
  const int32_t dx0 = up_x(x0), dy0 = up_x(y0), dx1 = up_x(x1), dy1 = up_x(y1);
  for (int32_t j = 0; j < t; ++j)
    for (int32_t i = 0; i < t; ++i) device_->draw_line(dx0 + i, dy0 + j, dx1 + i, dy1 + j, color);
}

void ScaledRenderer2D::draw_rect(const Rect2D& rect, uint32_t color) {
  if (percent_ == 100) { device_->draw_rect(rect, color); return; }
  if (rect.width == 0 || rect.height == 0) return;
  const int32_t r = rect.x + static_cast<int32_t>(rect.width) - 1, b = rect.y + static_cast<int32_t>(rect.height) - 1;
  device_->fill_rect(up(Rect2D{rect.x, rect.y, rect.width, 1}), color);      // (each side a row or column of units)
  if (rect.height > 1) device_->fill_rect(up(Rect2D{rect.x, b, rect.width, 1}), color);
  if (rect.height > 2) {
    device_->fill_rect(up(Rect2D{rect.x, rect.y + 1, 1, rect.height - 2}), color);
    if (rect.width > 1) device_->fill_rect(up(Rect2D{r, rect.y + 1, 1, rect.height - 2}), color);
  }
}

void ScaledRenderer2D::fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) {
  if (percent_ == 100) { device_->fill_circle(cx, cy, radius, color); return; }
  const Rect2D c = up(Rect2D{cx, cy, 1, 1});
  device_->fill_circle(c.x + static_cast<int32_t>(c.width / 2), c.y + static_cast<int32_t>(c.height / 2), static_cast<uint32_t>(float(radius) * factor()), color);
}

void ScaledRenderer2D::draw_image(const Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) {
  if (percent_ == 100) { device_->draw_image(source, source_rect, x, y); return; }
  device_->draw_image_scaled(source, RectF{float(source_rect.x), float(source_rect.y), float(source_rect.width), float(source_rect.height)},
                             up(Rect2D{x, y, source_rect.width, source_rect.height}), whole() ? ImageFilter::Nearest : ImageFilter::Bilinear);
}

// The 8x16 font enlarged: each glyph's rows as runs of set pixels, one rectangle per run (mapped by its edges).
void ScaledRenderer2D::draw_text(int32_t x, int32_t y, const char* text, uint32_t color) {
  if (percent_ == 100) { device_->draw_text(x, y, text, color); return; }
  if (!text) return;
  for (char32_t c = hardware::utf8_next(text); c != 0; c = hardware::utf8_next(text), x += static_cast<int32_t>(hardware::Font8x16::kWidth)) {
    uint8_t glyph[hardware::Font8x16::kHeight];
    hardware::Font8x16::glyph_of(c, glyph);
    for (uint32_t j = 0; j < hardware::Font8x16::kHeight; ++j) {
      uint32_t i = 0;
      while (i < hardware::Font8x16::kWidth) {
        if (!(glyph[j] & (0x80 >> i))) { ++i; continue; }
        uint32_t end = i;
        while (end < hardware::Font8x16::kWidth && (glyph[j] & (0x80 >> end))) ++end;
        device_->fill_rect(up(Rect2D{x + static_cast<int32_t>(i), y + static_cast<int32_t>(j), end - i, 1}), color);
        i = end;
      }
    }
  }
}

void ScaledRenderer2D::stroke_rounded_rect(const Rect2D& outer, const CornerRadii& radii, const EdgeWidths& widths, uint32_t color) {
  if (percent_ == 100) { device_->stroke_rounded_rect(outer, radii, widths, color); return; }
  auto w = [&](uint32_t v) { return v == 0 ? 0u : static_cast<uint32_t>(units_to_device(static_cast<int32_t>(v), percent_)); };
  device_->stroke_rounded_rect(up(outer), up(radii), {w(widths.top), w(widths.right), w(widths.bottom), w(widths.left)}, color);
}

bool ScaledRenderer2D::draw_image_tiled(const Surface& tile, int32_t origin_x, int32_t origin_y, uint32_t cell_w, uint32_t cell_h, const Rect2D& area) {
  if (percent_ == 100) return device_->draw_image_tiled(tile, origin_x, origin_y, cell_w, cell_h, area);
  if (!whole()) return false;      // (cells of a fractional size do not tile evenly: the caller draws the copies, each mapped by its edges)
  const uint32_t k = percent_ / 100;
  return device_->draw_image_tiled(tile, origin_x * static_cast<int32_t>(k), origin_y * static_cast<int32_t>(k), cell_w * k, cell_h * k, up(area));
}

bool ScaledRenderer2D::enlarge(const MaskView& mask, const Rect2D& source_rect, uint32_t ow, uint32_t oh, MaskView* out) {
  if (!mask.valid() || source_rect.x < 0 || source_rect.y < 0) return false;
  const uint64_t sx = static_cast<uint64_t>(source_rect.x), sy = static_cast<uint64_t>(source_rect.y);
  if (sx >= mask.width || sy >= mask.height) return false;
  const uint32_t w = static_cast<uint32_t>(source_rect.width < mask.width - sx ? source_rect.width : mask.width - sx);
  const uint32_t h = static_cast<uint32_t>(source_rect.height < mask.height - sy ? source_rect.height : mask.height - sy);
  if (w == 0 || h == 0 || ow == 0 || oh == 0 || ow > 8192 || oh > 8192) return false;      // (a glyph or a clip mask: never that big)
  scratch_.clear();
  if (!scratch_.reserve(uint64_t{ow} * oh)) return false;
  for (uint64_t y = 0; y < oh; ++y) {
    uint64_t my = y * h / oh;
    if (my >= h) my = h - 1;
    const uint64_t row = (sy + my) * mask.pitch;
    for (uint64_t x = 0; x < ow; ++x) {
      uint64_t mx = x * w / ow;
      if (mx >= w) mx = w - 1;
      scratch_.push_back(mask.coverage[row + sx + mx]);
    }
  }
  out->coverage = ConstByteView(scratch_.view().data(), scratch_.count());
  out->width = ow;
  out->height = oh;
  out->pitch = ow;
  return true;
}

void ScaledRenderer2D::draw_mask(const MaskView& mask, const Rect2D& source_rect, int32_t x, int32_t y, uint32_t color) {
  if (percent_ == 100) { device_->draw_mask(mask, source_rect, x, y, color); return; }
  const Rect2D d = up(Rect2D{x, y, source_rect.width, source_rect.height});
  MaskView big;
  if (!enlarge(mask, source_rect, d.width, d.height, &big)) return;
  device_->draw_mask(big, Rect2D{0, 0, big.width, big.height}, d.x, d.y, color);
}

void ScaledRenderer2D::draw_glyph(uint64_t id, const MaskView& mask, int32_t x, int32_t y, uint32_t color) {
  if (percent_ == 100) { device_->draw_glyph(id, mask, x, y, color); return; }
  const Rect2D d = up(Rect2D{x, y, mask.width, mask.height});
  MaskView big;
  if (!enlarge(mask, Rect2D{0, 0, mask.width, mask.height}, d.width, d.height, &big)) return;
  // (the enlarged pixels are other pixels, and at a fractional scale they differ with the glyph's place: another id, or none)
  if (whole()) device_->draw_glyph(id ^ (uint64_t{percent_} << 52), big, d.x, d.y, color);
  else device_->draw_mask(big, Rect2D{0, 0, big.width, big.height}, d.x, d.y, color);
}

bool ScaledRenderer2D::push_clip_mask(const MaskView& mask, int32_t x, int32_t y) {
  if (percent_ == 100) return device_->push_clip_mask(mask, x, y);
  const Rect2D d = up(Rect2D{x, y, mask.width, mask.height});
  MaskView big;
  if (!enlarge(mask, Rect2D{0, 0, mask.width, mask.height}, d.width, d.height, &big)) return device_->push_clip_rect(Rect2D{d.x, d.y, 0, 0});      // (nothing shows)
  return device_->push_clip_mask(big, d.x, d.y);      // (the device copies the coverage it keeps)
}

bool ScaledRenderer2D::copy_rect(const Rect2D& source, int32_t dest_x, int32_t dest_y) {
  if (percent_ == 100) return device_->copy_rect(source, dest_x, dest_y);
  return device_->copy_rect(up(source), up_x(dest_x), up_x(dest_y));
}

bool ScaledRenderer2D::draw_device_image(IGraphicsDevice& owner, IImage& image, const Rect2D& source, int32_t x, int32_t y) {
  if (percent_ == 100) return device_->draw_device_image(owner, image, source, x, y);
  return false;      // (a device image cannot be enlarged here: the caller draws it from the CPU's view)
}

bool ScaledRenderer2D::read_frame(ArrayView<uint32_t> out) {
  if (percent_ == 100) return device_->read_frame(out);
  const Extent2D d = device_->extent(), l = extent();
  const uint64_t device_pixels = uint64_t{d.width} * d.height, pixels = uint64_t{l.width} * l.height;
  if (out.size() < pixels) return false;
  frame_.clear();
  if (!frame_.reserve(device_pixels)) return false;
  for (uint64_t i = 0; i < device_pixels; ++i) frame_.push_back(0);
  if (!device_->read_frame(frame_.view())) return false;
  for (uint64_t y = 0; y < l.height; ++y) {
    const uint64_t dy = static_cast<uint64_t>(up_x(static_cast<int32_t>(y)));
    for (uint64_t x = 0; x < l.width; ++x) {
      const uint64_t dx = static_cast<uint64_t>(up_x(static_cast<int32_t>(x)));
      out[y * l.width + x] = dy < d.height && dx < d.width ? frame_[dy * d.width + dx] : 0;
    }
  }
  return true;
}

}  // namespace graphics
