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
#include "Hardware/RandomAccessMemory/IRandomAccessMemory.hpp"
#include "Hardware/RegisterMapper.hpp"

namespace hardware {

// For platforms where the firmware already maps every device (UEFI on AArch64 leaves the peripherals mapped as device memory, 1:1): "mapping" is the checking that
// the range is not memory the OS owns, and handing the physical address back as the CPU address.
class IdentityRegisterMapper final : public IRegisterMapper {
 public:
  explicit IdentityRegisterMapper(const IRandomAccessMemory& ram) : ram_(ram) {}
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    if (length == 0 || length > UINT64_MAX - physical || ram_.overlaps_memory(physical, length)) return false;
    *region = MmioRegion(physical, length);
    return true;
  }

 private:
  const IRandomAccessMemory& ram_;
};

}  // namespace hardware
