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

// The desktop on monitors of different scales, each drawn at its own density. The desktop draws once, in the layout's units; every call goes to
// each monitor's view - a ScaledRenderer2D at that monitor's scale whose origin is the monitor's place in the layout, over a device renderer of the
// monitor's own pixels - so text, lines and pictures are made at every monitor's resolution rather than drawn at the largest scale and resampled.
// Each view clips to its own device, so a call costs what it covers on each monitor plus the setup per view.
//
// Results that say whether something was done (draw_image_tiled, groups, conic gradients, device images) are true only when every view did it; a
// group a view could not begin is closed again on the views that did (nothing drawn yet), so begin/end stay matched. Retained layers and copy_rect
// are declined (a copy cannot cross from one monitor's pixels to another's): the caller draws instead. device_views() are the monitors' views, so
// a picture made at a device's resolution goes on each monitor 1:1 or resampled for it.
#pragma once
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "ObjectContainer/List.hpp"

namespace graphics {

class MultiScaleRenderer2D final : public IRenderer2D {
 public:
  static constexpr uint32_t kMaxViews = 16;
  // `display`: presented once after every view presented its pixels (null: not).
  MultiScaleRenderer2D(allocators::IAllocator& allocator, hardware::IGraphicsProcessingUnit* display) : display_(display), frame_(allocator), view_frame_(allocator) {}
  ~MultiScaleRenderer2D() = default;
  MultiScaleRenderer2D(const MultiScaleRenderer2D&) = delete;
  MultiScaleRenderer2D& operator=(const MultiScaleRenderer2D&) = delete;

  // The views (each a renderer in the layout's units that draws one device: device_view(0) says where) and the layout's size in units.
  bool set_views(ArrayView<IRenderer2D* const> views, Extent2D layout);
  uint32_t view_count() const { return count_; }

