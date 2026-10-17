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

#include "Gui/Controls/ImageView.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
constexpr int32_t kSteps[] = {10, 25, 50, 75, 100, 150, 200, 300, 400, 600, 800, 1200, 1600, 3200};
constexpr int32_t kStepCount = static_cast<int32_t>(sizeof kSteps / sizeof kSteps[0]);
constexpr uint32_t kCheckerLight = 0xFFFFFFFF, kCheckerDark = 0xFFC0C0C0;
constexpr int32_t kCheckerSize = 8;
constexpr int32_t kPanStep = 24;

int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

// `top` over `bottom`, both 0xAARRGGBB, `bottom` opaque.
uint32_t blend(uint32_t top, uint32_t bottom) {
  const uint32_t a = top >> 24;
  if (a == 255) return top;
  if (a == 0) return bottom;
  auto channel = [&](int shift) {
    const uint32_t t = (top >> shift) & 0xFF, b = (bottom >> shift) & 0xFF;
    return (t * a + b * (255 - a) + 127) / 255;
  };
  return 0xFF000000u | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}
}  // namespace

Rect2D ImageView::viewport() const {
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder, h = static_cast<int32_t>(bounds_.height) - 2 * kBorder;
  return {kBorder, kBorder, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0)};
}

int32_t ImageView::zoomed_width() const {
  if (!image_ || !image_->valid()) return 0;
  const int64_t w = int64_t{image_->width} * zoom_ / 100;
  return w < 1 ? 1 : static_cast<int32_t>(w);
}

int32_t ImageView::zoomed_height() const {
  if (!image_ || !image_->valid()) return 0;
  const int64_t h = int64_t{image_->height} * zoom_ / 100;
  return h < 1 ? 1 : static_cast<int32_t>(h);
}

// A picture smaller than the viewport is centred (a negative origin); a larger one stays covering it.
void ImageView::clamp_origin() {
  const Rect2D view = viewport();
  const int32_t zw = zoomed_width(), zh = zoomed_height(), vw = static_cast<int32_t>(view.width), vh = static_cast<int32_t>(view.height);
  origin_x_ = zw <= vw ? -(vw - zw) / 2 : clamp(origin_x_, 0, zw - vw);
  origin_y_ = zh <= vh ? -(vh - zh) / 2 : clamp(origin_y_, 0, zh - vh);
}

int32_t ImageView::fit_zoom() const {
  if (!image_ || !image_->valid()) return 100;
  const Rect2D view = viewport();
  if (view.width == 0 || view.height == 0) return 100;
  const int64_t by_width = int64_t{view.width} * 100 / image_->width, by_height = int64_t{view.height} * 100 / image_->height;
  int64_t zoom = by_width < by_height ? by_width : by_height;
  if (zoom > 100) zoom = 100;  // shrink to fit, never enlarge
  return zoom < kMinZoom ? kMinZoom : static_cast<int32_t>(zoom);
}

void ImageView::refit() {
  const int32_t zoom = fit_zoom();
  const bool changed = zoom != zoom_;
  zoom_ = zoom;
  origin_x_ = origin_y_ = 0;
  clamp_origin();
  invalidate();
  if (changed) fire(zoom_changed_);
}

void ImageView::set_image(const graphics::Surface* image) {
  UiGuard lock(this);
  image_ = image;
  ++generation_;
  fit_ = true;
  refit();
}

void ImageView::image_changed() {
  UiGuard lock(this);
  ++generation_;
  invalidate();
}

void ImageView::fit() {
  UiGuard lock(this);
  fit_ = true;
  refit();
}

void ImageView::layout() {
  if (fit_) refit();
  else {
    clamp_origin();
    invalidate();
  }
  Control::layout();
}

