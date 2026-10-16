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
#include "hardware/StorageDevice/IStorageDevice.hpp"
#include "uefi.hpp"

namespace hardware {

// Common part of the storage drivers: argument checking, statistics and
// splitting big requests into chunks that go through one DMA-able bounce buffer
// (page aligned, so every controller can use it as is). A driver fills in info_
// during initialize(), calls prepare_bounce(), and implements transfer().
class StorageDeviceBase : public IStorageDevice {
 public:
  static constexpr uint32_t kBounceBytes = 64 * 1024;

  StorageInfo info() const override { return info_; }
  StorageStatistics statistics() const override { return stats_; }
  bool read(uint64_t lba, uint32_t count, void* buffer) override;
  bool write(uint64_t lba, uint32_t count, const void* buffer) override;
  bool flush() override;

 protected:
  StorageDeviceBase(allocators::IAllocator& allocator, efi::SystemTable* st) : allocator_(allocator), st_(st) {}
  ~StorageDeviceBase() { release_bounce(); }

  // Moves `blocks` (at most max_blocks()) between the medium and `dma`.
  virtual bool transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) = 0;
  virtual bool flush_cache() { return true; }

  bool prepare_bounce();  // needs info_.block_size
  void release_bounce();
  uint32_t max_blocks() const { return bounce_bytes_ / info_.block_size; }

  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  uint64_t bounce_alignment_ = 4096;  // a driver can ask for more before prepare_bounce()
  StorageInfo info_;
  StorageStatistics stats_;

 private:
  bool run(bool write, uint64_t lba, uint32_t count, uint8_t* buffer);

  uint8_t* bounce_ = nullptr;
  uint32_t bounce_bytes_ = 0;
};

}  // namespace hardware
