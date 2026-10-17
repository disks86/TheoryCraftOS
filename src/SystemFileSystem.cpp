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

#include "SystemFileSystem.hpp"
#include "FileSystem/Fat/FatFileSystem.hpp"
#include "Hardware/StorageDevice/Partition/PartitionScanner.hpp"

using namespace hardware;
using namespace filesystem;

SystemFileSystem::SystemFileSystem(allocators::IAllocator& allocator, SystemStorageDevices& storage)
    : allocator_(allocator), ram_(allocator) {
  for (uint32_t d = 0; d < storage.count() && !fat_.get(); ++d) {
    IStorageDevice& device = *storage.at(d);
    List<PartitionInfo> partitions(allocator);
    PartitionScheme scheme = PartitionScheme::None;
    if (!scan_partitions(device, allocator, &partitions, &scheme)) continue;
    if (scheme == PartitionScheme::None) {
      try_mount(device, 0);
      continue;
    }
    for (uint64_t i = 0; i < partitions.count() && !fat_.get(); ++i) {
      const PartitionInfo& p = partitions[i];
      partition_ = UniquePointer<PartitionStorageDevice>::make(allocator, device, p.first_block, p.block_count, p.index);
      if (partition_.get() && !try_mount(*partition_, 0)) partition_.reset();
    }
  }
  if (!fat_.get()) populate_fallback();
}

bool SystemFileSystem::try_mount(IStorageDevice& device, uint64_t first_block) {
  return FatFileSystem::mount(allocator_, device, first_block, &fat_) == Status::Ok;
}

void SystemFileSystem::populate_fallback() {
  static const char kWelcome[] = "TheoryCraftOS found no FAT volume on this machine.\nThis folder lives in memory.\n";
  ram_.create_directory("/Documents");
  ram_.create_directory("/Programs");
  UniquePointer<IFile> file;
  uint64_t written = 0;
  if (ram_.open_file("/Welcome.txt", OpenMode::CreateTruncate, &file) == Status::Ok)
    file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(kWelcome), sizeof kWelcome - 1), &written);
}