  const char* name() const override { return "per-monitor scales"; }
  bool accelerated() const override { return false; }
  Extent2D extent() const override { return extent_; }
  bool gradients_as_stops() const override;
  void set_clip(const Rect2D& clip) override { for (IRenderer2D* v : views()) v->set_clip(clip); }
  void reset_clip() override { for (IRenderer2D* v : views()) v->reset_clip(); }
  void set_blend(BlendMode mode) override { for (IRenderer2D* v : views()) v->set_blend(mode); }
  void clear(uint32_t color) override { for (IRenderer2D* v : views()) v->clear(color); }
  void put_pixel(int32_t x, int32_t y, uint32_t color) override { for (IRenderer2D* v : views()) v->put_pixel(x, y, color); }
  void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) override { for (IRenderer2D* v : views()) v->draw_line(x0, y0, x1, y1, color); }
  void fill_rect(const Rect2D& rect, uint32_t color) override { for (IRenderer2D* v : views()) v->fill_rect(rect, color); }
  void draw_rect(const Rect2D& rect, uint32_t color) override { for (IRenderer2D* v : views()) v->draw_rect(rect, color); }
  void fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) override { for (IRenderer2D* v : views()) v->fill_circle(cx, cy, radius, color); }
  void fill_triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t color) override { for (IRenderer2D* v : views()) v->fill_triangle(a, b, c, color); }
  void draw_image(const Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) override { for (IRenderer2D* v : views()) v->draw_image(source, source_rect, x, y); }
  void draw_text(int32_t x, int32_t y, const char* text, uint32_t color) override { for (IRenderer2D* v : views()) v->draw_text(x, y, text, color); }
  void fill_rounded_rect(const Rect2D& rect, const CornerRadii& radii, uint32_t color) override { for (IRenderer2D* v : views()) v->fill_rounded_rect(rect, radii, color); }
  void stroke_rounded_rect(const Rect2D& outer, const CornerRadii& radii, const EdgeWidths& widths, uint32_t color) override {
    for (IRenderer2D* v : views()) v->stroke_rounded_rect(outer, radii, widths, color);
  }
  void draw_mask(const MaskView& mask, const Rect2D& source_rect, int32_t x, int32_t y, uint32_t color) override {
    for (IRenderer2D* v : views()) v->draw_mask(mask, source_rect, x, y, color);
  }
  void draw_glyph(uint64_t id, const MaskView& mask, int32_t x, int32_t y, uint32_t color) override { for (IRenderer2D* v : views()) v->draw_glyph(id, mask, x, y, color); }
  void draw_image_scaled(const Surface& source, const RectF& source_rect, const Rect2D& dest, ImageFilter filter) override {
    for (IRenderer2D* v : views()) v->draw_image_scaled(source, source_rect, dest, filter);
  }
  bool draw_image_transformed(const Surface& source, const Rect2D& source_rect, const Matrix2D& m, ImageFilter filter) override;
  bool draw_image_tiled(const Surface& tile, int32_t origin_x, int32_t origin_y, uint32_t cell_w, uint32_t cell_h, const Rect2D& area) override;
  bool begin_group(const Rect2D& bounds) override;
  bool end_group(const Matrix2D& m, uint8_t opacity, ImageFilter filter) override;
  bool supports_group_filters() const override;
  bool begin_filter_group(const Rect2D& bounds) override;
  bool group_blur(float sigma) override;
  bool group_color(const ColorTransform& t) override;
  bool group_drop_shadow(int32_t dx, int32_t dy, float sigma, uint32_t color) override;
  bool group_cut_out(const Rect2D& rect, const CornerRadii& radii) override;
  void fill_linear_gradient(const Rect2D& rect, Vec2 from, Vec2 to, ArrayView<const GradientStop> stops) override {
    for (IRenderer2D* v : views()) v->fill_linear_gradient(rect, from, to, stops);
  }
  void fill_radial_gradient(const Rect2D& rect, Vec2 center, Vec2 radii, ArrayView<const GradientStop> stops) override {
    for (IRenderer2D* v : views()) v->fill_radial_gradient(rect, center, radii, stops);
  }
  bool fill_conic_gradient(const Rect2D& rect, Vec2 center, float start_turns, ArrayView<const GradientStop> samples) override;
  bool push_clip_rect(const Rect2D& rect) override;
  bool push_clip_rounded_rect(const Rect2D& rect, const CornerRadii& radii) override;
  bool push_clip_mask(const MaskView& mask, int32_t x, int32_t y) override;
  void pop_clip() override { for (IRenderer2D* v : views()) v->pop_clip(); }
  bool push_layer(const Rect2D& bounds, uint8_t opacity) override;
  void pop_layer() override { for (IRenderer2D* v : views()) v->pop_layer(); }
  bool copy_rect(const Rect2D&, int32_t, int32_t) override { return false; }
  bool draw_device_image(IGraphicsDevice& owner, IImage& image, const Rect2D& source, int32_t x, int32_t y) override;
  bool draw_device_image_scaled(IGraphicsDevice& owner, IImage& image, const Rect2D& source, const Rect2D& dest, ImageFilter filter) override;
  uint32_t scale_percent() const override;      // (the largest of the views': what programs are told to draw at)
  uint32_t device_views() override;
  bool device_view(uint32_t index, DeviceView* out) override;
  void present() override;
  bool read_frame(ArrayView<uint32_t> out) override;      // (each view's frame, in its place in the layout)

 private:
  ArrayView<IRenderer2D* const> views() const { return ArrayView<IRenderer2D* const>(views_, count_); }

  hardware::IGraphicsProcessingUnit* display_;
  IRenderer2D* views_[kMaxViews] = {};
  uint32_t count_ = 0;
  Extent2D extent_;
  bool begun_[kMaxViews] = {};      // (a group the view began: closed again if another view could not)
  List<uint32_t> frame_, view_frame_;
};

}  // namespace graphics
