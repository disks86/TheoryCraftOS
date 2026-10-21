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

#include "Media/Zlib.hpp"
#include "Utility/AllocatedView.hpp"

namespace media {

uint32_t adler32(ConstByteView data, uint32_t adler) {
  uint32_t a = adler & 0xFFFF, b = adler >> 16;
  uint64_t i = 0;
  while (i < data.size()) {
    const uint64_t chunk = data.size() - i < 5552 ? data.size() - i : 5552;  // the most that cannot overflow 32 bits
    for (uint64_t k = 0; k < chunk; ++k) {
      a += data[i + k];
      b += a;
    }
    a %= 65521;
    b %= 65521;
    i += chunk;
  }
  return (b << 16) | a;
}

namespace {
constexpr uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistanceBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// A canonical Huffman code for decoding, in the compact form of zlib's "puff": how many codes there are of each length and
// the symbols in code order.
struct HuffmanCode {
  uint16_t count[16];
  uint16_t symbol[288];
};

// Builds the code from the length of each symbol's code. Returns <0 if over-subscribed, 0 if complete, >0 if incomplete.
int build(HuffmanCode* h, const uint8_t* lengths, uint32_t n) {
  for (int len = 0; len < 16; ++len) h->count[len] = 0;
  for (uint32_t i = 0; i < n; ++i) ++h->count[lengths[i]];
  if (h->count[0] == n) return 0;  // no codes: complete (an empty distance code is allowed)
  int left = 1;
  for (int len = 1; len < 16; ++len) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return left;
  }
  uint16_t offsets[16];
  offsets[1] = 0;
  for (int len = 1; len < 15; ++len) offsets[len + 1] = static_cast<uint16_t>(offsets[len] + h->count[len]);
  for (uint32_t i = 0; i < n; ++i)
    if (lengths[i]) h->symbol[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
  return left;
}

struct Inflater {
  ConstByteView in;
  uint64_t position = 0;
  uint32_t bit_buffer = 0;
  int bit_count = 0;
  ByteBuffer* out;
  uint64_t out_start;
  uint64_t max_output;
  bool failed = false;

  uint32_t bits(int need) {
    uint32_t value = bit_buffer;
    while (bit_count < need) {
      if (position >= in.size()) {
        failed = true;
        return 0;
      }
      value |= static_cast<uint32_t>(in[position++]) << bit_count;
      bit_count += 8;
    }
    bit_buffer = value >> need;
    bit_count -= need;
    return value & ((1u << need) - 1);
  }

  int decode(const HuffmanCode& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= static_cast<int>(bits(1));
      if (failed) return -1;
      const int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    failed = true;  // ran out of codes: not a valid code
    return -1;
  }

  bool put(uint8_t byte) {
    if (out->size() - out_start >= max_output || !out->put8(byte)) return false;
    return true;
  }

  // Copies `length` bytes from `distance` back in what this stream has written.
  bool copy(uint32_t length, uint32_t distance) {
    if (distance > out->size() - out_start) return false;
    for (uint32_t i = 0; i < length; ++i) {
      const uint8_t byte = out->view()[out->size() - distance];
      if (!put(byte)) return false;
    }
    return true;
  }

  bool codes(const HuffmanCode& lengths, const HuffmanCode& distances) {
    for (;;) {
      int symbol = decode(lengths);
      if (symbol < 0) return false;
      if (symbol < 256) {
        if (!put(static_cast<uint8_t>(symbol))) return false;
      } else if (symbol == 256) {
        return true;
      } else {
        symbol -= 257;
        if (symbol >= 29) return false;
        const uint32_t length = kLengthBase[symbol] + bits(kLengthExtra[symbol]);
        const int d = decode(distances);
        if (failed || d < 0 || d >= 30) return false;
        const uint32_t distance = kDistanceBase[d] + bits(kDistanceExtra[d]);
        if (failed || !copy(length, distance)) return false;
      }
    }
  }

  bool stored() {
    bit_buffer = 0;
    bit_count = 0;
    if (position + 4 > in.size()) return false;
    const uint32_t len = in[position] | (in[position + 1] << 8), inverse = in[position + 2] | (in[position + 3] << 8);
    position += 4;
    if (len != (~inverse & 0xFFFF) || position + len > in.size()) return false;
    for (uint32_t i = 0; i < len; ++i)
      if (!put(in[position + i])) return false;
    position += len;
    return true;
  }

  bool fixed() {
    uint8_t lengths[288];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    HuffmanCode lit, dist;
    build(&lit, lengths, 288);
    uint8_t d[30];
    for (int i = 0; i < 30; ++i) d[i] = 5;
    build(&dist, d, 30);
    return codes(lit, dist);
  }

