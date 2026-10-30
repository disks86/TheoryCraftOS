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

// Dell's hotkeys through ACPI-WMI (the "wmi" bus of Linux, drivers/platform/x86/dell/dell-wmi-base.c): a device with _HID PNP0C14 has a _WDG buffer listing the WMI blocks as 20-byte entries
// (GUID, object id - for an event: the Notify value -, instance count, flags). The firmware raises such an event with Notify(device, <notify id>); the data is the buffer returned by _WED(<notify id>),
// 16-bit little endian words: a length (the entry is length + 1 words), the event type, then codes. Type 0 has one code at word 2; types 0x10 and 0x11 list codes from word 2 on.
// Only the keys that mean something to the system are taken: mute, brightness down / up, and the mic mute (which the firmware has done already, so it only becomes a notice).
#pragma once
#include "Hardware/Acpi/AcpiEvents.hpp"

namespace hardware {

// One decoded hotkey: a key code of KeyCodes.hpp, or kMicMute for the key that has no code.
constexpr uint8_t kDellMicMute = 0xFF;
// The keys in a _WED buffer (at most `capacity`); returns how many. Nothing outside the buffer is read.
uint32_t dell_wmi_decode(ConstByteView buffer, uint8_t* keys, uint32_t capacity);

class DellWmiHotkeys final : public IAcpiNotifyListener {
 public:
  static constexpr uint32_t kMaxDevices = 4;
  using Sink = void (*)(void* context, uint8_t key);
  // `brightness`: whether the brightness keys are passed on (not when the video extensions' notifications do the same).
  DellWmiHotkeys(acpi::AmlInterpreter& vm, Sink sink, void* context, bool brightness) : vm_(vm), sink_(sink), context_(context), brightness_(brightness) {}

  // Finds the WMI devices that have the Dell event block. Returns how many.
  uint32_t scan();
  uint32_t device_count() const { return count_; }
  void on_notify(acpi::Node* device, uint32_t value) override;

 private:
  struct Device {
    acpi::Node* node = nullptr;
    uint8_t notify_id = 0;
  };
  acpi::AmlInterpreter& vm_;
  Sink sink_;
  void* context_;
  bool brightness_;
  Device devices_[kMaxDevices];
  uint32_t count_ = 0;
};

}  // namespace hardware
