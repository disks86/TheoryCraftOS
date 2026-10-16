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
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// FAT12, FAT16 and FAT32 volumes, read only - enough to read the EFI System
// Partition and removable media. Long file names are understood (and shown as
// UTF-8); the 8.3 alias of a file with a long name cannot be used to open it.
// Names are case insensitive (ASCII). Anything that would write returns ReadOnly.
//
// The volume starts at block `first_block` of the device (0 for a bare
// filesystem, the partition's first block otherwise) and mounting reads and
// validates its boot sector. The device must outlive the filesystem.
//
//   UniquePointer<IFileSystem> fs;
//   if (FatFileSystem::mount(allocator, device, partition_lba, &fs) == Status::Ok) ...
class FatFileSystem final : public IFileSystem {
 public:
  static Status mount(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block,
                      UniquePointer<IFileSystem>* result);

  // Use mount(): a constructed filesystem is not usable until it has read the volume.
  FatFileSystem(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block);
  ~FatFileSystem();
  FatFileSystem(const FatFileSystem&) = delete;
  FatFileSystem& operator=(const FatFileSystem&) = delete;

  FileSystemInfo info() const override;
  Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) override;
  Status open_directory(StringView path, UniquePointer<IDirectory>* directory) override;
  Status stat(StringView path, DirectoryEntry* entry) override;
  Status create_directory(StringView) override { return Status::ReadOnly; }
  Status remove(StringView) override { return Status::ReadOnly; }
  Status rename(StringView, StringView) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }

 private:
  class File;
  class Directory;
  enum class Kind : uint8_t { Fat12, Fat16, Fat32 };

  // A directory entry as found on disk.
  struct Node {
    DirectoryEntry entry;
    uint32_t cluster = 0;  // first cluster; for a directory 0 means the FAT12/16 root
  };
  // Where a directory listing is up to.
  struct Position {
    uint32_t first = 0;    // the directory's first cluster (0: the fixed root area)
    uint32_t cluster = 0;  // cluster holding entry `index`
    uint32_t index = 0;
    uint32_t hops = 0;     // clusters followed, to stop on a looping chain
    bool ended = false;
  };

  Status init();
  Status read_bytes(uint64_t offset, ByteView out);  // volume-relative byte offset
  Status next_cluster(uint32_t cluster, uint32_t* next, bool* end);
  uint64_t cluster_offset(uint32_t cluster) const { return (data_start_ + uint64_t{cluster - 2} * sectors_per_cluster_) * bytes_per_sector_; }
  Position start_of(const Node& directory) const;
  Status read_raw_entry(Position* position, uint8_t out[32]);
  Status next_node(Position* position, Node* node);
  Status find(const Node& directory, StringView name, Node* found);
  Status resolve(StringView path, Node* node);

  allocators::IAllocator& allocator_;
  hardware::IStorageDevice& device_;
  const uint64_t first_block_;
  mutable kernel::RecursiveMutex mutex_;

  ByteView block_;           // one device block, cached
  uint64_t block_number_ = ~uint64_t{0};
  uint32_t block_size_ = 0;

  Kind kind_ = Kind::Fat16;
  uint32_t bytes_per_sector_ = 0;
  uint32_t sectors_per_cluster_ = 0;
  uint32_t cluster_bytes_ = 0;
  uint32_t fat_start_ = 0;       // sectors
  uint32_t root_dir_start_ = 0;  // sectors (FAT12/16)
  uint32_t root_entries_ = 0;
  uint32_t root_cluster_ = 0;    // FAT32
  uint32_t data_start_ = 0;      // sectors
  uint32_t cluster_count_ = 0;
  FixedString<32> label_;
};

}  // namespace filesystem
