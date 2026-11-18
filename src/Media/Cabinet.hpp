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
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

// A reader of Microsoft cabinets (.cab, "MSCF": [MS-CAB]; what a Windows Installer package keeps its files in): the header with its reserved areas, the
// folders and the files, and folders that are stored, MSZIP or LZX compressed. A folder is one compressed stream holding its files one after another,
// in data blocks of up to 32768 bytes; to get at a file, everything before it in its folder has to be decoded. So extraction is a walk: extract_all()
// goes through the files in the order they lie in their folders and hands their bytes to a sink, keeping no more in memory than one block and the
// decompressor's history (32 KiB for MSZIP, the window for LZX).
//
// Not read: cabinets that are part of a set (files continued from or into another cabinet), Quantum compression, folders whose files overlap in part
// (files that are the same bytes under several names are read: see ICabinetSink::same_file). The cabinet is untrusted: every offset and size is checked against the source, and the checksum of a data block
// is verified where the cabinet has one.
namespace media {

struct CabinetEntry {
  static constexpr uint16_t kReadOnly = 0x01, kHidden = 0x02, kSystem = 0x04, kArchive = 0x20, kExecute = 0x40, kNameIsUtf8 = 0x80;

  explicit CabinetEntry(allocators::IAllocator& allocator) : name(allocator) {}
  CabinetEntry(CabinetEntry&&) = default;
  CabinetEntry& operator=(CabinetEntry&&) = default;
  String name;                    // as stored: '\' between folders; UTF-8 if name_is_utf8(), else in the code page of whoever made the cabinet
  uint64_t size = 0;
  uint64_t folder_offset = 0;     // of its first byte in what its folder decompresses to
  uint16_t folder = 0;            // which folder it is in
  uint16_t date = 0, time = 0;    // MS-DOS format
  uint16_t attributes = 0;
  bool name_is_utf8() const { return (attributes & kNameIsUtf8) != 0; }
};

// Where extract_all() puts the files. Any function returning false stops the walk, which then fails.
class ICabinetSink {
 public:
  // The next file. `*wanted` is true; set it to false to pass the file over (no write() or end_file() follows for it, and what need not be decoded
  // to reach the files after it is not).
  [[nodiscard]] virtual bool begin_file(const CabinetEntry& entry, bool* wanted) = 0;
  // The next bytes of the file (at most one data block: 32768 bytes).
  [[nodiscard]] virtual bool write(ConstByteView bytes) = 0;
  // All of the file's bytes were written.
  [[nodiscard]] virtual bool end_file() = 0;
  // `entry` is another name for the very bytes of `first`, the file that was last written whole (its end_file() has been called): a cabinet may list
  // the same data several times, and the tools that pack installers do so for files that are identical. The bytes are not handed over a second
  // time: the sink copies what it made of `first`, if it wants this one.
  [[nodiscard]] virtual bool same_file(const CabinetEntry& entry, const CabinetEntry& first) = 0;

 protected:
  ~ICabinetSink() = default;
};

class CabinetReader {
 public:
  static constexpr uint64_t kMaxEntries = 100000;      // (the format's own limit is 65535)

  CabinetReader(allocators::IAllocator& allocator, IZipSource& source)
      : allocator_(allocator), source_(source), folders_(allocator), entries_(allocator), order_(allocator) {}

  // Reads the header, the folders and the files. False if this is not a cabinet this reader can read, or memory ran out.
  [[nodiscard]] bool open();
  // The files, in the order the cabinet lists them.
  ArrayView<const CabinetEntry> entries() const { return entries_.view(); }
  uint64_t folder_count() const { return folders_.view().size(); }
  // The file of that name (exactly as stored), or nullptr.
  const CabinetEntry* find(StringView name) const;
  // Walks all files, folder by folder and in each in the order they lie in it (which need not be the order the cabinet lists them), handing each to `sink`. False if the sink stopped it, a file the sink
  // wanted could not be decoded (damaged, cut short, a failed checksum), or memory ran out; the sink may then have been given part of a file.
  [[nodiscard]] bool extract_all(ICabinetSink& sink);
  // Appends one file's contents to `out`. False if it is larger than `max_size`, not an entry of this reader, damaged, or memory ran out; `out` is
  // then as it was. This decodes the file's folder from its start: to take many files out of a cabinet, use extract_all().
  [[nodiscard]] bool extract(const CabinetEntry& entry, uint64_t max_size, ByteBuffer* out);

 private:
  struct Folder {
    uint64_t data_offset = 0;     // of its first data block
    uint32_t blocks = 0;
    uint16_t method = 0;          // 0 stored, 1 MSZIP, 3 LZX
    uint16_t window_bits = 0;     // LZX
    uint32_t first = 0, count = 0;      // its files: order_[first .. first + count)
  };

  [[nodiscard]] bool walk(ICabinetSink& sink, const CabinetEntry* only);

  allocators::IAllocator& allocator_;
  IZipSource& source_;
  List<Folder> folders_;
  List<CabinetEntry> entries_;
  List<uint32_t> order_;          // indices into entries_, by folder
  uint32_t data_reserve_ = 0;     // bytes the cabinet's maker reserved in every data block's header
};

}  // namespace media
