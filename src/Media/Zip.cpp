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

#include "Media/Zip.hpp"
#include "Media/Zlib.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {

constexpr uint32_t kEndSignature = 0x06054B50, kEnd64Signature = 0x06064B50, kLocator64Signature = 0x07064B50, kCentralSignature = 0x02014B50,
                   kLocalSignature = 0x04034B50;
constexpr uint64_t kEndSize = 22, kLocator64Size = 20, kEnd64Size = 56, kCentralSize = 46, kLocalSize = 30;
constexpr uint64_t kMaxComment = 65535;

struct CrcTable {
  uint32_t entry[256];
  constexpr CrcTable() : entry() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      entry[i] = c;
    }
  }
};
constexpr CrcTable kCrcTable;

// Reads `count` bytes at `offset` of the source into `out` (replacing what it holds). False if the source does not have them.
bool read_at(IZipSource& source, uint64_t offset, uint64_t count, ByteBuffer* out) {
  out->clear();
  if (offset > source.size() || count > source.size() - offset) return false;
  return out->put_zeros(count) && source.read(offset, out->writable_view());
}

}  // namespace

uint32_t crc32(ConstByteView data, uint32_t crc) {
  uint32_t c = ~crc;
  for (const uint8_t byte : data) c = kCrcTable.entry[(c ^ byte) & 0xFF] ^ (c >> 8);
  return ~c;
}

bool ZipReader::open() {
  entries_.clear();
  const uint64_t size = source_.size();
  if (size < kEndSize) return false;

  // The end record: the last one in the file, before at most 64 KiB of comment.
  const uint64_t tail_size = size < kEndSize + kMaxComment ? size : kEndSize + kMaxComment;
  ByteBuffer tail(allocator_);
  if (!read_at(source_, size - tail_size, tail_size, &tail)) return false;
  const ConstByteView bytes = tail.view();
  uint64_t end_at = ~uint64_t{0};
  for (uint64_t at = tail_size - kEndSize + 1; at-- > 0;) {
    if (bytes[at] == 0x50 && bytes[at + 1] == 0x4B && bytes[at + 2] == 0x05 && bytes[at + 3] == 0x06) {
      end_at = at;
      break;
    }
  }
  if (end_at == ~uint64_t{0}) return false;
  ByteReader end(bytes.subview(end_at, kEndSize));
  end.skip(4);
  const uint16_t disk = end.le16(), directory_disk = end.le16();
  const uint16_t entries_here = end.le16(), entries_all = end.le16();
  uint64_t count = entries_all, directory_size = end.le32(), directory_offset = end.le32();
  if (!end.ok()) return false;
  bool multi_disk = (disk != 0 && disk != 0xFFFF) || (directory_disk != 0 && directory_disk != 0xFFFF) || entries_here != entries_all;

  // ZIP64: a locator just before the end record names the real end record.
  const uint64_t end_offset = size - tail_size + end_at;
  if (entries_all == 0xFFFF || directory_size == 0xFFFFFFFFu || directory_offset == 0xFFFFFFFFu || disk == 0xFFFF) {
    if (end_offset < kLocator64Size) return false;
    ByteBuffer block(allocator_);
    if (!read_at(source_, end_offset - kLocator64Size, kLocator64Size, &block)) return false;
    ByteReader locator(block.view());
    if (locator.le32() != kLocator64Signature) return false;
    locator.skip(4);
    const uint64_t end64_offset = locator.le64();
    if (!locator.ok() || !read_at(source_, end64_offset, kEnd64Size, &block)) return false;
    ByteReader end64(block.view());
    if (end64.le32() != kEnd64Signature) return false;
    end64.skip(8 + 2 + 2);      // the record's size, the versions
    const uint32_t disk64 = end64.le32(), directory_disk64 = end64.le32();
    const uint64_t here64 = end64.le64();
    count = end64.le64();
    directory_size = end64.le64();
    directory_offset = end64.le64();
    if (!end64.ok()) return false;
    multi_disk = disk64 != 0 || directory_disk64 != 0 || here64 != count;
  }
  if (multi_disk || count > kMaxEntries || directory_size > kMaxDirectoryBytes) return false;
  if (count > directory_size / kCentralSize) return false;      // (every member takes at least a fixed header: a count that lies is caught before memory is asked for)

  ByteBuffer directory(allocator_);
  if (!read_at(source_, directory_offset, directory_size, &directory) || !entries_.reserve(count)) return false;
  ByteReader in(directory.view());
  for (uint64_t i = 0; i < count; ++i) {
    if (in.le32() != kCentralSignature) return false;
    in.skip(2 + 2);      // the versions
    const uint16_t flags = in.le16();
    ZipEntry entry(allocator_);
    entry.method = in.le16();
    in.skip(2 + 2);      // time, date
    entry.crc = in.le32();
    entry.compressed_size = in.le32();
    entry.size = in.le32();
    const uint16_t name_size = in.le16(), extra_size = in.le16(), comment_size = in.le16();
    const uint16_t entry_disk = in.le16();
    in.skip(2 + 4);      // the attributes
    entry.header_offset = in.le32();
    const ConstByteView name = in.take(name_size);
    ByteReader extra(in.take(extra_size));
    in.skip(comment_size);
    if (!in.ok()) return false;
    entry.encrypted = (flags & 1) != 0;
    // The ZIP64 extra field carries the sizes and the offset that did not fit, in this order, each only if its 32 bit field is all ones.
    bool disk_in_extra = entry_disk == 0xFFFF;
    while (extra.remaining() >= 4) {
      const uint16_t id = extra.le16(), length = extra.le16();
      ByteReader field(extra.take(length));
      if (!extra.ok()) return false;
      if (id != 0x0001) continue;
      if (entry.size == 0xFFFFFFFFu) entry.size = field.le64();
      if (entry.compressed_size == 0xFFFFFFFFu) entry.compressed_size = field.le64();
      if (entry.header_offset == 0xFFFFFFFFu) entry.header_offset = field.le64();
      if (disk_in_extra) disk_in_extra = field.le32() != 0;
      if (!field.ok()) return false;
    }
    if (disk_in_extra || (entry_disk != 0 && entry_disk != 0xFFFF)) return false;
    if (name.empty() || !entry.name.assign(StringView(name)) || !entries_.push_back(util::move(entry))) return false;
  }
  return true;
}

