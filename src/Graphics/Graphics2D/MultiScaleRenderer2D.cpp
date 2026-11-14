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

#include "Graphics/Graphics2D/MultiScaleRenderer2D.hpp"

namespace graphics {

bool MultiScaleRenderer2D::set_views(ArrayView<IRenderer2D* const> views, Extent2D layout) {
  if (views.size() == 0 || views.size() > kMaxViews) return false;
  for (IRenderer2D* v : views) if (!v) return false;      // (checked first: a refused set leaves the views as they were)
  count_ = 0;
  for (IRenderer2D* v : views) views_[count_++] = v;
  extent_ = layout;
  return true;
}

bool MultiScaleRenderer2D::gradients_as_stops() const {
  for (IRenderer2D* v : views()) if (v->gradients_as_stops()) return true;
  return false;
}

bool MultiScaleRenderer2D::draw_image_transformed(const Surface& source, const Rect2D& source_rect, const Matrix2D& m, ImageFilter filter) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->draw_image_transformed(source, source_rect, m, filter) && all;
  return all;
}

bool MultiScaleRenderer2D::draw_image_tiled(const Surface& tile, int32_t origin_x, int32_t origin_y, uint32_t cell_w, uint32_t cell_h, const Rect2D& area) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->draw_image_tiled(tile, origin_x, origin_y, cell_w, cell_h, area) && all;
  return all;
}

// A group is begun on every view or on none: views that began it while another could not are closed again (an empty picture goes on nothing).
bool MultiScaleRenderer2D::begin_group(const Rect2D& bounds) {
  bool all = true;
  for (uint32_t i = 0; i < count_; ++i) {
    begun_[i] = views_[i]->begin_group(bounds);
    all = all && begun_[i];
  }
  if (all) return true;
  for (uint32_t i = 0; i < count_; ++i) if (begun_[i]) (void)views_[i]->end_group(Matrix2D{}, 0, ImageFilter::Nearest);
  return false;
}

bool MultiScaleRenderer2D::begin_filter_group(const Rect2D& bounds) {
  bool all = true;
  for (uint32_t i = 0; i < count_; ++i) {
    begun_[i] = views_[i]->begin_filter_group(bounds);
    all = all && begun_[i];
  }
  if (all) return true;
  for (uint32_t i = 0; i < count_; ++i) if (begun_[i]) (void)views_[i]->end_group(Matrix2D{}, 0, ImageFilter::Nearest);
  return false;
}

bool MultiScaleRenderer2D::end_group(const Matrix2D& m, uint8_t opacity, ImageFilter filter) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->end_group(m, opacity, filter) && all;
  return all;
}

bool MultiScaleRenderer2D::supports_group_filters() const {
  for (IRenderer2D* v : views()) if (!v->supports_group_filters()) return false;
  return true;
}

bool MultiScaleRenderer2D::group_blur(float sigma) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->group_blur(sigma) && all;
  return all;
}

bool MultiScaleRenderer2D::group_color(const ColorTransform& t) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->group_color(t) && all;
  return all;
}

bool MultiScaleRenderer2D::group_drop_shadow(int32_t dx, int32_t dy, float sigma, uint32_t color) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->group_drop_shadow(dx, dy, sigma, color) && all;
  return all;
}

bool MultiScaleRenderer2D::group_cut_out(const Rect2D& rect, const CornerRadii& radii) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->group_cut_out(rect, radii) && all;
  return all;
}

bool MultiScaleRenderer2D::fill_conic_gradient(const Rect2D& rect, Vec2 center, float start_turns, ArrayView<const GradientStop> samples) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->fill_conic_gradient(rect, center, start_turns, samples) && all;
  return all;
}

bool MultiScaleRenderer2D::push_clip_rect(const Rect2D& rect) {
  bool exact = true;
  for (IRenderer2D* v : views()) exact = v->push_clip_rect(rect) && exact;      // (every view pushes: pops stay matched)
  return exact;
}

bool MultiScaleRenderer2D::push_clip_rounded_rect(const Rect2D& rect, const CornerRadii& radii) {
  bool exact = true;
  for (IRenderer2D* v : views()) exact = v->push_clip_rounded_rect(rect, radii) && exact;
  return exact;
}

bool MultiScaleRenderer2D::push_clip_mask(const MaskView& mask, int32_t x, int32_t y) {
  bool exact = true;
  for (IRenderer2D* v : views()) exact = v->push_clip_mask(mask, x, y) && exact;
  return exact;
}

bool MultiScaleRenderer2D::push_layer(const Rect2D& bounds, uint8_t opacity) {
  bool exact = true;
  for (IRenderer2D* v : views()) exact = v->push_layer(bounds, opacity) && exact;
  return exact;
}

bool MultiScaleRenderer2D::draw_device_image(IGraphicsDevice& owner, IImage& image, const Rect2D& source, int32_t x, int32_t y) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->draw_device_image(owner, image, source, x, y) && all;
  return all;
}

bool MultiScaleRenderer2D::draw_device_image_scaled(IGraphicsDevice& owner, IImage& image, const Rect2D& source, const Rect2D& dest, ImageFilter filter) {
  bool all = true;
  for (IRenderer2D* v : views()) all = v->draw_device_image_scaled(owner, image, source, dest, filter) && all;
  return all;
}

uint32_t MultiScaleRenderer2D::scale_percent() const {
  uint32_t largest = 100;
  for (IRenderer2D* v : views()) if (v->scale_percent() > largest) largest = v->scale_percent();
  return largest;
}

uint32_t MultiScaleRenderer2D::device_views() {
  uint32_t n = 0;
  for (IRenderer2D* v : views()) n += v->device_views();
  return n;
}

bool MultiScaleRenderer2D::device_view(uint32_t index, DeviceView* out) {
  for (IRenderer2D* v : views()) {
    const uint32_t n = v->device_views();
    if (index < n) return v->device_view(index, out);
    index -= n;
  }
  return false;
}

void MultiScaleRenderer2D::present() {
  if (presenter_) {
    presenter_->present_views();
    return;
  }
  for (IRenderer2D* v : views()) v->present();
  if (display_) display_->present();
}

bool MultiScaleRenderer2D::read_frame(ArrayView<uint32_t> out) {
  const uint64_t pixels = uint64_t{extent_.width} * extent_.height;
  if (out.size() < pixels) return false;
  for (uint64_t i = 0; i < pixels; ++i) out[i] = 0;
  for (IRenderer2D* v : views()) {
    DeviceView where;
    if (!v->device_view(0, &where)) return false;
    const Extent2D e = v->extent();
    const uint64_t n = uint64_t{e.width} * e.height;
    view_frame_.clear();
    if (!view_frame_.reserve(n)) return false;
    for (uint64_t i = 0; i < n; ++i) view_frame_.push_back(0);
    if (!v->read_frame(view_frame_.view())) return false;
    for (uint32_t y = 0; y < e.height; ++y) {
      const int64_t ly = int64_t{where.origin_y} + y;
      if (ly < 0 || ly >= extent_.height) continue;
      for (uint32_t x = 0; x < e.width; ++x) {
        const int64_t lx = int64_t{where.origin_x} + x;
        if (lx < 0 || lx >= extent_.width) continue;
        out[uint64_t(ly) * extent_.width + uint64_t(lx)] = view_frame_[uint64_t{y} * e.width + x];
      }
    }
  }
  return true;
}

}  // namespace graphics
