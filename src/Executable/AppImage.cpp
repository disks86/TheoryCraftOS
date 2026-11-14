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
