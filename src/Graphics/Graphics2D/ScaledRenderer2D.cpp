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

namespace {

int32_t clamp32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : static_cast<int32_t>(v); }

}  // namespace

Extent2D ScaledRenderer2D::extent() const {
  const Extent2D e = device_->extent();
  return {e.width / scale_, e.height / scale_};
}

Rect2D ScaledRenderer2D::up(const Rect2D& r) const {
  const uint64_t w = uint64_t{r.width} * scale_, h = uint64_t{r.height} * scale_;
  return {clamp32(int64_t{r.x} * scale_), clamp32(int64_t{r.y} * scale_), w > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(w),
          h > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(h)};
}

CornerRadii ScaledRenderer2D::up(const CornerRadii& r) const {
  const float k = float(scale_);
  return {r.tl_x * k, r.tl_y * k, r.tr_x * k, r.tr_y * k, r.br_x * k, r.br_y * k, r.bl_x * k, r.bl_y * k};
}

Matrix2D ScaledRenderer2D::up(const Matrix2D& m) const {
  const float k = float(scale_);
  return {m.a * k, m.b * k, m.c * k, m.d * k, m.e * k, m.f * k};
}

Matrix2D ScaledRenderer2D::conjugate(const Matrix2D& m) const {
  const float k = float(scale_);
  return {m.a, m.b, m.c, m.d, m.e * k, m.f * k};      // (S m S^-1: the linear part is the same, the translation is in device pixels)
}

void ScaledRenderer2D::put_pixel(int32_t x, int32_t y, uint32_t color) {
  if (scale_ == 1) { device_->put_pixel(x, y, color); return; }
  device_->fill_rect(up(Rect2D{x, y, 1, 1}), color);
}

// A line scale pixels thick: an upright one is a rectangle; any other is scale x scale copies of the line, one per sub-pixel offset.
void ScaledRenderer2D::draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
  if (scale_ == 1) { device_->draw_line(x0, y0, x1, y1, color); return; }
  if (x0 == x1 || y0 == y1) {
    const int32_t lx = x0 < x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1;
    const uint32_t w = static_cast<uint32_t>((x0 < x1 ? x1 - x0 : x0 - x1)) + 1, h = static_cast<uint32_t>((y0 < y1 ? y1 - y0 : y0 - y1)) + 1;
    device_->fill_rect(up(Rect2D{lx, ly, w, h}), color);
    return;
  }
  for (int32_t j = 0; j < s(); ++j)
    for (int32_t i = 0; i < s(); ++i) device_->draw_line(x0 * s() + i, y0 * s() + j, x1 * s() + i, y1 * s() + j, color);
}

void ScaledRenderer2D::draw_rect(const Rect2D& rect, uint32_t color) {
  if (scale_ == 1) { device_->draw_rect(rect, color); return; }
  if (rect.width == 0 || rect.height == 0) return;
  const Rect2D r = up(rect);
  device_->fill_rect({r.x, r.y, r.width, scale_}, color);
  device_->fill_rect({r.x, r.y + static_cast<int32_t>(r.height - scale_), r.width, scale_}, color);
  if (r.height > 2 * scale_) {
    device_->fill_rect({r.x, r.y + s(), scale_, r.height - 2 * scale_}, color);
    device_->fill_rect({r.x + static_cast<int32_t>(r.width - scale_), r.y + s(), scale_, r.height - 2 * scale_}, color);
  }
}

void ScaledRenderer2D::fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) {
  if (scale_ == 1) { device_->fill_circle(cx, cy, radius, color); return; }
  device_->fill_circle(cx * s() + s() / 2, cy * s() + s() / 2, radius * scale_, color);
}

void ScaledRenderer2D::draw_image(const Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) {
  if (scale_ == 1) { device_->draw_image(source, source_rect, x, y); return; }
  device_->draw_image_scaled(source, RectF{float(source_rect.x), float(source_rect.y), float(source_rect.width), float(source_rect.height)},
                             up(Rect2D{x, y, source_rect.width, source_rect.height}), ImageFilter::Nearest);
}

// The 8x16 font enlarged: each glyph's rows as runs of set pixels, one rectangle per run.
void ScaledRenderer2D::draw_text(int32_t x, int32_t y, const char* text, uint32_t color) {
  if (scale_ == 1) { device_->draw_text(x, y, text, color); return; }
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
  device_->stroke_rounded_rect(up(outer), up(radii), {widths.top * scale_, widths.right * scale_, widths.bottom * scale_, widths.left * scale_}, color);
}

