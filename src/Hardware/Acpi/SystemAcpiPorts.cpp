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

}  // namespace hardware
