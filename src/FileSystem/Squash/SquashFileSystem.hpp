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
#include "FileSystem/Squash/SquashFormat.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// SquashFS 4.0, read only: the compressed image format of AppImages, snaps, live CDs and initramfs-style system images. The image is read
// from a file (or any IFile) starting at byte `offset` of it, which is how the image appended to an AppImage's runtime is reached.
//
// Understood: every compressor mksquashfs offers (gzip, LZMA, LZO, XZ with its branch filters, LZ4, Zstandard), basic and extended inodes,
// files with block lists, holes and a tail in a fragment block, directories of any size, symbolic links (followed for every path component,
// the last one too, with ".." allowed in their targets; an absolute target is taken from the image's root), hard links (same inode number).
// Devices, FIFOs and sockets are listed as empty files. Extended attributes and the export table are ignored. Every write returns ReadOnly.
//
// Decompressed metadata and data blocks are cached (a few of each); decompression happens outside the lock, so readers on several cores
// only wait on each other for the copy out of the cache. Every value read from the image is checked before use: a damaged image gives
// Corrupt, never a read outside what was decompressed.
//
//   UniquePointer<SquashFileSystem> fs;
//   if (SquashFileSystem::mount(allocator, util::move(file), payload_offset, &fs) == Status::Ok) ...
class SquashFileSystem final : public IFileSystem {
 public:
  // Reads and checks the superblock and the fragment table. The filesystem owns `source` from then on.
  [[nodiscard]] static Status mount(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset, UniquePointer<SquashFileSystem>* result);
  // Is there a SquashFS superblock at the start of `head`?
  static bool looks_like_squash(ConstByteView head);

  SquashFileSystem(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset);
  ~SquashFileSystem() = default;
  SquashFileSystem(const SquashFileSystem&) = delete;
  SquashFileSystem& operator=(const SquashFileSystem&) = delete;

  FileSystemInfo info() const override;
  Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) override;
  Status open_directory(StringView path, UniquePointer<IDirectory>* directory) override;
  Status stat(StringView path, DirectoryEntry* entry) override;
  Status create_directory(StringView path) override { (void)path; return Status::ReadOnly; }
  Status remove(StringView path) override { (void)path; return Status::ReadOnly; }
  Status rename(StringView from, StringView to) override { (void)from; (void)to; return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }
  Status set_metadata(StringView path, const MetadataChange& change) override { (void)path; (void)change; return Status::ReadOnly; }
  Status create_hard_link(StringView existing, StringView link) override { (void)existing; (void)link; return Status::ReadOnly; }
  Status create_symbolic_link(StringView link, StringView target, bool target_is_directory) override {
    (void)link; (void)target; (void)target_is_directory;
    return Status::ReadOnly;
  }
  Status read_symbolic_link(StringView path, String* target, bool* target_is_directory) override;

  squash::Compression compression() const { return compression_; }
  uint32_t block_size() const { return block_size_; }
  uint32_t inode_count() const { return inode_count_; }

 private:
  class File;
  class Directory;
  class MetaReader;

  enum class Kind : uint8_t { Directory, File, Symlink, Other };
  // An inode as far as this driver needs it.
  struct Inode {
    Kind kind = Kind::Other;
    uint64_t ref = 0;
    uint32_t number = 0, links = 1, mtime = 0;
    uint16_t mode = 0;
    // Directory: where its listing starts in the directory table and how long it is.
    uint32_t listing_block = 0, listing_size = 0;
    uint16_t listing_offset = 0;
    // File: its size, first data block, fragment, and where its block-size list continues in the inode table.
    uint64_t size = 0, blocks_start = 0;
    uint32_t fragment = squash::kNoFragment, fragment_offset = 0;
    uint64_t list_block = 0;
    uint32_t list_offset = 0;
    uint64_t block_count = 0;
    // Symlink: the length of its target (which follows in the inode table, at list_block/list_offset).
    uint32_t target_size = 0;
  };
  struct Fragment {
    uint64_t start = 0;
    uint32_t size = 0;      // size word: bytes on disk | kDataUncompressed
  };
  // A cached decompressed block (metadata or data), by its position in the image.
  struct CacheEntry {
    explicit CacheEntry(allocators::IAllocator& allocator) : data(allocator) {}
    uint64_t position = ~uint64_t{0};
    uint64_t stamp = 0;
    uint32_t disk_size = 0;      // metadata: bytes the block takes on disk, header included
    ByteBuffer data;
  };
  static constexpr uint32_t kMetaCacheEntries = 32, kDataCacheEntries = 8;
  static constexpr uint32_t kMaxLinkHops = 40;
  static constexpr uint32_t kMaxTarget = 4096;

  Status load();
  // Raw bytes of the image (positions are from the start of the image, not of the file holding it).
  Status read_raw(uint64_t position, ByteView out) const;
  Status decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output) const;
  // The decompressed metadata block at `position` (absolute in the image) copied into *out, and its size on disk (header included).
  Status metadata_block(uint64_t position, ByteBuffer* out, uint32_t* disk_size);
  // Copies `out.size()` bytes at `offset` of the decompressed data block at `position` with size word `size_word` (expected decompressed
  // size `expected`) into `out`. A hole (size word 0) reads as zeros.
  Status read_data_block(uint64_t position, uint32_t size_word, uint32_t expected, uint64_t offset, ByteView out);
  Status read_inode(uint64_t ref, Inode* inode);
  // Finds `name` in a directory; NotFound if it is not there.
  Status lookup(const Inode& directory, StringView name, Inode* found);
  Status read_target(const Inode& link, String* target);
  // Resolves an absolute path, following symbolic links (the last component too if `follow_last`).
  Status resolve(StringView path, bool follow_last, Inode* inode);
  Status fill_entry(const Inode& inode, StringView name, StringView parent_path, DirectoryEntry* entry);
  Status fragment(uint32_t index, Fragment* out) const;

  allocators::IAllocator& allocator_;
  UniquePointer<IFile> source_;
  uint64_t offset_ = 0;
  uint64_t bytes_used_ = 0;
  squash::Compression compression_ = squash::Compression::Gzip;
  uint32_t block_size_ = 0, inode_count_ = 0, fragment_count_ = 0, modified_ = 0;
  uint16_t flags_ = 0;
  uint64_t root_ref_ = 0, inode_table_ = 0, directory_table_ = 0;
  List<Fragment> fragments_;

  mutable kernel::Mutex cache_mutex_;
  uint64_t clock_ = 0;      // (cache_mutex_)
  List<UniquePointer<CacheEntry>> meta_cache_, data_cache_;      // (cache_mutex_)
};

}  // namespace filesystem
