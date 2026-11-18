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

// Game controllers over Bluetooth, whichever transport brought them (HidPadChannel.hpp): the Bluetooth forms of the pads' reports.
//
//   - Xbox One S / Series / Elite 2 over Bluetooth (LE, HID over GATT; the oldest One S firmware: classic): a HID gamepad whose report
//     descriptor is read as usual, with Microsoft's button order (A, B, -, X, Y, -, LB, RB, -, -, View, Menu, Guide, LS, RS) and rumble as
//     output report 3.
//   - DualShock 4 (classic): reduced report 0x01 until feature report 0x02 is read, then report 0x11 (the USB layout two bytes on); output
//     report 0x11 with a CRC-32 over 0xA2 and the report.
//   - DualSense / Edge (classic): report 0x31 (the USB layout one byte on); output report 0x31 with a sequence tag and the CRC.
//   - Switch Pro (classic): simple report 0x3F until subcommand 0x03 asks for full reports 0x30 (the USB layout).
//   - Anything else with a joystick or gamepad collection (8BitDo, Stadia, Steam Controller BLE, Logitech): HidGamepad's decoder.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/GameController/Bluetooth/HidPadChannel.hpp"
#include "Hardware/GameController/IGameController.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct BluetoothPadIdentity {
  uint16_t vendor_id = 0, product_id = 0;   // from the Device Information service's PnP ID (LE) or the Device ID record (classic, SDP); 0 if unknown
  const char* name = nullptr;               // the device's own name (may be null)
  bool le = false;
};

// Whether a Bluetooth HID device is a game controller (by its ids, else by a joystick or gamepad collection in its report map). Transports
// ask this to route a device to the pads instead of the keyboards and mice.
bool is_bluetooth_gamepad(const BluetoothPadIdentity& identity, ConstByteView report_map);

// The pad behind a channel; null if it is no game controller or out of memory. It reads its reports from the channel when the game
// controller service polls it, and is present() while the channel is attached. Give it to GameControllerService::add().
UniquePointer<IGameController> make_bluetooth_gamepad(allocators::IAllocator& allocator, SharedPointer<HidPadChannel> channel,
                                                      const BluetoothPadIdentity& identity, ConstByteView report_map);

}  // namespace hardware
