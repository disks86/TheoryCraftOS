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

#include "Hardware/Pen/PenMouse.hpp"

namespace hardware {

bool PenMouse::poll(MouseEvent* out) {
  PenEvent e;
  while (pens_.poll(&e)) {
    uint8_t buttons = 0;
    uint32_t x = x_, y = y_;
    if (e.in_range) {
      x = e.x;
      y = e.y;
      max_x_ = e.max_x;
      max_y_ = e.max_y;
      if (e.tip && !e.inverted) buttons |= mouse_button::kLeft;
      if (e.tip && e.inverted) buttons |= mouse_button::kMiddle;
      if (e.buttons & (pen_button::kBarrel | pen_button::kSecondary)) buttons |= mouse_button::kRight;
    }
    if (e.in_range == in_range_ && buttons == buttons_ && x == x_ && y == y_) continue;   // (nothing changed)
    in_range_ = e.in_range;
    buttons_ = buttons;
    x_ = x;
    y_ = y;
    MouseEvent m = {};
    m.absolute = true;
    m.x = x;
    m.y = y;
    m.max_x = max_x_;
    m.max_y = max_y_;
    m.buttons = buttons;
    if (out) *out = m;
    return true;
  }
  return false;
}

}  // namespace hardware
