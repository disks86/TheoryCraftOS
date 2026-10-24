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
#include "Utility/ByteBuffer.hpp"

// Writing Ogg pages (RFC 3533), for turning packets from another container into the Ogg files the audio decoders read. Header only.
namespace media {
namespace ogg {

inline uint32_t crc(ConstByteView data) {
  // (a function-local table, built once, so that the header can be included anywhere)
  struct Table {
    uint32_t v[256];
    Table() {
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
        v[i] = r;
      }
    }
  };
  static const Table table;
  uint32_t sum = 0;
  for (uint8_t byte : data) sum = (sum << 8) ^ table.v[((sum >> 24) ^ byte) & 0xFF];
  return sum;
}

inline bool put_le(ByteBuffer* b, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i)
    if (!b->put8(static_cast<uint8_t>(v >> (8 * i)))) return false;
  return true;
}

// Page header types.
constexpr uint8_t kContinued = 0x01, kFirst = 0x02, kLast = 0x04;

// One page of whole packets; false if their segments do not fit in a page (255) or memory ran out. `granule` is the position in samples of the end of the
// last packet.
inline bool page(ByteBuffer* out, uint8_t type, int64_t granule, uint32_t serial, uint32_t sequence, const ConstByteView* packets, uint32_t count) {
  uint8_t table[255];
  uint32_t segments = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t size = packets[i].size();
    const uint64_t full = size / 255;
    if (segments + full + 1 > 255) return false;
    for (uint64_t k = 0; k < full; ++k) table[segments++] = 255;
    table[segments++] = static_cast<uint8_t>(size % 255);
  }
  const uint64_t start = out->size();
  const bool ok = out->append(ConstByteView(reinterpret_cast<const uint8_t*>("OggS"), 4)) && out->put8(0) && out->put8(type) &&
                  put_le(out, static_cast<uint64_t>(granule), 8) && put_le(out, serial, 4) && put_le(out, sequence, 4) && put_le(out, 0, 4) &&   // (the checksum, below)
                  out->put8(static_cast<uint8_t>(segments)) && out->append(ConstByteView(table, segments));
  if (!ok) {
    out->truncate(start);
    return false;
  }
  for (uint32_t i = 0; i < count; ++i)
    if (!out->append(packets[i])) {
      out->truncate(start);
      return false;
    }
  const uint32_t sum = crc(out->view().subview(start));
  for (int i = 0; i < 4; ++i)
    if (!out->patch8(start + 22 + i, static_cast<uint8_t>(sum >> (8 * i)))) return false;
  return true;
}

}  // namespace ogg
}  // namespace media
