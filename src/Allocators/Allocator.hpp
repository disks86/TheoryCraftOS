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
#include "Allocators/IAllocator.hpp"
#include "Hardware/RandomAccessMemory/IRandomAccessMemory.hpp"

namespace allocators {

// Default allocator: memory pools that carve RAM regions into blocks of one
// cache line. Every allocation is a run of blocks preceded by a one-block
// header, so returned pointers are cache-line aligned. Free runs are kept
// address-ordered and coalesced on deallocate.
//
// There are two pools: the low one (below 4 GiB, where initialize() puts the
// RAM's main region) and the high one (add_region() puts everything at or above
// 4 GiB there). allocate() and allocate_aligned() take from the high pool first,
// so that the scarce low memory stays for what devices can only reach there;
// allocate_dma() takes from the low pool only.
class Allocator final : public IAllocator {
 public:
  static constexpr uint64_t kBlockSize = 64;  // x86_64 cache line

  explicit constexpr Allocator(hardware::IRandomAccessMemory& ram) : ram_(ram) {}

  static constexpr uint64_t kLowLimit = 1ull << 32;  // what DMA-capable memory must stay below

  // Claims the RAM's usable region. Returns false if it is too small.
  bool initialize();
  // Adds more memory (page-aligned, writable, mapped 1:1 and not used by anything else). A region that
  // crosses 4 GiB is split there. False if it is too small to be worth a pool entry.
  bool add_region(void* base, uint64_t bytes);

  void* allocate(uint64_t size) override;
  void* allocate_aligned(uint64_t size, uint64_t alignment) override;
  void* allocate_dma(uint64_t size, uint64_t alignment) override;
  void deallocate(void* ptr) override;

  uint64_t total_bytes() const { return total_bytes_; }  // everything handed to the pools (headers included)
  uint64_t free_bytes() const;       // payload bytes still available in both pools (sum over runs)
  uint64_t free_bytes_low() const;   // ... in the pool below 4 GiB
  uint64_t free_bytes_high() const;  // ... in the pool above

 private:
  struct Header;

  void* allocate_from(Header** list, uint64_t size);
  void* allocate_aligned_from(Header** list, uint64_t size, uint64_t alignment);
  static uint64_t free_bytes_of(const Header* list);
  void add_run(uint64_t start, uint64_t end);
  Header** list_for(const void* address) { return reinterpret_cast<uint64_t>(address) >= kLowLimit ? &free_high_ : &free_low_; }

  hardware::IRandomAccessMemory& ram_;
  Header* free_low_ = nullptr;
  Header* free_high_ = nullptr;
  uint64_t total_bytes_ = 0;
};

}  // namespace allocators