  bool dynamic() {
    const uint32_t nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
    if (failed || nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320];
    for (int i = 0; i < 19; ++i) lengths[kCodeLengthOrder[i]] = i < static_cast<int>(ncode) ? static_cast<uint8_t>(bits(3)) : 0;
    if (failed) return false;
    HuffmanCode length_code;
    if (build(&length_code, lengths, 19) != 0) return false;
    uint32_t index = 0;
    while (index < nlen + ndist) {
      int symbol = decode(length_code);
      if (symbol < 0) return false;
      if (symbol < 16) {
        lengths[index++] = static_cast<uint8_t>(symbol);
      } else {
        uint8_t previous = 0;
        uint32_t repeat;
        if (symbol == 16) {
          if (index == 0) return false;
          previous = lengths[index - 1];
          repeat = 3 + bits(2);
        } else if (symbol == 17) {
          repeat = 3 + bits(3);
        } else {
          repeat = 11 + bits(7);
        }
        if (failed || index + repeat > nlen + ndist) return false;
        while (repeat--) lengths[index++] = previous;
      }
    }
    if (lengths[256] == 0) return false;  // there must be an end-of-block code
    HuffmanCode lit, dist;
    int err = build(&lit, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lit.count[0] != 1)) return false;
    err = build(&dist, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dist.count[0] != 1)) return false;
    return codes(lit, dist);
  }
};
}  // namespace

