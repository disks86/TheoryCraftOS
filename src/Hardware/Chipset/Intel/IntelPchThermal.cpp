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

#include "Hardware/Chipset/Intel/IntelPchThermal.hpp"

namespace hardware {

bool intel_pch_thermal_device(uint16_t device_id) {
  // drivers/thermal/intel/intel_pch_thermal.c
  constexpr uint16_t kIds[] = {0x9C24, 0x8C24, 0x9CA4, 0x9D31, 0xA131, 0x9DF9, 0xA379, 0x02F9, 0x06F9, 0xA1B1, 0x8D24};
  for (uint16_t id : kIds)
    if (id == device_id) return true;
  return false;
}

bool IntelPchThermal::attach(const MmioRegion& registers) {
  if (!registers.valid() || registers.length() < kRegisterBytes) return false;
  if (registers.read32(kTemp) == 0xFFFFFFFFu) return false;   // nothing behind the BAR
  registers_ = registers;
  return true;
}

bool IntelPchThermal::enabled() const { return attached() && (registers_.read8(kTsel) & kTselEnabled); }

bool IntelPchThermal::locked() const { return attached() && (registers_.read8(kTsel) & kTselLocked); }

bool IntelPchThermal::read(int32_t* millicelsius) {
  if (!enabled()) return false;
  *millicelsius = millicelsius_of(registers_.read16(kTemp));
  return true;
}

ThermalLimits IntelPchThermal::limits() {
  ThermalLimits limits;
  if (!attached()) return limits;
  const uint16_t trip = registers_.read16(kCtt) & kReadingMask;
  if (trip) limits.critical_mc = millicelsius_of(trip);
  return limits;
}

}  // namespace hardware
