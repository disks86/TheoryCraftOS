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
#include "Hardware/IHardware.hpp"

namespace hardware {

struct MemoryRegion {
  uint64_t base = 0;
  uint64_t bytes = 0;
};

// System memory as seen by the OS. Implementations live beside this file.
class IRandomAccessMemory : public IHardware {
 public:
  virtual uint64_t total_bytes() const = 0;   // all installed memory
  virtual uint64_t usable_bytes() const = 0;  // memory the OS may allocate
  // Start of one contiguous, writable region of usable_bytes() bytes: the main one, below 4 GiB when the machine has memory there
  // (it is what DMA buffers come from).
  virtual void* base() const = 0;

  // Further regions the OS may allocate from, mostly above 4 GiB. They are claimed (nobody else will use them) and may be handed to
  // the allocator for memory no device reads by itself. The platform's page tables must map them 1:1 (UEFI promises that for
  // everything in its memory map; kernel/Paging can check).
  virtual uint32_t extra_region_count() const { return 0; }
  virtual MemoryRegion extra_region(uint32_t index) const { (void)index; return MemoryRegion(); }
  // Does [address, address + bytes) overlap memory (as opposed to device registers)? What MMIO mappings must never cover.
  virtual bool overlaps_memory(uint64_t address, uint64_t bytes) const { (void)address; (void)bytes; return false; }

 protected:
  constexpr IRandomAccessMemory() = default;
  ~IRandomAccessMemory() = default;
};

}  // namespace hardware
