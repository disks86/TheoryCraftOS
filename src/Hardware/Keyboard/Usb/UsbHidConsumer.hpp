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
#include "Hardware/Hid/HidDescriptor.hpp"
#include "Hardware/Keyboard/KeyboardBase.hpp"

namespace hardware {

// The media keys of a USB keyboard: the interface that is not the boot keyboard (many keyboards have a second one) or the same one in report protocol, whose report descriptor has a consumer
// control collection (usage page 0x0C, usage 1: volume, mute, play/pause, ...). They become the key codes of KeyCodes.hpp, like the media keys of a PS/2 keyboard. initialize() fails for
// an interface without such a report, which is then left to the next kind of driver.
class UsbHidConsumer final : public KeyboardBase {
 public:
  UsbHidConsumer(IUsbHostController& host, uint32_t port, const UsbHidInterface& interface)
      : host_(host), port_(port), interface_(interface), generation_(host.port_generation(port)) {}

  const char* name() const override { return "USB HID media keys"; }
  bool initialize() override;
  void shutdown() override {}
  bool present() const { return host_.port_generation(port_) == generation_; }

 private:
  void fetch() override;
  void on_release_all() override;
  void apply(const hid::KeyboardState& state);

  IUsbHostController& host_;
  uint32_t port_;
  UsbHidInterface interface_;
  uint32_t generation_;
  hid::ConsumerDecoder decoder_;
  uint8_t report_id_ = 0;
  hid::KeyboardState held_;
};

}  // namespace hardware
