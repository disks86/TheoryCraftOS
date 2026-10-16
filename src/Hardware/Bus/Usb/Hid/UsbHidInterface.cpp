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

#include "Hardware/Bus/Usb/Hid/UsbHidInterface.hpp"

namespace hardware {

namespace {
constexpr uint8_t kTypeConfiguration = 2;
constexpr uint8_t kTypeInterface = 4;
constexpr uint8_t kTypeEndpoint = 5;
constexpr uint8_t kTypeHid = 0x21;
constexpr uint8_t kClassHid = 3;
constexpr uint8_t kTransferInterrupt = 3;
}  // namespace

uint32_t find_hid_interfaces(const uint8_t* c, uint16_t length, UsbHidInterface* out,
                             uint32_t capacity) {
  if (!c || length < 9 || c[1] != kTypeConfiguration) return 0;
  const uint8_t configuration_value = c[5];

  uint32_t found = 0;
  UsbHidInterface current{};
  bool in_hid = false;      // inside a HID interface (alternate setting 0)
  bool have_endpoint = false;
  for (uint16_t i = 0; i + 2 <= length && c[i] >= 2;) {
    const uint8_t size = c[i], type = c[i + 1];
    if (i + size > length) break;

    if (type == kTypeInterface && size >= 9) {
      in_hid = c[i + 5] == kClassHid && c[i + 3] == 0;  // class, alternate setting
      current = UsbHidInterface{};
      current.configuration_value = configuration_value;
      current.number = c[i + 2];
      current.subclass = c[i + 6];
      current.protocol = c[i + 7];
      have_endpoint = false;
    } else if (in_hid && type == kTypeHid && size >= 9) {
      current.report_descriptor_length = c[i + 7] | (c[i + 8] << 8);  // first class descriptor
    } else if (in_hid && type == kTypeEndpoint && size >= 7 && !have_endpoint) {
      const bool interrupt_in = (c[i + 2] & 0x80) && (c[i + 3] & 3) == kTransferInterrupt;
      if (interrupt_in) {
        current.endpoint_address = c[i + 2];
        current.max_packet = (c[i + 4] | (c[i + 5] << 8)) & 0x7FF;
        current.interval = c[i + 6];
        have_endpoint = true;
        if (found < capacity) out[found] = current;
        ++found;
      }
    }
    i += size;
  }
  return found;
}

}  // namespace hardware
