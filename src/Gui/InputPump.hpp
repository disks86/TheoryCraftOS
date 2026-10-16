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

// Feeds a Window from the keyboard and mouse drivers: key presses become KeyArgs (with the
// character the key types), mouse movement and buttons become window-coordinate events. A relative
// mouse moves a cursor position kept here (clamped to the window); an absolute one (tablet) is scaled
// to the window.
#pragma once
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/IKeyboard.hpp"
#include "Hardware/Mouse/IMouse.hpp"

namespace gui {

class InputPump {
 public:
  // Either device may be null.
  InputPump(Window& window, hardware::IKeyboard* keyboard, hardware::IMouse* mouse)
      : window_(window), keyboard_(keyboard), mouse_(mouse) {
    x_ = static_cast<int32_t>(window.bounds().width / 2);
    y_ = static_cast<int32_t>(window.bounds().height / 2);
    window_.mouse_move(x_, y_);  // the cursor starts in the middle, not at the corner
  }

  // Delivers everything the devices have right now; returns how many events there were.
  uint32_t pump();
  Point cursor() const { return {x_, y_}; }

 private:
  Window& window_;
  hardware::IKeyboard* keyboard_;
  hardware::IMouse* mouse_;
  int32_t x_ = 0, y_ = 0;
  uint8_t buttons_ = 0;
};

}  // namespace gui
