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

#include "Hardware/Bus/Thunderbolt/ThunderboltXdomain.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace tb_xdp {

// (xdomain.c: tb_xdp_uuid, b638d70e-42ff-40bb-97c2-90e2c0b2ff07)
const uint8_t kProtocolUuid[16] = {0xB6, 0x38, 0xD7, 0x0E, 0x42, 0xFF, 0x40, 0xBB, 0x97, 0xC2, 0x90, 0xE2, 0xC0, 0xB2, 0xFF, 0x07};

namespace {
constexpr uint32_t kLengthMask = 0x3F, kSequenceShift = 27, kSequenceMask = 3u << 27;

void header(ByteWriter& w, uint64_t route, uint8_t sequence, uint32_t type, uint32_t total_bytes) {
  w.le32(static_cast<uint32_t>(route >> 32));
  w.le32(static_cast<uint32_t>(route));
  w.le32(((total_bytes - 12) / 4 & kLengthMask) | ((static_cast<uint32_t>(sequence) << kSequenceShift) & kSequenceMask));
  w.bytes(ConstByteView(kProtocolUuid, 16));
  w.le32(type);
}
uint32_t finish(const ByteWriter& w) { return w.ok() ? static_cast<uint32_t>(w.position()) : 0; }
}  // namespace

uint32_t build_uuid_request(uint64_t route, uint8_t sequence, ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, kUuidRequest, kUuidRequestBytes);
  return finish(w);
}

uint32_t build_uuid_response(uint64_t route, uint8_t sequence, const uint8_t uuid[16], ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, kUuidResponse, kUuidResponseBytes);
  w.bytes(ConstByteView(uuid, 16));
  w.le32(static_cast<uint32_t>(route >> 32));
  w.le32(static_cast<uint32_t>(route));
  return finish(w);
}

uint32_t build_properties_request(uint64_t route, uint8_t sequence, const uint8_t source[16], const uint8_t destination[16], uint16_t offset, ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, kPropertiesRequest, kPropertiesRequestBytes);
  w.bytes(ConstByteView(source, 16));
  w.bytes(ConstByteView(destination, 16));
  w.le16(offset);
  w.le16(0);
  return finish(w);
}

uint32_t build_error_response(uint64_t route, uint8_t sequence, uint32_t error, ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, kErrorResponse, kErrorResponseBytes);
  w.le32(error);
  return finish(w);
}

uint32_t build_properties_changed_response(uint64_t route, uint8_t sequence, ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, kPropertiesChangedResponse, kHeaderBytes);
  return finish(w);
}

uint32_t build_properties_response(uint64_t route, uint8_t sequence, const uint8_t source[16], const uint8_t destination[16], ArrayView<const uint32_t> block, uint32_t generation, uint16_t offset,
                                   ByteView out) {
  if (offset >= block.size() || block.size() > kMaxBlockWords) return 0;
  uint64_t count = block.size() - offset;
  if (count > kMaxDataWords) count = kMaxDataWords;
  ByteWriter w(out);
  header(w, route, sequence, kPropertiesResponse, static_cast<uint32_t>(kPropertiesResponseHeaderBytes + 4 * count));
  w.bytes(ConstByteView(source, 16));
  w.bytes(ConstByteView(destination, 16));
  w.le16(offset);
  w.le16(static_cast<uint16_t>(block.size()));
  w.le32(generation);
  for (uint64_t i = 0; i < count; ++i) w.le32(block[offset + i]);
  return finish(w);
}

bool parse_header(ConstByteView packet, Header* out) {
  if (packet.size() < kHeaderBytes || packet.size() % 4 != 0) return false;
  ByteReader r(packet);
  const uint32_t hi = r.le32(), lo = r.le32(), length_sn = r.le32();
  Header h;
  const bool got = r.read_into(ByteView(h.uuid, 16));
  h.type = r.le32();
  if (!r.ok() || !got) return false;
  h.route = ((static_cast<uint64_t>(hi) << 32) | lo) & ~(1ull << 63);
  h.length_words = length_sn & kLengthMask;
  h.sequence = static_cast<uint8_t>((length_sn & kSequenceMask) >> kSequenceShift);
  // the length counts the words after the route and length part
  if (12 + 4ull * h.length_words > packet.size()) return false;
  for (uint32_t i = 0; i < 16; ++i)
    if (h.uuid[i] != kProtocolUuid[i]) return false;
  *out = h;
  return true;
}

