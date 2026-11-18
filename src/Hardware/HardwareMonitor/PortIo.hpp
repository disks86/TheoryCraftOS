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

// The 8-bit x86 I/O port space as the Super I/O code sees it: X86PortIo on the machine, a scripted model in the tests. (The SMBus host's
// ISmbusHostPorts is relative to one base and 8-bit wide, so it cannot reach both the configuration ports and the sensor window.)
#pragma once
#include <stdint.h>

namespace hardware {

class IPortIo {
 public:
  virtual uint8_t read8(uint16_t port) = 0;
  virtual void write8(uint16_t port, uint8_t value) = 0;

 protected:
  constexpr IPortIo() = default;
  ~IPortIo() = default;
};

// The in/out instructions.
class X86PortIo final : public IPortIo {
 public:
  uint8_t read8(uint16_t port) override;
  void write8(uint16_t port, uint8_t value) override;
};

// The address/data register pair most sensor chips put at base+5 / base+6 of their I/O window.
class IndexedPorts {
 public:
  static constexpr uint16_t kAddressOffset = 5, kDataOffset = 6;
  IndexedPorts(IPortIo& io, uint16_t base) : io_(io), base_(base) {}
  uint16_t base() const { return base_; }
  uint8_t read(uint8_t index) {
    io_.write8(static_cast<uint16_t>(base_ + kAddressOffset), index);
    return io_.read8(static_cast<uint16_t>(base_ + kDataOffset));
  }
  void write(uint8_t index, uint8_t value) {
    io_.write8(static_cast<uint16_t>(base_ + kAddressOffset), index);
    io_.write8(static_cast<uint16_t>(base_ + kDataOffset), value);
  }

 private:
  IPortIo& io_;
  uint16_t base_;
};

}  // namespace hardware
