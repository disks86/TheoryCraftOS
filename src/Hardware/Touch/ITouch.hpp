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

// A touchscreen (fingers on the screen): the interface that drivers of every kind of touch device implement - HID over I2C, virtio-input (QEMU's
// virtio-multitouch) - and that the desktop reads. Each finger is a contact with an id that stays the same from the moment it touches (Down) until it
// lifts (Up); it moves in between (Motion). Cancel: the device lost track of it (it went away, or was reset). Polled like the other input devices.
//
// A touch device is also a mouse until someone reads its touches (set_pointer_emulation(false)): the first finger down is the left button at its
// position, so that every program works with it. The desktop turns that off and does it itself for the windows that do not take touches.
#pragma once
#include <stdint.h>
#include "Hardware/IHardware.hpp"

namespace hardware {

enum class TouchPhase : uint8_t { Down, Motion, Up, Cancel };

struct TouchEvent {
  TouchPhase phase = TouchPhase::Down;
  uint32_t id = 0;                // the contact (unique among the contacts down at once, of all touch devices when read through a CompositeTouch)
  uint32_t x = 0, y = 0;          // position, 0..max_x / 0..max_y of the producing device (an Up or Cancel carries the last one)
  uint32_t max_x = 0, max_y = 0;
};

class ITouch : public IHardware {
 public:
  virtual bool poll(TouchEvent* out) = 0;
  // Whether the device also moves the pointer (as a mouse: the first finger is the left button). On by default.
  virtual void set_pointer_emulation(bool on) = 0;

 protected:
  constexpr ITouch() = default;
  ~ITouch() = default;
};

}  // namespace hardware
