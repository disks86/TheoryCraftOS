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
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// ISO 9660 (ECMA-119), read only: CD images and the image of a type 1 AppImage (whose runtime ELF sits in the 32 KiB system area the standard
// leaves to the creator). The image is read from a file starting at byte `offset` of it.
//
// Of the three trees an image may carry, the most capable one is used:
//   - Rock Ridge (SUSP: SP, CE, PX, NM, SL, TF, CL, RE, ZF) on the primary volume: POSIX names, modes, symbolic links (followed as in
//     LinkWalk.hpp), deep folders that mkisofs relocated (CL/RE), and files compressed with zisofs (zlib blocks, "pz");
//   - otherwise Joliet (UCS-2 names, turned into UTF-8);
//   - otherwise the plain ISO names, without their ";1" version and shown in lower case, as Linux shows them.
// Files of several extents (over 4 GiB) are read as one. Interleaved files, zisofs2 and sparse files (SF) read as IoError. Every write is ReadOnly.
//
// Nothing is cached except one decompressed zisofs block per open file; the calls keep no shared state, so any number of threads can use it at once.
class IsoFileSystem final : public IFileSystem {
 public:
  [[nodiscard]] static Status mount(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset, UniquePointer<IsoFileSystem>* result);
  // Is there an ISO 9660 volume descriptor at byte 32768 of `file` (from `offset`)?
  static bool looks_like_iso(IFile& file, uint64_t offset);

  IsoFileSystem(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset);
  ~IsoFileSystem() = default;
  IsoFileSystem(const IsoFileSystem&) = delete;
  IsoFileSystem& operator=(const IsoFileSystem&) = delete;

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

  enum class Tree : uint8_t { RockRidge, Joliet, Plain };
  Tree tree() const { return tree_; }

 private:
  class File;
  class Directory;
  struct WalkOps;

  enum class Kind : uint8_t { Directory, File, Symlink };
  // A directory record as far as this driver needs it (a plain value: the walk keeps a stack of them).
  struct Node {
    Kind kind = Kind::File;
    uint64_t extent = 0;          // first logical block of the data
    uint64_t size = 0;            // bytes of data (a directory: of its records); for a zisofs file, the uncompressed size
    uint64_t record = 0;          // byte position of its record in the image (links and multi-extent files read it again)
    uint64_t directory_end = 0;   // where the folder that holds the record ends (multi-extent files)
    uint64_t mtime = 0, serial = 0;
    uint32_t links = 1;
    bool multi_extent = false, interleaved = false, sparse = false;
    bool zisofs = false;
    uint8_t zisofs_header = 0, zisofs_log2 = 0;   // header size in bytes, log2 of the block size
    uint64_t stored = 0;          // a zisofs file: bytes of the compressed form
  };
  // What a record's System Use area says (Rock Ridge).
  struct Susp {
    explicit Susp(allocators::IAllocator& allocator) : name(allocator), target(allocator) {}
    String name, target;
    bool has_name = false, is_link = false, has_mode = false, relocated = false, link_continues = false;
    uint32_t mode = 0, links = 0;
    uint64_t serial = 0, mtime = 0, child = 0;
    bool has_child = false, zisofs = false, zisofs_unknown = false, sparse = false;
    uint8_t zisofs_header = 0, zisofs_log2 = 0;
    uint64_t zisofs_size = 0;
  };

  Status load();
  Status read_raw(uint64_t position, ByteView out) const;
  // The records of a folder, whole.
  Status read_directory(const Node& directory, ByteBuffer* out) const;
  // Parses the record at `at` of a folder's bytes (which start at byte `base` of the image). *skip: a record not to list ("." "..", a relocated
  // folder in its old place). *name: the name in the tree in use.
  Status parse_record(ConstByteView records, uint64_t at, uint64_t base, uint64_t directory_end, Node* node, String* name, bool* skip) const;
  Status parse_susp(ConstByteView area, Susp* susp, uint32_t depth) const;
  Status lookup(const Node& directory, StringView name, Node* found) const;
  Status read_target(const Node& link, String* target) const;
  // The Rock Ridge entries of the record at byte `record` of the image.
  Status record_susp(uint64_t record, Susp* susp) const;
  Status resolve(StringView path, bool follow_last, Node* node, String* exit = nullptr) const;
  Status fill_entry(const Node& node, StringView name, DirectoryEntry* entry) const;

  allocators::IAllocator& allocator_;
  UniquePointer<IFile> source_;
  uint64_t offset_ = 0, size_ = 0;
  uint32_t block_size_ = 2048;
  uint64_t volume_blocks_ = 0;
  Tree tree_ = Tree::Plain;
  uint32_t susp_skip_ = 0;
  Node root_;
  FixedString<33> label_;
};

}  // namespace filesystem
