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

#include "Gui/Controls/PageRasterizer.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Paint/DisplayPainter.hpp"

namespace gui {

bool changed_objects_area(const paint::DisplayList& list, ArrayView<const uint32_t> nodes, int32_t dx, int32_t dy, const graphics::Rect2D& bounds,
                          graphics::Rect2D* area) {
  int64_t ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
  bool any = false;
  uint32_t nesting = 0;   // open transforms and filters
  paint::IRect outer;     // the outermost one's bounds
  for (uint64_t i = 0; i < list.count(); ++i) {
    const paint::Op& op = list.op(i);
    if (op.kind == paint::OpKind::PushTransform || op.kind == paint::OpKind::PushFilter) {
      if (nesting++ == 0) outer = op.rect;
      continue;
    }
    if (op.kind == paint::OpKind::PopTransform || op.kind == paint::OpKind::PopFilter) {
      if (nesting) --nesting;
      continue;
    }
    if (op.kind != paint::OpKind::Object) continue;
    bool wanted = false;
    for (const uint32_t node : nodes) wanted = wanted || node == op.node;
    if (!wanted) continue;
    const paint::IRect& rect = nesting ? outer : op.rect;
    if (rect.empty()) continue;
    const int64_t x0 = int64_t{dx} + rect.x, y0 = int64_t{dy} + rect.y;
    const int64_t x1 = x0 + rect.width, y1 = y0 + rect.height;
    if (!any) ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1;
    else {
      if (x0 < ax0) ax0 = x0;
      if (y0 < ay0) ay0 = y0;
      if (x1 > ax1) ax1 = x1;
      if (y1 > ay1) ay1 = y1;
    }
    any = true;
  }
  if (!any) return false;
  // (inside the bounds, so the numbers fit)
  const int64_t bx1 = int64_t{bounds.x} + bounds.width, by1 = int64_t{bounds.y} + bounds.height;
  if (ax0 < bounds.x) ax0 = bounds.x;
  if (ay0 < bounds.y) ay0 = bounds.y;
  if (ax1 > bx1) ax1 = bx1;
  if (ay1 > by1) ay1 = by1;
  if (ax1 <= ax0 || ay1 <= ay0) return false;
  *area = {static_cast<int32_t>(ax0), static_cast<int32_t>(ay0), static_cast<uint32_t>(ax1 - ax0), static_cast<uint32_t>(ay1 - ay0)};
  return true;
}

namespace {
graphics::Rect2D union_of(const graphics::Rect2D& a, const graphics::Rect2D& b) {
  if (!a.width || !a.height) return b;
  if (!b.width || !b.height) return a;
  const int64_t x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
  const int64_t ax1 = int64_t{a.x} + a.width, bx1 = int64_t{b.x} + b.width, ay1 = int64_t{a.y} + a.height, by1 = int64_t{b.y} + b.height;
  const int64_t x1 = ax1 > bx1 ? ax1 : bx1, y1 = ay1 > by1 ? ay1 : by1;
  return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
}

// The rows of `area` from one buffer to the other (the same size).
void copy_area(const graphics::Surface& from, const graphics::Surface& to, const graphics::Rect2D& area) {
  for (uint32_t j = 0; j < area.height; ++j) {
    const uint32_t y = static_cast<uint32_t>(area.y) + j;
    const ArrayView<const uint32_t> src = ArrayView<const uint32_t>(from.row(y), from.width).subview(static_cast<uint64_t>(area.x), area.width);
    ArrayView<uint32_t>(to.row(y), to.width).subview(static_cast<uint64_t>(area.x), area.width).copy_from(src);
  }
}
}  // namespace

bool PageRasterizer::start(void (*ready)(void* context), void* context) {
  if (!kernel::running() || running()) return false;
  ready_ = ready;
  ready_context_ = context;
  __atomic_store_n(&stop_, false, __ATOMIC_RELEASE);
  kernel::Thread* thread = kernel::spawn(&PageRasterizer::thread_main, this, "page-paint");
  if (!thread) return false;
  __atomic_store_n(&thread_, thread, __ATOMIC_RELEASE);
  return true;
}

void PageRasterizer::stop() {
  kernel::Thread* thread = __atomic_load_n(&thread_, __ATOMIC_ACQUIRE);
  if (thread) {
    __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
    wake_.set();
    kernel::join(thread);
    __atomic_store_n(&thread_, static_cast<kernel::Thread*>(nullptr), __ATOMIC_RELEASE);
  }
  kernel::ScopedMutex guard(mutex_);
  pending_ = Request();
  has_pending_ = false;
  release_buffers();
}

void PageRasterizer::request(SharedPointer<browser::PagePicture> picture, uint64_t version, int32_t scroll_y, uint32_t width, uint32_t height,
                             uint32_t paper, ArrayView<const uint32_t> nodes) {
  {
    kernel::ScopedMutex guard(mutex_);
    Request& r = pending_;
    if (!has_pending_) {
      r.node_count = 0;
      r.all_nodes = false;
    }
    r.picture = util::move(picture);
    r.version = version;
    r.scroll_y = scroll_y;
    r.width = width;
    r.height = height;
    r.paper = paper | 0xFF000000u;   // (the page is laid on as it is: opaque)
    for (const uint32_t node : nodes) {
      bool known = false;
      for (uint32_t i = 0; i < r.node_count && !known; ++i) known = r.nodes[i] == node;
      if (known) continue;
      if (r.node_count == kMaxChangedObjects) r.all_nodes = true;
      else r.nodes[r.node_count++] = node;
    }
    has_pending_ = true;
  }
  wake_.set();
}

void PageRasterizer::set_object_painter(browser::IObjectPainter* painter) {
  kernel::ScopedMutex guard(paint_mutex_);   // (a drawing in progress finishes with the painter it began with)
  objects_ = painter;
}

bool PageRasterizer::wait_idle(uint64_t timeout_us) {
  for (uint64_t waited = 0;; waited += 1000) {
    {
      kernel::ScopedMutex guard(mutex_);
      if (!has_pending_ && !busy_) return true;
    }
    if (waited >= timeout_us) return false;
    kernel::sleep_us(1000);
  }
}

void PageRasterizer::thread_main(void* self) { static_cast<PageRasterizer*>(self)->run(); }

void PageRasterizer::run() {
  fonts_ = UniquePointer<font::FontSet>::make(allocator_, allocator_);   // (fonts of its own: a font set caches what it draws, and is not shared between threads)
  if (fonts_) fonts_->add_bundled_fonts();
  while (!__atomic_load_n(&stop_, __ATOMIC_ACQUIRE)) {
    wake_.wait(100000);
    Request r;
    {
      kernel::ScopedMutex guard(mutex_);
      if (!has_pending_) continue;
      r = util::move(pending_);
      pending_ = Request();
      has_pending_ = false;
      busy_ = true;
    }
    const bool drawn = r.picture && fonts_ && draw(r);
    r.picture = SharedPointer<browser::PagePicture>();   // (the last owner may be this thread: the picture goes here, not under a lock)
    {
      kernel::ScopedMutex guard(mutex_);
      busy_ = false;
    }
    if (drawn && ready_) ready_(ready_context_);
  }
  fonts_ = UniquePointer<font::FontSet>();
}

bool PageRasterizer::ensure_buffers(uint32_t width, uint32_t height) {
  if (front_.width == width && front_.height == height && !front_.pixels.empty() && !back_.pixels.empty()) return true;
  kernel::ScopedMutex guard(mutex_);   // (the front buffer is the view's to read)
  release_buffers();
  const uint64_t count = uint64_t{width} * height;
  front_.pixels = allocate_view<uint32_t>(allocator_, count);
  back_.pixels = allocate_view<uint32_t>(allocator_, count);
  if (front_.pixels.empty() || back_.pixels.empty()) {
    release_buffers();
    return false;
  }
  front_.width = back_.width = front_.pitch = back_.pitch = width;
  front_.height = back_.height = height;
  drawn_any_ = false;
  return true;
}

void PageRasterizer::release_buffers() {
  if (!front_.pixels.empty()) release_view(allocator_, front_.pixels);
  if (!back_.pixels.empty()) release_view(allocator_, back_.pixels);
  front_ = graphics::Surface();
  back_ = graphics::Surface();
  front_complete_ = false;
  damage_ = {};
}

bool PageRasterizer::draw(const Request& r) {
  if (!r.width || !r.height || uint64_t{r.width} * r.height > (uint64_t{1} << 24)) return false;
  if (!ensure_buffers(r.width, r.height)) return false;
  const Rect2D whole{0, 0, r.width, r.height};
  Rect2D area = whole;
  const bool full = !drawn_any_ || r.version != drawn_version_ || r.scroll_y != drawn_scroll_ || r.all_nodes;
  if (!full && !changed_objects_area(r.picture->list, ArrayView<const uint32_t>(r.nodes, r.node_count), 0, -r.scroll_y, whole, &area)) return false;   // (nothing shown changed)
  {
    kernel::ScopedMutex guard(paint_mutex_);
    graphics::SoftwareRenderer2D renderer(back_, nullptr, &allocator_);
    renderer.set_clip(area);
    renderer.set_blend(graphics::BlendMode::Alpha);
    renderer.fill_rect(area, r.paper);
    paint::DisplayPainter painter(allocator_, *fonts_, objects_);
    painter.paint(renderer, r.picture->list, 0, -r.scroll_y, area);
  }
  {
    kernel::ScopedMutex guard(mutex_);
    const graphics::Surface done = back_;
    back_ = front_;
    front_ = done;
    front_complete_ = true;
    damage_ = union_of(damage_, area);
    ++drawings_;
  }
  // The new back buffer is the drawing before this one: it gets this drawing's part, so the two agree again. (The view reads only the front buffer.)
  if (full) copy_area(front_, back_, whole);
  else copy_area(front_, back_, area);
  drawn_any_ = true;
  drawn_version_ = r.version;
  drawn_scroll_ = r.scroll_y;
  return true;
}

}  // namespace gui
