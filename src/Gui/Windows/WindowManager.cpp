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

void WindowManager::keep_inside(Frame* frame) {
  if (bounds_.width == 0 || bounds_.height == 0) return;  // not laid out yet
  Rect2D b = frame->bounds();
  const int32_t area_w = static_cast<int32_t>(bounds_.width), area_h = static_cast<int32_t>(bounds_.height);
  int32_t x = b.x, y = b.y;
  if (y < 0) y = 0;
  if (y > area_h - Frame::kBorder - Frame::kTitleHeight) y = area_h - Frame::kBorder - Frame::kTitleHeight;
  if (x + static_cast<int32_t>(b.width) < 80) x = 80 - static_cast<int32_t>(b.width);
  if (x > area_w - 80) x = area_w - 80;
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
  frame->modal_ = modal;
  frame->state_ = Frame::State::Normal;
  frame->set_visible(true);
  keep_inside(frame);
  activate(frame);
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
  frame->modal_ = false;
  frame->active_ = false;
  frame->set_enabled(true);
  frame->pressed_ = frame->dragging_ = Frame::Part::None;
  if (frame->state_ == Frame::State::Maximized || frame->docked_ != Frame::Dock::None) frame->set_bounds(frame->restore_bounds_);
  frame->docked_ = Frame::Dock::None;
  frame->state_ = Frame::State::Normal;
  frame->set_visible(true);
  if (active_ == frame) active_ = nullptr;
  refresh();
  return true;
}

void WindowManager::close_window(Frame* frame) {
  UiGuard lock(this);
  if (!frame || frame->manager_ != this) return;
  const Handler closed = frame->closed_;
  Window* w = window();
  remove_window(frame);
  if (!closed) return;
  if (w) w->dispatch(closed, *frame);
  else closed(*frame);
}

Frame* WindowManager::top_usable() const {
  for (uint32_t i = child_count(); i > 0; --i) {
    Frame* f = as_frame(child(i - 1));
    if (f->visible() && f->enabled() && f->state() != Frame::State::Minimized) return f;
  }
  return nullptr;
}

void WindowManager::refresh() {
  // The topmost modal window blocks every other one.
  Frame* modal = nullptr;
  for (uint32_t i = child_count(); i > 0 && !modal; --i)
    if (as_frame(child(i - 1))->modal()) modal = as_frame(child(i - 1));
  if (modal) move_child_to_front(modal);
  for (uint32_t i = 0; i < child_count(); ++i) {
    Frame* f = as_frame(child(i));
    const bool usable = !modal || f == modal;
    if (f->enabled() != usable) f->set_enabled(usable);
  }
  if (modal) active_ = modal;
  else if (!active_ || !active_->visible() || !active_->enabled()) active_ = top_usable();
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
  }
  move_child_to_front(frame);
  active_ = frame;
  refresh();
}

void WindowManager::minimize(Frame* frame) {
  UiGuard lock(this);
  if (!frame || frame->manager_ != this || frame->state_ == Frame::State::Minimized) return;
  frame->before_minimize_ = frame->state_;
  frame->state_ = Frame::State::Minimized;
  frame->set_visible(false);
  if (active_ == frame) active_ = nullptr;
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
    frame->set_bounds({0, 0, bounds_.width, bounds_.height});
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

Rect2D WindowManager::dock_bounds(Frame::Dock edge) const {
  const uint32_t w = bounds_.width, h = bounds_.height, half_w = w / 2, half_h = h / 2;
  switch (edge) {
    case Frame::Dock::Left: return {0, 0, half_w, h};
    case Frame::Dock::Right: return {static_cast<int32_t>(half_w), 0, w - half_w, h};
    case Frame::Dock::Top: return {0, 0, w, half_h};
    default: return {0, static_cast<int32_t>(half_h), w, h - half_h};
  }
}

void WindowManager::dock(Frame* frame, Frame::Dock edge) {
  UiGuard lock(this);
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
  frame->set_bounds(dock_bounds(edge));
  activate(frame);
}

void WindowManager::layout() {
  for (uint32_t i = 0; i < child_count(); ++i) {
    Frame* f = as_frame(child(i));
    if (f->state() == Frame::State::Maximized) f->set_bounds({0, 0, bounds_.width, bounds_.height});
    else if (f->docked() != Frame::Dock::None) f->set_bounds(dock_bounds(f->docked()));
    else keep_inside(f);
  }
  Control::layout();
}

void WindowManager::on_descendant_pressed(Control* child) {
  Frame* f = as_frame(child);
  if (f->enabled() && (f != active_ || stacking_index(f) + 1 != child_count())) activate(f);
}

}  // namespace gui
