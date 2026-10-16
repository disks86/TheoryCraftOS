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

#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

struct KeyEvent {
  uint8_t usage;  // USB HID keyboard usage (see KeyCodes.hpp)
  bool pressed;   // false = released
};

// A keyboard delivering key press/release events. Polled: nothing blocks.
// Implementations live in subfolders by transport (Ps2/, Usb/, Virtio/).
class IKeyboard : public IHardware {
 public:
  // Returns the next event, or false when there is none right now.
  virtual bool poll(KeyEvent* out) = 0;
  // Modifier keys currently held (modifier:: bits).
  virtual uint8_t modifiers() const = 0;

 protected:
  constexpr IKeyboard() = default;
  ~IKeyboard() = default;
};

}  // namespace hardware
