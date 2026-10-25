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
#include "FileSystem/Ext/ExtFormat.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// The Linux filesystems ext2, ext3 and ext4 (what mke2fs makes), all through this one class.
//
// ext2: block groups with bitmaps and inode tables, files mapped by the classic 12 direct / indirect / double / triple indirect block
// numbers, directories as linear lists of entries (the hash tree of a directory made by ext3/ext4 is read as the linear list it also
// is, and flattened into one before the first change to that directory). Revision 0 and 1 volumes, any block size from 1 KiB to 64 KiB,
// inodes of 128 bytes and more, sparse superblock backups, large files, the filetype and extended attribute features.
// Regular files and directories are read and written; a symbolic link reads as a file holding its target; devices, sockets and pipes
// are not listed. Names are the bytes stored (UTF-8 in practice) and differ by case. Owners, permissions, links to other owners and
// extended attributes are left as they are; only the immutable flag makes a file ReadOnly.
//
// ext3: the journal is a hidden file. A volume that was not unmounted cleanly (the needs_recovery feature) has its journal replayed when
// it is mounted on a device that can be written, as e2fsck or the kernel would do: committed transactions are copied to their places,
// revoked blocks are skipped, an unfinished transaction is dropped, and the journal is left empty. A journal that cannot be replayed
// here (one on another device, or using fast commits) leaves the volume read only. Changes made afterwards are not journaled.
//
// ext4: extent trees (any depth; a file is read and written through them, holes and unwritten extents read as zeros, and a write into an
// unwritten extent is Unsupported), 64-bit block numbers and group descriptors, flex_bg, uninitialized block groups, huge files, extra inode
// fields, and the checksums: metadata_csum (crc32c over the superblock, group descriptors, bitmaps, inodes, extent blocks and directory
// blocks, kept up to date for everything that is written) or the older uninit_bg group descriptor crc16. Not supported: meta_bg, inline
// data, encryption, case folding, bigalloc (read only), quotas and the other features that need more bookkeeping than this does (read
// only), extended attributes (kept as they are, a block that holds them is released with its last file).
//
// Time stamps come from the system wall clock (set_unix_clock): a new inode gets the time as its creation, change, modification and access
// time; writing or cutting a file sets its modification and change times; adding, removing or renaming entries does the same to the
// directories concerned (and linking or renaming to the inode's change time). Reading does not touch the access time. While no clock is
// known stamps are left alone and new inodes get 1980-01-01. DirectoryEntry::modified and ::created report what is stored.
//
// Writing: files and directories can be created, written, grown, cut, renamed (into other directories too) and removed; blocks and
// inodes come from the bitmaps of the block groups, near their directory. Nothing is journaled: a volume is mounted writable only if its
// device is, it was unmounted cleanly (state "valid", no orphan list, nothing left to replay) and it uses only features written here;
// any other volume is read only (writes return ReadOnly). The "valid" flag is cleared before the first change and set again by flush() (and when the filesystem is
// destroyed), so a crash in between leaves the volume for e2fsck.
//
// The volume starts at block `first_block` of the device (0 for a bare filesystem, the partition's first block otherwise). The device
// must outlive the filesystem.
//
//   UniquePointer<IFileSystem> fs;
//   if (ExtFileSystem::mount(allocator, device, partition_lba, &fs) == Status::Ok) ...
class ExtFileSystem final : public IFileSystem {
 public:
  static Status mount(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block,
                      UniquePointer<IFileSystem>* result);

  // Use mount(): a constructed filesystem is not usable until it has read the volume.
  ExtFileSystem(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block);
  ~ExtFileSystem();
  ExtFileSystem(const ExtFileSystem&) = delete;
  ExtFileSystem& operator=(const ExtFileSystem&) = delete;

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

