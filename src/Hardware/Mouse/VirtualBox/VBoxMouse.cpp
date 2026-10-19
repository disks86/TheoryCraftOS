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

#include "Hardware/Mouse/VirtualBox/VBoxMouse.hpp"

namespace hardware {

void VBoxMouse::push_position(int32_t wheel) {
  MouseEvent m{};
  m.absolute = true;
  m.x = x_;
  m.y = y_;
  m.max_x = max_x();
  m.max_y = max_y();
  m.buttons = buttons_;
  m.wheel = wheel;
  push(m);
}

void VBoxMouse::fetch() {
  uint32_t x, y;
  const bool moved = vmm_.take_pointer(&x, &y);
  if (moved) {
    x_ = x;
    y_ = y;
    have_position_ = true;
  }
  const bool absolute = vmm_.host_wants_absolute() && have_position_;

  MouseEvent e;
  bool reported = false;
  while (ps2_.poll(&e)) {
    buttons_ = e.buttons;
    if (absolute) {
      if (moved && !reported) { push_position(0); reported = true; }   // the move comes before the click that followed it
      push_position(e.wheel);
    } else {
      push(e);
    }
  }
  if (absolute && moved && !reported) push_position(0);
}

}  // namespace hardware
