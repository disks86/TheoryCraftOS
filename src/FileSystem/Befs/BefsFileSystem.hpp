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

// BeFS (the Be File System of BeOS and Haiku). Little-endian volumes (x86) with 1 to 8 KiB blocks are understood; the
// big-endian PowerPC variant is Unsupported. Names are case sensitive UTF-8, directories are read through their B+tree (listing and
// lookup), file data through the direct, indirect and double indirect block runs. Symbolic links are shown as small files that hold the
// link target (they are not followed). Attributes, indices and the journal are ignored.
//
// Writing: the data of existing regular files can be changed, grown and cut (open_file() with ReadWrite, write(), resize()); blocks come
// from the allocation bitmap. Creating, removing and renaming are not there yet (Unsupported on a writable volume). Metadata changes
// are collected in a transaction (a set of changed blocks) and written when the operation is done, or dropped if it failed; there is
// no journal, so a crash in the middle of the final write can leave the volume needing a check. A volume is mounted writable only if
// its device is, it was shut down cleanly (flag clean, journal empty) and its bitmap is laid out as expected; any other volume is
// read only (every write returns ReadOnly). Time stamps are not kept up to date (no clock here).
//
// The volume starts at block `first_block` of the device and mounting reads and validates its super block (at byte 512 of the volume).
// The device must outlive the filesystem.
//
//   UniquePointer<IFileSystem> fs;
//   if (BefsFileSystem::mount(allocator, device, partition_lba, &fs) == Status::Ok) ...
class BefsFileSystem final : public IFileSystem {
 public:
  static Status mount(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block,
                      UniquePointer<IFileSystem>* result);

  // Use mount(): a constructed filesystem is not usable until it has read the volume.
  BefsFileSystem(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block);
  ~BefsFileSystem();
  BefsFileSystem(const BefsFileSystem&) = delete;
  BefsFileSystem& operator=(const BefsFileSystem&) = delete;

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

  // A run of consecutive blocks: allocation group, first block within the group, length in blocks.
  struct Run {
    uint32_t group = 0;
    uint16_t start = 0, length = 0;
  };
  // An inode as read from disk. Immutable on a read-only volume, so handles keep a copy.
  struct Inode {
    uint64_t block = 0;
    uint32_t mode = 0, flags = 0;
    uint64_t size = 0;
    Run direct[12];
    Run indirect, double_indirect;
    uint64_t max_direct = 0, max_indirect = 0, max_double = 0;  // bytes mapped, each counted from the start of the file
    uint8_t link[144] = {};  // a short symbolic link's target, stored in the inode itself
    uint32_t link_size = 0;
    bool inline_link = false;
    bool is_directory() const { return (mode & 0xF000) == 0x4000; }
    bool is_file() const { return (mode & 0xF000) == 0x8000 || (mode & 0xF000) == 0xA000; }
    bool is_regular() const { return (mode & 0xF000) == 0x8000; }
  };
  // The header of a directory's B+tree.
  struct Tree {
    uint32_t node_size = 0;
    uint64_t root = 0;
  };
  // A B+tree node parsed in place: `data` is the node as read.
  struct Node {
    ConstByteView data;
    uint64_t right = 0, overflow = 0;
    uint32_t count = 0, keys_size = 0, ends_at = 0, values_at = 0;
    bool leaf() const { return overflow == ~uint64_t{0}; }
  };
  // State of one search through a file's block runs (see locate()).
  struct Seek {
    uint64_t want = 0, skipped = 0;
    uint32_t budget = 0;
    bool found = false, ended = false;
    uint64_t block = 0, left = 0;
  };

  // Where the next block run of a stream goes, and the last one there is (see find_tail()).
  struct Slot {
    uint8_t level = 0;      // 0: a direct run, 1: the indirect array, 2: a level-2 array of the double indirect one
    uint32_t index = 0;     // entry in that array (the direct run number for level 0)
    uint32_t top_index = 0; // level 2: the entry of the top array that names the level-2 array
    Run array;              // level 2: that level-2 array
  };
  struct Tail {
    Slot next, last;
    Run last_run;
    bool has_last = false, full = false;
    bool need_indirect = false, need_top = false, need_level2 = false;  // the arrays holding `next` do not exist yet
  };
  // A changed block that is not on the medium yet.
  struct TxBlock {
    uint64_t block = 0;
    ByteView data;
  };

