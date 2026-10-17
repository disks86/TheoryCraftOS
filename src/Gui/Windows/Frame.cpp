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

#include "Gui/Windows/Frame.hpp"
#include "Gui/Window.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
constexpr int32_t kCorner = 18;  // how far along an edge a corner grip reaches
}

void Frame::minimize() {
  UiGuard lock(this);
  if (manager_) manager_->minimize(this);
}
void Frame::maximize() {
  UiGuard lock(this);
  if (manager_) manager_->maximize(this);
}
void Frame::restore() {
  UiGuard lock(this);
  if (manager_) manager_->restore(this);
}
void Frame::close() {
  UiGuard lock(this);
  if (manager_) manager_->close_window(this);
}
void Frame::activate() {
  UiGuard lock(this);
  if (manager_) manager_->activate(this);
}

Rect2D Frame::client_rect() const {
  const int32_t top = kBorder + kTitleHeight + 1;
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder, h = static_cast<int32_t>(bounds_.height) - top - kBorder;
  return {kBorder + padding().left, top + padding().top,
          static_cast<uint32_t>(w - padding().left - padding().right > 0 ? w - padding().left - padding().right : 0),
          static_cast<uint32_t>(h - padding().top - padding().bottom > 0 ? h - padding().top - padding().bottom : 0)};
}

Rect2D Frame::button_rect(Part button) const {
  const int32_t y = kBorder + (kTitleHeight - kButtonHeight) / 2;
  const int32_t close_x = static_cast<int32_t>(bounds_.width) - kBorder - 2 - kButtonWidth;
  const int32_t max_x = close_x - 2 - kButtonWidth;
  const int32_t min_x = max_x - kButtonWidth;
  const int32_t x = button == Part::Close ? close_x : button == Part::Maximize ? max_x : min_x;
  return {x, y, static_cast<uint32_t>(kButtonWidth), static_cast<uint32_t>(kButtonHeight)};
}

bool Frame::button_enabled(Part button) const {
  if (!enabled()) return false;
  return button == Part::Minimize ? can_minimize_ : button == Part::Maximize ? can_maximize_ : can_close_;
}

Frame::Part Frame::part_at(int32_t x, int32_t y) const {
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  if (x < 0 || y < 0 || x >= w || y >= h) return Part::None;
  static const Part kButtons[3] = {Part::Close, Part::Maximize, Part::Minimize};
  for (Part b : kButtons)
    if (contains(button_rect(b), x, y)) return b;
  if (resizable_ && state_ == State::Normal) {
    const bool left = x < kBorder, right = x >= w - kBorder, top = y < kBorder, bottom = y >= h - kBorder;
    const bool near_left = x < kCorner, near_right = x >= w - kCorner, near_top = y < kCorner, near_bottom = y >= h - kCorner;
    if ((left && near_top) || (top && near_left)) return Part::TopLeft;
    if ((right && near_top) || (top && near_right)) return Part::TopRight;
    if ((left && near_bottom) || (bottom && near_left)) return Part::BottomLeft;
    if ((right && near_bottom) || (bottom && near_right)) return Part::BottomRight;
    if (left) return Part::Left;
    if (right) return Part::Right;
    if (top) return Part::Top;
    if (bottom) return Part::Bottom;
  }
  if (contains({kBorder, kBorder, bounds_.width - 2 * kBorder, static_cast<uint32_t>(kTitleHeight)}, x, y)) return Part::TitleBar;
  return Part::Client;
}

CursorShape Frame::cursor_shape(int32_t x, int32_t y) const {
  switch (dragging_ != Part::None ? dragging_ : part_at(x, y)) {
    case Part::Left:
    case Part::Right: return CursorShape::ResizeHorizontal;
    case Part::Top:
    case Part::Bottom: return CursorShape::ResizeVertical;
    case Part::TopLeft:
    case Part::BottomRight: return CursorShape::ResizeDiagonalDown;
    case Part::TopRight:
    case Part::BottomLeft: return CursorShape::ResizeDiagonalUp;
    default: return CursorShape::Arrow;
  }
}

void Frame::draw_button(Painter& painter, Part button) {
  const Theme& t = painter.theme();
  const Rect2D r = button_rect(button);
  const bool on = button_enabled(button);
  const bool down = pressed_ == button && pressed_inside_;
  const int32_t o = down ? 1 : 0;
  const uint32_t ink = on ? t.text : t.shadow;
  painter.fill(r, t.face);
  painter.bevel(r, down);
  const int32_t x = r.x + o, y = r.y + o;
  switch (button) {
    case Part::Minimize:
      painter.fill({x + 4, y + 9, 7, 2}, ink);
      break;
    case Part::Maximize:
      if (state_ == State::Maximized) {  // the restore glyph: two overlapping boxes
        painter.outline({x + 6, y + 3, 6, 5}, ink);
        painter.line(x + 6, y + 4, x + 11, y + 4, ink);
        painter.outline({x + 3, y + 5, 6, 5}, ink);
        painter.line(x + 3, y + 6, x + 8, y + 6, ink);
      } else {
        painter.outline({x + 3, y + 3, 10, 8}, ink);
        painter.line(x + 4, y + 4, x + 11, y + 4, ink);
      }
      break;
    default:  // close: a bold X
      painter.line(x + 4, y + 3, x + 11, y + 10, ink);
      painter.line(x + 5, y + 3, x + 12, y + 10, ink);
      painter.line(x + 11, y + 3, x + 4, y + 10, ink);
      painter.line(x + 12, y + 3, x + 5, y + 10, ink);
      break;
  }
}

