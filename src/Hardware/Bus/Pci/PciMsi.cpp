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

#include "Hardware/Bus/Pci/PciMsi.hpp"

namespace hardware {

namespace {
constexpr uint32_t kControlEnable = 1u << 0, kControlMultipleEnableMask = 7u << 4, kControl64Bit = 1u << 7, kControlPerVectorMasking = 1u << 8;
constexpr uint16_t kCommand = 0x04;
constexpr uint32_t kCommandIntxDisable = 1u << 10;

// The command register is the low half of the dword at 4; the high half is the status register, whose bits are cleared by writing ones, so it is written as zeros.
void set_intx_disable(const IPciBus& bus, PciAddress function, bool disable) {
  const uint32_t command = bus.read32(function, kCommand) & 0xFFFF;
  bus.write32(function, kCommand, disable ? (command | kCommandIntxDisable) : (command & ~kCommandIntxDisable));
}
}  // namespace

bool PciMsi::enable(PciAddress function, uint64_t message_address, uint16_t message_data) {
  if (capability_) return false;
  const uint16_t cap = bus_.find_capability(function, kCapabilityId);
  if (!cap) return false;
  const uint32_t first = bus_.read32(function, cap);   // id, next pointer, message control (high half)
  const uint32_t control = first >> 16;
  const bool is64 = (control & kControl64Bit) != 0;
  if ((message_address & 3) != 0 || (!is64 && (message_address >> 32) != 0)) return false;

  // Off while the message is written, one vector, then on.
  const uint32_t quiet = control & ~(kControlEnable | kControlMultipleEnableMask);
  bus_.write32(function, cap, (first & 0xFFFF) | (quiet << 16));
  bus_.write32(function, cap + 4, static_cast<uint32_t>(message_address));
  if (is64) {
    bus_.write32(function, cap + 8, static_cast<uint32_t>(message_address >> 32));
    bus_.write32(function, cap + 12, message_data);
    if (control & kControlPerVectorMasking) bus_.write32(function, cap + 16, 0);   // the vector is not masked
  } else {
    bus_.write32(function, cap + 8, message_data);
    if (control & kControlPerVectorMasking) bus_.write32(function, cap + 12, 0);
  }
  bus_.write32(function, cap, (first & 0xFFFF) | ((quiet | kControlEnable) << 16));
  if (!(bus_.read32(function, cap) & (kControlEnable << 16))) {   // the function would not take it
    bus_.write32(function, cap, (first & 0xFFFF) | (quiet << 16));
    return false;
  }
  set_intx_disable(bus_, function, true);
  function_ = function;
  capability_ = cap;
  return true;
}

void PciMsi::disable() {
  if (!capability_) return;
  const uint32_t first = bus_.read32(function_, capability_);
  bus_.write32(function_, capability_, first & ~(kControlEnable << 16));
  set_intx_disable(bus_, function_, false);
  capability_ = 0;
}

}  // namespace hardware
