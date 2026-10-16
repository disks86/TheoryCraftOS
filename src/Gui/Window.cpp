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

#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

Window::Window(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, const Theme& theme,
               const graphics::Extent2D& size)
    : Control(allocator), renderer_(renderer), theme_(theme), title_(allocator) {
  window_ = this;
  bounds_ = {0, 0, size.width, size.height};
}

void Window::set_title(StringView title) {
  title_.assign(title);
  mark_dirty();
}

Rect2D Window::client_rect() const {
  Rect2D r = Control::client_rect();
  if (!title_.empty()) {
    const uint32_t bar = kTitleHeight;
    r.y += kTitleHeight;
    r.height = r.height > bar ? r.height - bar : 0;
  }
  return r;
}

void Window::paint(Painter& painter) {
  painter.fill({0, 0, bounds_.width, bounds_.height}, theme_.face);
  if (!title_.empty()) {
    painter.fill({0, 0, bounds_.width, static_cast<uint32_t>(kTitleHeight)}, theme_.title);
    painter.text(6, (kTitleHeight - Painter::kGlyphHeight) / 2, title_.view(), theme_.title_text);
  }
}

void Window::render() {
  layout();
  Painter painter(renderer_, theme_);
  paint_tree(painter);
  if (popup_) popup_->paint_tree(painter);
  if (cursor_visible_) {
    // A classic arrow: black triangle with a white edge.
    painter.renderer().reset_clip();
    const float x = static_cast<float>(cursor_.x), y = static_cast<float>(cursor_.y);
    renderer_.fill_triangle({x - 1, y - 1}, {x + 13, y + 11}, {x + 3, y + 13}, theme_.highlight);
    renderer_.fill_triangle({x, y}, {x + 11, y + 10}, {x + 3, y + 11}, theme_.dark);
  }
  renderer_.reset_clip();
  renderer_.present();
  dirty_ = false;
}

// --- Input -------------------------------------------------------------------

bool Window::is_inside(const Control* ancestor, const Control* c) {
  for (; c; c = c->parent_)
    if (c == ancestor) return true;
  return false;
}

Control* Window::target_at(int32_t x, int32_t y, Point* origin) {
  if (popup_ && contains(popup_->bounds(), x, y)) {
    Control* hit = popup_->hit_test(x - popup_->bounds().x, y - popup_->bounds().y);
    *origin = hit ? hit->window_origin() : Point{};
    if (hit) {  // popup children (if any) are positioned relative to the popup
      origin->x += popup_->bounds().x;
      origin->y += popup_->bounds().y;
    }
    return hit;
  }
  Control* hit = hit_test(x, y);
  *origin = hit ? hit->window_origin() : Point{};
  return hit;
}

void Window::deliver_move(Control* target, const Point& origin, int32_t x, int32_t y) {
  if (target != hover_) {
    if (hover_) {
      hover_->hovered_ = false;
      hover_->on_mouse_leave();
    }
    hover_ = target;
    if (hover_) {
      hover_->hovered_ = true;
      hover_->on_mouse_enter();
    }
  }
  if (target && target->effectively_enabled()) {
    MouseArgs a;
    a.x = x - origin.x;
    a.y = y - origin.y;
    target->on_mouse_move(a);
  }
  mark_dirty();
}

void Window::mouse_move(int32_t x, int32_t y) {
  cursor_ = {x, y};
  Point origin;
  if (captured_) {
    origin = captured_->window_origin();
    if (is_inside(popup_, captured_) && popup_) {
      origin = {popup_->bounds().x, popup_->bounds().y};
    }
    MouseArgs a;
    a.x = x - origin.x;
    a.y = y - origin.y;
    captured_->on_mouse_move(a);
    mark_dirty();
    return;
  }
  Control* target = target_at(x, y, &origin);
  deliver_move(target, origin, x, y);
}

void Window::mouse_button(int32_t x, int32_t y, MouseButton button, bool pressed) {
  cursor_ = {x, y};
  mark_dirty();
  Point origin;
  if (!pressed) {
    Control* target = captured_ ? captured_ : target_at(x, y, &origin);
    if (captured_) {
      origin = (popup_ && captured_ == popup_) ? Point{popup_->bounds().x, popup_->bounds().y} : captured_->window_origin();
    }
    captured_ = nullptr;
    if (target && target->effectively_enabled()) {
      MouseArgs a;
      a.x = x - origin.x;
      a.y = y - origin.y;
      a.button = button;
      target->on_mouse_up(a);
    }
    return;
  }
  Control* target = target_at(x, y, &origin);
  if (popup_ && !contains(popup_->bounds(), x, y)) {
    // A click outside the popup dismisses it. A click on whatever opened it only dismisses (so a
    // second click on a menu title or drop-down closes it); anything else also gets the click.
    Control* owner = popup_owner_;
    close_popup();
    if (target && owner && is_inside(owner, target)) return;
    target = target_at(x, y, &origin);
  }
  if (!target) return;
  // Ancestors hear about the press first (a window manager raises the clicked window), even when
  // the control itself is disabled - the manager decides.
  for (Control* c = target; c && c->parent_ && c != popup_; c = c->parent_) c->parent_->on_descendant_pressed(c);
  if (!target->effectively_enabled()) return;
  if (target->focusable()) set_focus(target);
  captured_ = target;
  MouseArgs a;
  a.x = x - origin.x;
  a.y = y - origin.y;
  a.button = button;
  target->on_mouse_down(a);
}

