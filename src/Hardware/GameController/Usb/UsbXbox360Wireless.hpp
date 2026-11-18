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

// One pad of an Xbox 360 Wireless Receiver (045E:0719 and its copies): the receiver has an interface per pad it can pair (four), and each is
// driven on its own, like a wired pad that comes and goes without the USB device leaving. connected() follows the receiver's pairing
// messages, so the game controller service gives the pad a player slot while it is paired and takes it back when it is switched off.
// Needs interrupt OUT (xHCI): the receiver says nothing about a pad until it is asked.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/GameController/IGameController.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct UsbWirelessChannel {
  uint8_t configuration = 0, interface_number = 0;
  uint8_t in_endpoint = 0, out_endpoint = 0;
  uint16_t in_max_packet = 0, out_max_packet = 0;
  uint8_t in_interval = 0, out_interval = 0;
};

// The pad interfaces of a receiver on a root port (subclass 0x5D, protocol 0x81 with both endpoints); returns how many (up to out.size()).
uint32_t find_xbox360_wireless_channels(ConstByteView configuration, ArrayView<UsbWirelessChannel> out);

class UsbXbox360WirelessPad final : public IGameController {
 public:
  UsbXbox360WirelessPad(IUsbHostController& host, uint32_t port, const UsbWirelessChannel& channel, uint16_t vendor, uint16_t product);
  const char* name() const override { return info_.name; }
  bool initialize() override;
  void shutdown() override;
  const gamepad::Info& info() const override { return info_; }
  bool poll() override;
  const gamepad::State& state() const override { return state_; }
  [[nodiscard]] bool set_output(const gamepad::Output& output) override;
  bool present() const override { return host_.port_generation(port_) == generation_; }
  bool connected() const override { return connected_ && present(); }

 private:
  bool send(ConstByteView message);

  IUsbHostController& host_;
  uint32_t port_;
  UsbWirelessChannel channel_;
  uint32_t generation_;
  gamepad::Info info_;
  gamepad::State state_;
  gamepad::Output output_;
  bool connected_ = false;
};

}  // namespace hardware
