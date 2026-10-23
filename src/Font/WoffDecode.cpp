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
#include "Font/WoffDecode.hpp"
#include "Media/Zlib.hpp"
#include "Utility/ByteStream.hpp"

namespace font {

namespace {
constexpr uint32_t kWoffSignature = 0x774F4646;  // 'wOFF'
constexpr uint32_t kWoff2Signature = 0x774F4632;  // 'wOF2'
constexpr uint32_t kMaxWoffTables = 64;

struct WoffTable {
  uint32_t tag = 0;
  uint32_t offset = 0;
  uint32_t compressed = 0;
  uint32_t original = 0;
  uint32_t checksum = 0;
  uint32_t out_offset = 0;
};
}  // namespace

bool is_woff(ConstByteView data) {
  ByteReader r(data);
  return r.be32() == kWoffSignature && r.ok();
}

bool is_woff2(ConstByteView data) {
  ByteReader r(data);
  return r.be32() == kWoff2Signature && r.ok();
}

bool decode_woff(ConstByteView woff, ByteBuffer* sfnt, uint64_t max_output) {
  ByteReader r(woff);
  if (r.be32() != kWoffSignature) return false;
  const uint32_t flavor = r.be32();
  const uint32_t length = r.be32();
  const uint32_t num_tables = r.be16();
  r.be16();  // reserved
  r.be32();  // totalSfntSize: advisory, often wrong, so the real sizes are summed below
  if (!r.skip(24) || !r.ok()) return false;  // version, metadata and private block fields
  if (length > woff.size() || num_tables == 0 || num_tables > kMaxWoffTables) return false;
  if (flavor != 0x00010000 && flavor != 0x74727565) return false;  // TrueType outlines only

  WoffTable tables[kMaxWoffTables];
  uint64_t out_offset = 12 + uint64_t{16} * num_tables;
  for (uint32_t i = 0; i < num_tables; ++i) {
    WoffTable& t = tables[i];
    t.tag = r.be32();
    t.offset = r.be32();
    t.compressed = r.be32();
    t.original = r.be32();
    t.checksum = r.be32();
    if (!r.ok()) return false;
    if (t.offset > length || t.compressed > length - t.offset || t.compressed > t.original) return false;
    if (out_offset > max_output || t.original > max_output - out_offset) return false;
    t.out_offset = static_cast<uint32_t>(out_offset);
    out_offset += t.original;
    out_offset += (4 - (out_offset & 3)) & 3;  // tables are 4-byte aligned in the sfnt
    if (out_offset > max_output) return false;
  }

  const uint64_t base = sfnt->size();
  // sfnt header and table directory
  uint32_t search_range = 1, entry_selector = 0;
  while (search_range * 2 <= num_tables) {
    search_range *= 2;
    ++entry_selector;
  }
  sfnt->put8(static_cast<uint8_t>(flavor >> 24));
  sfnt->put8(static_cast<uint8_t>(flavor >> 16));
  sfnt->put8(static_cast<uint8_t>(flavor >> 8));
  sfnt->put8(static_cast<uint8_t>(flavor));
  uint8_t header[8];
  ByteWriter w(ByteView(header, 8));
  w.be16(static_cast<uint16_t>(num_tables));
  w.be16(static_cast<uint16_t>(search_range * 16));
  w.be16(static_cast<uint16_t>(entry_selector));
  w.be16(static_cast<uint16_t>(num_tables * 16 - search_range * 16));
  if (!sfnt->append(ConstByteView(header, 8))) return false;
  for (uint32_t i = 0; i < num_tables; ++i) {
    const WoffTable& t = tables[i];
    uint8_t entry[16];
    ByteWriter e(ByteView(entry, 16));
    e.be32(t.tag);
    e.be32(t.checksum);
    e.be32(t.out_offset);
    e.be32(t.original);
    if (!sfnt->append(ConstByteView(entry, 16))) return false;
  }
  for (uint32_t i = 0; i < num_tables; ++i) {
    const WoffTable& t = tables[i];
    if (sfnt->size() - base > t.out_offset) return false;  // directory entries out of order
    if (!sfnt->put_zeros(t.out_offset - (sfnt->size() - base))) return false;
    ConstByteView data;
    if (!woff.try_subview(t.offset, t.compressed, &data)) return false;
    if (t.compressed == t.original) {
      if (!sfnt->append(data)) return false;
    } else {
      const uint64_t before = sfnt->size();
      if (!media::inflate(data, sfnt, t.original) || sfnt->size() - before != t.original) return false;
    }
  }
  // pad the last table so the file ends on a 4-byte boundary like any sfnt
  const uint64_t used = sfnt->size() - base;
  if (used > max_output || (max_output - used) < ((4 - (used & 3)) & 3)) return false;
  return sfnt->put_zeros((4 - (used & 3)) & 3);
}

bool font_to_sfnt(ConstByteView data, ByteBuffer* sfnt, uint64_t max_output) {
  if (is_woff(data)) return decode_woff(data, sfnt, max_output);
  if (is_woff2(data)) return false;
  ByteReader r(data);
  const uint32_t version = r.be32();
  if (!r.ok()) return false;
  const bool plain = version == 0x00010000 || version == 0x74727565 /* 'true' */ || version == 0x74746366 /* 'ttcf' */;
  if (!plain || data.size() > max_output) return false;
  return sfnt->append(data);
}

}  // namespace font