  // An inode as stored (inode_size bytes, never more than kMaxInodeSize).
  struct Inode {
    uint32_t number = 0;
    uint32_t length = 0;
    uint8_t raw[ext::kMaxInodeSize] = {};
    ByteView bytes() { return ByteView(raw, length); }
    ConstByteView bytes() const { return ConstByteView(raw, length); }
    uint16_t mode() const { return ext::rd16(bytes(), ext::kInMode); }
    uint32_t flags() const { return ext::rd32(bytes(), ext::kInFlags); }
    uint16_t links() const { return ext::rd16(bytes(), ext::kInLinks); }
    bool is_directory() const { return (mode() & ext::kModeTypeMask) == ext::kModeDirectory; }
    bool is_regular() const { return (mode() & ext::kModeTypeMask) == ext::kModeRegular; }
    bool is_symlink() const { return (mode() & ext::kModeTypeMask) == ext::kModeLink; }
    uint64_t size() const {
      const uint64_t low = ext::rd32(bytes(), ext::kInSizeLo);
      return is_regular() || is_directory() ? low | (uint64_t{ext::rd32(bytes(), ext::kInSizeHi)} << 32) : low;
    }
    void set_size(uint64_t size) {
      ext::wr32(bytes(), ext::kInSizeLo, static_cast<uint32_t>(size));
      if (is_regular() || is_directory()) ext::wr32(bytes(), ext::kInSizeHi, static_cast<uint32_t>(size >> 32));
    }
    uint32_t block_pointer(uint32_t index) const { return ext::rd32(bytes(), ext::kInBlock + 4 * index); }
    void set_block_pointer(uint32_t index, uint32_t block) { ext::wr32(bytes(), ext::kInBlock + 4 * index, block); }
  };

  // Where one block of a file is: block 0 is a hole. `fresh` says it was allocated by this very call (so it holds no data yet).
  struct Mapping {
    uint64_t block = 0;
    bool fresh = false;
  };
  static constexpr uint32_t kChunk = 256;  // blocks mapped and written at a time

  // An extent of a file: `length` blocks from file block `block` stored from disk block `start` (an unwritten one reads as zeros).
  struct Extent {
    uint64_t block = 0, start = 0;
    uint32_t length = 0;
    bool unwritten = false;
  };
  using ExtentList = List<Extent>;
  using BlockList = List<uint64_t>;

  // A name in a directory block.
  struct DirEntry {
    uint32_t inode = 0;
    uint32_t offset = 0, record_length = 0, name_length = 0;
    uint8_t type = 0;
  };
  // A path resolved to an inode.
  struct Resolved {
    uint32_t number = 0;
    uint32_t parent = 0;  // the directory it was found in (the root: itself)
    bool directory = false;
  };
  // Where a directory listing is up to.
  struct Cursor {
    uint64_t block = 0;
    uint32_t offset = 0;
  };

  // Mounting and device access.
  Status init();
  Status load_volume();
  Status recover();
  Status replay_journal();
  void decide_writable();
  Status read_bytes(uint64_t offset, ByteView out);  // volume-relative byte offset
  Status write_bytes(uint64_t offset, ConstByteView data);
  Status read_block(uint64_t block, ByteView out);  // one whole block
  Status write_block(uint64_t block, ConstByteView data);
  Status read_word(uint64_t block, uint64_t index, uint32_t* value);  // the index'th 32-bit number of a block
  Status write_word(uint64_t block, uint64_t index, uint32_t value);
  Status begin_write();
  Status finish_write();
  Status write_super();

  // Superblock and group descriptors.
  uint32_t sb32(uint32_t offset) const { return ext::rd32(sb_.view(), offset); }
  uint16_t sb16(uint32_t offset) const { return ext::rd16(sb_.view(), offset); }
  uint64_t group_first_block(uint32_t group) const { return first_data_block_ + uint64_t{group} * blocks_per_group_; }
  uint32_t group_block_count(uint32_t group) const;
  bool group_has_super(uint32_t group) const;
  uint64_t gd_get(uint32_t group, uint32_t low, uint32_t high, bool wide16) const;
  void gd_set(uint32_t group, uint32_t low, uint32_t high, bool wide16, uint64_t value);
  uint64_t gd_block_bitmap(uint32_t group) const { return gd_get(group, ext::kGdBlockBitmap, ext::kGdBlockBitmapHi, false); }
  uint64_t gd_inode_bitmap(uint32_t group) const { return gd_get(group, ext::kGdInodeBitmap, ext::kGdInodeBitmapHi, false); }
  uint64_t gd_inode_table(uint32_t group) const { return gd_get(group, ext::kGdInodeTable, ext::kGdInodeTableHi, false); }
  uint64_t gd_free_blocks(uint32_t group) const { return gd_get(group, ext::kGdFreeBlocks, ext::kGdFreeBlocksHi, true); }
  uint64_t gd_free_inodes(uint32_t group) const { return gd_get(group, ext::kGdFreeInodes, ext::kGdFreeInodesHi, true); }
  uint64_t gd_used_dirs(uint32_t group) const { return gd_get(group, ext::kGdUsedDirs, ext::kGdUsedDirsHi, true); }
  uint32_t gd_flags(uint32_t group) const { return ext::rd16(gd_.view().subview(uint64_t{group} * descriptor_size_, descriptor_size_), ext::kGdFlags); }
  void gd_set_flags(uint32_t group, uint32_t flags);
  Status store_group(uint32_t group);
  void group_checksum_set(uint32_t group);
  void bitmap_checksum_set(uint32_t group, bool inode_bitmap, ConstByteView bitmap);
  void synth_block_bitmap(uint32_t group, ByteView bitmap) const;

