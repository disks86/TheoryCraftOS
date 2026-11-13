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

void InputPump::set_cursor(int32_t x, int32_t y) {
  const int32_t max_x = static_cast<int32_t>(window_.bounds().width) - 1, max_y = static_cast<int32_t>(window_.bounds().height) - 1;
  x = x < 0 ? 0 : x > max_x ? max_x : x;
  y = y < 0 ? 0 : y > max_y ? max_y : y;
  if (confine_) confine_(confine_context_, &x, &y);
  x_ = x;
  y_ = y;
  window_.mouse_move(x_, y_);
}

void InputPump::deliver(const hardware::MouseEvent& m) {
  const int32_t max_x = static_cast<int32_t>(window_.bounds().width) - 1;
  const int32_t max_y = static_cast<int32_t>(window_.bounds().height) - 1;
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
  if (mouse_tap_) {
    mouse_tap_(tap_context_, m, &x, &y);
    x = x < 0 ? 0 : x > max_x ? max_x : x;      // (what the tap asks for stays inside the window too)
    y = y < 0 ? 0 : y > max_y ? max_y : y;
  }
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

uint32_t InputPump::pump() {
  uint32_t events = 0;
  hardware::KeyEvent key;
  while (keyboard_ && keyboard_->poll(&key)) {
    ++events;
    if (key.usage >= hardware::key::kLeftControl && key.usage <= hardware::key::kRightGui) {
      const uint8_t bit = static_cast<uint8_t>(1u << (key.usage - hardware::key::kLeftControl));
      modifiers_ = key.pressed ? (modifiers_ | bit) : (modifiers_ & ~bit);
    }
    if (key_tap_) key_tap_(tap_context_, key, modifiers_);
    if (!key.pressed) continue;
    KeyArgs args;
    args.usage = key.usage;
    args.modifiers = modifiers_;
    args.character = hardware::key_to_ascii(key.usage, args.modifiers);
    window_.key(args);
  }
  if (keyboard_) modifiers_ = keyboard_->modifiers();  // everything is delivered: the keyboard's own state is the truth again

  hardware::MouseEvent m;
  while (mouse_ && mouse_->poll(&m)) {
    ++events;
    deliver(m);
  }

  hardware::TouchEvent t;
  while (touch_ && touch_->poll(&t)) {
    ++events;
    const int32_t max_x = static_cast<int32_t>(window_.bounds().width) - 1, max_y = static_cast<int32_t>(window_.bounds().height) - 1;
    int32_t x = t.max_x ? static_cast<int32_t>(static_cast<uint64_t>(t.x) * (max_x + 1) / (uint64_t{t.max_x} + 1)) : x_;
    int32_t y = t.max_y ? static_cast<int32_t>(static_cast<uint64_t>(t.y) * (max_y + 1) / (uint64_t{t.max_y} + 1)) : y_;
    x = x < 0 ? 0 : x > max_x ? max_x : x;
    y = y < 0 ? 0 : y > max_y ? max_y : y;
    if (touch_tap_ && touch_tap_(touch_context_, t, x, y)) continue;      // (a program's window has it)
    // pointer emulation: the first contact nobody took is the left button where it is
    const bool down = t.phase == hardware::TouchPhase::Down || t.phase == hardware::TouchPhase::Motion;
    if (t.phase == hardware::TouchPhase::Down && !emulating_) {
      emulating_ = true;
      emulated_id_ = t.id;
    } else if (!emulating_ || t.id != emulated_id_) {
      continue;
    }
    hardware::MouseEvent e{};
    e.absolute = true;
    e.x = t.x;
    e.y = t.y;
    e.max_x = t.max_x;
    e.max_y = t.max_y;
    e.buttons = down ? hardware::mouse_button::kLeft : 0;
    if (t.phase == hardware::TouchPhase::Down && (buttons_ & hardware::mouse_button::kLeft)) {      // (a mouse holds the button: let go first)
      hardware::MouseEvent up = e;
      up.buttons = static_cast<uint8_t>(buttons_ & ~hardware::mouse_button::kLeft);
      deliver(up);
    }
    if (t.phase == hardware::TouchPhase::Down) {      // (moves there first, then presses: a click lands where the finger is)
      hardware::MouseEvent move = e;
      move.buttons = 0;
      deliver(move);
    }
    deliver(e);
    if (!down) emulating_ = false;
  }
  hardware::PenEvent p;
  while (pen_ && pen_->poll(&p)) {
    ++events;
    const int32_t max_x = static_cast<int32_t>(window_.bounds().width) - 1, max_y = static_cast<int32_t>(window_.bounds().height) - 1;
    int32_t x = p.max_x ? static_cast<int32_t>(static_cast<uint64_t>(p.x) * (max_x + 1) / (uint64_t{p.max_x} + 1)) : x_;
    int32_t y = p.max_y ? static_cast<int32_t>(static_cast<uint64_t>(p.y) * (max_y + 1) / (uint64_t{p.max_y} + 1)) : y_;
    x = x < 0 ? 0 : x > max_x ? max_x : x;
    y = y < 0 ? 0 : y > max_y ? max_y : y;
    uint8_t buttons = 0;
    uint32_t px = pen_x_, py = pen_y_;
    bool in_range = p.in_range;
    if (pen_tap_ && pen_tap_(pen_context_, p, x, y)) {
      in_range = false;      // (a program's window has it: the pointer lets go of what the pen held)
    } else if (p.in_range) {
      px = p.x;
      py = p.y;
      pen_max_x_ = p.max_x;
      pen_max_y_ = p.max_y;
      if (p.tip && !p.inverted) buttons |= hardware::mouse_button::kLeft;
      if (p.tip && p.inverted) buttons |= hardware::mouse_button::kMiddle;
      if (p.buttons & (hardware::pen_button::kBarrel | hardware::pen_button::kSecondary)) buttons |= hardware::mouse_button::kRight;
    }
    if (in_range == pen_in_range_ && buttons == pen_buttons_ && px == pen_x_ && py == pen_y_) continue;      // (nothing changed for the pointer)
    pen_in_range_ = in_range;
    pen_buttons_ = buttons;
    pen_x_ = px;
    pen_y_ = py;
    hardware::MouseEvent m{};
    m.absolute = true;
    m.x = px;
    m.y = py;
    m.max_x = pen_max_x_;
    m.max_y = pen_max_y_;
    m.buttons = buttons;
    deliver(m);
  }
  return events;
}

}  // namespace gui
