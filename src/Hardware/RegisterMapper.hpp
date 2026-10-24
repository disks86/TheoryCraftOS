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
#include "Hardware/Mmio.hpp"

namespace hardware {

// Gets a device's registers (or any range of physical addresses that is not memory the OS owns) mapped for the CPU: MmioMapper does it on the real machine, a test
// hands out plain memory. A range that overlaps memory the OS uses is refused, so asking is also how a driver finds out it must not touch a range.
class IRegisterMapper {
 public:
  virtual bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) = 0;

 protected:
  constexpr IRegisterMapper() = default;
  ~IRegisterMapper() = default;
};

}  // namespace hardware
