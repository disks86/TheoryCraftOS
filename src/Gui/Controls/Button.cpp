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

#include "Gui/Controls/Button.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

void Button::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.face);
  painter.bevel(all, pressed_);
  const int32_t nudge = pressed_ ? 1 : 0;
  painter.text_centered({nudge, nudge, bounds_.width, bounds_.height}, text_.view(),
                        enabled() ? t.text : t.disabled_text);
  if (focused() && bounds_.width > 8 && bounds_.height > 8)
    painter.focus_frame({4, 4, bounds_.width - 8, bounds_.height - 8});
}

void Button::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left) {
    held_ = pressed_ = true;
    invalidate();
  }
}

void Button::on_mouse_move(const MouseArgs& args) {
  // The window keeps delivering moves while the button is held, even outside the bounds.
  const bool now = held_ && inside(args);
  if (now != pressed_) {
    pressed_ = now;
    invalidate();
  }
}

void Button::on_mouse_up(const MouseArgs& args) {
  const bool fire = held_ && args.button == MouseButton::Left && inside(args);
  held_ = pressed_ = false;
  invalidate();
  if (fire) click();
}

bool Button::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kSpace || args.usage == hardware::key::kEnter) {
    click();
    return true;
  }
  return false;
}

}  // namespace gui