bool ScaledRenderer2D::enlarge(const MaskView& mask, const Rect2D& source_rect, MaskView* out) {
  if (!mask.valid() || source_rect.x < 0 || source_rect.y < 0) return false;
  const uint64_t sx = static_cast<uint64_t>(source_rect.x), sy = static_cast<uint64_t>(source_rect.y);
  if (sx >= mask.width || sy >= mask.height) return false;
  const uint32_t w = static_cast<uint32_t>(source_rect.width < mask.width - sx ? source_rect.width : mask.width - sx);
  const uint32_t h = static_cast<uint32_t>(source_rect.height < mask.height - sy ? source_rect.height : mask.height - sy);
  if (w == 0 || h == 0) return false;
  const uint64_t ow = uint64_t{w} * scale_, oh = uint64_t{h} * scale_;
  if (ow > 8192 || oh > 8192) return false;      // (a glyph or a clip mask: never that big)
  scratch_.clear();
  if (!scratch_.reserve(ow * oh)) return false;
  for (uint64_t y = 0; y < oh; ++y) {
    const uint64_t row = (sy + y / scale_) * mask.pitch;
    for (uint64_t x = 0; x < ow; ++x) scratch_.push_back(mask.coverage[row + sx + x / scale_]);
  }
  out->coverage = ConstByteView(scratch_.view().data(), scratch_.count());
  out->width = static_cast<uint32_t>(ow);
  out->height = static_cast<uint32_t>(oh);
  out->pitch = static_cast<uint32_t>(ow);
  return true;
}

void ScaledRenderer2D::draw_mask(const MaskView& mask, const Rect2D& source_rect, int32_t x, int32_t y, uint32_t color) {
  if (scale_ == 1) { device_->draw_mask(mask, source_rect, x, y, color); return; }
  MaskView big;
  if (!enlarge(mask, source_rect, &big)) return;
  device_->draw_mask(big, Rect2D{0, 0, big.width, big.height}, x * s(), y * s(), color);
}

void ScaledRenderer2D::draw_glyph(uint64_t id, const MaskView& mask, int32_t x, int32_t y, uint32_t color) {
  if (scale_ == 1) { device_->draw_glyph(id, mask, x, y, color); return; }
  MaskView big;
  if (!enlarge(mask, Rect2D{0, 0, mask.width, mask.height}, &big)) return;
  device_->draw_glyph(id ^ (uint64_t{scale_} << 56), big, x * s(), y * s(), color);      // (the enlarged pixels are other pixels: another id)
}

bool ScaledRenderer2D::push_clip_mask(const MaskView& mask, int32_t x, int32_t y) {
  if (scale_ == 1) return device_->push_clip_mask(mask, x, y);
  MaskView big;
  if (!enlarge(mask, Rect2D{0, 0, mask.width, mask.height}, &big)) return device_->push_clip_rect(Rect2D{x * s(), y * s(), 0, 0});      // (nothing shows)
  return device_->push_clip_mask(big, x * s(), y * s());      // (the device copies the coverage it keeps)
}

bool ScaledRenderer2D::draw_device_image(IGraphicsDevice& owner, IImage& image, const Rect2D& source, int32_t x, int32_t y) {
  if (scale_ == 1) return device_->draw_device_image(owner, image, source, x, y);
  return false;      // (a device image cannot be enlarged here: the caller draws it from the CPU's view)
}

bool ScaledRenderer2D::read_frame(ArrayView<uint32_t> out) {
  if (scale_ == 1) return device_->read_frame(out);
  const Extent2D d = device_->extent(), l = extent();
  const uint64_t device_pixels = uint64_t{d.width} * d.height, pixels = uint64_t{l.width} * l.height;
  if (out.size() < pixels) return false;
  frame_.clear();
  if (!frame_.reserve(device_pixels)) return false;
  for (uint64_t i = 0; i < device_pixels; ++i) frame_.push_back(0);
  if (!device_->read_frame(frame_.view())) return false;
  for (uint64_t y = 0; y < l.height; ++y)
    for (uint64_t x = 0; x < l.width; ++x) out[y * l.width + x] = frame_[(y * scale_) * d.width + x * scale_];
  return true;
}

}  // namespace graphics