const ZipEntry* ZipReader::find(StringView name) const {
  for (const ZipEntry& entry : entries_.view())
    if (entry.name.view().equals(name)) return &entry;
  return nullptr;
}

bool ZipReader::extract(const ZipEntry& entry, uint64_t max_size, ByteBuffer* out) {
  if (entry.encrypted || (entry.method != 0 && entry.method != 8) || entry.size > max_size) return false;
  if (entry.method == 0 && entry.compressed_size != entry.size) return false;
  // The local header says where the data starts (its name and extra field need not be as long as the directory's).
  ByteBuffer block(allocator_);
  if (!read_at(source_, entry.header_offset, kLocalSize, &block)) return false;
  ByteReader local(block.view());
  if (local.le32() != kLocalSignature) return false;
  local.skip(22);
  const uint64_t name_size = local.le16(), extra_size = local.le16();
  if (!local.ok()) return false;
  const uint64_t data_offset = entry.header_offset + kLocalSize + name_size + extra_size;      // (three 16 bit sizes on a checked offset: cannot wrap)
  const uint64_t start = out->size();
  bool ok;
  if (entry.method == 0) {
    ok = data_offset <= source_.size() && entry.size <= source_.size() - data_offset && out->put_zeros(entry.size) &&
         source_.read(data_offset, out->writable_view().subview(start));
  } else {
    ok = read_at(source_, data_offset, entry.compressed_size, &block) && inflate(block.view(), out, entry.size, true) && out->size() - start == entry.size;
  }
  ok = ok && crc32(out->view().subview(start)) == entry.crc;
  if (!ok) out->truncate(start);
  return ok;
}

}  // namespace media