  // Inodes.
  Status inode_offset(uint32_t number, uint64_t* offset);
  Status read_inode(uint32_t number, Inode* inode);
  Status write_inode(const Inode& inode);
  // Time stamps: seconds since 1970 in the 32-bit field plus, in the extra part of a large inode, two more bits (the years after 2038).
  uint64_t inode_time(const Inode& inode, uint32_t field, uint32_t extra_field) const;
  void set_inode_time(Inode* inode, uint32_t field, uint32_t extra_field, uint64_t seconds) const;
  void touch(Inode* inode, bool contents) const;  // ctime (and mtime if the contents changed) become now; nothing without a clock
  uint32_t inode_seed(const Inode& inode) const;  // the checksum seed of one inode: its number and generation folded into the volume's
  void inode_checksum_set(Inode* inode) const;
  void extent_checksum_set(const Inode& inode, ByteView node) const;
  bool inode_immutable(const Inode& inode) const { return (inode.flags() & (ext::kFlagImmutable)) != 0; }
  uint64_t inode_blocks(const Inode& inode) const;
  void inode_add_blocks(Inode* inode, int64_t delta);  // delta in filesystem blocks
  bool is_fast_symlink(const Inode& inode) const;  // the target is in i_block, not in a block

  // Mapping file blocks to disk blocks.
  bool uses_extents(const Inode& inode) const { return (inode.flags() & ext::kFlagExtents) != 0; }
  Status map_run(const Inode& inode, uint64_t first, uint64_t max, uint64_t* block, uint64_t* run);
  // Maps `count` blocks from `first`; with `create` the holes are allocated. *done is how many leading blocks were mapped (all of them when Ok).
  Status map_range(Inode* inode, uint64_t first, uint32_t count, bool create, ArrayView<Mapping> out, uint32_t* done);
  Status free_from(Inode* inode, uint64_t first_block);  // frees every block of the file from this file block on
  Status bm_get(const Inode& inode, uint64_t index, uint64_t* block);
  Status bm_alloc(Inode* inode, uint64_t index, uint64_t goal, Mapping* result);
  Status bm_free_node(Inode* inode, uint64_t block, uint32_t level, uint64_t base, uint64_t keep, bool* emptied);
  Status bm_free_from(Inode* inode, uint64_t keep);
  Status check_pointer(uint64_t block) const;
  Status queue_free(Inode* inode, uint64_t block);
  Status flush_frees();

  // Extent trees (ExtExtent.cpp).
  uint32_t ext_node_capacity() const { return (block_size_ - ext::kExtentHeader) / ext::kExtentEntry; }
  static uint64_t ext_nodes_needed(uint64_t extents, uint32_t capacity, uint32_t* depth);
  void ext_init_root(Inode* inode) const;
  Status ext_lookup(const Inode& inode, uint64_t block, Extent* found, bool* mapped, uint64_t* hole_end);
  Status ext_map_run(const Inode& inode, uint64_t first, uint64_t max, uint64_t* block, uint64_t* run);
  Status ext_map_range(Inode* inode, uint64_t first, uint32_t count, bool create, ArrayView<Mapping> out, uint32_t* done);
  Status ext_load(const Inode& inode, ExtentList* extents, BlockList* nodes);
  Status ext_load_node(const Inode& inode, ConstByteView node, uint32_t expected_depth, ExtentList* extents, BlockList* nodes);
  Status ext_store(Inode* inode, const ExtentList& extents, BlockList* nodes);
  Status ext_free_from(Inode* inode, uint64_t keep);

  // Reading and writing the data of an inode.
  Status read_data(const Inode& inode, uint64_t offset, ByteView out, uint64_t* transferred);
  Status write_file_data(Inode* inode, uint64_t offset, ConstByteView data, uint64_t* transferred);
  Status resize_inode(Inode* inode, uint64_t new_size);
  Status zero_tail(Inode* inode, uint64_t size);  // zeroes what follows `size` in its (mapped) last block
  Status check_size(const Inode& inode, uint64_t size) const;
  void note_size(const Inode& inode);  // sets the large_file feature once a file passes 2 GiB

