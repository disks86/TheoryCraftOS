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
#include "Hardware/IHardware.hpp"

namespace hardware {

namespace mouse_button {
constexpr uint8_t kLeft = 1 << 0;
constexpr uint8_t kRight = 1 << 1;
constexpr uint8_t kMiddle = 1 << 2;
}  // namespace mouse_button

struct MouseEvent {
  int32_t dx, dy;     // relative devices: movement, +y is down
  int32_t wheel;      // +1 per notch away from the user
  uint8_t buttons;    // mouse_button:: bits currently held
  bool absolute;      // true: x/y below are positions, dx/dy are 0
  uint32_t x, y;      // positions, 0..max_x / 0..max_y (absolute events only)
  uint32_t max_x, max_y;  // the producing device's range (absolute events only)
};

// A mouse or tablet. Relative devices (mice) report movement; absolute devices
// (tablets, as QEMU's usb-tablet) report a position in 0..max_x() x 0..max_y().
// Polled: nothing blocks.
class IMouse : public IHardware {
 public:
  virtual bool poll(MouseEvent* out) = 0;

  virtual bool is_absolute() const = 0;
  virtual uint32_t max_x() const = 0;  // absolute devices only; 0 otherwise
  virtual uint32_t max_y() const = 0;

 protected:
  constexpr IMouse() = default;
  ~IMouse() = default;
};

}  // namespace hardware
