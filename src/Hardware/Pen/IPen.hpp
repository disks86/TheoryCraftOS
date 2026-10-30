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

// A pen (stylus on a digitizer): the interface that drivers of every kind of pen implement - HID over I2C today, USB or Bluetooth pens tomorrow - and that programs read (a drawing
// program wants the pressure and the tilt, the desktop wants the position). Polled like the other input devices: nothing blocks.
#pragma once
#include <stdint.h>
#include "Hardware/IHardware.hpp"

namespace hardware {

namespace pen_button {
constexpr uint8_t kBarrel = 1 << 0;       // the button on the barrel
constexpr uint8_t kSecondary = 1 << 1;    // a second barrel button
constexpr uint8_t kEraser = 1 << 2;       // the eraser switch (a pen with an eraser button, not the inverted end)
}  // namespace pen_button

struct PenEvent {
  bool in_range = false;                  // the pen is near enough to the screen to be tracked (hovering or touching)
  bool tip = false;                       // the tip touches the screen
  bool inverted = false;                  // the eraser end is the one near the screen
  uint8_t buttons = 0;                    // pen_button:: bits held
  uint32_t x = 0, y = 0;                  // position, 0..max_x / 0..max_y of the producing device
  uint32_t max_x = 0, max_y = 0;
  uint16_t pressure = 0;                  // 0..max_pressure (both 0 when the device reports no pressure: use `tip`)
  uint16_t max_pressure = 0;
  int16_t tilt_x = 0, tilt_y = 0;         // degrees from upright (-90..90); 0 when the device does not say
};

class IPen : public IHardware {
 public:
  virtual bool poll(PenEvent* out) = 0;
  virtual uint32_t max_x() const = 0;
  virtual uint32_t max_y() const = 0;
  virtual uint16_t max_pressure() const = 0;   // 0: the device reports no pressure
  virtual bool has_tilt() const = 0;

 protected:
  constexpr IPen() = default;
  ~IPen() = default;
};

}  // namespace hardware
