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

// The words the hardware inventory and the device Properties window share for the same devices.
#pragma once
#include "Hardware/Bus/Usb/UsbTypes.hpp"
#include "Utility/String.hpp"

namespace system_inventory {

inline void append_hex(String* out, uint32_t value, uint32_t digits) {
  static const char kDigits[] = "0123456789ABCDEF";
  for (int shift = static_cast<int>(digits) * 4 - 4; shift >= 0; shift -= 4) out->push_back(kDigits[(value >> shift) & 15]);
}

inline const char* pci_class_name(uint8_t class_code) {
  switch (class_code) {
    case 0x00: return "unclassified";
    case 0x01: return "storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x05: return "memory controller";
    case 0x06: return "bridge";
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input device";
    case 0x0C: return "serial bus controller";
    case 0x0D: return "wireless controller";
    default: return "device";
  }
}

inline const char* usb_controller_name(hardware::UsbControllerKind kind) {
  switch (kind) {
    case hardware::UsbControllerKind::Uhci: return "UHCI USB controller";
    case hardware::UsbControllerKind::Ohci: return "OHCI USB controller";
    case hardware::UsbControllerKind::Ehci: return "EHCI USB controller";
    case hardware::UsbControllerKind::Xhci: return "xHCI USB controller";
    default: return "USB controller";
  }
}

inline const char* usb_class_name(uint8_t device_class) {
  switch (device_class) {
    case 0x01: return "audio";
    case 0x03: return "input";
    case 0x07: return "printer";
    case 0x08: return "storage";
    case 0x09: return "hub";
    case 0x0E: return "video";
    case 0xEF: return "composite";
    default: return "";
  }
}


}  // namespace system_inventory
