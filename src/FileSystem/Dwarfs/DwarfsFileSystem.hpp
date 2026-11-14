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
#include "FileSystem/Dwarfs/DwarfsMetadata.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// DwarFS (format 2.3 to 2.5), read only: the deduplicating compressed image format that uruntime AppImages (the "anylinux" builds of
// pkgforge-dev and others) carry instead of SquashFS. The image is a run of sections (data BLOCKs, the metadata schema, the metadata, an
// index, history), read from a file starting at byte `offset` of it.
//
// Understood: blocks stored plain or compressed with Zstandard, LZMA (xz), LZ4 and LZ4HC; packed and plain metadata (FSST-compressed
// names and link targets, packed directory, chunk and shared-file tables); deduplicated (shared) files; holes of sparse files; symbolic
// links (followed like SquashFS's, see LinkWalk.hpp; targets written with '\' on Windows are read with '/'); hard links (same file_id).
// Devices, FIFOs and sockets are listed as empty files of their SpecialNode. Brotli, FLAC and ricepp blocks give Unsupported when read;
// an image of format 2.2 or older, or with a feature this reader does not know, does not mount (Unsupported). Every write returns ReadOnly.
//
// Every section's XXH3-64 checksum is checked when it is read, and every index from the metadata before it is used: a damaged image gives
// Corrupt. Decompressed blocks (often 16 MiB each) are cached, a few of them; decompression happens outside the lock.
class DwarfsFileSystem final : public IFileSystem {
 public:
  // Reads the section list and the metadata. The filesystem owns `source` from then on.
  [[nodiscard]] static Status mount(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset, UniquePointer<DwarfsFileSystem>* result);
  // Is there a DwarFS section header (of a format version this reader may know) at the start of `head`?
  static bool looks_like_dwarfs(ConstByteView head);

  DwarfsFileSystem(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset);
  ~DwarfsFileSystem() = default;
  DwarfsFileSystem(const DwarfsFileSystem&) = delete;
  DwarfsFileSystem& operator=(const DwarfsFileSystem&) = delete;

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
  Status resolve_exit(StringView path, bool follow_last, String* exit) override;

  uint32_t block_count() const { return static_cast<uint32_t>(blocks_.count()); }
  uint32_t block_size() const { return meta_.block_size; }
  uint32_t inode_count() const { return static_cast<uint32_t>(meta_.inodes.count()); }

 private:
  class File;
  class Directory;
  struct WalkOps;

  // A section of the image: where its checksummed part (from the section number on) starts, its length, and how it is compressed.
  struct Section {
    uint64_t position = 0;      // of the section header, from the start of the image
    uint64_t length = 0;        // of the data after the 64-byte header
    uint16_t type = 0, compression = 0;
    uint64_t checksum = 0;      // XXH3-64 of everything from the section number on
  };
  // A node in the tree, as path resolution sees it.
  struct Node {
    uint32_t inode = 0;
  };
  struct CacheEntry {
    explicit CacheEntry(allocators::IAllocator& allocator) : data(allocator) {}
    uint32_t block = ~0u;
    uint64_t stamp = 0;
    ByteBuffer data;
  };
  enum class Kind : uint8_t { Directory, Link, File, Other };

  Status load();
  Status read_raw(uint64_t position, ByteView out) const;
  Status read_header(uint64_t position, Section* section) const;
  // The section's data, checksum checked, decompressed (to at most `max_output` bytes).
  Status section_data(const Section& section, uint64_t max_output, ByteBuffer* out) const;
  // Copies bytes [offset, offset + out.size()) of data block `block` into `out`.
  Status read_block(uint32_t block, uint64_t offset, ByteView out) const;

  Kind kind(uint32_t inode) const;
  // The chunks of a regular file: [*first, *last) in meta_.chunks.
  Status file_chunks(uint32_t inode, uint32_t* first, uint32_t* last) const;
  Status chunk_length(const dwarfs::Chunk& chunk, uint64_t* length) const;
  bool is_hole(const dwarfs::Chunk& chunk) const { return meta_.has_holes && chunk.block == meta_.hole_block; }
  Status file_size(uint32_t inode, uint64_t* size) const;
  Status lookup(const Node& directory, StringView name, Node* found) const;
  Status read_target(const Node& link, String* target) const;
  Status entry_name(uint32_t entry, ByteBuffer* out) const;
  Status resolve(StringView path, bool follow_last, Node* node, String* exit = nullptr) const;
  Status fill_entry(const Node& node, StringView name, StringView parent_path, DirectoryEntry* entry) const;

  allocators::IAllocator& allocator_;
  UniquePointer<IFile> source_;
  uint64_t offset_ = 0, image_size_ = 0;
  dwarfs::Metadata meta_;
  List<Section> blocks_;

  mutable kernel::Mutex cache_mutex_;
  // The cache changes on reads, which are const (a read does not change the file system).
  mutable uint64_t clock_ = 0;                                // (cache_mutex_)
  mutable List<UniquePointer<CacheEntry>> cache_;             // (cache_mutex_)
};

}  // namespace filesystem