void Frame::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.face);
  painter.bevel(all, false);
  const Rect2D bar{kBorder, kBorder, bounds_.width - 2 * kBorder, static_cast<uint32_t>(kTitleHeight)};
  const bool lit = active_ && enabled();
  painter.fill(bar, lit ? t.title : t.inactive_title);
  const int32_t text_right = button_rect(Part::Minimize).x - 2;
  painter.push_frame({bar.x, bar.y, static_cast<uint32_t>(text_right > bar.x ? text_right - bar.x : 0), bar.height});
  painter.text(4, (kTitleHeight - Painter::kGlyphHeight) / 2, title_.view(), lit ? t.title_text : t.inactive_title_text);
  painter.pop_frame();
  draw_button(painter, Part::Minimize);
  draw_button(painter, Part::Maximize);
  draw_button(painter, Part::Close);
}

void Frame::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const Part part = part_at(args.x, args.y);
  const Point o = window_origin();
  grab_x_ = o.x + args.x;
  grab_y_ = o.y + args.y;
  grab_bounds_ = bounds_;
  if (part == Part::Minimize || part == Part::Maximize || part == Part::Close) {
    if (button_enabled(part)) {
      pressed_ = part;
      pressed_inside_ = true;
    }
  } else if (part == Part::TitleBar) {
    if (state_ == State::Normal) dragging_ = Part::TitleBar;
  } else if (part != Part::Client && part != Part::None) {
    dragging_ = part;
  }
  invalidate();
}

void Frame::on_mouse_move(const MouseArgs& args) {
  if (pressed_ != Part::None) {
    const bool inside = part_at(args.x, args.y) == pressed_;
    if (inside != pressed_inside_) {
      pressed_inside_ = inside;
      invalidate();
    }
    return;
  }
  if (dragging_ == Part::None) return;
  const Point o = window_origin();
  const int32_t dx = o.x + args.x - grab_x_, dy = o.y + args.y - grab_y_;
  int32_t x = grab_bounds_.x, y = grab_bounds_.y, w = static_cast<int32_t>(grab_bounds_.width), h = static_cast<int32_t>(grab_bounds_.height);
  const Rect2D area = parent() ? parent()->bounds() : Rect2D{0, 0, 1 << 20, 1 << 20};
  const int32_t area_w = static_cast<int32_t>(area.width), area_h = static_cast<int32_t>(area.height);
  const bool west = dragging_ == Part::Left || dragging_ == Part::TopLeft || dragging_ == Part::BottomLeft;
  const bool east = dragging_ == Part::Right || dragging_ == Part::TopRight || dragging_ == Part::BottomRight;
  const bool north = dragging_ == Part::Top || dragging_ == Part::TopLeft || dragging_ == Part::TopRight;
  const bool south = dragging_ == Part::Bottom || dragging_ == Part::BottomLeft || dragging_ == Part::BottomRight;
  if (dragging_ == Part::TitleBar) {
    x += dx;
    y += dy;
    // Keep a grip on screen: the title bar stays inside the area and a strip of it stays reachable.
    if (y < 0) y = 0;
    if (y > area_h - kBorder - kTitleHeight) y = area_h - kBorder - kTitleHeight;
    if (x + w < 80) x = 80 - w;
    if (x > area_w - 80) x = area_w - 80;
  } else {
    if (east) w += dx;
    if (south) h += dy;
    if (west) {
      w -= dx;
      x += dx;
    }
    if (north) {
      h -= dy;
      y += dy;
    }
    if (w < kMinWidth) {
      if (west) x -= kMinWidth - w;
      w = kMinWidth;
    }
    if (h < kMinHeight) {
      if (north) y -= kMinHeight - h;
      h = kMinHeight;
    }
  }
  set_bounds({x, y, static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
}

void Frame::on_mouse_up(const MouseArgs& args) {
  const Part pressed = pressed_;
  const bool fire = pressed != Part::None && pressed_inside_ && part_at(args.x, args.y) == pressed;
  pressed_ = Part::None;
  pressed_inside_ = false;
  dragging_ = Part::None;
  invalidate();
  if (!fire) return;
  if (pressed == Part::Minimize) minimize();
  else if (pressed == Part::Maximize) maximize();
  else if (pressed == Part::Close) close();
}

bool Frame::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEscape && escape_closes_ && can_close_) {
    close();
    return true;
  }
  return false;
}

}  // namespace gui
