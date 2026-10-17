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

// The filesystem the desktop's file explorer browses: the first FAT volume found on the storage devices
// (a partition, or a whole device without a partition table), or, on a machine with none, a small
// filesystem in memory so the explorer still has something to show.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/StorageDevice/Partition/PartitionStorageDevice.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"

class SystemFileSystem {
 public:
  // The storage devices must outlive this object.
  SystemFileSystem(allocators::IAllocator& allocator, hardware::SystemStorageDevices& storage);
  SystemFileSystem(const SystemFileSystem&) = delete;
  SystemFileSystem& operator=(const SystemFileSystem&) = delete;

  // False when nothing could be mounted and the in-memory fallback is in use.
  bool from_storage() const { return fat_.get() != nullptr; }
  filesystem::IFileSystem& file_system() { return fat_.get() ? *fat_ : static_cast<filesystem::IFileSystem&>(ram_); }

 private:
  bool try_mount(hardware::IStorageDevice& device, uint64_t first_block);
  void populate_fallback();

  allocators::IAllocator& allocator_;
  filesystem::RamFileSystem ram_;
  UniquePointer<hardware::PartitionStorageDevice> partition_;  // before fat_: the volume reads through it
  UniquePointer<filesystem::IFileSystem> fat_;
};
