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

#include "Hardware/Acpi/SystemAcpiPorts.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

uint8_t SystemAcpiPorts::in8(uint16_t port) { return IoPort(port).read8(); }
void SystemAcpiPorts::out8(uint16_t port, uint8_t value) { IoPort(port).write8(value); }
uint16_t SystemAcpiPorts::in16(uint16_t port) { return IoPort(port).read16(); }
void SystemAcpiPorts::out16(uint16_t port, uint16_t value) { IoPort(port).write16(value); }
void SystemAcpiPorts::delay_us(uint32_t us) { hardware::delay_us(us); }

// The mapped page that holds [address, address + size), mapping it on first use; nullptr if the mapper refuses.
const MmioRegion* SystemAcpiPorts::window(uint64_t address, uint32_t size) {
  for (uint32_t i = 0; i < window_count_; ++i)
    if (address >= windows_[i].address() && address - windows_[i].address() <= windows_[i].length() - size) return &windows_[i];
  if (!mapper_ || window_count_ >= 4) return nullptr;
  MmioRegion region;
  const uint64_t page = address & ~uint64_t{0xFFF};
  if (address - page > 0x1000 - size || !mapper_->map_registers(page, 0x1000, &region)) return nullptr;
  windows_[window_count_] = region;
  return &windows_[window_count_++];
}

uint8_t SystemAcpiPorts::mem_in8(uint64_t address) {
  const MmioRegion* w = window(address, 1);
  return w ? w->read8(address - w->address()) : 0;
}
void SystemAcpiPorts::mem_out8(uint64_t address, uint8_t value) {
  if (const MmioRegion* w = window(address, 1)) w->write8(address - w->address(), value);
}
uint16_t SystemAcpiPorts::mem_in16(uint64_t address) {
  const MmioRegion* w = window(address, 2);
  return (w && (address & 1) == 0) ? w->read16(address - w->address()) : 0;
}
void SystemAcpiPorts::mem_out16(uint64_t address, uint16_t value) {
  if (const MmioRegion* w = window(address, 2)) if ((address & 1) == 0) w->write16(address - w->address(), value);
}

}  // namespace hardware
