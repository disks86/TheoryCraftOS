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
#include "Hardware/Bus/Usb/Hid/HidReportParser.hpp"
#include "Hardware/Bus/Usb/Hid/UsbHidInterface.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/Mouse/MouseBase.hpp"

namespace hardware {

// A USB mouse or tablet. The device's HID report descriptor is parsed to find
// the button, X, Y and wheel fields, so relative mice and absolute tablets (such
// as QEMU's usb-tablet) both work. A boot-protocol mouse whose descriptor cannot
// be parsed falls back to the fixed 3-byte boot layout.
class UsbHidMouse final : public MouseBase {
 public:
  UsbHidMouse(IUsbHostController& host, uint32_t port, const UsbHidInterface& interface)
      : host_(host), port_(port), interface_(interface), generation_(host.port_generation(port)) {}

  const char* name() const override { return layout_.x.relative ? "USB HID mouse" : "USB HID tablet"; }
  bool initialize() override;
  void shutdown() override {}

  // False once the mouse was unplugged (the port's generation moved on).
  bool present() const { return host_.port_generation(port_) == generation_; }

  bool is_absolute() const override { return !layout_.x.relative; }
  uint32_t max_x() const override { return is_absolute() ? layout_.x.logical_max : 0; }
  uint32_t max_y() const override { return is_absolute() ? layout_.y.logical_max : 0; }

 private:
  void fetch() override;
  void on_release_all() override;

  IUsbHostController& host_;
  uint32_t port_;
  UsbHidInterface interface_;
  uint32_t generation_;
  HidPointerLayout layout_;
  MouseEvent last_{};   // the latest report, to let go of its buttons
};

}  // namespace hardware
