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

#include "Hardware/Bus/Thunderbolt/ThunderboltPower.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

bool TbPciPower::available() { return capability() != 0; }

bool TbPciPower::can_wake() {
  const uint16_t cap = capability();
  return cap != 0 && (bus_.read32(address_, cap) >> 16 & kPmeFromD3hot) != 0;
}

uint8_t TbPciPower::state() {
  const uint16_t cap = capability();
  return cap == 0 ? 0xFF : static_cast<uint8_t>(bus_.read32(address_, cap + 4) & kStateMask);
}

bool TbPciPower::wake_pending() {
  const uint16_t cap = capability();
  return cap != 0 && (bus_.read32(address_, cap + 4) & kPmeStatus) != 0;
}

bool TbPciPower::enter_d3hot(bool wake) {
  const uint16_t cap = capability();
  if (cap == 0 || (wake && !can_wake())) return false;
  uint32_t value = bus_.read32(address_, cap + 4);
  // (the status bit clears when a one is written: written as zero here so that a pending wake is not lost)
  value &= ~(kStateMask | kPmeStatus);
  value |= kStateD3hot;
  if (wake) value |= kPmeEnable;
  else value &= ~kPmeEnable;
  bus_.write32(address_, cap + 4, value);
  return (bus_.read32(address_, cap + 4) & kStateMask) == kStateD3hot;
}

bool TbPciPower::enter_d0() {
  const uint16_t cap = capability();
  if (cap == 0) return false;
  uint32_t value = bus_.read32(address_, cap + 4);
  value &= ~(kStateMask | kPmeEnable);
  value |= kPmeStatus;                                 // (clears what is pending)
  bus_.write32(address_, cap + 4, value);
  delay_us(kD3hotRecoveryUs);
  return (bus_.read32(address_, cap + 4) & kStateMask) == kStateD0;
}

}  // namespace hardware
