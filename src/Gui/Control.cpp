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

#include "Gui/Control.hpp"
#include "Gui/Window.hpp"

namespace gui {

Rect2D Control::client_rect() const {
  const int64_t w = static_cast<int64_t>(bounds_.width) - padding_.left - padding_.right;
  const int64_t h = static_cast<int64_t>(bounds_.height) - padding_.top - padding_.bottom;
  return {padding_.left, padding_.top, w > 0 ? static_cast<uint32_t>(w) : 0, h > 0 ? static_cast<uint32_t>(h) : 0};
}

void Control::layout() {
  Rect2D rest = client_rect();
  for (uint64_t i = 0; i < children_.count(); ++i) {
    Control* c = children_[i];
    if (!c->visible_) continue;
    const Size want = c->preferred_size();
    auto clamp = [](int32_t wanted, uint32_t available) {
      return wanted < 0 ? 0u : (static_cast<uint32_t>(wanted) < available ? static_cast<uint32_t>(wanted) : available);
    };
    switch (c->dock_) {
      case DockStyle::None:
        break;
      case DockStyle::Top: {
        const uint32_t h = clamp(want.height, rest.height);
        c->bounds_ = {rest.x, rest.y, rest.width, h};
        rest.y += static_cast<int32_t>(h);
        rest.height -= h;
        break;
      }
      case DockStyle::Bottom: {
        const uint32_t h = clamp(want.height, rest.height);
        c->bounds_ = {rest.x, rest.y + static_cast<int32_t>(rest.height - h), rest.width, h};
        rest.height -= h;
        break;
      }
      case DockStyle::Left: {
        const uint32_t w = clamp(want.width, rest.width);
        c->bounds_ = {rest.x, rest.y, w, rest.height};
        rest.x += static_cast<int32_t>(w);
        rest.width -= w;
        break;
      }
      case DockStyle::Right: {
        const uint32_t w = clamp(want.width, rest.width);
        c->bounds_ = {rest.x + static_cast<int32_t>(rest.width - w), rest.y, w, rest.height};
        rest.width -= w;
        break;
      }
      case DockStyle::Fill:
        c->bounds_ = rest;
        rest.width = rest.height = 0;
        break;
    }
    c->layout();
  }
}

bool Control::add(Control* child) {
  if (!child || child == this || child->parent_) return false;
  if (!children_.push_back(child)) return false;
  child->parent_ = this;
  invalidate();
  return true;
}

bool Control::remove(Control* child) {
  for (uint64_t i = 0; i < children_.count(); ++i) {
    if (children_[i] != child) continue;
    if (Window* w = window()) w->control_removed(child);
    children_.remove_at(i);
    child->parent_ = nullptr;
    invalidate();
    return true;
  }
  return false;
}

Window* Control::window() const {
  for (const Control* c = this; c; c = c->parent_)
    if (c->window_) return c->window_;
  return nullptr;
}

Point Control::window_origin() const {
  Point p;
  for (const Control* c = this; c && !c->window_; c = c->parent_) {
    p.x += c->bounds_.x;
    p.y += c->bounds_.y;
  }
  return p;
}

void Control::invalidate() const {
  if (Window* w = window()) w->mark_dirty();
}

void Control::paint_tree(Painter& painter) {
  if (!visible_) return;
  painter.push_frame(bounds_);
  if (!painter.clip_empty()) {
    paint(painter);  // the frame's origin is this control's top left
    for (uint64_t i = 0; i < children_.count(); ++i) children_[i]->paint_tree(painter);
  }
  painter.pop_frame();
}

Control* Control::hit_test(int32_t x, int32_t y) {
  if (!visible_ || !contains({0, 0, bounds_.width, bounds_.height}, x, y)) return nullptr;
  for (uint64_t i = children_.count(); i > 0; --i) {
    Control* c = children_[i - 1];
    if (Control* hit = c->hit_test(x - c->bounds_.x, y - c->bounds_.y)) return hit;
  }
  return this;
}

}  // namespace gui
