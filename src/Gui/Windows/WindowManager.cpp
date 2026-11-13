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

#include "Gui/Windows/WindowManager.hpp"
#include "Gui/Window.hpp"

namespace gui {

namespace {
Frame* as_frame(Control* c) { return static_cast<Frame*>(c); }  // a manager only ever holds Frames
bool same_rect(const Rect2D& a, const Rect2D& b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; }
}

bool WindowManager::has_modal() const {
  for (uint32_t i = 0; i < child_count(); ++i)
    if (as_frame(child(i))->modal()) return true;
  return false;
}

uint32_t WindowManager::stacking_index(const Frame* frame) const {
  for (uint32_t i = 0; i < child_count(); ++i)
    if (child(i) == frame) return i;
  return ~0u;
}

// --- Areas --------------------------------------------------------------------------------------------

Rect2D WindowManager::full_area(uint32_t index) const {
  if (index < areas_.count()) return areas_[index];
  return {0, 0, bounds_.width, bounds_.height};
}

Rect2D WindowManager::area(uint32_t index) const { return without_reserved(full_area(index)); }

// Each window that reserves an edge and overlaps the area takes that much off it, from that edge (several on one edge: the largest). The area keeps at
// least a pixel each way.
Rect2D WindowManager::without_reserved(const Rect2D& a) const {
  uint32_t inset[5] = {0, 0, 0, 0, 0};      // by Frame::Edge
  for (uint32_t i = 0; i < child_count(); ++i) {
    const Frame* f = as_frame(child(i));
    if (f->reserve_edge_ == Frame::Edge::None || f->reserve_size_ == 0 || !f->visible() || f->state_ == Frame::State::Minimized) continue;
    const Rect2D b = f->bounds();
    const bool overlap = int64_t{b.x} < int64_t{a.x} + a.width && int64_t{a.x} < int64_t{b.x} + b.width && int64_t{b.y} < int64_t{a.y} + a.height &&
                         int64_t{a.y} < int64_t{b.y} + b.height;
    if (!overlap) continue;
    uint32_t& k = inset[static_cast<uint8_t>(f->reserve_edge_)];
    if (f->reserve_size_ > k) k = f->reserve_size_;
  }
  const uint32_t l = inset[1], t = inset[2], r = inset[3], b = inset[4];
  Rect2D out = a;
  const uint64_t across = uint64_t{l} + r, down = uint64_t{t} + b;
  if (across + 1 > a.width || down + 1 > a.height) {      // (more than the area: kept at a pixel, from the left or top edge's side)
    out.x = a.x + static_cast<int32_t>(l < a.width ? l : a.width - 1);
    out.y = a.y + static_cast<int32_t>(t < a.height ? t : a.height - 1);
    out.width = across + 1 > a.width ? 1 : a.width - static_cast<uint32_t>(across);
    out.height = down + 1 > a.height ? 1 : a.height - static_cast<uint32_t>(down);
    if (a.width == 0) out.width = 0, out.x = a.x;
    if (a.height == 0) out.height = 0, out.y = a.y;
    return out;
  }
  out.x = a.x + static_cast<int32_t>(l);
  out.y = a.y + static_cast<int32_t>(t);
  out.width = a.width - static_cast<uint32_t>(across);
  out.height = a.height - static_cast<uint32_t>(down);
  return out;
}

void WindowManager::reserves_changed() {
  ++reserve_serial_;
  layout();      // maximized, docked and tiled windows follow
  invalidate();
}

// --- Layers -------------------------------------------------------------------------------------------

void WindowManager::raise_in_layer(Frame* frame) {
  // directly below the lowest window of a higher layer, else on top
  for (uint32_t i = 0; i < child_count(); ++i) {
    Frame* f = as_frame(child(i));
    if (f != frame && f->layer_ > frame->layer_) {
      move_child_next_to(frame, f, false);
      return;
    }
  }
  move_child_to_front(frame);
}

void WindowManager::sort_layers() {
  bool sorted = true;
  for (uint32_t i = 1; i < child_count() && sorted; ++i)
    if (as_frame(child(i - 1))->layer_ > as_frame(child(i))->layer_) sorted = false;
  if (sorted) return;
  // stable: band by band from the bottom, each window to the front in the order it had
  constexpr uint32_t kMax = 256;
  Frame* stack[kMax];
  const uint32_t count = child_count() < kMax ? child_count() : kMax;
  for (uint32_t i = 0; i < count; ++i) stack[i] = as_frame(child(i));
  for (int layer = -2; layer <= 2; ++layer)
    for (uint32_t i = 0; i < count; ++i)
      if (static_cast<int>(stack[i]->layer_) == layer) move_child_to_front(stack[i]);
  invalidate();
}

bool WindowManager::place_next_to(Frame* frame, Frame* anchor, bool above) {
  UiGuard lock(this);
  const bool moved = move_child_next_to(frame, anchor, above);
  sort_layers();
  return moved;
}

Rect2D WindowManager::area_of_rect(const Rect2D& r) const { return without_reserved(raw_area_of_rect(r)); }

Rect2D WindowManager::raw_area_of_rect(const Rect2D& r) const {
  if (areas_.count() == 0) return {0, 0, bounds_.width, bounds_.height};
  int64_t best = -1;
  uint64_t best_overlap = 0;
  for (uint64_t i = 0; i < areas_.count(); ++i) {
    const Rect2D& a = areas_[i];
    const int64_t l = a.x > r.x ? a.x : r.x, t = a.y > r.y ? a.y : r.y;
    const int64_t rr = a.x + int64_t{a.width} < r.x + int64_t{r.width} ? a.x + int64_t{a.width} : r.x + int64_t{r.width};
    const int64_t bb = a.y + int64_t{a.height} < r.y + int64_t{r.height} ? a.y + int64_t{a.height} : r.y + int64_t{r.height};
    const uint64_t overlap = rr > l && bb > t ? static_cast<uint64_t>(rr - l) * static_cast<uint64_t>(bb - t) : 0;
    if (overlap > best_overlap) {
      best = static_cast<int64_t>(i);
      best_overlap = overlap;
    }
  }
  if (best >= 0) return areas_[static_cast<uint64_t>(best)];
  // On no area at all: the nearest to its centre.
  const int64_t cx = r.x + int64_t{r.width} / 2, cy = r.y + int64_t{r.height} / 2;
  uint64_t nearest = 0, nearest_distance = ~uint64_t{0};
  for (uint64_t i = 0; i < areas_.count(); ++i) {
    const Rect2D& a = areas_[i];
    const int64_t px = cx < a.x ? a.x : cx >= a.x + int64_t{a.width} ? a.x + int64_t{a.width} - 1 : cx;
    const int64_t py = cy < a.y ? a.y : cy >= a.y + int64_t{a.height} ? a.y + int64_t{a.height} - 1 : cy;
    const uint64_t d = static_cast<uint64_t>((cx - px) * (cx - px) + (cy - py) * (cy - py));
    if (d < nearest_distance) {
      nearest = i;
      nearest_distance = d;
    }
  }
  return areas_[nearest];
}

Rect2D WindowManager::area_for(const Frame* frame) const { return area_of_rect(frame->bounds()); }

Rect2D WindowManager::dialog_area() const {
  if (areas_.count() == 0) return without_reserved({0, 0, bounds_.width, bounds_.height});
  if (active_ && !active_->modal()) return area_for(active_);
  return without_reserved(areas_[0]);
}

void WindowManager::set_areas(const Rect2D* areas, uint32_t count) {
  UiGuard lock(this);
  areas_.clear();
  for (uint32_t i = 0; i < count; ++i) areas_.push_back(areas[i]);
  layout();   // maximized, docked and tiled windows follow; the others are kept where they can be reached
  invalidate();
}

void WindowManager::keep_inside(Frame* frame) {
  if (bounds_.width == 0 || bounds_.height == 0) return;  // not laid out yet
  if (frame->layer_ != Frame::Layer::Normal) return;      // (placed where its program says)
  Rect2D b = frame->bounds();
  const Rect2D a = area_for(frame);
  const int32_t left = a.x, top = a.y, right = a.x + static_cast<int32_t>(a.width), bottom = a.y + static_cast<int32_t>(a.height);
  int32_t x = b.x, y = b.y;
  if (y < top) y = top;
  if (y > bottom - Frame::kBorder - Frame::kTitleHeight) y = bottom - Frame::kBorder - Frame::kTitleHeight;
  if (x + static_cast<int32_t>(b.width) < left + 80) x = left + 80 - static_cast<int32_t>(b.width);
  if (x > right - 80) x = right - 80;
  if (x != b.x || y != b.y) frame->set_bounds({x, y, b.width, b.height});
}

bool WindowManager::add_window(Frame* frame, bool modal) {
  UiGuard lock(this);
  if (!frame || frame->manager_) return false;
  if (!order_.push_back(frame)) return false;
  if (!add(frame)) {
    order_.remove_at(order_.count() - 1);
    return false;
  }
  __atomic_store_n(&frame->manager_, this, __ATOMIC_RELEASE);
  frame->manager_changed();
  frame->modal_ = modal;
  frame->state_ = Frame::State::Normal;
  frame->set_visible(true);
  keep_inside(frame);
  if (frame->activatable()) activate(frame);
  else {
    raise_in_layer(frame);
    refresh();
  }
  if (frame->reserve_edge_ != Frame::Edge::None) reserves_changed();
  return true;
}

bool WindowManager::remove_window(Frame* frame) {
  UiGuard lock(this);
  if (!frame || frame->manager_ != this) return false;
  for (uint64_t i = 0; i < order_.count(); ++i)
    if (order_[i] == frame) {
      order_.remove_at(i);
      break;
    }
  remove(frame);
  __atomic_store_n(&frame->manager_, static_cast<WindowManager*>(nullptr), __ATOMIC_RELEASE);
  frame->manager_changed();
  frame->modal_ = false;
  frame->active_ = false;
  frame->set_enabled(true);
  frame->pressed_ = frame->dragging_ = Frame::Part::None;
  if (frame->state_ == Frame::State::Maximized || frame->docked_ != Frame::Dock::None) frame->set_bounds(frame->restore_bounds_);
  const bool reserved = frame->reserve_edge_ != Frame::Edge::None;
  frame->docked_ = Frame::Dock::None;
  frame->state_ = Frame::State::Normal;
  frame->set_visible(true);
  if (active_ == frame) active_ = nullptr;
  refresh();
  if (reserved) reserves_changed();
  return true;
}

void WindowManager::close_window(Frame* frame) {
  UiGuard lock(this);
  if (!frame || frame->manager_ != this) return;
  const Handler closed = frame->closed_;
  Window* w = window();
  remove_window(frame);
  frame->window_closed(w);
  if (!closed) return;
  if (w) w->dispatch(closed, *frame);
  else closed(*frame);
}

Frame* WindowManager::top_usable() const {
  for (uint32_t i = child_count(); i > 0; --i) {
    Frame* f = as_frame(child(i - 1));
    if (f->visible() && f->enabled() && f->activatable() && f->state() != Frame::State::Minimized) return f;
  }
  return nullptr;
}

void WindowManager::refresh() {
  // The topmost modal window blocks every other one.
  Frame* modal = nullptr;
  for (uint32_t i = child_count(); i > 0 && !modal; --i)
    if (as_frame(child(i - 1))->modal()) modal = as_frame(child(i - 1));
  if (modal) raise_in_layer(modal);
  for (uint32_t i = 0; i < child_count(); ++i) {
    Frame* f = as_frame(child(i));
    const bool usable = !modal || f == modal;
    if (f->enabled() != usable) f->set_enabled(usable);
  }
  if (modal) active_ = modal;
  else if (!active_ || !active_->visible() || !active_->enabled() || !active_->activatable()) active_ = top_usable();
  for (uint32_t i = 0; i < child_count(); ++i) as_frame(child(i))->active_ = as_frame(child(i)) == active_;
  if (Window* w = window()) w->set_focus_scope(active_);
  invalidate();
}

void WindowManager::activate(Frame* frame) {
  UiGuard lock(this);
  if (!frame || frame->manager_ != this || !frame->enabled()) return;
  if (frame->state_ == Frame::State::Minimized) {
    frame->state_ = frame->before_minimize_;
    frame->set_visible(true);
    if (frame->reserve_edge_ != Frame::Edge::None) reserves_changed();
  }
  raise_in_layer(frame);
  if (frame->activatable()) active_ = frame;
  refresh();
}

void WindowManager::cycle(bool forward) {
  UiGuard lock(this);
  if (has_modal()) return;
  // (only the normal band: panels, backgrounds and overlays are not cycled through)
  constexpr uint32_t kMax = 64;
  Frame* stack[kMax];
  uint32_t count = 0;
  for (uint32_t i = 0; i < child_count(); ++i) {
    Frame* f = as_frame(child(i));
    if (f->layer_ != Frame::Layer::Normal) continue;
    if (count == kMax) return;
    stack[count++] = f;
  }
  if (count < 2) return;
  if (forward) {
    activate(stack[0]);
    return;
  }
  // Each window but the front one comes to the front in turn, bottom first: the old front window ends at the bottom, the one that was
  // directly behind it is in front now. (Minimized windows are passed over, unless it is the one to come out in front.)
  for (uint32_t i = 0; i + 1 < count; ++i)
    if (stack[i]->state() != Frame::State::Minimized || i + 2 == count) activate(stack[i]);
}

void WindowManager::minimize(Frame* frame) {
  UiGuard lock(this);
  if (!frame || frame->manager_ != this || frame->state_ == Frame::State::Minimized) return;
  frame->before_minimize_ = frame->state_;
  frame->state_ = Frame::State::Minimized;
  frame->set_visible(false);
  if (active_ == frame) active_ = nullptr;
  if (frame->reserve_edge_ != Frame::Edge::None) reserves_changed();
  refresh();
}

void WindowManager::maximize(Frame* frame) {
  UiGuard lock(this);
  if (!frame || frame->manager_ != this) return;
  if (frame->state_ == Frame::State::Minimized) activate(frame);
  if (frame->state_ == Frame::State::Maximized) {
    frame->state_ = Frame::State::Normal;
    frame->set_bounds(frame->restore_bounds_);
  } else {
    if (frame->docked_ == Frame::Dock::None) frame->restore_bounds_ = frame->bounds();   // (docked: the size from before stays)
    frame->docked_ = Frame::Dock::None;
    frame->state_ = Frame::State::Maximized;
    frame->set_bounds(area_for(frame));
  }
  activate(frame);
}

void WindowManager::restore(Frame* frame) {
  UiGuard lock(this);
  if (!frame || frame->manager_ != this) return;
  if (frame->state_ == Frame::State::Minimized) {
    activate(frame);
  } else if (frame->state_ == Frame::State::Maximized || frame->docked_ != Frame::Dock::None) {
    frame->state_ = Frame::State::Normal;
    frame->docked_ = Frame::Dock::None;
    frame->set_bounds(frame->restore_bounds_);
    invalidate();
  }
}

Rect2D WindowManager::dock_bounds(Frame::Dock edge, const Rect2D& area) const {
  const uint32_t w = area.width, h = area.height, half_w = w / 2, half_h = h / 2;
  const int32_t x = area.x, y = area.y;
  switch (edge) {
    case Frame::Dock::Left: return {x, y, half_w, h};
    case Frame::Dock::Right: return {x + static_cast<int32_t>(half_w), y, w - half_w, h};
    case Frame::Dock::Top: return {x, y, w, half_h};
    default: return {x, y + static_cast<int32_t>(half_h), w, h - half_h};
  }
}

Rect2D WindowManager::tile_bounds(uint32_t slot, uint32_t count, const Rect2D& area) const {
  const uint32_t w = area.width, h = area.height, half_w = w / 2, half_h = h / 2;
  const int32_t x = area.x, y = area.y, mid_x = x + static_cast<int32_t>(half_w), mid_y = y + static_cast<int32_t>(half_h);
  if (count <= 1) return {x, y, w, h};
  if (count == 2) return slot == 0 ? Rect2D{x, y, half_w, h} : Rect2D{mid_x, y, w - half_w, h};
  if (count == 3 && slot == 2) return {x, mid_y, w, h - half_h};
  const bool right = slot & 1, lower = slot >= 2;
  return {right ? mid_x : x, lower ? mid_y : y, right ? w - half_w : half_w, lower ? h - half_h : half_h};
}

void WindowManager::tile(Frame* origin) {
  UiGuard lock(this);
  if (has_modal() || bounds_.width == 0 || bounds_.height == 0) return;
  if (origin && origin->docked_ == Frame::Dock::Tile) {   // pressed in a tiled window: everything goes back
    for (uint32_t i = 0; i < child_count(); ++i)
      if (as_frame(child(i))->docked_ == Frame::Dock::Tile) restore(as_frame(child(i)));
    return;
  }
  // The windows on the monitor of the one whose button was pressed (the primary's without one) are put in a grid on it.
  const Rect2D where = origin ? area_for(origin) : area(0);
  Frame* picked[4];
  uint32_t n = 0;
  for (uint32_t i = child_count(); i > 0 && n < 4; --i) {
    Frame* f = as_frame(child(i - 1));
    if (f->state() == Frame::State::Minimized || !f->visible() || !f->dock_enabled() || f->layer_ != Frame::Layer::Normal) continue;
    if (!same_rect(area_for(f), where)) continue;
    picked[n++] = f;
  }
  if (n == 0) return;
  for (uint32_t k = 0; k < n; ++k) {
    Frame* f = picked[k];
    if (f->state_ == Frame::State::Maximized) f->state_ = Frame::State::Normal;   // (the size from before is the one to go back to)
    else if (f->docked_ == Frame::Dock::None) f->restore_bounds_ = f->bounds();
    f->docked_ = Frame::Dock::Tile;
    f->tile_slot_ = static_cast<uint8_t>(k);
    f->tile_count_ = static_cast<uint8_t>(n);
    f->set_bounds(tile_bounds(k, n, where));
  }
  activate(origin && origin->docked_ == Frame::Dock::Tile ? origin : picked[0]);
}

void WindowManager::dock(Frame* frame, Frame::Dock edge) {
  UiGuard lock(this);
  if (edge == Frame::Dock::Tile) return;
  if (!frame || frame->manager_ != this || !frame->dock_enabled() || bounds_.width == 0 || bounds_.height == 0) return;
  if (edge == Frame::Dock::None || edge == frame->docked_) {   // put it back
    if (frame->docked_ != Frame::Dock::None) restore(frame);
    return;
  }
  if (frame->state_ == Frame::State::Minimized) activate(frame);
  if (frame->state_ == Frame::State::Maximized) {   // the size from before the maximizing stays the one to go back to
    frame->state_ = Frame::State::Normal;
  } else if (frame->docked_ == Frame::Dock::None) {
    frame->restore_bounds_ = frame->bounds();
  }
  frame->docked_ = edge;
  frame->set_bounds(dock_bounds(edge, area_for(frame)));
  activate(frame);
}

void WindowManager::layout() {
  for (uint32_t i = 0; i < child_count(); ++i) {
    Frame* f = as_frame(child(i));
    if (f->state() == Frame::State::Maximized) f->set_bounds(area_for(f));
    else if (f->docked() == Frame::Dock::Tile) f->set_bounds(tile_bounds(f->tile_slot_, f->tile_count_, area_for(f)));
    else if (f->docked() != Frame::Dock::None) f->set_bounds(dock_bounds(f->docked(), area_for(f)));
    else keep_inside(f);
  }
  Control::layout();
}

void WindowManager::on_descendant_pressed(Control* child) {
  Frame* f = as_frame(child);
  if (f->enabled() && (f != active_ || stacking_index(f) + 1 != child_count())) activate(f);      // (a window that is not activatable is only raised)
}

}  // namespace gui
