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

#include "Gui/Controls/MenuButton.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

void MenuButton::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.face);
  painter.bevel(all, open_);
  const int32_t nudge = open_ ? 1 : 0;
  painter.text_centered({nudge, nudge, bounds_.width, bounds_.height}, text_.view(), enabled() ? t.text : t.disabled_text);
  if (focused() && !open_ && bounds_.width > 8 && bounds_.height > 8)
    painter.focus_frame({4, 4, bounds_.width - 8, bounds_.height - 8});
}

void MenuButton::open_menu() {
  UiGuard lock(this);
  Window* w = window();
  if (!w || open_ || menu_.item_count() == 0) return;
  popup_.show(&menu_);
  const Point o = window_origin();
  const int32_t y = direction_ == Direction::Up ? o.y - popup_.preferred_size().height : o.y + static_cast<int32_t>(bounds_.height);
  if (w->open_popup(&popup_, this, o.x, y)) open_ = true;  // after open_popup: it closes any previous popup
  invalidate();
}

void MenuButton::close_menu() {
  UiGuard lock(this);
  if (open_ && window()) window()->close_popup();
  open_ = false;
  invalidate();
}

void MenuButton::activate(MenuItem* item) {
  const Handler handler = item->on_click;
  close_menu();
  fire(handler);
}

void MenuButton::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left) open_menu();  // a click while open is consumed by the window
}

bool MenuButton::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kSpace || args.usage == hardware::key::kEnter) {
    open_menu();
    return true;
  }
  return false;
}

}  // namespace gui
