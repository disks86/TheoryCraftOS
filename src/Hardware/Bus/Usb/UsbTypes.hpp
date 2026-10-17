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
#include <stdint.h>

namespace hardware {

enum class UsbControllerKind : uint8_t { Uhci, Ohci, Ehci, Xhci, Unknown };
enum class UsbSpeed : uint8_t { Unknown, Low, Full, High, Super, SuperPlus };

struct UsbPortInfo {
  bool connected;  // a device is attached
  UsbSpeed speed;  // meaningful only when connected
};

// A device appearing on or disappearing from a root port after start-up (see IUsbHostController::poll_hotplug).
struct UsbHotplugEvent {
  uint32_t controller = 0;  // filled in by the bus; a host controller leaves it 0
  uint32_t port = 0;
  bool connected = false;   // true: enumerated and ready for class drivers; false: gone
};

// USB 2.0 standard device descriptor (section 9.6.1), as sent on the wire.
struct UsbDeviceDescriptor {
  uint8_t length;
  uint8_t descriptor_type;  // 1
  uint16_t usb_version;     // BCD, e.g. 0x0200
  uint8_t device_class;
  uint8_t device_subclass;
  uint8_t device_protocol;
  uint8_t max_packet_size0;  // for SuperSpeed this is an exponent: size = 2^value
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t device_version;
  uint8_t manufacturer_string;
  uint8_t product_string;
  uint8_t serial_string;
  uint8_t configuration_count;
} __attribute__((packed));
static_assert(sizeof(UsbDeviceDescriptor) == 18);

}  // namespace hardware
