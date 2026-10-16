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

#include "hardware/Mouse/Virtio/VirtioMouse.hpp"

namespace hardware {

namespace {
constexpr uint16_t kRelX = 0, kRelY = 1, kRelWheel = 8;
constexpr uint16_t kAbsX = 0, kAbsY = 1;
constexpr uint16_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112;
}  // namespace

void VirtioMouse::fetch() {
  VirtioInputEvent e;
  while (device_.next_event(&e)) {
    switch (e.type) {
      case kEvRel:
        if (e.code == kRelX) dx_ += static_cast<int32_t>(e.value);
        else if (e.code == kRelY) dy_ += static_cast<int32_t>(e.value);
        else if (e.code == kRelWheel) wheel_ += static_cast<int32_t>(e.value);
        break;
      case kEvAbs:
        if (e.code == kAbsX) x_ = e.value;
        else if (e.code == kAbsY) y_ = e.value;
        break;
      case kEvKey: {
        uint8_t bit = 0;
        if (e.code == kBtnLeft) bit = mouse_button::kLeft;
        else if (e.code == kBtnRight) bit = mouse_button::kRight;
        else if (e.code == kBtnMiddle) bit = mouse_button::kMiddle;
        if (e.value) buttons_ |= bit; else buttons_ &= ~bit;
        break;
      }
      case kEvSyn: {
        MouseEvent m{};
        m.buttons = buttons_;
        m.wheel = wheel_;
        if (is_absolute()) {
          m.absolute = true;
          m.x = x_;
          m.y = y_;
        } else {
          m.dx = dx_;
          m.dy = dy_;
        }
        push(m);
        dx_ = dy_ = wheel_ = 0;
        break;
      }
      default: break;
    }
  }
}

}  // namespace hardware