  Status init();
  Status read_bytes(uint64_t offset, ByteView out);  // volume-relative byte offset; sees the open transaction's changes
  Status raw_read(uint64_t offset, ByteView out);    // the medium only
  Status raw_write(uint64_t offset, ConstByteView data);
  Status writable() const { return writable_ ? Status::Ok : Status::ReadOnly; }
  uint64_t max_file_size() const { return block_count_ << block_shift_; }

  // Transactions.
  void tx_begin();
  Status tx_write(uint64_t offset, ConstByteView data);
  Status tx_commit();
  void tx_rollback();
  bool tx_full() const { return tx_count_ >= tx_soft_limit(); }
  static uint32_t tx_soft_limit();
  Status tx_zero(uint64_t block, uint32_t count);
  Status checkpoint(const Inode& inode);  // commits a consistent state and starts a new transaction

  // The allocation bitmap.
  Status check_bitmap();
  Status load_bitmap(uint64_t bitmap_block);
  Status clear_run(uint32_t group, uint32_t from, uint32_t limit, uint32_t max, uint32_t* count);
  Status find_free(uint32_t group, uint32_t limit, uint32_t want, uint32_t min, uint32_t* start, uint32_t* length);
  Status set_bits(uint64_t first, uint32_t count, bool used);
  Status allocate_at(uint64_t first, uint32_t want, uint32_t* got);
  Status allocate_any(uint64_t hint, uint32_t want, uint32_t min, Run* run);
  Status free_run(const Run& run);
  uint32_t group_bits(uint32_t group) const;
  static void encode_run(const Run& run, uint8_t out[8]);

  // Writing streams.
  Status store_inode(const Inode& inode);
  Status read_entry(const Run& array, uint32_t index, Run* out);
  Status write_entry(const Run& array, uint32_t index, const Run& run);
  Status count_entries(const Run& array, uint32_t* count);
  Status find_tail(const Inode& inode, Tail* tail);
  Status set_slot(Inode* inode, const Slot& slot, const Run& run);
  static uint64_t mapped_blocks(const Inode& inode, uint32_t shift);
  void adjust_ranges(Inode* inode, uint8_t level, int64_t delta);
  Status grow_stream(Inode* inode, uint64_t blocks);
  Status shrink_stream(Inode* inode, uint64_t blocks);
  Status write_stream(const Inode& inode, uint64_t offset, ConstByteView data);
  Status zero_stream(const Inode& inode, uint64_t from, uint64_t to);
  bool run_to_block(const Run& run, uint64_t* block) const;
  Status read_inode(uint64_t block, Inode* inode);
  Status consider(Seek* seek, const Run& run);
  Status walk_array(Seek* seek, const Run& array, uint32_t depth);
  Status locate(const Inode& inode, uint64_t file_block, uint64_t* block, uint64_t* left);
  Status read_stream(const Inode& inode, uint64_t offset, ByteView out);
  Status open_tree(const Inode& directory, Tree* tree);
  Status read_node(const Inode& directory, const Tree& tree, uint64_t at, ByteView buffer, Node* node);
  static bool node_key(const Node& node, uint32_t index, ConstByteView* key);
  static bool node_value(const Node& node, uint32_t index, uint64_t* value);
  Status find(const Inode& directory, StringView name, uint64_t* inode_block);
  Status resolve(StringView path, Inode* inode, StringView* leaf);
  static void fill_entry(const Inode& inode, StringView name, DirectoryEntry* entry);

  allocators::IAllocator& allocator_;
  hardware::IStorageDevice& device_;
  const uint64_t first_block_;
  mutable kernel::RecursiveMutex mutex_;

  ByteView block_;    // one device block, cached
  uint64_t block_number_ = ~uint64_t{0};
  uint32_t device_block_size_ = 0;
  uint64_t volume_bytes_ = 0;
  ByteView scratch_;  // two filesystem blocks: the arrays of block runs being walked (one per level)
  ByteView bitmap_;   // one filesystem block: the bitmap block being looked at
  uint64_t bitmap_loaded_ = ~uint64_t{0};
  ByteView zeros_;    // one filesystem block of zeros
  ArrayView<TxBlock> tx_;
  uint32_t tx_count_ = 0;
  uint64_t tx_used_blocks_ = 0;  // used_blocks_ when the transaction began
  bool writable_ = false;
  uint32_t bitmap_per_group_ = 0;

  uint32_t block_size_ = 0, block_shift_ = 0;
  uint64_t block_count_ = 0, used_blocks_ = 0;
  uint32_t group_shift_ = 0, group_count_ = 0;
  Run root_run_;
  uint64_t root_block_ = 0;
  FixedString<32> label_;
};

}  // namespace filesystem