// Changes the zoom so that the picture point under (focus_x, focus_y) of the viewport stays where it is.
void ImageView::apply_zoom(int32_t percent, int32_t focus_x, int32_t focus_y) {
  percent = clamp(percent, kMinZoom, kMaxZoom);
  if (percent == zoom_ && !fit_) return;
  const int64_t old_zoom = zoom_;
  const int64_t picture_x = (int64_t{origin_x_} + focus_x) * 100 / old_zoom;  // in picture pixels (x100 for precision below)
  const int64_t picture_y = (int64_t{origin_y_} + focus_y) * 100 / old_zoom;
  const bool changed = percent != zoom_;
  fit_ = false;
  zoom_ = percent;
  origin_x_ = static_cast<int32_t>(picture_x * percent / 100 - focus_x);
  origin_y_ = static_cast<int32_t>(picture_y * percent / 100 - focus_y);
  clamp_origin();
  invalidate();
  if (changed) fire(zoom_changed_);
}

void ImageView::set_zoom(int32_t percent) {
  UiGuard lock(this);
  if (!image_ || !image_->valid()) return;
  const Rect2D view = viewport();
  apply_zoom(percent, static_cast<int32_t>(view.width) / 2, static_cast<int32_t>(view.height) / 2);
}

void ImageView::zoom_in() {
  UiGuard lock(this);
  for (int32_t i = 0; i < kStepCount; ++i)
    if (kSteps[i] > zoom_) {
      set_zoom(kSteps[i]);
      return;
    }
}

void ImageView::zoom_out() {
  UiGuard lock(this);
  for (int32_t i = kStepCount - 1; i >= 0; --i)
    if (kSteps[i] < zoom_) {
      set_zoom(kSteps[i]);
      return;
    }
  set_zoom(kMinZoom);
}

void ImageView::scroll_to(int32_t x, int32_t y) {
  UiGuard lock(this);
  origin_x_ = x;
  origin_y_ = y;
  clamp_origin();
  invalidate();
}

// The colour of the picture at a position of the zoomed picture. Zooming in repeats pixels; zooming out averages
// the block of picture pixels the position stands for.
uint32_t ImageView::sample(int32_t zx, int32_t zy) const {
  const graphics::Surface& s = *image_;
  if (zoom_ >= 100) {
    const uint32_t x = static_cast<uint32_t>(int64_t{zx} * 100 / zoom_), y = static_cast<uint32_t>(int64_t{zy} * 100 / zoom_);
    return s.decode(s.row(y < s.height ? y : s.height - 1)[x < s.width ? x : s.width - 1]);
  }
  const int64_t x0 = int64_t{zx} * 100 / zoom_, x1 = (int64_t{zx} + 1) * 100 / zoom_;
  const int64_t y0 = int64_t{zy} * 100 / zoom_, y1 = (int64_t{zy} + 1) * 100 / zoom_;
  const uint32_t xe = static_cast<uint32_t>(x1 > x0 ? x1 : x0 + 1), ye = static_cast<uint32_t>(y1 > y0 ? y1 : y0 + 1);
  uint64_t sum[4] = {0, 0, 0, 0};
  uint32_t count = 0;
  for (uint32_t y = static_cast<uint32_t>(y0); y < ye && y < s.height; ++y) {
    const uint32_t* row = s.row(y);
    for (uint32_t x = static_cast<uint32_t>(x0); x < xe && x < s.width; ++x) {
      const uint32_t p = s.decode(row[x]);
      sum[0] += p >> 24;
      sum[1] += (p >> 16) & 0xFF;
      sum[2] += (p >> 8) & 0xFF;
      sum[3] += p & 0xFF;
      ++count;
    }
  }
  if (count == 0) return 0;
  return (static_cast<uint32_t>(sum[0] / count) << 24) | (static_cast<uint32_t>(sum[1] / count) << 16) |
         (static_cast<uint32_t>(sum[2] / count) << 8) | static_cast<uint32_t>(sum[3] / count);
}

