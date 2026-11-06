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
#include "Hardware/Chipset/Smbus/SmbusHost.hpp"

namespace hardware {

// The SMBus host's registers as x86 I/O ports from `base` (the i801's SMBus BAR, the FCH's SmbusAsfIo base).
class SmbusIoPorts final : public ISmbusHostPorts {
 public:
  explicit constexpr SmbusIoPorts(uint16_t base) : base_(base) {}
  uint16_t base() const { return base_; }

  uint8_t read8(uint8_t reg) override;
  void write8(uint8_t reg, uint8_t value) override;
  void pause_us(uint32_t us) override;

 private:
  uint16_t base_;
};

}  // namespace hardware
