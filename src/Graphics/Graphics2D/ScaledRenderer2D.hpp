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

// HiDPI: a renderer in logical coordinates (units) over one in device pixels, `percent` / 100 device pixels per unit (100 .. 400, Graphics/DeviceScale.hpp's
// mapping). Everything drawn is enlarged: rectangles, lines and shapes by their edges (so what touches still touches; at a whole scale every unit is
// exactly scale x scale pixels), the 8x16 font as its pixels' rectangles, masks by nearest sampling, images by nearest sampling at a whole scale and
// filtered at a fractional one. A picture of the desktop at 100 % looks the same, only sharper where something was drawn at the device's
// resolution (scale_percent() and device() let such a picture be drawn 1:1). The extent is the device's in units. At 100 every call goes straight through.
// The device renderer may be swapped (a new display mode) with set_device; the object stays, so whoever holds it keeps holding it.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/DeviceScale.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "ObjectContainer/List.hpp"

namespace graphics {

class ScaledRenderer2D final : public IRenderer2D {
 public:
  ScaledRenderer2D(allocators::IAllocator& allocator, IRenderer2D& device, uint32_t percent) : scratch_(allocator), frame_(allocator) { set_device(device, percent); }
  ~ScaledRenderer2D() = default;
  ScaledRenderer2D(const ScaledRenderer2D&) = delete;
  ScaledRenderer2D& operator=(const ScaledRenderer2D&) = delete;

  void set_device(IRenderer2D& device, uint32_t percent) {
    device_ = &device;
    percent_ = clamp_scale_percent(percent);
  }
  uint32_t percent() const { return percent_; }

