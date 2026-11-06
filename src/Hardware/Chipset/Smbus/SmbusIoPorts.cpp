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

#include "Hardware/Chipset/Smbus/SmbusIoPorts.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

uint8_t SmbusIoPorts::read8(uint8_t reg) { return IoPort(static_cast<uint16_t>(base_ + reg)).read8(); }

void SmbusIoPorts::write8(uint8_t reg, uint8_t value) { IoPort(static_cast<uint16_t>(base_ + reg)).write8(value); }

void SmbusIoPorts::pause_us(uint32_t us) { delay_spin_us(us); }

}  // namespace hardware
