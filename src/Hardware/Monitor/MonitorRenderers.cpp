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

#include "Hardware/Monitor/MonitorRenderers.hpp"
#include "Graphics/DeviceScale.hpp"

namespace hardware {

void MonitorRenderers::Placed::fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  if (x >= w_ || y >= h_) return;
  if (w > w_ - x) w = w_ - x;
  if (h > h_ - y) h = h_ - y;
  gpu_.fill_rect(x_ + x, y_ + y, w, h, color);
}

void MonitorRenderers::Placed::blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels, uint32_t pitch_pixels) {
  if (x >= w_ || y >= h_) return;      // (the software renderer blits its own picture: never past it)
  if (w > w_ - x) w = w_ - x;
  if (h > h_ - y) h = h_ - y;
  gpu_.blit(x_ + x, y_ + y, w, h, pixels, pitch_pixels);
}

bool MonitorRenderers::mixed_scales(const MonitorLayout& layout) {
  uint32_t seen = 0;
  for (uint32_t i = 0; i < layout.count(); ++i) {
    const MonitorSlot& s = layout.at(i);
    if (!s.enabled) continue;
    const uint32_t k = s.scale_percent ? s.scale_percent : 100;
    if (seen != 0 && k != seen) return true;
    seen = k;
  }
  return false;
}

bool MonitorRenderers::update(const MonitorLayout& layout, const ScanoutPlacement* physical, uint32_t count, graphics::ICellTextFace* text,
                              graphics::IGraphicsDevice* device) {
  // on a GPU: the display-sized picture first (no device, or one that cannot make it: the monitors are drawn on the CPU)
  UniquePointer<graphics::DeviceRenderer2D> compositor;
  if (device) {
    compositor = UniquePointer<graphics::DeviceRenderer2D>::make(allocator_, *device, allocator_, graphics::Extent2D{gpu_.width(), gpu_.height()});
    if (compositor && !compositor->valid()) compositor.reset();
  }
  List<UniquePointer<Output>> fresh(allocator_);
  graphics::IRenderer2D* views[graphics::MultiScaleRenderer2D::kMaxViews];
  uint32_t n = 0;
  int64_t right = 0, bottom = 0;
  for (uint32_t i = 0; i < layout.count(); ++i) {
    const MonitorSlot& s = layout.at(i);
    if (!s.enabled) continue;
    const ScanoutPlacement* place = nullptr;
    for (uint32_t j = 0; j < count; ++j) if (physical[j].connector == s.id) place = &physical[j];
    if (!place || place->width == 0 || place->height == 0 || n >= graphics::MultiScaleRenderer2D::kMaxViews) return false;
    if (place->x > gpu_.width() || place->width > gpu_.width() - place->x || place->y > gpu_.height() || place->height > gpu_.height() - place->y) return false;
    UniquePointer<Output> out = UniquePointer<Output>::make(allocator_, allocator_);
    if (!out) return false;
    out->x = place->x;
    out->y = place->y;
    out->w = place->width;
    out->h = place->height;
    graphics::IRenderer2D* target = nullptr;
    if (compositor) {      // the monitor's picture on the GPU
      out->gpu = UniquePointer<graphics::DeviceRenderer2D>::make(allocator_, *device, allocator_, graphics::Extent2D{place->width, place->height}, true);
      if (!out->gpu || !out->gpu->valid()) return false;
      target = out->gpu.get();
    } else {      // on the CPU, presented into its place on the display
      if (!out->pixels.reserve(uint64_t{place->width} * place->height)) return false;
      for (uint64_t p = 0; p < uint64_t{place->width} * place->height; ++p) out->pixels.push_back(0xFF000000u);
      out->placed = UniquePointer<Placed>::make(allocator_, gpu_, place->x, place->y, place->width, place->height);
      graphics::Surface surface;
      surface.pixels = out->pixels.view();
      surface.width = surface.pitch = place->width;
      surface.height = place->height;
      if (!out->placed) return false;
      out->software = UniquePointer<graphics::SoftwareRenderer2D>::make(allocator_, surface, out->placed.get(), &allocator_);
      if (!out->software) return false;
      target = out->software.get();
    }
    out->scaled = UniquePointer<graphics::ScaledRenderer2D>::make(allocator_, allocator_, *target, s.scale_percent ? s.scale_percent : 100);
    if (!out->scaled) return false;
    out->scaled->set_origin(s.rect.x, s.rect.y);
    out->scaled->set_text_face(text);
    views[n++] = out->scaled.get();
    if (int64_t{s.rect.x} + s.rect.width > right) right = int64_t{s.rect.x} + s.rect.width;
    if (int64_t{s.rect.y} + s.rect.height > bottom) bottom = int64_t{s.rect.y} + s.rect.height;
    if (!fresh.push_back(static_cast<UniquePointer<Output>&&>(out))) return false;
  }
  if (n == 0 || right <= 0 || bottom <= 0 || right > 65535 || bottom > 65535) return false;
  if (!outputs_.reserve(fresh.count())) return false;      // (so that moving the new outputs over cannot fail half way)
  if (!multi_.set_views(ArrayView<graphics::IRenderer2D* const>(views, n), graphics::Extent2D{uint32_t(right), uint32_t(bottom)})) return false;
  outputs_.clear();
  for (UniquePointer<Output>& out : fresh.view()) outputs_.push_back(static_cast<UniquePointer<Output>&&>(out));
  compositor_ = static_cast<UniquePointer<graphics::DeviceRenderer2D>&&>(compositor);
  device_ = compositor_ ? device : nullptr;
  multi_.set_presenter(compositor_ ? this : nullptr);
  return true;
}

// On a GPU: each monitor's picture laid at its place in the display-sized one (pixels copied as they are), which is presented.
bool MonitorRenderers::compose() {
  if (!compositor_ || !device_) return false;
  bool all = true;
  compositor_->set_blend(graphics::BlendMode::Replace);
  for (const UniquePointer<Output>& out : outputs_.view()) {
    graphics::IImage* image = out->gpu ? out->gpu->frame_image() : nullptr;
    all = image && compositor_->draw_device_image(*device_, *image, graphics::Rect2D{0, 0, out->w, out->h}, int32_t(out->x), int32_t(out->y)) && all;
  }
  compositor_->set_blend(graphics::BlendMode::Alpha);
  return all;
}

void MonitorRenderers::present_views() {
  if (compose() || compositor_) compositor_->present();
}

}  // namespace hardware