bool inflate(ConstByteView in, ByteBuffer* out, uint64_t max_output, bool raw, uint64_t* consumed) {
  Inflater z{in, 0, 0, 0, out, out->size(), max_output};
  if (!raw) {
    if (in.size() < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return false;  // deflate, no preset dictionary
    z.position = 2;
  }
  bool last;
  do {
    last = z.bits(1);
    const uint32_t type = z.bits(2);
    if (z.failed) return false;
    bool ok;
    switch (type) {
      case 0: ok = z.stored(); break;
      case 1: ok = z.fixed(); break;
      case 2: ok = z.dynamic(); break;
      default: ok = false; break;
    }
    if (!ok || z.failed) return false;
  } while (!last);
  if (!raw) {
    // The checksum follows, on a byte boundary.
    if (z.position + 4 > in.size()) return false;
    const uint32_t want = (uint32_t{in[z.position]} << 24) | (uint32_t{in[z.position + 1]} << 16) | (uint32_t{in[z.position + 2]} << 8) | in[z.position + 3];
    z.position += 4;
    if (adler32(out->view().subview(z.out_start)) != want) return false;
  }
  if (consumed) *consumed = z.position;
  return true;
}

bool inflate_message(ConstByteView in, ByteBuffer* out, uint64_t max_output) {
  Inflater z{in, 0, 0, 0, out, out->size(), max_output};
  while (z.position < in.size()) {   // (the data ends at a block boundary: only padding bits can be left)
    const bool last = z.bits(1) != 0;
    const uint32_t type = z.bits(2);
    if (z.failed) return false;
    bool ok;
    switch (type) {
      case 0: ok = z.stored(); break;
      case 1: ok = z.fixed(); break;
      case 2: ok = z.dynamic(); break;
      default: ok = false; break;
    }
    if (!ok || z.failed) return false;
    if (last) return true;
  }
  return true;
}

// ---- Deflate ---------------------------------------------------------------------------

namespace {
struct BitWriter {
  ByteBuffer* out;
  uint32_t buffer = 0;
  int count = 0;
  bool ok = true;
  void bits(uint32_t value, int n) {  // least significant first
    buffer |= value << count;
    count += n;
    while (count >= 8) {
      ok = ok && out->put8(static_cast<uint8_t>(buffer));
      buffer >>= 8;
      count -= 8;
    }
  }
  void code(uint32_t code, int n) {  // Huffman codes go most significant bit first
    uint32_t reversed = 0;
    for (int i = 0; i < n; ++i) reversed |= ((code >> i) & 1) << (n - 1 - i);
    bits(reversed, n);
  }
  void flush() {
    if (count > 0) bits(0, 8 - count);
  }
};

void put_literal(BitWriter& w, uint32_t symbol) {
  if (symbol < 144) w.code(0x30 + symbol, 8);
  else if (symbol < 256) w.code(0x190 + (symbol - 144), 9);
  else if (symbol < 280) w.code(symbol - 256, 7);
  else w.code(0xC0 + (symbol - 280), 8);
}

void put_match(BitWriter& w, uint32_t length, uint32_t distance) {
  int l = 28;
  while (kLengthBase[l] > length) --l;
  put_literal(w, 257 + l);
  w.bits(length - kLengthBase[l], kLengthExtra[l]);
  int d = 29;
  while (kDistanceBase[d] > distance) --d;
  w.code(static_cast<uint32_t>(d), 5);
  w.bits(distance - kDistanceBase[d], kDistanceExtra[d]);
}

constexpr uint32_t kHashBits = 15, kHashSize = 1u << kHashBits, kWindow = 32768;
uint32_t hash3(const uint8_t* p) { return ((uint32_t{p[0]} << 10) ^ (uint32_t{p[1]} << 5) ^ p[2]) * 2654435761u >> (32 - kHashBits); }
}  // namespace

static bool deflate_once(ConstByteView in, ByteBuffer* out, uint32_t level, allocators::IAllocator& allocator, bool raw, bool sync, uint32_t window = 32768);

bool deflate_message(ConstByteView in, ByteBuffer* out, uint32_t level, allocators::IAllocator& allocator, uint32_t window_bits) {
  if (window_bits < 8) window_bits = 8;
  if (window_bits > 15) window_bits = 15;
  if (!deflate_once(in, out, level, allocator, true, true, 1u << window_bits)) return false;
  out->truncate(out->size() - 4);   // (the 00 00 FF FF that ends a sync flush is left off the wire)
  return true;
}

bool deflate(ConstByteView in, ByteBuffer* out, uint32_t level, allocators::IAllocator& allocator, bool raw) {
  const uint64_t start = out->size();
  if (!deflate_once(in, out, level, allocator, raw, false)) return false;
  // Data that does not compress (a photograph's noise, already compressed input) is better stored.
  if (level > 0 && out->size() - start > in.size() + 5 * (in.size() / 65535 + 1) + 6) {
    out->truncate(start);
    return deflate_once(in, out, 0, allocator, raw, false);
  }
  return true;
}

static bool deflate_once(ConstByteView in, ByteBuffer* out, uint32_t level, allocators::IAllocator& allocator, bool raw, bool sync, uint32_t window) {
  if (!raw && !(out->put8(0x78) && out->put8(level <= 1 ? 0x01 : level < 6 ? 0x5E : level == 6 ? 0x9C : 0xDA))) return false;
  BitWriter w{out};
  if (level == 0) {  // stored blocks of up to 65535 bytes
    uint64_t at = 0;
    do {
      const uint64_t n = in.size() - at < 65535 ? in.size() - at : 65535;
      const bool last = at + n >= in.size();
      w.bits(last && !sync ? 1 : 0, 1);
      w.bits(0, 2);
      w.flush();
      w.ok = w.ok && out->put16(static_cast<uint32_t>(n)) && out->put16(~static_cast<uint32_t>(n) & 0xFFFF) && out->append(in.subview(at, n));
      at += n;
    } while (at < in.size());
  } else {
    // One fixed-Huffman block with LZ77 matches found through hash chains.
    ArrayView<int32_t> head = allocate_view<int32_t>(allocator, kHashSize);
    ArrayView<int32_t> chain = allocate_view<int32_t>(allocator, in.size() ? in.size() : 1);
    if (head.empty() || chain.empty()) {
      release_view(allocator, head);
      release_view(allocator, chain);
      return false;
    }
    head.fill(-1);
    const uint32_t max_chain = level == 1 ? 4 : level < 4 ? 16 : level < 7 ? 48 : level == 8 ? 128 : 256;
    w.bits(sync ? 0 : 1, 1);  // final block (not with a sync flush: more follows, or the empty stored block)
    w.bits(1, 2);  // fixed Huffman
    const uint8_t* data = in.data();
    const uint64_t n = in.size();
    uint64_t i = 0;
    auto insert = [&](uint64_t pos) {
      if (pos + 2 < n) {
        const uint32_t h = hash3(data + pos);
        chain[pos] = head[h];
        head[h] = static_cast<int32_t>(pos);
      }
    };
    while (i < n) {
      uint32_t best_length = 0, best_distance = 0;
      if (i + 2 < n) {
        const uint32_t h = hash3(data + i);
        int32_t candidate = head[h];
        uint32_t tries = max_chain;
        const uint32_t limit = n - i < 258 ? static_cast<uint32_t>(n - i) : 258;
        while (candidate >= 0 && tries-- > 0 && i - static_cast<uint64_t>(candidate) <= window) {
          uint32_t length = 0;
          while (length < limit && data[candidate + length] == data[i + length]) ++length;
          if (length > best_length) {
            best_length = length;
            best_distance = static_cast<uint32_t>(i - static_cast<uint64_t>(candidate));
            if (length == limit) break;
          }
          candidate = chain[candidate];
        }
      }
      if (best_length >= 3) {
        put_match(w, best_length, best_distance);
        for (uint32_t k = 0; k < best_length; ++k) insert(i + k);
        i += best_length;
      } else {
        put_literal(w, data[i]);
        insert(i);
        ++i;
      }
    }
    w.code(0, 7);  // end of block (symbol 256)
    release_view(allocator, head);
    release_view(allocator, chain);
  }
  if (sync) {   // the empty stored block that a sync flush ends with: header bits, padding, then 00 00 FF FF
    w.bits(0, 1);
    w.bits(0, 2);
    w.flush();
    w.ok = w.ok && out->put16(0) && out->put16(0xFFFF);
  }
  w.flush();
  if (!raw) {
    const uint32_t a = adler32(in);
    w.ok = w.ok && out->put8(static_cast<uint8_t>(a >> 24)) && out->put8(static_cast<uint8_t>(a >> 16)) && out->put8(static_cast<uint8_t>(a >> 8)) &&
           out->put8(static_cast<uint8_t>(a));
  }
  return w.ok;
}

}  // namespace media
