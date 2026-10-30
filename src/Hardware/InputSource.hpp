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

// How an input driver that SystemInput does not know about gives its keyboards and mice to the system. A source (Bluetooth HID, a future I2C keyboard, a remote-desktop input channel...)
// is registered once with SystemInput::add_source(); from then on SystemInput::poll() asks it to bring devices in and out through an IInputSink, and the merged keyboard() and mouse()
// everyone else reads show them. Nothing that reads input (the desktop, programs) has to change when a source is added, and neither does SystemInput.
#pragma once
#include <stdint.h>
#include "Hardware/Keyboard/IKeyboard.hpp"
#include "Hardware/Mouse/IMouse.hpp"
#include "Utility/StringView.hpp"

namespace hardware {

// What a source adds its devices to. The devices are not owned by the sink: the source keeps them alive until it has removed them. Before a device is removed its held keys and buttons
// should be released (KeyboardBase::release_all, MouseBase::release_all), so nothing stays pressed in whoever reads the events; the merged devices keep delivering what a removed
// device had left.
class IInputSink {
 public:
  virtual bool add_keyboard(IKeyboard* keyboard) = 0;
  virtual bool remove_keyboard(IKeyboard* keyboard) = 0;
  virtual bool add_mouse(IMouse* mouse) = 0;
  virtual bool remove_mouse(IMouse* mouse) = 0;
  // Tells the user something happened ("Bluetooth keyboard: connected"); may do nothing.
  virtual void notify(StringView text) = 0;

 protected:
  constexpr IInputSink() = default;
  ~IInputSink() = default;
};

class IInputDeviceSource {
 public:
  virtual const char* name() const = 0;
  // Brings in the devices that appeared since the last call and takes out those that went; returns how many changed. Called from the thread that polls SystemInput (one at a time).
  virtual uint32_t poll(IInputSink& sink) = 0;
  // The input system is shutting down (or the source is being unregistered): release and remove every device.
  virtual void detach_all(IInputSink& sink) = 0;

 protected:
  constexpr IInputDeviceSource() = default;
  ~IInputDeviceSource() = default;
};

}  // namespace hardware
