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
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"

namespace hardware {

// Fallback for storage controllers this OS has no driver for (USB mass storage,
// SCSI HBAs, SD/MMC, floppy, ...): borrows the firmware's driver through the
// UEFI Block I/O protocol. Works with anything the firmware can read and needs
// no hardware knowledge, but only while boot services are active.
class UefiBlockStorageDevice final : public StorageDeviceBase {
 public:
  UefiBlockStorageDevice(efi::BlockIo* io, allocators::IAllocator& allocator, efi::SystemTable* st)
      : StorageDeviceBase(allocator, st), io_(io) {}

  const char* name() const override { return "UEFI block device (firmware driver)"; }
  bool initialize() override;
  void shutdown() override { release_bounce(); }

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) override;
  bool flush_cache() override { return io_->flush_blocks(io_) == efi::kSuccess; }

 private:
  efi::BlockIo* io_;
};

}  // namespace hardware