void Window::mouse_wheel(int32_t x, int32_t y, int32_t delta) {
  Point origin;
  Control* target = target_at(x, y, &origin);
  if (!target || !target->effectively_enabled()) return;
  MouseArgs a;
  a.x = x - origin.x;
  a.y = y - origin.y;
  a.wheel = delta;
  target->on_mouse_wheel(a);
  mark_dirty();
}

void Window::key(const KeyArgs& args) {
  mark_dirty();
  if (popup_) {
    popup_->on_key(args);
    return;
  }
  for (Control* c = focus_; c; c = c->parent_)
    if (c->effectively_enabled() && c->on_key(args)) return;
  if (args.usage == hardware::key::kTab) focus_next(!(args.modifiers & hardware::modifier::kShift));
}

// --- Focus -------------------------------------------------------------------

void Window::set_focus(Control* control) {
  if (control == focus_) return;
  Control* old = focus_;
  focus_ = control;
  if (old) {
    old->focused_ = false;
    old->on_focus_changed(false);
  }
  if (control) {
    control->focused_ = true;
    control->on_focus_changed(true);
  }
  mark_dirty();
}

uint32_t Window::collect_focusable(Control* from, Control** out, uint32_t count, uint32_t capacity) {
  if (!from->visible() || !from->enabled()) return count;
  if (from->focusable() && count < capacity) out[count++] = from;
  for (uint32_t i = 0; i < from->child_count(); ++i) count = collect_focusable(from->child(i), out, count, capacity);
  return count;
}

void Window::set_focus_scope(Control* scope) {
  focus_scope_ = scope;
  if (!scope || (focus_ && is_inside(scope, focus_))) return;
  Control* order[64];
  const uint32_t n = collect_focusable(scope, order, 0, 64);
  set_focus(n ? order[0] : nullptr);
}

void Window::focus_next(bool forward) {
  Control* order[64];
  const uint32_t n = collect_focusable(focus_scope_ ? focus_scope_ : this, order, 0, 64);
  if (n == 0) return;
  uint32_t at = n;  // "none" until found
  for (uint32_t i = 0; i < n; ++i)
    if (order[i] == focus_) at = i;
  const uint32_t next = at == n ? (forward ? 0 : n - 1) : (forward ? (at + 1) % n : (at + n - 1) % n);
  set_focus(order[next]);
}

// --- Popups ------------------------------------------------------------------

bool Window::open_popup(Control* popup, Control* owner, int32_t x, int32_t y) {
  if (!popup || popup->parent_) return false;
  close_popup();
  popup->window_ = this;
  const Size size = popup->preferred_size();
  if (x + size.width > static_cast<int32_t>(bounds_.width)) x = static_cast<int32_t>(bounds_.width) - size.width;
  if (y + size.height > static_cast<int32_t>(bounds_.height)) y = static_cast<int32_t>(bounds_.height) - size.height;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  popup->bounds_ = {x, y, static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height)};
  popup->layout();
  popup_ = popup;
  popup_owner_ = owner;
  mark_dirty();
  return true;
}

void Window::close_popup() {
  if (!popup_) return;
  Control* popup = popup_;
  Control* owner = popup_owner_;
  popup_ = popup_owner_ = nullptr;
  if (captured_ == popup) captured_ = nullptr;
  if (hover_ == popup) hover_ = nullptr;
  popup->window_ = nullptr;
  if (owner) owner->on_popup_closed(popup);
  mark_dirty();
}

void Window::control_removed(Control* control) {
  if (focus_scope_ && is_inside(control, focus_scope_)) focus_scope_ = nullptr;
  if (focus_ && is_inside(control, focus_)) set_focus(nullptr);
  if (hover_ && is_inside(control, hover_)) hover_ = nullptr;
  if (captured_ && is_inside(control, captured_)) captured_ = nullptr;
  if (popup_owner_ && is_inside(control, popup_owner_)) close_popup();
}

}  // namespace gui
