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
#include "uefi.hpp"

namespace hardware {

// System memory discovered through UEFI boot services. initialize() reads the
// firmware memory map for the total/usable figures and reserves one contiguous
// pool for the OS allocator: the whole of the largest free region below 4 GiB (the device drivers
// hand this memory to hardware for DMA, and some controllers - RTL8139, OHCI, UHCI - only reach 32-bit
// addresses), or of the largest free region anywhere if there is none down there - less a reserve at
// its end (kFirmwareReserve) that the firmware keeps for its own buffers. The largest free regions above 4 GiB are claimed
// as well (all but a small reserve at the end of each) and reported as extra regions, for memory that devices do not touch.
class UefiRandomAccessMemory final : public IRandomAccessMemory {
 public:
  explicit constexpr UefiRandomAccessMemory(efi::SystemTable* st) : st_(st) {}

  const char* name() const override { return "UEFI RAM"; }
  bool initialize() override;
  void shutdown() override;

  uint64_t total_bytes() const override { return total_bytes_; }
  uint64_t usable_bytes() const override { return pool_bytes_; }
  void* base() const override { return reinterpret_cast<void*>(pool_base_); }
  uint32_t extra_region_count() const override { return extra_count_; }
  MemoryRegion extra_region(uint32_t index) const override { return index < extra_count_ ? extra_[index] : MemoryRegion(); }
  bool overlaps_memory(uint64_t address, uint64_t bytes) const override;

  // Free (conventional) memory the firmware reported, claimed or not.
  uint64_t free_bytes() const { return free_bytes_; }

 private:
  static constexpr uint64_t kPageSize = 4096;
  static constexpr uint64_t kFirmwareReserve = 128ull * 1024 * 1024;
  static constexpr uint32_t kMaxExtra = 16, kMaxRamRanges = 96;
  static constexpr uint64_t kMinExtraPages = 256;                 // regions under 1 MiB are not worth a pool
  static constexpr uint64_t kHighReservePages = 4096;             // at most 16 MiB at the end of each high region stays with the firmware

  efi::SystemTable* st_;
  uint64_t total_bytes_ = 0;
  uint64_t free_bytes_ = 0;
  uint64_t pool_base_ = 0;
  uint64_t pool_bytes_ = 0;
  MemoryRegion extra_[kMaxExtra];
  uint32_t extra_count_ = 0;
  MemoryRegion ram_ranges_[kMaxRamRanges];  // everything the firmware lists as memory (any type that is RAM), merged where adjacent
  uint32_t ram_range_count_ = 0;
};

}  // namespace hardware
