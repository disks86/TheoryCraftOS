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
#include "Utility/ArrayView.hpp"

namespace hardware {

// An I2C bus as the master sees it: one transaction is an optional write, then (after a repeated start) an optional read, ending with a stop. 7 bit addresses.
class II2cMaster {
 public:
  virtual bool transfer(uint16_t address, ConstByteView write, ByteView read) = 0;

 protected:
  constexpr II2cMaster() = default;
  ~II2cMaster() = default;
};

}  // namespace hardware
