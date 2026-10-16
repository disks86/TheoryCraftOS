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

#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

// One x86 I/O port (in/out instructions).
class IoPort final : public IHardware {
 public:
  explicit constexpr IoPort(uint16_t port) : port_(port) {}

  const char* name() const override { return "I/O port"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  uint16_t port() const { return port_; }
  uint8_t read8() const;
  void write8(uint8_t value) const;
  uint16_t read16() const;
  void write16(uint16_t value) const;
  uint32_t read32() const;
  void write32(uint32_t value) const;

 private:
  uint16_t port_;
};

}  // namespace hardware
