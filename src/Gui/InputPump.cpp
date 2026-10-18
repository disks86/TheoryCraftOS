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

#include "Gui/InputPump.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

uint32_t InputPump::pump() {
  uint32_t events = 0;
  hardware::KeyEvent key;
  while (keyboard_ && keyboard_->poll(&key)) {
    ++events;
    if (key.usage >= hardware::key::kLeftControl && key.usage <= hardware::key::kRightGui) {
      const uint8_t bit = static_cast<uint8_t>(1u << (key.usage - hardware::key::kLeftControl));
      modifiers_ = key.pressed ? (modifiers_ | bit) : (modifiers_ & ~bit);
    }
    if (!key.pressed) continue;
    KeyArgs args;
    args.usage = key.usage;
    args.modifiers = modifiers_;
    args.character = hardware::key_to_ascii(key.usage, args.modifiers);
    window_.key(args);
  }
  if (keyboard_) modifiers_ = keyboard_->modifiers();  // everything is delivered: the keyboard's own state is the truth again

  hardware::MouseEvent m;
  const int32_t max_x = static_cast<int32_t>(window_.bounds().width) - 1;
  const int32_t max_y = static_cast<int32_t>(window_.bounds().height) - 1;
  while (mouse_ && mouse_->poll(&m)) {
    ++events;
    int32_t x = x_, y = y_;
    if (m.absolute) {
      x = m.max_x ? static_cast<int32_t>(static_cast<uint64_t>(m.x) * (max_x + 1) / (uint64_t{m.max_x} + 1)) : x;
      y = m.max_y ? static_cast<int32_t>(static_cast<uint64_t>(m.y) * (max_y + 1) / (uint64_t{m.max_y} + 1)) : y;
    } else {
      x += m.dx;
      y += m.dy;
    }
    x = x < 0 ? 0 : x > max_x ? max_x : x;
    y = y < 0 ? 0 : y > max_y ? max_y : y;
    if (confine_) confine_(confine_context_, &x, &y);
    // Press and release at the position the pointer had when the button changed, after moving there.
    if (x != x_ || y != y_) {
      x_ = x;
      y_ = y;
      window_.mouse_move(x_, y_);
    }
    const uint8_t changed = m.buttons ^ buttons_;
    static const struct {
      uint8_t mask;
      MouseButton button;
    } kButtons[] = {{hardware::mouse_button::kLeft, MouseButton::Left},
                    {hardware::mouse_button::kRight, MouseButton::Right},
                    {hardware::mouse_button::kMiddle, MouseButton::Middle}};
    for (const auto& b : kButtons)
      if (changed & b.mask) window_.mouse_button(x_, y_, b.button, (m.buttons & b.mask) != 0);
    buttons_ = m.buttons;
    if (m.wheel) window_.mouse_wheel(x_, y_, m.wheel);
  }
  return events;
}

}  // namespace gui