namespace {
Parsed error_of(ConstByteView packet, const Header& h, uint32_t* error) {
  if (h.type != kErrorResponse) return Parsed::Malformed;
  if (packet.size() < kErrorResponseBytes) return Parsed::Malformed;
  ByteReader r(packet);
  r.skip(kHeaderBytes);
  const uint32_t e = r.le32();
  if (!r.ok()) return Parsed::Malformed;
  if (error) *error = e;
  return Parsed::Error;
}
}  // namespace

Parsed parse_uuid_response(ConstByteView packet, UuidResponse* out, uint32_t* error) {
  Header h;
  if (!parse_header(packet, &h)) return Parsed::Malformed;
  if (h.type == kErrorResponse) return error_of(packet, h, error);
  if (h.type != kUuidResponse || packet.size() < kUuidResponseBytes) return Parsed::Malformed;
  ByteReader r(packet);
  r.skip(kHeaderBytes);
  UuidResponse u;
  const bool got = r.read_into(ByteView(u.uuid, 16));
  const uint32_t hi = r.le32(), lo = r.le32();
  if (!r.ok() || !got) return Parsed::Malformed;
  u.route = (static_cast<uint64_t>(hi) << 32) | lo;
  *out = u;
  return Parsed::Ok;
}

Parsed parse_properties_response(ConstByteView packet, PropertiesResponse* out, uint32_t* error) {
  Header h;
  if (!parse_header(packet, &h)) return Parsed::Malformed;
  if (h.type == kErrorResponse) return error_of(packet, h, error);
  if (h.type != kPropertiesResponse) return Parsed::Malformed;
  // the length covers the 72 byte header (less the 12 byte route / length part) and the data
  if (h.length_words < (kPropertiesResponseHeaderBytes - 12) / 4 || packet.size() < kPropertiesResponseHeaderBytes) return Parsed::Malformed;
  PropertiesResponse p;
  p.words = h.length_words + 3 - kPropertiesResponseHeaderBytes / 4;
  if (p.words > kMaxDataWords || packet.size() < kPropertiesResponseHeaderBytes + 4ull * p.words) return Parsed::Malformed;
  ByteReader r(packet);
  r.skip(kHeaderBytes + 32);
  p.offset = r.le16();
  p.data_length = r.le16();
  p.generation = r.le32();
  if (!r.ok()) return Parsed::Malformed;
  *out = p;
  return Parsed::Ok;
}

bool parse_properties_request(ConstByteView packet, PropertiesRequest* out) {
  Header h;
  if (!parse_header(packet, &h) || h.type != kPropertiesRequest || packet.size() < kPropertiesRequestBytes) return false;
  ByteReader r(packet);
  r.skip(kHeaderBytes);
  PropertiesRequest q;
  const bool a = r.read_into(ByteView(q.source, 16)), b = r.read_into(ByteView(q.destination, 16));
  q.offset = r.le16();
  if (!r.ok() || !a || !b) return false;
  *out = q;
  return true;
}

}  // namespace tb_xdp

// ---- property blocks

namespace {
constexpr uint32_t kRootMagic = 0x55584401, kMaxDepth = 8, kMaxEntries = 128;

uint32_t word(ArrayView<const uint32_t> block, uint64_t index) {
  uint32_t v = 0;
  block.try_get(index, &v);
  return v;
}

// The characters of a key or text: the bytes of the words, most significant first.
char byte_of(ArrayView<const uint32_t> block, uint64_t first_word, uint64_t index) { return static_cast<char>(word(block, first_word + index / 4) >> (24 - 8 * (index % 4))); }

bool parse_directory(ArrayView<const uint32_t> block, uint64_t offset, uint64_t length, bool root, uint32_t depth, List<TbPropertyEntry>& out) {
  if (depth > kMaxDepth) return false;
  uint64_t content, content_length;
  if (root) {
    content = offset + 2;
    content_length = length;
    if (content + content_length > block.size()) return false;
  } else {
    if (length < 4 || offset + length > block.size()) return false;
    content = offset + 4;                 // (the directory's UUID comes first)
    content_length = length - 4;
  }
  const uint64_t entries = content_length / 4;
  for (uint64_t i = 0; i < entries; ++i) {
    if (out.count() >= kMaxEntries) return false;
    const uint64_t at = content + 4 * i;
    const uint32_t w2 = word(block, at + 2), value = word(block, at + 3);
    const uint32_t entry_length = w2 & 0xFFFF;
    const uint8_t type = static_cast<uint8_t>(w2 >> 24);
    TbPropertyEntry e;
    for (uint32_t k = 0; k < 8; ++k) {
      const char c = byte_of(block, at, k);
      if (c == 0) break;
      e.key[k] = c;
    }
    e.depth = static_cast<uint8_t>(depth);
    switch (type) {
      case 0x44: case 0x64: case 0x74:
        // the entry points into the block: a length of at least a word, all of it inside
        if (entry_length == 0 || entry_length > block.size() || static_cast<uint64_t>(value) + entry_length > block.size()) return false;
        e.type = static_cast<TbPropertyType>(type);
        e.value = entry_length;
        if (type == 0x74) {
          const uint64_t chars = static_cast<uint64_t>(entry_length) * 4;
          for (uint64_t k = 0; k + 1 < sizeof e.text && k < chars; ++k) {
            const char c = byte_of(block, value, k);
            if (c == 0) break;
            e.text[k] = c;
          }
        }
        break;
      case 0x76:
        if (entry_length != 1) return false;
        e.type = TbPropertyType::Value;
        e.value = value;
        break;
      default:
        e.type = TbPropertyType::Unknown;
        break;
    }
    if (!out.push_back(e)) return false;
    if (e.type == TbPropertyType::Directory && !parse_directory(block, value, entry_length, false, depth + 1, out)) return false;
  }
  return true;
}
}  // namespace

