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

#include "Network/Quic/Qpack.hpp"
#include "Utility/ByteStream.hpp"

namespace network::h3 {

namespace {

struct StaticEntry {
  const char* name;
  const char* value;
};
constexpr StaticEntry kStaticTable[99] = {
    {":authority", ""},
    {":path", "/"},
    {"age", "0"},
    {"content-disposition", ""},
    {"content-length", "0"},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"referer", ""},
    {"set-cookie", ""},
    {":method", "CONNECT"},
    {":method", "DELETE"},
    {":method", "GET"},
    {":method", "HEAD"},
    {":method", "OPTIONS"},
    {":method", "POST"},
    {":method", "PUT"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "103"},
    {":status", "200"},
    {":status", "304"},
    {":status", "404"},
    {":status", "503"},
    {"accept", "*/*"},
    {"accept", "application/dns-message"},
    {"accept-encoding", "gzip, deflate, br"},
    {"accept-ranges", "bytes"},
    {"access-control-allow-headers", "cache-control"},
    {"access-control-allow-headers", "content-type"},
    {"access-control-allow-origin", "*"},
    {"cache-control", "max-age=0"},
    {"cache-control", "max-age=2592000"},
    {"cache-control", "max-age=604800"},
    {"cache-control", "no-cache"},
    {"cache-control", "no-store"},
    {"cache-control", "public, max-age=31536000"},
    {"content-encoding", "br"},
    {"content-encoding", "gzip"},
    {"content-type", "application/dns-message"},
    {"content-type", "application/javascript"},
    {"content-type", "application/json"},
    {"content-type", "application/x-www-form-urlencoded"},
    {"content-type", "image/gif"},
    {"content-type", "image/jpeg"},
    {"content-type", "image/png"},
    {"content-type", "text/css"},
    {"content-type", "text/html; charset=utf-8"},
    {"content-type", "text/plain"},
    {"content-type", "text/plain;charset=utf-8"},
    {"range", "bytes=0-"},
    {"strict-transport-security", "max-age=31536000"},
    {"strict-transport-security", "max-age=31536000; includesubdomains"},
    {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
    {"vary", "accept-encoding"},
    {"vary", "origin"},
    {"x-content-type-options", "nosniff"},
    {"x-xss-protection", "1; mode=block"},
    {":status", "100"},
    {":status", "204"},
    {":status", "206"},
    {":status", "302"},
    {":status", "400"},
    {":status", "403"},
    {":status", "421"},
    {":status", "425"},
    {":status", "500"},
    {"accept-language", ""},
    {"access-control-allow-credentials", "FALSE"},
    {"access-control-allow-credentials", "TRUE"},
    {"access-control-allow-headers", "*"},
    {"access-control-allow-methods", "get"},
    {"access-control-allow-methods", "get, post, options"},
    {"access-control-allow-methods", "options"},
    {"access-control-expose-headers", "content-length"},
    {"access-control-request-headers", "content-type"},
    {"access-control-request-method", "get"},
    {"access-control-request-method", "post"},
    {"alt-svc", "clear"},
    {"authorization", ""},
    {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
    {"early-data", "1"},
    {"expect-ct", ""},
    {"forwarded", ""},
    {"if-range", ""},
    {"origin", ""},
    {"purpose", "prefetch"},
    {"server", ""},
    {"timing-allow-origin", "*"},
    {"upgrade-insecure-requests", "1"},
    {"user-agent", ""},
    {"x-forwarded-for", ""},
    {"x-frame-options", "deny"},
    {"x-frame-options", "sameorigin"},
};

// The Huffman code of RFC 7541 Appendix B: the code and its length for each byte value and for EOS (256).
constexpr uint32_t kHuffmanCodes[257] = {
    0x1ff8, 0x7fffd8, 0xfffffe2, 0xfffffe3, 0xfffffe4, 0xfffffe5, 0xfffffe6, 0xfffffe7,
    0xfffffe8, 0xffffea, 0x3ffffffc, 0xfffffe9, 0xfffffea, 0x3ffffffd, 0xfffffeb, 0xfffffec,
    0xfffffed, 0xfffffee, 0xfffffef, 0xffffff0, 0xffffff1, 0xffffff2, 0x3ffffffe, 0xffffff3,
    0xffffff4, 0xffffff5, 0xffffff6, 0xffffff7, 0xffffff8, 0xffffff9, 0xffffffa, 0xffffffb,
    0x14, 0x3f8, 0x3f9, 0xffa, 0x1ff9, 0x15, 0xf8, 0x7fa,
    0x3fa, 0x3fb, 0xf9, 0x7fb, 0xfa, 0x16, 0x17, 0x18,
    0x0, 0x1, 0x2, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
    0x1e, 0x1f, 0x5c, 0xfb, 0x7ffc, 0x20, 0xffb, 0x3fc,
    0x1ffa, 0x21, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
    0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72,
    0xfc, 0x73, 0xfd, 0x1ffb, 0x7fff0, 0x1ffc, 0x3ffc, 0x22,
    0x7ffd, 0x3, 0x23, 0x4, 0x24, 0x5, 0x25, 0x26,
    0x27, 0x6, 0x74, 0x75, 0x28, 0x29, 0x2a, 0x7,
    0x2b, 0x76, 0x2c, 0x8, 0x9, 0x2d, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7ffe, 0x7fc, 0x3ffd, 0x1ffd, 0xffffffc,
    0xfffe6, 0x3fffd2, 0xfffe7, 0xfffe8, 0x3fffd3, 0x3fffd4, 0x3fffd5, 0x7fffd9,
    0x3fffd6, 0x7fffda, 0x7fffdb, 0x7fffdc, 0x7fffdd, 0x7fffde, 0xffffeb, 0x7fffdf,
    0xffffec, 0xffffed, 0x3fffd7, 0x7fffe0, 0xffffee, 0x7fffe1, 0x7fffe2, 0x7fffe3,
    0x7fffe4, 0x1fffdc, 0x3fffd8, 0x7fffe5, 0x3fffd9, 0x7fffe6, 0x7fffe7, 0xffffef,
    0x3fffda, 0x1fffdd, 0xfffe9, 0x3fffdb, 0x3fffdc, 0x7fffe8, 0x7fffe9, 0x1fffde,
    0x7fffea, 0x3fffdd, 0x3fffde, 0xfffff0, 0x1fffdf, 0x3fffdf, 0x7fffeb, 0x7fffec,
    0x1fffe0, 0x1fffe1, 0x3fffe0, 0x1fffe2, 0x7fffed, 0x3fffe1, 0x7fffee, 0x7fffef,
    0xfffea, 0x3fffe2, 0x3fffe3, 0x3fffe4, 0x7ffff0, 0x3fffe5, 0x3fffe6, 0x7ffff1,
    0x3ffffe0, 0x3ffffe1, 0xfffeb, 0x7fff1, 0x3fffe7, 0x7ffff2, 0x3fffe8, 0x1ffffec,
    0x3ffffe2, 0x3ffffe3, 0x3ffffe4, 0x7ffffde, 0x7ffffdf, 0x3ffffe5, 0xfffff1, 0x1ffffed,
    0x7fff2, 0x1fffe3, 0x3ffffe6, 0x7ffffe0, 0x7ffffe1, 0x3ffffe7, 0x7ffffe2, 0xfffff2,
    0x1fffe4, 0x1fffe5, 0x3ffffe8, 0x3ffffe9, 0xffffffd, 0x7ffffe3, 0x7ffffe4, 0x7ffffe5,
    0xfffec, 0xfffff3, 0xfffed, 0x1fffe6, 0x3fffe9, 0x1fffe7, 0x1fffe8, 0x7ffff3,
    0x3fffea, 0x3fffeb, 0x1ffffee, 0x1ffffef, 0xfffff4, 0xfffff5, 0x3ffffea, 0x7ffff4,
    0x3ffffeb, 0x7ffffe6, 0x3ffffec, 0x3ffffed, 0x7ffffe7, 0x7ffffe8, 0x7ffffe9, 0x7ffffea,
    0x7ffffeb, 0xffffffe, 0x7ffffec, 0x7ffffed, 0x7ffffee, 0x7ffffef, 0x7fffff0, 0x3ffffee,
    0x3fffffff,
};
constexpr uint8_t kHuffmanBits[257] = {
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 30, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 6, 10, 10, 12, 13, 6, 8, 11, 10, 10, 8, 11, 8, 6, 6, 6,
    5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8, 15, 6, 12, 10, 13, 6, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6,
    15, 5, 6, 5, 6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5, 6, 7, 6, 5, 5, 6, 7, 7,
    7, 7, 7, 15, 11, 14, 13, 28, 20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
    24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24, 22, 21, 20, 22, 22, 23, 23, 21,
    23, 22, 22, 24, 21, 22, 23, 23, 21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
    26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25, 19, 21, 26, 27, 27, 26, 27, 24,
    21, 21, 26, 26, 28, 27, 27, 27, 20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26, 30,
};

// A decoding tree made from the table the first time it is needed: node n has children next[n][0] and next[n][1]; a negative number is a leaf (-1 - symbol).
struct HuffmanTree {
  int16_t next[520][2];
  int count = 1;
  HuffmanTree() {
    for (auto& n : next) n[0] = n[1] = 0;
    for (int symbol = 0; symbol < 257; ++symbol) {
      int node = 0;
      for (int bit = kHuffmanBits[symbol] - 1; bit >= 0; --bit) {
        const int b = (kHuffmanCodes[symbol] >> bit) & 1;
        if (bit == 0) {
          next[node][b] = static_cast<int16_t>(-1 - symbol);
        } else {
          if (next[node][b] == 0) next[node][b] = static_cast<int16_t>(count++);
          node = next[node][b];
        }
      }
    }
  }
};
const HuffmanTree& huffman_tree() {
  static const HuffmanTree tree;
  return tree;
}

// An integer with an N-bit prefix (RFC 7541 5.1): `first` holds the prefix bits already read from the first byte.
bool read_prefixed(ByteReader& r, uint32_t first_byte, uint32_t prefix_bits, uint64_t* out) {
  const uint32_t max = (1u << prefix_bits) - 1;
  uint64_t value = first_byte & max;
  if (value < max) {
    *out = value;
    return true;
  }
  uint32_t shift = 0;
  for (;;) {
    const uint8_t b = r.u8();
    if (!r.ok() || shift > 56) return false;
    value += static_cast<uint64_t>(b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0) break;
  }
  *out = value;
  return true;
}

bool write_prefixed(ByteBuffer* out, uint8_t first_bits, uint32_t prefix_bits, uint64_t value) {
  const uint32_t max = (1u << prefix_bits) - 1;
  if (value < max) return out->put8(static_cast<uint8_t>(first_bits | value));
  if (!out->put8(static_cast<uint8_t>(first_bits | max))) return false;
  value -= max;
  while (value >= 128) {
    if (!out->put8(static_cast<uint8_t>((value & 0x7F) | 0x80))) return false;
    value >>= 7;
  }
  return out->put8(static_cast<uint8_t>(value));
}

// A string literal whose length has a `prefix_bits` prefix (the Huffman flag is the bit above it).
bool read_string(ByteReader& r, uint32_t first_byte, uint32_t prefix_bits, uint64_t max_length, String* out) {
  const bool huffman = (first_byte >> prefix_bits) & 1;
  uint64_t length = 0;
  if (!read_prefixed(r, first_byte, prefix_bits, &length) || length > r.remaining() || length > (1u << 20)) return false;
  const ConstByteView bytes = r.take(length);
  out->clear();
  if (huffman) return huffman_decode(bytes, out, max_length);
  if (length > max_length) return false;
  return out->append(StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool write_string(ByteBuffer* out, StringView text, uint8_t first_bits, uint32_t prefix_bits) {
  return write_prefixed(out, first_bits, prefix_bits, text.size()) && out->append(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

}  // namespace

bool HeaderList::add(StringView name, StringView value) {
  Entry e{static_cast<uint32_t>(pool_.size()), static_cast<uint32_t>(name.size()), 0, static_cast<uint32_t>(value.size())};
  if (!pool_.append(name)) return false;
  e.value_at = static_cast<uint32_t>(pool_.size());
  return pool_.append(value) && entries_.push_back(e);
}

StringView HeaderList::find(StringView name) const {
  for (uint64_t i = 0; i < entries_.count(); ++i)
    if (this->name(i).equals(name)) return value(i);
  return StringView();
}

bool huffman_decode(ConstByteView in, String* out, uint64_t max_length) {
  const HuffmanTree& tree = huffman_tree();
  int node = 0;
  uint32_t pending_bits = 0;     // bits since the last complete symbol
  bool all_ones = true;
  for (uint64_t i = 0; i < in.size(); ++i) {
    for (int bit = 7; bit >= 0; --bit) {
      const int b = (in[i] >> bit) & 1;
      const int16_t child = tree.next[node][b];
      ++pending_bits;
      if (!b) all_ones = false;
      if (child < 0) {
        const int symbol = -1 - child;
        if (symbol == 256 || out->size() >= max_length) return false;   // EOS inside the string
        if (!out->push_back(static_cast<char>(symbol))) return false;
        node = 0;
        pending_bits = 0;
        all_ones = true;
      } else if (child == 0) {
        return false;
      } else {
        node = child;
      }
    }
  }
  // what is left must be the start of EOS: fewer than 8 bits, all ones
  return pending_bits < 8 && all_ones;
}

uint64_t huffman_encoded_size(StringView text) {
  uint64_t bits = 0;
  for (uint64_t i = 0; i < text.size(); ++i) bits += kHuffmanBits[static_cast<uint8_t>(text[i])];
  return (bits + 7) / 8;
}

bool huffman_encode(StringView text, ByteBuffer* out) {
  uint64_t accumulator = 0;
  uint32_t bits = 0;
  for (uint64_t i = 0; i < text.size(); ++i) {
    const uint8_t symbol = static_cast<uint8_t>(text[i]);
    accumulator = (accumulator << kHuffmanBits[symbol]) | kHuffmanCodes[symbol];
    bits += kHuffmanBits[symbol];
    while (bits >= 8) {
      bits -= 8;
      if (!out->put8(static_cast<uint8_t>(accumulator >> bits))) return false;
    }
    accumulator &= (uint64_t{1} << bits) - 1;
  }
  if (bits > 0) return out->put8(static_cast<uint8_t>((accumulator << (8 - bits)) | ((1u << (8 - bits)) - 1)));
  return true;
}

bool qpack_encode(const HeaderList& fields, ByteBuffer* out) {
  if (!out->put8(0) || !out->put8(0)) return false;   // Required Insert Count 0, Delta Base 0
  for (uint64_t i = 0; i < fields.count(); ++i) {
    const StringView name = fields.name(i), value = fields.value(i);
    int full = -1, named = -1;
    for (int k = 0; k < 99; ++k) {
      if (!name.equals(kStaticTable[k].name)) continue;
      if (named < 0) named = k;
      if (value.equals(kStaticTable[k].value)) {
        full = k;
        break;
      }
    }
    if (full >= 0) {
      if (!write_prefixed(out, 0xC0, 6, static_cast<uint64_t>(full))) return false;   // indexed, static table
    } else if (named >= 0) {
      if (!write_prefixed(out, 0x50, 4, static_cast<uint64_t>(named)) || !write_string(out, value, 0x00, 7)) return false;   // literal, name from the static table
    } else {
      if (!write_string(out, name, 0x20, 3) || !write_string(out, value, 0x00, 7)) return false;   // literal name
    }
  }
  return true;
}

QpackResult qpack_decode(ConstByteView section, HeaderList* fields, uint64_t max_bytes) {
  ByteReader r(section);
  uint64_t required = 0, delta = 0;
  const uint8_t b0 = r.u8();
  if (!r.ok() || !read_prefixed(r, b0, 8, &required)) return QpackResult::Malformed;
  const uint8_t b1 = r.u8();
  if (!r.ok() || !read_prefixed(r, b1, 7, &delta)) return QpackResult::Malformed;
  if (required != 0) return QpackResult::DynamicTable;
  uint64_t total = 0;
  String name(fields->allocator()), value(fields->allocator());
  while (!r.at_end()) {
    const uint8_t first = r.u8();
    name.clear();
    value.clear();
    if (first & 0x80) {   // indexed field line
      uint64_t index = 0;
      if (!read_prefixed(r, first, 6, &index)) return QpackResult::Malformed;
      if (!(first & 0x40)) return QpackResult::DynamicTable;
      if (index >= 99) return QpackResult::Malformed;
      if (!name.assign(kStaticTable[index].name) || !value.assign(kStaticTable[index].value)) return QpackResult::OutOfMemory;
    } else if (first & 0x40) {   // literal field line with name reference
      uint64_t index = 0;
      if (!read_prefixed(r, first, 4, &index)) return QpackResult::Malformed;
      if (!(first & 0x10)) return QpackResult::DynamicTable;
      if (index >= 99) return QpackResult::Malformed;
      if (!name.assign(kStaticTable[index].name)) return QpackResult::OutOfMemory;
      const uint8_t v = r.u8();
      if (!r.ok() || !read_string(r, v, 7, max_bytes, &value)) return QpackResult::Malformed;
    } else if (first & 0x20) {   // literal field line with literal name
      if (!read_string(r, first, 3, max_bytes, &name)) return QpackResult::Malformed;
      const uint8_t v = r.u8();
      if (!r.ok() || !read_string(r, v, 7, max_bytes, &value)) return QpackResult::Malformed;
    } else {
      return QpackResult::DynamicTable;   // post-base forms
    }
    total += name.size() + value.size() + 32;
    if (total > max_bytes) return QpackResult::TooLarge;
    if (!fields->add(name.view(), value.view())) return QpackResult::OutOfMemory;
  }
  return r.ok() ? QpackResult::Ok : QpackResult::Malformed;
}

}  // namespace network::h3