void ImageView::rebuild_cache() {
  const Rect2D view = viewport();
  if (view.width == 0 || view.height == 0) return;
  const bool same = cache_width_ == view.width && cache_height_ == view.height && cache_zoom_ == zoom_ && cache_x_ == origin_x_ &&
                    cache_y_ == origin_y_ && cache_generation_ == generation_ && cache_image_ == image_ && !cache_.empty();
  if (same) return;
  if (cache_width_ != view.width || cache_height_ != view.height || cache_.empty()) {
    release_view(allocator(), cache_);
    cache_ = allocate_view<uint32_t>(allocator(), uint64_t{view.width} * view.height);
    if (cache_.empty()) {
      cache_width_ = cache_height_ = 0;
      return;
    }
  }
  const int32_t zw = zoomed_width(), zh = zoomed_height();
  for (uint32_t vy = 0; vy < view.height; ++vy) {
    const int32_t zy = origin_y_ + static_cast<int32_t>(vy);
    for (uint32_t vx = 0; vx < view.width; ++vx) {
      const int32_t zx = origin_x_ + static_cast<int32_t>(vx);
      uint32_t color;
      const bool checker_dark = ((static_cast<int32_t>(vx) / kCheckerSize) + (static_cast<int32_t>(vy) / kCheckerSize)) % 2 != 0;
      if (!image_ || zx < 0 || zy < 0 || zx >= zw || zy >= zh) color = 0xFF808080;  // outside the picture
      else color = blend(sample(zx, zy), checker_dark ? kCheckerDark : kCheckerLight);
      cache_[uint64_t{vy} * view.width + vx] = color;
    }
  }
  cache_width_ = view.width;
  cache_height_ = view.height;
  cache_zoom_ = zoom_;
  cache_x_ = origin_x_;
  cache_y_ = origin_y_;
  cache_generation_ = generation_;
  cache_image_ = image_;
}

void ImageView::paint(Painter& painter) {
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, 0xFF808080);
  painter.bevel(all, true);
  const Rect2D view = viewport();
  if (view.width == 0 || view.height == 0) return;
  if (!image_ || !image_->valid()) {
    painter.text_centered(view, "No image", 0xFFC0C0C0);
    return;
  }
  rebuild_cache();
  if (cache_.empty()) return;
  graphics::Surface surface;
  surface.pixels = cache_;
  surface.width = view.width;
  surface.height = view.height;
  surface.pitch = view.width;
  painter.image(surface, {0, 0, view.width, view.height}, view.x, view.y);
  if (focused()) painter.focus_frame({1, 1, bounds_.width - 2, bounds_.height - 2});
}

void ImageView::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  panning_ = true;
  pan_x_ = args.x;
  pan_y_ = args.y;
  pan_origin_x_ = origin_x_;
  pan_origin_y_ = origin_y_;
}

void ImageView::on_mouse_move(const MouseArgs& args) {
  if (!panning_) return;
  origin_x_ = pan_origin_x_ - (args.x - pan_x_);
  origin_y_ = pan_origin_y_ - (args.y - pan_y_);
  clamp_origin();
  invalidate();
}

void ImageView::on_mouse_wheel(const MouseArgs& args) {
  if (!image_ || !image_->valid() || args.wheel == 0) return;
  const Rect2D view = viewport();
  const int32_t fx = clamp(args.x - view.x, 0, static_cast<int32_t>(view.width)), fy = clamp(args.y - view.y, 0, static_cast<int32_t>(view.height));
  int32_t target = zoom_;
  if (args.wheel > 0) {
    for (int32_t i = 0; i < kStepCount; ++i)
      if (kSteps[i] > zoom_) { target = kSteps[i]; break; }
  } else {
    target = kMinZoom;
    for (int32_t i = kStepCount - 1; i >= 0; --i)
      if (kSteps[i] < zoom_) { target = kSteps[i]; break; }
  }
  apply_zoom(target, fx, fy);
}

bool ImageView::on_key(const KeyArgs& args) {
  using namespace hardware;
  switch (args.usage) {
    case key::kLeft: scroll_to(origin_x_ - kPanStep, origin_y_); return true;
    case key::kRight: scroll_to(origin_x_ + kPanStep, origin_y_); return true;
    case key::kUp: scroll_to(origin_x_, origin_y_ - kPanStep); return true;
    case key::kDown: scroll_to(origin_x_, origin_y_ + kPanStep); return true;
    default: break;
  }
  switch (args.character) {
    case '+':
    case '=': zoom_in(); return true;
    case '-': zoom_out(); return true;
    case '0':
    case '1': actual_size(); return true;
    case 'f':
    case 'F': fit(); return true;
    default: return false;
  }
}

}  // namespace gui
