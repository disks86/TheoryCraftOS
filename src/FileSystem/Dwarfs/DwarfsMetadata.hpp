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
#include "FileSystem/FileSystemTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

// The metadata of a DwarFS image (format 2.3 to 2.5, see docs/AppImage.md and the format's own dwarfs-format(5)).
//
// The METADATA_V2 section is a Frozen2 structure: bit-packed fields whose widths and positions are not fixed but described by the
// METADATA_V2_SCHEMA section, a Thrift compact-encoded apache::thrift::frozen::schema::Schema. FrozenSchema reads that schema,
// FrozenView navigates the packed data with it, and decode_metadata() turns the result into the plain tables below (the packed
// directory, chunk and shared-file tables unpacked, the names and link targets in StringTables), checking every index on the way.
namespace filesystem::dwarfs {

// One layout of the schema: its size (bytes, when the object is byte-aligned) or width in bits, and its fields by Thrift field id.
struct FrozenField {
  int16_t id = 0;
  int16_t layout = 0;
  int16_t offset = 0;      // >= 0: bytes from the owner's start; < 0: -(bits from the owner's start)
};
struct FrozenLayout {
  bool present = false;
  int32_t size = 0;
  int16_t bits = 0;
  uint32_t first_field = 0, field_count = 0;      // into FrozenSchema::fields_
};

class FrozenSchema {
 public:
  explicit FrozenSchema(allocators::IAllocator& allocator) : layouts_(allocator), fields_(allocator) {}
  // Parses a Thrift compact-encoded Schema. False when it is damaged, or of a newer Frozen file version.
  [[nodiscard]] bool parse(ConstByteView compact);
  int16_t root() const { return root_; }
  // Null when `id` names no layout.
  const FrozenLayout* layout(int32_t id) const;
  ArrayView<const FrozenField> fields(const FrozenLayout& layout) const { return fields_.view().subview(layout.first_field, layout.field_count); }

 private:
  friend class SchemaParser;
  List<FrozenLayout> layouts_;      // by layout id
  List<FrozenField> fields_;
  int16_t root_ = 0;
};

// A position in the frozen data: a layout and where the object starts (bit_offset may be past 8). layout < 0 is an absent object,
// which reads as zero, false, an empty string or an empty list (Frozen2 leaves out what is always empty).
struct FrozenRef {
  int32_t layout = -1;
  uint64_t byte = 0, bit = 0;
};

struct FrozenArray {
  uint64_t count = 0;
  int32_t item_layout = -1;
  uint64_t start = 0;            // byte where the items start
  uint64_t stride_bytes = 0;     // items are byte-aligned at this stride, or (when 0)
  uint64_t stride_bits = 0;      // packed at this many bits each
};

class FrozenView {
 public:
  FrozenView(const FrozenSchema& schema, ConstByteView data) : schema_(schema), data_(data) {}
  FrozenRef root() const;
  // The field with Thrift id `id` of the struct at `owner` (absent if the layout has no such field).
  FrozenRef field(FrozenRef owner, int16_t id) const;
  // False when the value lies outside the data or is wider than 64 bits.
  [[nodiscard]] bool read_uint(FrozenRef at, uint64_t* value) const;
  [[nodiscard]] bool read_bool(FrozenRef at, bool* value) const;
  // An optional<T>: *value is the T when it is set.
  [[nodiscard]] bool read_optional(FrozenRef at, bool* is_set, FrozenRef* value) const;
  // A list, set or map (a map's items are pairs: field 1 the key, 2 the value). Checks that every item lies inside the data.
  [[nodiscard]] bool read_array(FrozenRef at, FrozenArray* array) const;
  FrozenRef item(const FrozenArray& array, uint64_t index) const;
  [[nodiscard]] bool read_string(FrozenRef at, ConstByteView* bytes) const;

  // Shorthands: a field read as an unsigned number or a flag (absent: 0 / false).
  [[nodiscard]] bool uint_field(FrozenRef owner, int16_t id, uint64_t* value) const { return read_uint(field(owner, id), value); }
  [[nodiscard]] bool bool_field(FrozenRef owner, int16_t id, bool* value) const { return read_bool(field(owner, id), value); }

 private:
  const FrozenSchema& schema_;
  ConstByteView data_;
};

// The names or link targets: strings stored back to back with an index (count + 1 offsets), optionally compressed with FSST (each byte a
// code for a symbol of 1 to 8 bytes, 255 escaping one literal byte).
class StringTable {
 public:
  explicit StringTable(allocators::IAllocator& allocator) : data_(allocator), index_(allocator) {}
  // From a list<string>.
  [[nodiscard]] bool load_plain(const FrozenView& view, FrozenRef list);
  // From a string_table struct (compact_names / compact_symlinks).
  [[nodiscard]] bool load_compact(const FrozenView& view, FrozenRef table);
  uint64_t count() const { return index_.count() ? index_.count() - 1 : 0; }
  // String `index` into *out (replacing what was there). False when it cannot be decoded (a damaged code) or memory runs out.
  [[nodiscard]] bool get(uint64_t index, ByteBuffer* out) const;

 private:
  [[nodiscard]] bool load_symbols(ConstByteView symtab);
  ByteBuffer data_;
  List<uint64_t> index_;
  bool compressed_ = false;
  uint8_t symbol_length_[255] = {};
  uint8_t symbols_[255][8] = {};
};

struct Chunk {
  uint32_t block = 0, offset = 0, size = 0;
};
struct DirEntry {
  uint32_t name = 0, inode = 0;
};
struct InodeInfo {
  uint32_t mode = 0;          // st_mode (type and permission bits)
  uint32_t links = 1;
  uint64_t modified = 0;      // seconds since 1970
};

// POSIX file types in InodeInfo::mode.
constexpr uint32_t kTypeMask = 0170000, kTypeDirectory = 0040000, kTypeLink = 0120000, kTypeRegular = 0100000;

struct Metadata {
  explicit Metadata(allocators::IAllocator& allocator)
      : chunks(allocator), directories(allocator), inodes(allocator), chunk_table(allocator), symlink_table(allocator), entries(allocator),
        shared_files(allocator), large_holes(allocator), names(allocator), links(allocator) {}

  List<Chunk> chunks;
  List<uint32_t> directories;      // first entry of each directory inode, unpacked, with the sentinel (directory_count + 1 values)
  List<InodeInfo> inodes;
  List<uint32_t> chunk_table;      // unpacked: chunks of file i are chunk_table[i] .. chunk_table[i + 1]
  List<uint32_t> symlink_table;
  List<DirEntry> entries;          // entries[0] is the root
  List<uint32_t> shared_files;     // unpacked: chunk_table index (less unique_files) of each shared file inode
  List<uint64_t> large_holes;
  StringTable names, links;
  uint32_t block_size = 0;
  uint64_t total_size = 0;
  bool has_holes = false;
  uint32_t hole_block = 0;
  bool backslash_links = false;      // link targets were written with '\\' as the separator (an image made on Windows)
  // The inodes come in this order: directories, links, regular files (unique, then shared), then the rest.
  uint32_t directory_count = 0, link_count = 0, file_count = 0, unique_files = 0;
};

// Decodes the metadata. `blocks` is the number of BLOCK sections (chunks must point inside them). Unsupported for an older format
// (no dir_entries) or a feature this reader does not know; Corrupt for anything inconsistent.
[[nodiscard]] Status decode_metadata(allocators::IAllocator& allocator, ConstByteView schema, ConstByteView data, uint32_t blocks, Metadata* out);

}  // namespace filesystem::dwarfs
