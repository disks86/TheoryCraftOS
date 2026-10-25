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

// The x86 I/O instructions as the port access of the serial hardware drivers.
#pragma once
#include "Hardware/IoPort.hpp"
#include "Hardware/SerialPort/Uart16550.hpp"

namespace hardware {

class X86SerialPortIo final : public ISerialPortIo {
 public:
  uint8_t read8(uint16_t port) override { return IoPort(port).read8(); }
  void write8(uint16_t port, uint8_t value) override { IoPort(port).write8(value); }
};

}  // namespace hardware