  const char* name() const override { return device_->name(); }
  bool accelerated() const override { return device_->accelerated(); }
  Extent2D extent() const override;
  bool gradients_as_stops() const override { return device_->gradients_as_stops(); }
  void set_clip(const Rect2D& clip) override { device_->set_clip(up(clip)); }
  void reset_clip() override { device_->reset_clip(); }
  void set_blend(BlendMode mode) override { device_->set_blend(mode); }
  void clear(uint32_t color) override { device_->clear(color); }
  void put_pixel(int32_t x, int32_t y, uint32_t color) override;
  void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) override;
  void fill_rect(const Rect2D& rect, uint32_t color) override { device_->fill_rect(up(rect), color); }
  void draw_rect(const Rect2D& rect, uint32_t color) override;
  void fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) override;
  void fill_triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t color) override { device_->fill_triangle(up(a), up(b), up(c), color); }
  void draw_image(const Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) override;
  void draw_text(int32_t x, int32_t y, const char* text, uint32_t color) override;
  void fill_rounded_rect(const Rect2D& rect, const CornerRadii& radii, uint32_t color) override { device_->fill_rounded_rect(up(rect), up(radii), color); }
  void stroke_rounded_rect(const Rect2D& outer, const CornerRadii& radii, const EdgeWidths& widths, uint32_t color) override;
  void draw_mask(const MaskView& mask, const Rect2D& source_rect, int32_t x, int32_t y, uint32_t color) override;
  void draw_glyph(uint64_t id, const MaskView& mask, int32_t x, int32_t y, uint32_t color) override;
  void draw_image_scaled(const Surface& source, const RectF& source_rect, const Rect2D& dest, ImageFilter filter) override {
    device_->draw_image_scaled(source, source_rect, up(dest), filter);
  }
  bool draw_image_transformed(const Surface& source, const Rect2D& source_rect, const Matrix2D& m, ImageFilter filter) override {
    return device_->draw_image_transformed(source, source_rect, up(m), filter);
  }
  bool draw_image_tiled(const Surface& tile, int32_t origin_x, int32_t origin_y, uint32_t cell_w, uint32_t cell_h, const Rect2D& area) override;
  bool begin_group(const Rect2D& bounds) override { return device_->begin_group(up(bounds)); }
  bool end_group(const Matrix2D& m, uint8_t opacity, ImageFilter filter) override { return device_->end_group(conjugate(m), opacity, filter); }
  bool supports_group_filters() const override { return device_->supports_group_filters(); }
  bool begin_filter_group(const Rect2D& bounds) override { return device_->begin_filter_group(up(bounds)); }
  bool group_blur(float sigma) override { return device_->group_blur(sigma * factor()); }
  bool group_color(const ColorTransform& t) override { return device_->group_color(t); }
  bool group_drop_shadow(int32_t dx, int32_t dy, float sigma, uint32_t color) override { return device_->group_drop_shadow(up_x(dx), up_x(dy), sigma * factor(), color); }
  bool group_cut_out(const Rect2D& rect, const CornerRadii& radii) override { return device_->group_cut_out(up(rect), up(radii)); }
  void fill_linear_gradient(const Rect2D& rect, Vec2 from, Vec2 to, ArrayView<const GradientStop> stops) override {
    device_->fill_linear_gradient(up(rect), up(from), up(to), stops);
  }
  void fill_radial_gradient(const Rect2D& rect, Vec2 center, Vec2 radii, ArrayView<const GradientStop> stops) override {
    device_->fill_radial_gradient(up(rect), up(center), Vec2{radii.x * factor(), radii.y * factor()}, stops);
  }
  bool fill_conic_gradient(const Rect2D& rect, Vec2 center, float start_turns, ArrayView<const GradientStop> samples) override {
    return device_->fill_conic_gradient(up(rect), up(center), start_turns, samples);
  }
  bool push_clip_rect(const Rect2D& rect) override { return device_->push_clip_rect(up(rect)); }
  bool push_clip_rounded_rect(const Rect2D& rect, const CornerRadii& radii) override { return device_->push_clip_rounded_rect(up(rect), up(radii)); }
  bool push_clip_mask(const MaskView& mask, int32_t x, int32_t y) override;
  void pop_clip() override { device_->pop_clip(); }
  bool push_layer(const Rect2D& bounds, uint8_t opacity) override { return device_->push_layer(up(bounds), opacity); }
  void pop_layer() override { device_->pop_layer(); }
  bool copy_rect(const Rect2D& source, int32_t dest_x, int32_t dest_y) override;
  bool draw_device_image(IGraphicsDevice& owner, IImage& image, const Rect2D& source, int32_t x, int32_t y) override;
  bool begin_retained(uint64_t key, const Rect2D& bounds, const Rect2D& update, bool* fresh) override {
    return device_->begin_retained(key, up(bounds), up(update), fresh);
  }
  void end_retained() override { device_->end_retained(); }
  bool draw_retained(uint64_t key, const Rect2D& bounds) override { return device_->draw_retained(key, up(bounds)); }
  uint32_t scale_percent() const override { return static_cast<uint32_t>(uint64_t{percent_} * device_->scale_percent() / 100); }
  IRenderer2D& device() override { return device_->device(); }
  void present() override { device_->present(); }
  bool read_frame(ArrayView<uint32_t> out) override;      // (the device's frame, the pixel at each unit's top left)

  // The mapped geometry (public for whoever draws on the device itself).
  Rect2D up(const Rect2D& r) const;
  int32_t up_x(int32_t v) const { return units_to_device(v, percent_); }
  Vec2 up(Vec2 v) const { return {v.x * factor(), v.y * factor()}; }
  CornerRadii up(const CornerRadii& r) const;
  Matrix2D up(const Matrix2D& m) const;          // logical target coordinates -> device: scale after m
  Matrix2D conjugate(const Matrix2D& m) const;   // a map of the logical plane as a map of the device plane

 private:
  float factor() const { return float(percent_) / 100.0f; }
  bool whole() const { return percent_ % 100 == 0; }
  // `source_rect` of `mask` enlarged into scratch_ to (out_w x out_h) by nearest sampling; false when there is nothing to draw or no memory.
  bool enlarge(const MaskView& mask, const Rect2D& source_rect, uint32_t out_w, uint32_t out_h, MaskView* out);

  IRenderer2D* device_ = nullptr;
  uint32_t percent_ = 100;
  List<uint8_t> scratch_;
  List<uint32_t> frame_;
};

}  // namespace graphics
