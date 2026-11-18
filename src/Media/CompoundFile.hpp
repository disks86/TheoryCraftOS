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
#include "Media/Zip.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

// A reader of compound files ([MS-CFB], "OLE2 structured storage": what a Windows Installer package (.msi), an old Office document and a
// thumbnail cache are): a small file system in a file, made of sectors chained by a file allocation table, with a directory of named streams. This
// reads the streams of the root storage, of versions 3 (512-byte sectors) and 4 (4096-byte sectors), the small ones out of the mini stream. Streams
// inside sub-storages are not listed. The file is untrusted: every sector number and size is checked against the source, chains cannot loop, and
// the tables that are read into memory are limited.
namespace media {

struct CompoundEntry {
  uint16_t name[32] = {};      // UTF-16 code units, as stored
  uint32_t name_units = 0;     // how many of them (no terminator)
  uint64_t size = 0;
  uint32_t first_sector = 0;
  ArrayView<const uint16_t> units() const { return ArrayView<const uint16_t>(name).first(name_units); }
};

class CompoundFile;

// One stream of a compound file, readable at any offset (so a cabinet or an archive inside it is read in place). Opened by CompoundFile::open_stream;
// the file must outlive it.
class CompoundStream final : public IZipSource {
 public:
  explicit CompoundStream(allocators::IAllocator& allocator) : chain_(allocator) {}
  uint64_t size() const override { return size_; }
  [[nodiscard]] bool read(uint64_t offset, ByteView out) override;

 private:
  friend class CompoundFile;
  CompoundFile* file_ = nullptr;
  List<uint32_t> chain_;      // the stream's sectors (mini sectors, if it is in the mini stream), in order
  uint64_t size_ = 0;
  bool mini_ = false;
};

class CompoundFile {
 public:
  static constexpr uint32_t kMaxEntries = 65536;
  static constexpr uint64_t kMaxTableSectors = 1u << 24;      // of the allocation tables: 16 M sectors, the chains of an 8 GiB file of 512-byte sectors

  CompoundFile(allocators::IAllocator& allocator, IZipSource& source) : allocator_(allocator), source_(source), fat_(allocator), mini_fat_(allocator), entries_(allocator), root_(allocator) {}

  // Reads the header, the allocation tables and the directory. False if this is not a compound file this reader can read, or memory ran out.
  [[nodiscard]] bool open();
  // The streams of the root storage.
  ArrayView<const CompoundEntry> entries() const { return entries_.view(); }
  // The stream whose name is exactly these UTF-16 code units, or nullptr.
  const CompoundEntry* find(ArrayView<const uint16_t> name) const;
  // Makes `stream` read `entry`. False if its chain of sectors is damaged, or memory ran out.
  [[nodiscard]] bool open_stream(const CompoundEntry& entry, CompoundStream* stream);
  // Appends the whole stream to `out`. False if it is longer than `max_size`, damaged, or memory ran out.
  [[nodiscard]] bool read_stream(const CompoundEntry& entry, uint64_t max_size, ByteBuffer* out);

 private:
  friend class CompoundStream;
  bool read_sector(uint32_t sector, uint64_t offset, ByteView out);
  bool next_in(const ByteBuffer& table, uint32_t sector, uint32_t* next) const;
  bool build_chain(const ByteBuffer& table, uint32_t first, uint64_t sectors, List<uint32_t>* chain) const;
  bool load_table(uint32_t first, uint64_t count, ByteBuffer* out);

  allocators::IAllocator& allocator_;
  IZipSource& source_;
  uint32_t sector_shift_ = 9;
  uint32_t mini_cutoff_ = 4096;
  uint64_t sector_count_ = 0;       // sectors the source holds after the header
  ByteBuffer fat_;                  // the file allocation table: one little-endian u32 per sector
  ByteBuffer mini_fat_;             // the same for the 64-byte sectors of the mini stream
  List<CompoundEntry> entries_;
  CompoundStream root_;             // the mini stream: the root entry's own chain
};

}  // namespace media
