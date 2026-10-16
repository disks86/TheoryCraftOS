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

// A HID interface found in a configuration descriptor, with its first
// interrupt-IN endpoint.
struct UsbHidInterface {
  uint8_t configuration_value;
  uint8_t number;
  uint8_t subclass;  // 1 = boot interface
  uint8_t protocol;  // with subclass 1: 1 = keyboard, 2 = mouse
  uint8_t endpoint_address;
  uint16_t max_packet;
  uint8_t interval;
  uint16_t report_descriptor_length;

  bool is_boot_keyboard() const { return subclass == 1 && protocol == 1; }
  bool is_boot_mouse() const { return subclass == 1 && protocol == 2; }
};

// Lists the HID interfaces (class 3) that have an interrupt-IN endpoint.
// Returns how many were found (up to `capacity` are stored).
uint32_t find_hid_interfaces(const uint8_t* configuration, uint16_t length, UsbHidInterface* out,
                             uint32_t capacity);

}  // namespace hardware
