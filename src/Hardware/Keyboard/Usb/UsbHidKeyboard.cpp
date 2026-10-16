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

#include "Hardware/Keyboard/Usb/UsbHidKeyboard.hpp"

namespace hardware {

namespace {
constexpr uint8_t kRequestSetIdle = 0x0A;
constexpr uint8_t kRequestSetProtocol = 0x0B;
constexpr uint8_t kClassInterfaceOut = 0x21;  // host-to-device, class, interface

bool contains(ConstByteView keys, uint8_t usage) {  // among the six key slots of a boot report
  for (uint64_t i = 2; i < 8; ++i)
    if (keys[i] == usage) return true;
  return false;
}
}  // namespace

bool UsbHidKeyboard::initialize() {
  if (!host_.set_configuration(port_, interface_.configuration_value)) return false;
  // Boot protocol (a fixed report layout) and "report only on change".
  if (!host_.control_out(port_, kClassInterfaceOut, kRequestSetProtocol, 0, interface_.number)) return false;
  host_.control_out(port_, kClassInterfaceOut, kRequestSetIdle, 0, interface_.number);  // optional
  return host_.start_interrupt_in(port_, interface_.endpoint_address, interface_.max_packet,
                                  interface_.interval);
}

void UsbHidKeyboard::fetch() {
  uint8_t report[8];
  uint16_t length = 0;
  while (host_.poll_interrupt_in(port_, interface_.endpoint_address, ByteView(report), &length)) {
    if (length < 8) continue;
    const ConstByteView current(report);
    const ConstByteView before(previous_);

    for (int bit = 0; bit < 8; ++bit) {  // modifier byte: one bit per modifier key
      const bool now = current[0] & (1u << bit), was = before[0] & (1u << bit);
      if (now != was) push(key::kLeftControl + bit, now);
    }
    for (int i = 2; i < 8; ++i) {
      if (current[i] > 3 && !contains(before, current[i])) push(current[i], true);   // 1-3 = rollover errors
      if (before[i] > 3 && !contains(current, before[i])) push(before[i], false);
    }
    ByteView(previous_).copy_from(current);
  }
}

}  // namespace hardware
