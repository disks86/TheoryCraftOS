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

// The gmux's ports on the machine (apart from AppleGmux.cpp so the host tests, which fake the ports, do not need the in/out instructions).
#include "Hardware/GraphicsProcessingUnit/Multi/AppleGmux.hpp"
#include "Hardware/IoPort.hpp"

namespace hardware {

uint8_t X86GmuxPorts::read8(uint16_t port) { return IoPort(port).read8(); }
void X86GmuxPorts::write8(uint16_t port, uint8_t value) { IoPort(port).write8(value); }
uint32_t X86GmuxPorts::read32(uint16_t port) { return IoPort(port).read32(); }

}  // namespace hardware