bool tb_properties_parse(ArrayView<const uint32_t> block, List<TbPropertyEntry>& out) {
  out.clear();
  if (block.size() < 2 || block.size() > tb_xdp::kMaxBlockWords || word(block, 0) != kRootMagic) return false;
  const uint32_t length = word(block, 1);
  if (length > block.size()) return false;
  if (!parse_directory(block, 0, length, true, 0, out)) {
    out.clear();
    return false;
  }
  return true;
}

const TbPropertyEntry* tb_properties_find(const List<TbPropertyEntry>& entries, const char* key, TbPropertyType type) {
  for (uint64_t i = 0; i < entries.count(); ++i) {
    const TbPropertyEntry& e = entries[i];
    if (e.depth != 0 || e.type != type) continue;
    uint32_t k = 0;
    while (key[k] != 0 && e.key[k] == key[k] && k < 8) ++k;
    if (key[k] == 0 && e.key[k] == 0) return &e;
  }
  return nullptr;
}

bool tb_properties_build_local(uint32_t vendor_id, uint32_t device_id, uint32_t device_revision, const char* vendor_name, const char* device_name, List<uint32_t>& out) {
  struct Item {
    const char* key;
    const char* text;               // nullptr: a value
    uint32_t value;
  };
  const Item items[] = {{"vendorid", nullptr, vendor_id}, {"vendorid", vendor_name, 0}, {"deviceid", nullptr, device_id}, {"deviceid", device_name, 0}, {"devicerv", nullptr, device_revision}};
  constexpr uint32_t n = sizeof items / sizeof items[0];
  // (a value and a text may share a key: they are told apart by their type, as Linux does)
  out.clear();
  uint32_t data_words = 0, text_words[n] = {};
  for (uint32_t i = 0; i < n; ++i) {
    if (!items[i].text) continue;
    uint32_t len = 0;
    while (items[i].text[len] != 0 && len < 120) ++len;
    text_words[i] = (len + 1 + 3) / 4;
    data_words += text_words[i];
  }
  const uint32_t total = 2 + 4 * n + data_words;
  if (total > tb_xdp::kMaxBlockWords || !out.reserve(total)) return false;
  for (uint32_t i = 0; i < total; ++i) out.push_back(0);
  out[0] = kRootMagic;
  out[1] = 4 * n;
  uint32_t data_at = 2 + 4 * n;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t at = 2 + 4 * i;
    uint8_t key[8] = {};
    for (uint32_t k = 0; k < 8 && items[i].key[k] != 0; ++k) key[k] = static_cast<uint8_t>(items[i].key[k]);
    out[at] = (uint32_t{key[0]} << 24) | (uint32_t{key[1]} << 16) | (uint32_t{key[2]} << 8) | key[3];
    out[at + 1] = (uint32_t{key[4]} << 24) | (uint32_t{key[5]} << 16) | (uint32_t{key[6]} << 8) | key[7];
    if (!items[i].text) {
      out[at + 2] = 1u | (0x76u << 24);
      out[at + 3] = items[i].value;
    } else {
      out[at + 2] = text_words[i] | (0x74u << 24);
      out[at + 3] = data_at;
      for (uint32_t k = 0; k < text_words[i] * 4; ++k) {
        const uint8_t c = k < 120 && items[i].text[k] != 0 && k < text_words[i] * 4 - 1 ? static_cast<uint8_t>(items[i].text[k]) : 0;
        out[data_at + k / 4] |= uint32_t{c} << (24 - 8 * (k % 4));
        if (c == 0) break;
      }
      data_at += text_words[i];
    }
  }
  return true;
}

}  // namespace hardware
