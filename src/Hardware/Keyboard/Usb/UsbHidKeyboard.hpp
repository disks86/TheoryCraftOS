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
#include "Hardware/Bus/Usb/Hid/UsbHidInterface.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/Keyboard/KeyboardBase.hpp"

namespace hardware {

// A USB keyboard speaking the HID boot protocol: 8-byte reports with a modifier
// byte and up to six pressed keys, which are diffed into press/release events.
class UsbHidKeyboard final : public KeyboardBase {
 public:
  UsbHidKeyboard(IUsbHostController& host, uint32_t port, const UsbHidInterface& interface)
      : host_(host), port_(port), interface_(interface), generation_(host.port_generation(port)) {}

  const char* name() const override { return "USB HID keyboard"; }
  bool initialize() override;
  void shutdown() override {}

  // False once the keyboard was unplugged (the port's generation moved on).
  bool present() const { return host_.port_generation(port_) == generation_; }

 private:
  void fetch() override;
  void on_release_all() override;

  IUsbHostController& host_;
  uint32_t port_;
  UsbHidInterface interface_;
  uint32_t generation_;
  uint8_t previous_[8] = {};
};

}  // namespace hardware