  // Blocks and inodes.
  Status load_block_bitmap(uint32_t group, ByteBuffer* bitmap);
  Status load_inode_bitmap(uint32_t group, ByteBuffer* bitmap);
  Status store_inode_bitmap(uint32_t group, ByteBuffer& bitmap);
  Status store_block_bitmap(uint32_t group, ByteBuffer& bitmap);
  Status alloc_one(const Inode& inode, uint64_t goal, uint64_t* block);
  Status alloc_blocks(uint32_t goal_group, uint64_t goal_block, uint64_t want, uint64_t* start, uint64_t* got);
  Status free_blocks(uint64_t start, uint64_t count);
  Status alloc_inode(bool directory, uint32_t parent, uint32_t* number);
  Status free_inode(uint32_t number, bool directory);
  Status release_xattr_block(uint64_t block);

  // Directories.
  Status dir_parse(ConstByteView block, uint32_t offset, DirEntry* entry) const;
  Status dir_next(const Inode& dir, Cursor* cursor, DirEntry* entry, FixedString<kMaxNameLength + 1>* name);
  Status dir_lookup(const Inode& dir, StringView name, uint32_t* number, uint8_t* type);
  Status dir_find(const Inode& dir, StringView name, uint64_t* block_index, DirEntry* entry);
  Status dir_empty(const Inode& dir, bool* empty);
  Status dir_add(Inode* dir, StringView name, uint32_t number, uint8_t type);
  Status dir_remove(const Inode& dir, StringView name);
  Status dir_set_parent(const Inode& dir, uint32_t parent);
  Status dir_parent(const Inode& dir, uint32_t* parent);
  Status dir_flatten(Inode* dir);
  uint32_t dir_limit() const { return block_size_ - (metadata_csum_ ? 12 : 0); }  // where the entries of a directory block end (a checksum follows)
  void dir_tail_set(const Inode& dir, ByteView block) const;
  Status store_dir_block(const Inode& dir, uint64_t disk, ByteBuffer& block);
  void dir_init_block(ByteView block, uint32_t self, uint32_t parent);
  void put_entry(ByteView block, uint32_t offset, uint32_t number, uint32_t record_length, StringView name, uint8_t type);
  Status zero_block(uint64_t block);
  Status new_indirect(Inode* inode, uint64_t goal, uint64_t* block);
  Status resolve(StringView path, Resolved* result);
  Status split(StringView path, Resolved* parent, StringView* leaf);
  Status describe(const Inode& inode, StringView name, DirectoryEntry* entry);
  bool is_open(uint32_t number) const;

  // Namespace.
  Status create_node(StringView path, bool directory, uint32_t* number);
  Status init_inode(Inode* inode, uint16_t mode, uint32_t links);
  Status delete_inode(Inode* inode);
  uint8_t entry_type(uint16_t mode) const;

  allocators::IAllocator& allocator_;
  hardware::IStorageDevice& device_;
  const uint64_t first_block_;
  mutable kernel::RecursiveMutex mutex_;
  struct OpenHandle {
    uint32_t number = 0;
    OpenHandle* next = nullptr;
  };
  OpenHandle* open_ = nullptr;

  ByteView block_;  // one device block, cached
  uint64_t block_number_ = ~uint64_t{0};
  uint32_t device_block_size_ = 0;

  ByteBuffer sb_;  // the superblock as read, with the counts and state kept up to date
  ByteBuffer gd_;  // the group descriptor table
  bool writable_ = false, dirty_set_ = false;
  uint32_t block_size_ = 0, inode_size_ = 0, first_inode_ = 0, descriptor_size_ = 0;
  uint32_t compat_ = 0, incompat_ = 0, ro_compat_ = 0;
  bool metadata_csum_ = false, group_csum_ = false;  // metadata_csum; metadata_csum or uninit_bg (checksummed group descriptors, uninitialized groups)
  uint32_t csum_seed_ = 0;
  uint64_t blocks_count_ = 0, first_data_block_ = 0, gdt_start_ = 0;
  uint32_t blocks_per_group_ = 0, inodes_per_group_ = 0, inodes_count_ = 0, group_count_ = 0, itable_blocks_ = 0;
  uint64_t free_blocks_ = 0, free_inodes_ = 0;
  FixedString<32> label_;

  // Blocks freed by the operation in progress, merged into runs.
  uint64_t pending_start_ = 0, pending_count_ = 0;
};

}  // namespace filesystem
