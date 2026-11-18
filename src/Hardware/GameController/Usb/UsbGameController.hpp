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

// A game controller on a USB root port, whichever protocol it speaks (GamepadTypes.hpp: Xbox 360, Xbox One, DualShock 4, DualSense, Switch Pro,
// generic HID). probe() looks at the device's descriptors and says whether it is one and how to talk to it; open() takes it over.
//
// Reports arrive on the interface's interrupt IN endpoint and are read without waiting by poll(). Output (motors, lights, the Xbox One power-on
// and acknowledgements, the Switch handshake) goes to the interrupt OUT endpoint, or for a HID pad without one as SET_REPORT on endpoint 0. A
// controller whose host controller has no interrupt OUT (anything but xHCI) still reports, except the Xbox One pads, which stay silent until
// they are powered on and are therefore not taken.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/GameController/HidGamepad.hpp"
#include "Hardware/GameController/IGameController.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct UsbGameControllerProbe {
  gamepad::Protocol protocol = gamepad::Protocol::None;
  uint16_t vendor_id = 0, product_id = 0;
  const char* name = nullptr;              // the catalogue's name, or null
  uint8_t configuration = 0;
  uint8_t interface_number = 0;
  uint8_t in_endpoint = 0, out_endpoint = 0;   // 0: none
  uint16_t in_max_packet = 0, out_max_packet = 0;
  uint8_t in_interval = 0, out_interval = 0;
  uint16_t report_descriptor_length = 0;   // (HID)
};

// Whether the device on a root port is a game controller, and how it is reached. A HID device that is not in the catalogue counts only if its
// report descriptor (which this reads, setting the configuration) has a joystick or gamepad collection.
[[nodiscard]] bool probe_usb_game_controller(IUsbHostController& host, uint32_t port, UsbGameControllerProbe* out);

class UsbGameController final : public IGameController {
 public:
  // Takes over the controller on the port and starts it; null if it is none or cannot be started.
  static UniquePointer<UsbGameController> open(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator);

  UsbGameController(IUsbHostController& host, uint32_t port, const UsbGameControllerProbe& probe);
  const char* name() const override { return info_.name; }
  bool initialize() override;
  void shutdown() override;
  const gamepad::Info& info() const override { return info_; }
  bool poll() override;
  const gamepad::State& state() const override { return state_; }
  [[nodiscard]] bool set_output(const gamepad::Output& output) override;
  bool present() const override { return host_.port_generation(port_) == generation_; }

  uint32_t port() const { return port_; }
  bool handshake_done() const { return handshake_step_ >= kHandshakeEnd; }

 private:
  static constexpr uint32_t kHandshakeEnd = 6;      // Switch Pro: four handshake reports, the report mode, the player LEDs
  static constexpr uint32_t kHandshakePolls = 8;    // polls to wait for the pad's answer before going on regardless
  bool send(ConstByteView message);
  bool send_report(ConstByteView report);           // a HID output report: the OUT endpoint, else SET_REPORT
  bool decode(ConstByteView report);
  void advance_handshake(bool answered);

  IUsbHostController& host_;
  uint32_t port_;
  UsbGameControllerProbe probe_;
  uint32_t generation_;
  gamepad::Info info_;
  gamepad::State state_;
  gamepad::Output output_;
  hid::GamepadDecoder hid_;
  uint8_t hid_report_id_ = 0;
  uint8_t sequence_ = 0;                            // GIP and the Switch's running message counts
  uint32_t handshake_step_ = kHandshakeEnd;
  uint32_t handshake_waited_ = 0;
  bool out_ready_ = false;
};

}  // namespace hardware
