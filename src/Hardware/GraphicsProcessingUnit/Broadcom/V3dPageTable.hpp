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
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dRegisters.hpp"
#include "Hardware/Mmio.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The V3D's own page table: a flat array of 32-bit entries, one per 4 KiB page of the GPU's address space (the table for all 4 GiB is 4 MiB), each holding the
// page number of a bus address plus valid and writeable flags. The table lives in memory the GPU can read (physically contiguous, 4 KiB aligned);
// this class only fills it in, with every index checked.
class V3dPageTable {
 public:
  explicit V3dPageTable(ArrayView<uint32_t> entries) : entries_(entries) {}
  static constexpr uint64_t kPageSize = uint64_t{1} << v3d::kMmuPageShift;

  ArrayView<uint32_t> entries() const { return entries_; }
  // Maps `pages` pages from GPU address `iova` to the bus addresses `bus_address`, `bus_address + 4 KiB`, .... Both must be page aligned; false if they do not fit or
  // a bus address would not fit the 28-bit page number.
  bool map(uint64_t iova, uint64_t bus_address, uint64_t pages, bool writeable);
  bool unmap(uint64_t iova, uint64_t pages);
  // Is this page mapped (and writeable)?
  bool is_mapped(uint64_t iova, bool* writeable = nullptr) const;
  // Marks every page unmapped.
  void clear();

 private:
  ArrayView<uint32_t> entries_;
};

}  // namespace hardware
