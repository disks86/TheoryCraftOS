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

#include "Executable/AppImage.hpp"
#include "Utility/ByteStream.hpp"

namespace exe {

namespace {

constexpr uint8_t kClass32 = 1, kClass64 = 2, kLittle = 1, kBig = 2;

bool read_exact(filesystem::IFile& file, uint64_t offset, ByteView out) {
  uint64_t done = 0;
  while (done < out.size()) {
    uint64_t got = 0;
    if (file.read(offset + done, out.subview(done), &got) != filesystem::Status::Ok || got == 0) return false;
    done += got;
  }
  return true;
}

// Reads unsigned fields of the ELF's byte order.
struct Fields {
  ConstByteView bytes;
  bool big = false;
  uint64_t at(uint64_t offset, uint32_t size) const {
    ByteReader reader(bytes.subview(offset, size));
    if (size == 2) return big ? reader.be16() : reader.le16();
    if (size == 4) return big ? reader.be32() : reader.le32();
    if (!big) return reader.le64();
    const uint64_t high = reader.be32();
    return high << 32 | reader.be32();
  }
};

// How far past the end of a SquashFS image a DwarFS image is looked for (the stand-in is padded to a block of the real image, 128 KiB as built today).
constexpr uint64_t kDwarfsSearchLimit = 1024 * 1024;

// The first DwarFS section header in [from, from + kDwarfsSearchLimit) of `file` (`size` bytes long): "DWARFS", format major version 2, and section
// number 0, as the first section of an image has. Mounting checks the rest (the checksums of every section).
bool find_dwarfs_image(filesystem::IFile& file, uint64_t from, uint64_t size, uint64_t* found) {
  constexpr uint64_t kHeader = 64, kNumberAt = 48;
  static constexpr uint8_t kMagic[] = {'D', 'W', 'A', 'R', 'F', 'S', 2};
  if (from > size || size - from < kHeader) return false;
  const uint64_t stop = size - from < kDwarfsSearchLimit ? size : from + kDwarfsSearchLimit;   // one past the last byte looked at
  uint8_t chunk[4096 + kHeader];
  uint64_t at = from;
  while (stop - at >= kHeader) {
    // Chunks overlap by a header less one byte, so a header across a chunk's end is seen whole in the next.
    const uint64_t length = stop - at < sizeof(chunk) ? stop - at : sizeof(chunk);
    const ByteView window = ByteView(chunk).first(length);
    if (!read_exact(file, at, window)) return false;
    const ConstByteView bytes(window);
    for (uint64_t i = 0; i <= length - kHeader; ++i) {
      if (!bytes.subview(i, sizeof(kMagic)).equals(ConstByteView(kMagic))) continue;
      if (ByteReader(bytes.subview(i + kNumberAt, 4)).le32() != 0) continue;
      *found = at + i;
      return true;
    }
    at += length - (kHeader - 1);
  }
  return false;
}

}  // namespace

AppImageType identify_appimage(ConstByteView head) {
  if (head.size() < 16) return AppImageType::None;
  if (head[0] != 0x7F || head[1] != 'E' || head[2] != 'L' || head[3] != 'F') return AppImageType::None;
  if (head[8] != 'A' || head[9] != 'I') return AppImageType::None;
  if (head[10] == 1) return AppImageType::Type1;
  if (head[10] == 2) return AppImageType::Type2;
  return AppImageType::None;
}

bool appimage_payload_offset(filesystem::IFile& file, uint64_t* offset, AppImagePayload* payload) {
  uint8_t header[64];
  if (file.size() < sizeof(header) || !read_exact(file, 0, ByteView(header))) return false;
  if (identify_appimage(ConstByteView(header)) != AppImageType::Type2) return false;
  const uint8_t elf_class = header[4], order = header[5];
  if ((elf_class != kClass32 && elf_class != kClass64) || (order != kLittle && order != kBig)) return false;
  const Fields h{ConstByteView(header), order == kBig};
  const bool wide = elf_class == kClass64;
  const uint64_t shoff = wide ? h.at(0x28, 8) : h.at(0x20, 4);
  const uint64_t shentsize = h.at(wide ? 0x3A : 0x2E, 2), shnum = h.at(wide ? 0x3C : 0x30, 2);
  const uint64_t size = file.size();
  if (shnum == 0 || shentsize < (wide ? 64u : 40u) || shoff > size || shentsize * shnum > size - shoff) return false;
  uint64_t end = shoff + shentsize * shnum;
  // The last section may lie past the table (the runtime's own rule).
  uint8_t section[64];
  const ByteView last = ByteView(section).first(wide ? 64 : 40);
  if (!read_exact(file, shoff + shentsize * (shnum - 1), last)) return false;
  const Fields s{ConstByteView(last), order == kBig};
  const uint64_t section_offset = wide ? s.at(0x18, 8) : s.at(0x10, 4), section_size = wide ? s.at(0x20, 8) : s.at(0x14, 4);
  const uint64_t type = s.at(4, 4);
  if (type != 8 /* SHT_NOBITS */ && section_offset <= size && section_size <= size - section_offset && section_offset + section_size > end)
    end = section_offset + section_size;
  // A SquashFS superblock (96 bytes, "hsqs") or a DwarFS section header (64 bytes, "DWARFS").
  uint8_t magic[6];
  if (end > size || size - end < 64 || !read_exact(file, end, ByteView(magic))) return false;
  AppImagePayload found;
  if (magic[0] == 'h' && magic[1] == 's' && magic[2] == 'q' && magic[3] == 's' && size - end >= 96) {
    found = AppImagePayload::SquashFs;
    // uruntime's newer AppImages put a small stand-in SquashFS image first (an AppRun that only says the payload is DwarFS, the icon and the desktop
    // entry, for tools that know SquashFS alone) and the DwarFS image after it. The real image is the one to mount.
    uint8_t used[8];
    if (read_exact(file, end + 40, ByteView(used))) {
      const uint64_t bytes_used = ByteReader(ConstByteView(used)).le64();
      uint64_t dwarfs = 0;
      if (bytes_used >= 96 && bytes_used < size - end && find_dwarfs_image(file, end + bytes_used, size, &dwarfs)) {
        end = dwarfs;
        found = AppImagePayload::DwarFs;
      }
    }
  } else if (magic[0] == 'D' && magic[1] == 'W' && magic[2] == 'A' && magic[3] == 'R' && magic[4] == 'F' && magic[5] == 'S') {
    found = AppImagePayload::DwarFs;
  } else {
    return false;
  }
  *offset = end;
  if (payload) *payload = found;
  return true;
}

}  // namespace exe
