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
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

// A reader of .zip archives (PKWARE's APPNOTE; what a .zip download, an .msix package and a .nupkg are): the central directory, with the ZIP64
// records of archives that have them (an .msix always does), and members that are stored or deflated. Encrypted members, other compression methods
// and archives split over several disks are refused. The archive is untrusted: every offset and size is checked against the source, the number of
// members and the size of what is inflated are limited, and a member's CRC-32 must match.
namespace media {

// CRC-32 (the one of zip, gzip and PNG: polynomial 0xEDB88320), continuing from `crc` (0 to start).
uint32_t crc32(ConstByteView data, uint32_t crc = 0);

// Where the archive's bytes are: memory, or a file read piece by piece.
class IZipSource {
 public:
  virtual uint64_t size() const = 0;
  // Fills `out` with the bytes at `offset`. False if they cannot all be read.
  [[nodiscard]] virtual bool read(uint64_t offset, ByteView out) = 0;

 protected:
  ~IZipSource() = default;
};

class MemoryZipSource final : public IZipSource {
 public:
  explicit MemoryZipSource(ConstByteView data) : data_(data) {}
  uint64_t size() const override { return data_.size(); }
  [[nodiscard]] bool read(uint64_t offset, ByteView out) override {
    ConstByteView from;
    return data_.try_subview(offset, out.size(), &from) && out.copy_from(from);
  }

 private:
  ConstByteView data_;
};

struct ZipEntry {
  explicit ZipEntry(allocators::IAllocator& allocator) : name(allocator) {}
  ZipEntry(ZipEntry&&) = default;
  ZipEntry& operator=(ZipEntry&&) = default;
  String name;                    // as stored: '/' between folders; a folder's own entry ends in '/'
  uint64_t compressed_size = 0;
  uint64_t size = 0;
  uint64_t header_offset = 0;     // of the member's local header
  uint32_t crc = 0;
  uint16_t method = 0;            // 0 stored, 8 deflated
  bool encrypted = false;
  bool directory() const { return name.view().ends_with('/'); }
};

class ZipReader {
 public:
  static constexpr uint64_t kMaxEntries = 100000;
  static constexpr uint64_t kMaxDirectoryBytes = 64ull << 20;

  ZipReader(allocators::IAllocator& allocator, IZipSource& source) : allocator_(allocator), source_(source), entries_(allocator) {}

  // Reads the central directory. False if this is not a zip archive this reader can read, or memory ran out.
  [[nodiscard]] bool open();
  ArrayView<const ZipEntry> entries() const { return entries_.view(); }
  // The member of that name (exactly as stored), or nullptr.
  const ZipEntry* find(StringView name) const;
  // Appends the member's contents to `out`. False if it is larger than `max_size`, encrypted, packed in a way this reader does not know, damaged (the
  // CRC-32 is checked), or memory ran out; `out` is then as it was.
  [[nodiscard]] bool extract(const ZipEntry& entry, uint64_t max_size, ByteBuffer* out);

 private:
  allocators::IAllocator& allocator_;
  IZipSource& source_;
  List<ZipEntry> entries_;
};

}  // namespace media
