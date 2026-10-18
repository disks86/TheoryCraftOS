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

// FAT12, FAT16 and FAT32 volumes - enough to read and write the EFI System Partition, removable media and data disks. Long file names
// are understood (shown as UTF-8) and created when a name does not fit 8.3 (with a "~N" alias); a file can be opened by its long name,
// not by its 8.3 alias. Names are case insensitive (ASCII). Writing goes straight to the device (no write-back cache here; flush() asks
// the device to commit its own). A volume on a read-only device, a read-only file and anything else that cannot be written return
// ReadOnly. Files are at most 4 GiB - 1. Time stamps are not kept up to date (no clock here): new entries get 1980-01-01.
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
  Status create_directory(StringView path) override;
  Status remove(StringView path) override;
  Status rename(StringView from, StringView to) override;
  Status flush() override;

 private:
  class File;
  class Directory;
  enum class Kind : uint8_t { Fat12, Fat16, Fat32 };

  // Where a directory listing is up to.
  struct Position {
    uint32_t first = 0;    // the directory's first cluster (0: the fixed root area)
    uint32_t cluster = 0;  // cluster holding entry `index`
    uint32_t index = 0;
    uint32_t hops = 0;     // clusters followed, to stop on a looping chain
    bool ended = false;
  };
  // A directory entry as found on disk.
  struct Node {
    DirectoryEntry entry;
    uint32_t cluster = 0;       // first cluster; for a directory 0 means the FAT12/16 root
    uint64_t short_offset = 0;  // volume-relative byte offset of the 8.3 entry (0 for the root)
    Position first_pos;         // where the entry's first slot (its long-name pieces, if any) is
    uint32_t slots = 0;         // 32-byte slots the entry takes, long-name pieces included (0 for the root)
  };
  // A remembered place in a cluster chain, so that sequential access does not walk the chain from its start every time.
  struct ChainCursor {
    uint32_t first = 0, cluster = 0;
    uint64_t index = 0, epoch = 0;
  };

  Status init();
  Status read_bytes(uint64_t offset, ByteView out);  // volume-relative byte offset
  Status write_bytes(uint64_t offset, ConstByteView data);
  Status get_fat(uint32_t cluster, uint32_t* value);
  Status set_fat(uint32_t cluster, uint32_t value);  // in every copy of the FAT
  Status next_cluster(uint32_t cluster, uint32_t* next, bool* end);
  uint64_t cluster_offset(uint32_t cluster) const { return (data_start_ + uint64_t{cluster - 2} * sectors_per_cluster_) * bytes_per_sector_; }
  uint32_t end_mark() const { return kind_ == Kind::Fat12 ? 0xFFF : kind_ == Kind::Fat16 ? 0xFFFF : 0x0FFFFFFF; }
  Position start_of(const Node& directory) const;
  uint64_t entry_offset(const Position& position) const;
  Status read_raw_entry(Position* position, uint8_t out[32]);
  Status next_node(Position* position, Node* node);
  Status find(const Node& directory, StringView name, Node* found);
  Status resolve(StringView path, Node* node);

  // Writing.
  Status writable() const { return read_only_ ? Status::ReadOnly : Status::Ok; }
  Status seek_cluster(uint32_t first, uint64_t index, ChainCursor* cursor);
  Status allocate_cluster(uint32_t* cluster, uint32_t link_from);  // zeroed, end of chain, linked after `link_from` (0: not linked)
  Status zero_cluster(uint32_t cluster);
  Status free_chain(uint32_t first);
  Status truncate_chain(uint32_t first, uint64_t keep);  // keeps the first `keep` (>= 1) clusters
  Status resize_chain(uint32_t* first, uint32_t old_size, uint32_t new_size);
  Status write_chain(uint32_t first, uint64_t offset, const uint8_t* data, uint64_t length, ChainCursor* cursor);  // null data: zeros
  Status update_entry(uint64_t short_offset, uint32_t first, uint32_t size);
  uint32_t free_cluster_count();
  Status find_slots(const Node& directory, uint32_t needed, uint64_t* offsets);
  bool alias_taken(const Node& directory, const uint8_t name11[11], uint64_t ignore_offset);
  Status make_entry(const Node& parent, StringView name, uint8_t attributes, uint32_t cluster, uint32_t size, uint64_t ignore_offset, Node* created);
  Status delete_slots(const Node& node);
  Status split(StringView path, Node* parent, StringView* leaf);
  bool is_open(uint64_t short_offset) const;

  allocators::IAllocator& allocator_;
  hardware::IStorageDevice& device_;
  const uint64_t first_block_;
  mutable kernel::RecursiveMutex mutex_;
  File* open_files_ = nullptr;   // the open file handles, newest first (see File)
  uint64_t epoch_ = 0;           // changes whenever a cluster chain loses clusters, which makes ChainCursors start over
  bool read_only_ = false;
  uint32_t fat_count_ = 0, fat_sectors_ = 0;
  uint32_t alloc_hint_ = 2;      // where the search for a free cluster starts
  uint32_t free_clusters_ = 0;
  bool free_known_ = false;

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
