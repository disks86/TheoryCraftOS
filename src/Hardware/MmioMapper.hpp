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
#include "Hardware/Mmio.hpp"
#include "Hardware/RandomAccessMemory/IRandomAccessMemory.hpp"
#include "Kernel/Paging.hpp"

namespace hardware {

enum class MmioKind : uint8_t {
  Registers,     // device registers: uncached
  FrameBuffer,   // video memory and the like: write-combining where the CPU can, else uncached
};

// Makes ranges of physical addresses that belong to devices usable: maps them 1:1 into the page tables, uncached (or write-combining), no-execute, even if
// they are above 4 GiB, and hands out MmioRegion windows. Never maps anything the firmware lists as memory: that would put RAM in a device's memory type.
//
// Mappings stay in the page tables when they are released (other pages of the same large mapping may be in use, and the firmware may have had the range
// mapped to begin with); release() only gives back the bookkeeping.
class MmioMapper {
 public:
  static constexpr uint32_t kMaxMappings = 128;
  MmioMapper(kernel::PageTables& tables, const IRandomAccessMemory& ram) : tables_(tables), ram_(ram) {}

  // Maps [physical, physical + length) with the memory type of `kind`, replacing the mapping that was there. The same range mapped again returns the same
  // window (and counts a use). False for an empty range, for memory, for a range that overlaps one mapped as another kind, when the table of mappings is
  // full, and when the page tables cannot be changed.
  bool map(uint64_t physical, uint64_t length, MmioKind kind, MmioRegion* region);
  // Makes sure the range is mapped, but keeps the attributes of whatever the firmware already mapped there (only holes are filled, uncached).
  bool ensure_mapped(uint64_t physical, uint64_t length, MmioRegion* region);
  // Gives back one use of a window from map() / ensure_mapped().
  void release(const MmioRegion& region);
  uint32_t mapping_count() const { return count_; }
  // Is the whole range already mapped 1:1 (by the firmware or by us)?
  bool is_mapped(uint64_t physical, uint64_t length) const { return tables_.is_mapped(physical, length); }

 private:
  struct Mapping {
    uint64_t start, end;   // page-aligned
    MmioKind kind;
    bool kept;             // from ensure_mapped(): the attributes are the firmware's
    uint32_t uses;
  };
  bool add(uint64_t physical, uint64_t length, MmioKind kind, bool kept, MmioRegion* region);
  kernel::PageTables& tables_;
  const IRandomAccessMemory& ram_;
  Mapping mappings_[kMaxMappings];
  uint32_t count_ = 0;
};

}  // namespace hardware
