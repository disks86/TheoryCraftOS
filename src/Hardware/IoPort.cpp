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

#include "Hardware/IoPort.hpp"
#include "Hardware/x86_64/asm.hpp"

namespace hardware {
uint8_t IoPort::read8() const { return tcos_inb(port_); }
void IoPort::write8(uint8_t value) const { tcos_outb(port_, value); }
uint16_t IoPort::read16() const { return tcos_inw(port_); }
void IoPort::write16(uint16_t value) const { tcos_outw(port_, value); }
uint32_t IoPort::read32() const { return tcos_inl(port_); }
void IoPort::write32(uint32_t value) const { tcos_outl(port_, value); }
}  // namespace hardware
