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
#include "Utility/ArrayView.hpp"

namespace filesystem {
namespace ext {

// The checksums of ext4: crc32c (metadata_csum: superblock, group descriptors, bitmaps, inodes, extent blocks, directory blocks, extended
// attribute blocks) and crc16 (the uninit_bg group descriptor checksum). Neither is inverted at the start or the end: the caller passes the
// seed (usually ~0), exactly as the Linux kernel and e2fsprogs do.

namespace detail {
struct Crc32cTable {
  uint32_t entry[256];
  constexpr Crc32cTable() : entry() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      entry[i] = c;
    }
  }
};
struct Crc16Table {
  uint16_t entry[256];
  constexpr Crc16Table() : entry() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xA001u : c >> 1;
      entry[i] = static_cast<uint16_t>(c);
    }
  }
};
inline constexpr Crc32cTable kCrc32c;
inline constexpr Crc16Table kCrc16;
}  // namespace detail

inline uint32_t crc32c(uint32_t crc, ConstByteView data) {
  for (uint8_t byte : data) crc = detail::kCrc32c.entry[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  return crc;
}
inline uint32_t crc32c_u32(uint32_t crc, uint32_t value) {
  const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return crc32c(crc, ConstByteView(bytes));
}
inline uint32_t crc32c_zeros(uint32_t crc, uint32_t count) {
  while (count-- > 0) crc = detail::kCrc32c.entry[crc & 0xFF] ^ (crc >> 8);
  return crc;
}

inline uint16_t crc16(uint16_t crc, ConstByteView data) {
  for (uint8_t byte : data) crc = static_cast<uint16_t>((crc >> 8) ^ detail::kCrc16.entry[(crc ^ byte) & 0xFF]);
  return crc;
}

}  // namespace ext
}  // namespace filesystem
