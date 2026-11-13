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

// Zstandard decompression (RFC 8878). Everything read from the input goes through checked views and the bit readers
// below, which return zero bits past either end of their stream; every count and offset taken from the input is
// checked against the space it refers to before it is used.

#include "Media/Zstd.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

namespace {

constexpr uint64_t kP1 = 11400714785074694791ull, kP2 = 14029467366897019727ull, kP3 = 1609587929392839161ull,
                   kP4 = 9650029242287828579ull, kP5 = 2870177450012600261ull;

uint64_t rotl(uint64_t v, uint32_t r) { return (v << r) | (v >> (64 - r)); }

// Little-endian load of `n` (at most 8) bytes at `at`; bytes past the end of `data` read as zero.
uint64_t load_le(ConstByteView data, uint64_t at, uint32_t n) {
  uint64_t value = 0;
  if (at >= data.size()) return 0;
  const uint64_t available = data.size() - at;
  const uint32_t count = available < n ? static_cast<uint32_t>(available) : n;
  for (uint32_t i = 0; i < count; ++i) value |= static_cast<uint64_t>(data[at + i]) << (8 * i);
  return value;
}

uint64_t xxh_round(uint64_t acc, uint64_t input) {
  acc += input * kP2;
  acc = rotl(acc, 31);
  return acc * kP1;
}

uint64_t xxh_merge(uint64_t acc, uint64_t value) {
  acc ^= xxh_round(0, value);
  return acc * kP1 + kP4;
}

}  // namespace

uint64_t xxh64(ConstByteView data, uint64_t seed) {
  const uint64_t length = data.size();
  uint64_t i = 0, h;
  if (length >= 32) {
    uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
    for (; length - i >= 32; i += 32) {
      v1 = xxh_round(v1, load_le(data, i, 8));
      v2 = xxh_round(v2, load_le(data, i + 8, 8));
      v3 = xxh_round(v3, load_le(data, i + 16, 8));
      v4 = xxh_round(v4, load_le(data, i + 24, 8));
    }
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    h = xxh_merge(h, v1);
    h = xxh_merge(h, v2);
    h = xxh_merge(h, v3);
    h = xxh_merge(h, v4);
  } else {
    h = seed + kP5;
  }
  h += length;
  for (; length - i >= 8; i += 8) {
    h ^= xxh_round(0, load_le(data, i, 8));
    h = rotl(h, 27) * kP1 + kP4;
  }
  if (length - i >= 4) {
    h ^= load_le(data, i, 4) * kP1;
    h = rotl(h, 23) * kP2 + kP3;
    i += 4;
  }
  for (; i < length; ++i) {
    h ^= data[i] * kP5;
    h = rotl(h, 11) * kP1;
  }
  h ^= h >> 33;
  h *= kP2;
  h ^= h >> 29;
  h *= kP3;
  h ^= h >> 32;
  return h;
}

namespace {

constexpr uint32_t kFrameMagic = 0xFD2FB528u;
constexpr uint32_t kSkippableMagic = 0x184D2A50u;  // ... to 0x184D2A5F
constexpr uint64_t kMaxBlockSize = 128 * 1024;
constexpr uint32_t kMaxHuffmanBits = 11;

uint32_t highest_bit(uint64_t v) { return 63 - static_cast<uint32_t>(__builtin_clzll(v)); }  // v != 0

// `n` (at most 57) bits of `data` read as one little-endian number, starting at bit `start`; bits outside the data
// (before bit 0 or past the end) read as zero.
uint64_t bits_at(ConstByteView data, int64_t start, uint32_t n) {
  if (n == 0 || start + static_cast<int64_t>(n) <= 0) return 0;
  uint64_t value;
  if (start >= 0) {
    const uint64_t s = static_cast<uint64_t>(start);
    value = load_le(data, s >> 3, 8) >> (s & 7);
  } else {
    value = load_le(data, 0, 8) << static_cast<uint32_t>(-start);
  }
  return value & ((uint64_t{1} << n) - 1);
}

// A bit stream read forwards from bit 0 (normalized-count headers).
struct ForwardBits {
  ConstByteView data;
  uint64_t position = 0;
  uint64_t peek(uint32_t n) const { return bits_at(data, static_cast<int64_t>(position), n); }
  uint64_t read(uint32_t n) {
    const uint64_t v = peek(n);
    position += n;
    return v;
  }
};

// A bit stream read backwards (Huffman and FSE streams): the last byte's highest set bit marks the end, and reading
// goes down from just below it. Reading past the start gives zero bits and makes `position` negative.
struct BackwardBits {
  ConstByteView data;
  int64_t position = 0;  // bits [0, position) are still unread

  [[nodiscard]] bool init(ConstByteView stream) {
    data = stream;
    if (stream.empty()) return false;
    const uint8_t last = stream[stream.size() - 1];
    if (last == 0) return false;
    position = static_cast<int64_t>((stream.size() - 1) * 8 + highest_bit(last));
    return true;
  }
  uint64_t peek(uint32_t n) const { return bits_at(data, position - static_cast<int64_t>(n), n); }
  uint64_t read(uint32_t n) {
    const uint64_t v = peek(n);
    position -= n;
    return v;
  }
  bool overflowed() const { return position < 0; }
  bool finished() const { return position == 0; }
};

struct FseEntry {
  uint16_t base;
  uint8_t symbol;
  uint8_t bits;
};

constexpr uint32_t kMaxFseSymbols = 64;

struct FseTable {
  FseEntry entries[512];
  uint32_t log = 0;
  bool valid = false;
  ArrayView<FseEntry> view() { return ArrayView<FseEntry>(entries); }
};

struct HuffmanEntry {
  uint8_t symbol;
  uint8_t bits;
};

struct Decoder {
  FseTable literal_lengths, offsets, match_lengths, weights;
  HuffmanEntry huffman[1u << kMaxHuffmanBits];
  uint32_t huffman_log = 0;
  bool huffman_valid = false;
  uint64_t repeat[3] = {1, 4, 8};

  void reset() {
    literal_lengths.valid = offsets.valid = match_lengths.valid = false;
    huffman_valid = false;
    repeat[0] = 1;
    repeat[1] = 4;
    repeat[2] = 8;
  }
};

// Reads an FSE normalized-count header (RFC 8878 4.1.1) into `counts` (-1 = "less than one"). `symbols` gets one more
// than the last symbol coded, `consumed` the whole bytes the header takes.
[[nodiscard]] bool read_counts(ConstByteView in, ArrayView<int16_t> counts, uint32_t max_symbol, uint32_t max_log, uint32_t* log_out,
                               uint32_t* symbols, uint64_t* consumed) {
  if (in.empty() || max_symbol >= counts.size()) return false;
  ForwardBits r{in, 0};
  const uint32_t log = static_cast<uint32_t>(r.read(4)) + 5;
  if (log > max_log) return false;
  int32_t remaining = (1 << log) + 1;
  int32_t threshold = 1 << log;
  uint32_t bits = log + 1;
  uint32_t symbol = 0;
  bool previous_zero = false;
  while (remaining > 1) {
    if (previous_zero) {
      uint32_t repeat;
      do {
        repeat = static_cast<uint32_t>(r.read(2));
        for (uint32_t k = 0; k < repeat; ++k) {
          if (symbol > max_symbol) return false;
          counts[symbol++] = 0;
        }
        if (r.position > in.size() * 8) return false;
      } while (repeat == 3);
    }
    if (symbol > max_symbol) return false;
    const int32_t max = 2 * threshold - 1 - remaining;
    const int32_t value = static_cast<int32_t>(r.peek(bits));
    int32_t count;
    if ((value & (threshold - 1)) < max) {
      count = value & (threshold - 1);
      r.position += bits - 1;
    } else {
      count = value & (2 * threshold - 1);
      if (count >= threshold) count -= max;
      r.position += bits;
    }
    --count;  // -1: a probability below one
    remaining -= count < 0 ? -count : count;
    if (remaining < 1) return false;
    counts[symbol++] = static_cast<int16_t>(count);
    previous_zero = count == 0;
    while (remaining < threshold) {
      --bits;
      threshold >>= 1;
    }
    if (r.position > in.size() * 8) return false;
  }
  if (remaining != 1 || r.position > in.size() * 8) return false;
  for (uint32_t s = symbol; s < counts.size(); ++s) counts[s] = 0;
  *log_out = log;
  *symbols = symbol;
  *consumed = (r.position + 7) / 8;
  return true;
}

// Builds the decoding table of an FSE code from its normalized counts (they must add up to 1 << log).
[[nodiscard]] bool build_fse(FseTable* table, ArrayView<const int16_t> counts, uint32_t log) {
  table->valid = false;
  if (log > 9 || counts.empty() || counts.size() > kMaxFseSymbols) return false;
  const uint32_t size = 1u << log;
  ArrayView<FseEntry> entries = table->view().first(size);
  uint16_t next[kMaxFseSymbols];
  uint32_t high = size;  // symbols of probability "less than one" take single entries from the top down
  uint32_t total = 0;
  for (uint32_t s = 0; s < counts.size(); ++s) {
    const int16_t c = counts[s];
    if (c < -1) return false;
    if (c == -1) {
      if (high == 0) return false;
      entries[--high].symbol = static_cast<uint8_t>(s);
      next[s] = 1;
      ++total;
    } else {
      if (static_cast<uint32_t>(c) > size - total) return false;
      next[s] = static_cast<uint16_t>(c);
      total += static_cast<uint32_t>(c);
    }
  }
  if (total != size) return false;
  const uint32_t high_threshold = high - 1;  // only used when some symbol has a positive count, so high > 0
  const uint32_t step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
  uint32_t position = 0;
  for (uint32_t s = 0; s < counts.size(); ++s) {
    for (int32_t i = 0; i < counts[s]; ++i) {
      entries[position].symbol = static_cast<uint8_t>(s);
      do position = (position + step) & mask;
      while (position > high_threshold);
    }
  }
  if (position != 0) return false;
  for (FseEntry& e : entries) {
    const uint32_t state = next[e.symbol]++;
    if (state == 0) return false;
    const uint32_t bits = log - highest_bit(state);
    e.bits = static_cast<uint8_t>(bits);
    e.base = static_cast<uint16_t>((state << bits) - size);
  }
  table->log = log;
  table->valid = true;
  return true;
}

void build_rle(FseTable* table, uint8_t symbol) {
  table->entries[0] = FseEntry{0, symbol, 0};
  table->log = 0;
  table->valid = true;
}

// The weights of a Huffman tree description coded with FSE (two interleaved states).
[[nodiscard]] bool decode_fse_weights(Decoder& d, ConstByteView in, ArrayView<uint8_t> weights, uint32_t* count) {
  int16_t counts_storage[16];
  ArrayView<int16_t> counts(counts_storage);
  uint32_t log = 0, symbols = 0;
  uint64_t used = 0;
  if (!read_counts(in, counts, 12, 6, &log, &symbols, &used)) return false;
  if (used >= in.size()) return false;
  if (!build_fse(&d.weights, ArrayView<const int16_t>(counts_storage, symbols), log)) return false;
  BackwardBits b;
  if (!b.init(in.subview(used))) return false;
  ArrayView<FseEntry> table = d.weights.view().first(1u << log);
  uint32_t s1 = static_cast<uint32_t>(b.read(log)), s2 = static_cast<uint32_t>(b.read(log));
  uint32_t n = 0;
  const uint32_t max = static_cast<uint32_t>(weights.size());
  for (;;) {
    if (n + 2 > max) return false;
    weights[n++] = table[s1].symbol;
    s1 = table[s1].base + static_cast<uint32_t>(b.read(table[s1].bits));
    if (b.overflowed()) {
      weights[n++] = table[s2].symbol;
      break;
    }
    if (n + 2 > max) return false;
    weights[n++] = table[s2].symbol;
    s2 = table[s2].base + static_cast<uint32_t>(b.read(table[s2].bits));
    if (b.overflowed()) {
      weights[n++] = table[s1].symbol;
      break;
    }
  }
  *count = n;
  return true;
}

// Reads a Huffman tree description and builds the literal decoding table. `consumed` gets its size in bytes.
[[nodiscard]] bool read_huffman_tree(Decoder& d, ConstByteView in, uint64_t* consumed) {
  d.huffman_valid = false;
  if (in.empty()) return false;
  uint8_t weight_storage[256];
  ArrayView<uint8_t> weights(weight_storage);
  uint32_t count = 0;
  const uint8_t header = in[0];
  if (header < 128) {
    ConstByteView fse;
    if (header == 0 || !in.try_subview(1, header, &fse)) return false;
    if (!decode_fse_weights(d, fse, weights.first(255), &count)) return false;
    *consumed = 1 + uint64_t{header};
  } else {
    count = header - 127u;
    const uint64_t bytes = (count + 1) / 2;
    ConstByteView packed;
    if (!in.try_subview(1, bytes, &packed)) return false;
    for (uint32_t i = 0; i < count; ++i) weights[i] = (i & 1) ? (packed[i / 2] & 15) : (packed[i / 2] >> 4);
    *consumed = 1 + bytes;
  }
  // The last symbol's weight is implied: the one that makes the code complete.
  uint32_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (weights[i] > kMaxHuffmanBits) return false;
    if (weights[i]) total += 1u << (weights[i] - 1);
  }
  if (total == 0) return false;
  const uint32_t max_bits = highest_bit(total) + 1;
  if (max_bits > kMaxHuffmanBits) return false;
  const uint32_t left = (1u << max_bits) - total;
  if (left & (left - 1)) return false;
  weights[count] = static_cast<uint8_t>(highest_bit(left) + 1);
  const uint32_t symbols = count + 1;
  uint32_t rank[kMaxHuffmanBits + 2] = {};
  for (uint32_t i = 0; i < symbols; ++i) ++rank[weights[i]];
  if (rank[1] < 2 || (rank[1] & 1)) return false;
  // Longest codes first, each weight's symbols in order: a symbol of weight w covers 2^(w-1) table entries.
  ArrayView<HuffmanEntry> table(d.huffman);
  uint32_t position = 0;
  for (uint32_t w = 1; w <= max_bits; ++w) {
    for (uint32_t s = 0; s < symbols; ++s) {
      if (weights[s] != w) continue;
      const uint32_t span = 1u << (w - 1);
      if (span > (1u << max_bits) - position) return false;
      for (uint32_t k = 0; k < span; ++k) table[position + k] = HuffmanEntry{static_cast<uint8_t>(s), static_cast<uint8_t>(max_bits + 1 - w)};
      position += span;
    }
  }
  if (position != (1u << max_bits)) return false;
  d.huffman_log = max_bits;
  d.huffman_valid = true;
  return true;
}

// Decodes one Huffman stream, which must end exactly where `out` is full.
[[nodiscard]] bool decode_huffman_stream(const Decoder& d, ConstByteView stream, ByteView out) {
  BackwardBits b;
  if (!b.init(stream)) return false;
  ArrayView<const HuffmanEntry> table(d.huffman);
  const uint32_t log = d.huffman_log;
  for (uint8_t& byte : out) {
    const HuffmanEntry& e = table[b.peek(log)];
    byte = e.symbol;
    b.position -= e.bits;
    if (b.overflowed()) return false;
  }
  return b.finished();
}

// The literals section of a compressed block: `literals` gets the literals (a view of the input or of `buffer`),
// `consumed` the section's size.
[[nodiscard]] bool read_literals(Decoder& d, ConstByteView in, uint64_t block_max, ByteBuffer& buffer, ConstByteView* literals,
                                 uint64_t* consumed) {
  if (in.empty()) return false;
  const uint8_t b0 = in[0];
  const uint32_t type = b0 & 3, format = (b0 >> 2) & 3;
  if (type <= 1) {  // raw or RLE
    uint64_t size, header;
    if ((format & 1) == 0) {
      size = b0 >> 3;
      header = 1;
    } else if (format == 1) {
      if (in.size() < 2) return false;
      size = (b0 >> 4) + (uint64_t{in[1]} << 4);
      header = 2;
    } else {
      if (in.size() < 3) return false;
      size = (b0 >> 4) + (uint64_t{in[1]} << 4) + (uint64_t{in[2]} << 12);
      header = 3;
    }
    if (size > block_max) return false;
    if (type == 0) {
      if (!in.try_subview(header, size, literals)) return false;
      *consumed = header + size;
    } else {
      if (in.size() <= header) return false;
      buffer.clear();
      if (!buffer.put_zeros(size)) return false;
      buffer.writable_view().fill(in[header]);
      *literals = buffer.view();
      *consumed = header + 1;
    }
    return true;
  }
  // Huffman-coded: compressed (with a tree) or treeless (the previous block's tree).
  const uint32_t header = format <= 1 ? 3 : format == 2 ? 4 : 5;
  const uint32_t field_bits = format <= 1 ? 10 : format == 2 ? 14 : 18;
  if (in.size() < header) return false;
  const uint64_t fields = load_le(in, 0, header) >> 4;
  const uint64_t size = fields & ((uint64_t{1} << field_bits) - 1);
  const uint64_t compressed = (fields >> field_bits) & ((uint64_t{1} << field_bits) - 1);
  const bool four_streams = format != 0;
  if (size > block_max) return false;
  ConstByteView data;
  if (!in.try_subview(header, compressed, &data)) return false;
  *consumed = header + compressed;
  if (type == 2) {
    uint64_t tree = 0;
    if (!read_huffman_tree(d, data, &tree)) return false;
    data = data.subview(tree);
  } else if (!d.huffman_valid) {
    return false;
  }
  buffer.clear();
  if (!buffer.put_zeros(size)) return false;
  ByteView out = buffer.writable_view();
  if (!four_streams) {
    if (!decode_huffman_stream(d, data, out)) return false;
  } else {
    if (data.size() < 6) return false;
    const uint64_t s1 = load_le(data, 0, 2), s2 = load_le(data, 2, 2), s3 = load_le(data, 4, 2);
    const uint64_t rest = data.size() - 6;
    if (s1 + s2 + s3 > rest) return false;
    const uint64_t s4 = rest - s1 - s2 - s3;
    const uint64_t segment = (size + 3) / 4;
    if (3 * segment > size) return false;
    const uint64_t sizes[4] = {s1, s2, s3, s4};
    uint64_t at = 6;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint64_t length = i < 3 ? segment : size - 3 * segment;
      if (!decode_huffman_stream(d, data.subview(at, sizes[i]), out.subview(i * segment, length))) return false;
      at += sizes[i];
    }
  }
  *literals = buffer.view();
  return true;
}

constexpr int16_t kLiteralLengthDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
                                               2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
constexpr int16_t kMatchLengthDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                             1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
constexpr int16_t kOffsetDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

constexpr uint32_t kLiteralLengthBase[36] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,   10,  11,  12,  13,   14,   15,   16,   18,
                                             20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
constexpr uint8_t kLiteralLengthBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr uint32_t kMatchLengthBase[53] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  12,  13,  14,  15,   16,   17,   18,   19,    20,
                                           21, 22, 23, 24, 25, 26, 27, 28, 29,  30,  31,  32,  33,   34,   35,   37,   39,    41,
                                           43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
constexpr uint8_t kMatchLengthBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// Sets up one of the three sequence codes for `mode` (0 predefined, 1 RLE, 2 FSE-coded, 3 repeat), reading what
// that needs from `in` at `*at`.
[[nodiscard]] bool read_sequence_table(FseTable* table, uint32_t mode, ConstByteView in, uint64_t* at, ArrayView<const int16_t> defaults,
                                       uint32_t default_log, uint32_t max_symbol, uint32_t max_log) {
  switch (mode) {
    case 0:
      return build_fse(table, defaults, default_log);
    case 1: {
      uint8_t symbol;
      if (!in.try_get(*at, &symbol) || symbol > max_symbol) return false;
      ++*at;
      build_rle(table, symbol);
      return true;
    }
    case 2: {
      int16_t storage[kMaxFseSymbols];
      uint32_t log = 0, symbols = 0;
      uint64_t used = 0;
      if (*at > in.size()) return false;
      if (!read_counts(in.subview(*at), ArrayView<int16_t>(storage), max_symbol, max_log, &log, &symbols, &used)) return false;
      if (!build_fse(table, ArrayView<const int16_t>(storage, symbols), log)) return false;
      *at += used;
      return true;
    }
    default:
      return table->valid;
  }
}

struct SequenceState {
  ArrayView<FseEntry> table;
  uint32_t state = 0;
  void init(FseTable& t, BackwardBits& b) {
    table = t.view().first(1u << t.log);
    state = static_cast<uint32_t>(b.read(t.log));
  }
  uint8_t symbol() const { return table[state].symbol; }
  void update(BackwardBits& b) {
    const FseEntry& e = table[state];
    state = e.base + static_cast<uint32_t>(b.read(e.bits));
  }
};

// Where a block's output goes: `data` is the whole of `out`, the block writes from `position` up to `limit`, and
// matches may reach back to `frame_start`.
struct BlockOutput {
  ByteView data;
  uint64_t frame_start;
  uint64_t position;
  uint64_t limit;

  [[nodiscard]] bool literals(ConstByteView bytes) {
    if (bytes.size() > limit - position) return false;
    data.subview(position, bytes.size()).copy_from(bytes);
    position += bytes.size();
    return true;
  }
  [[nodiscard]] bool match(uint64_t offset, uint64_t length) {
    if (offset == 0 || offset > position - frame_start || length > limit - position) return false;
    const uint64_t from = position - offset;
    if (offset >= length) {
      data.subview(position, length).copy_from(ConstByteView(data.subview(from, length)));
    } else {
      ByteView span = data.subview(from, offset + length);
      for (uint64_t i = 0; i < length; ++i) span[offset + i] = span[i];
    }
    position += length;
    return true;
  }
};

// The sequences section of a compressed block, executed against `literals` into `out`.
[[nodiscard]] bool execute_sequences(Decoder& d, ConstByteView in, ConstByteView literals, BlockOutput& out) {
  if (in.empty()) return false;
  uint64_t at = 0;
  uint64_t count = in[0];
  if (count == 0) {
    if (in.size() != 1) return false;
    return out.literals(literals);
  }
  if (count >= 128) {
    if (in.size() < 2) return false;
    if (count < 255) {
      count = ((count - 128) << 8) + in[1];
      at = 2;
    } else {
      if (in.size() < 3) return false;
      count = in[1] + (uint64_t{in[2]} << 8) + 0x7F00;
      at = 3;
    }
  } else {
    at = 1;
  }
  uint8_t modes;
  if (!in.try_get(at++, &modes) || (modes & 3) != 0) return false;
  if (!read_sequence_table(&d.literal_lengths, modes >> 6, in, &at, ArrayView<const int16_t>(kLiteralLengthDefault), 6, 35, 9)) return false;
  if (!read_sequence_table(&d.offsets, (modes >> 4) & 3, in, &at, ArrayView<const int16_t>(kOffsetDefault), 5, 31, 8)) return false;
  if (!read_sequence_table(&d.match_lengths, (modes >> 2) & 3, in, &at, ArrayView<const int16_t>(kMatchLengthDefault), 6, 52, 9)) return false;
  if (at > in.size()) return false;
  BackwardBits b;
  if (!b.init(in.subview(at))) return false;
  SequenceState ll, of, ml;
  ll.init(d.literal_lengths, b);
  of.init(d.offsets, b);
  ml.init(d.match_lengths, b);
  uint64_t literal_position = 0;
  for (uint64_t n = 0; n < count; ++n) {
    const uint32_t ll_code = ll.symbol(), of_code = of.symbol(), ml_code = ml.symbol();
    if (ll_code > 35 || ml_code > 52 || of_code > 31) return false;
    const uint64_t offset_value = (uint64_t{1} << of_code) + b.read(of_code);
    const uint64_t match_length = kMatchLengthBase[ml_code] + b.read(kMatchLengthBits[ml_code]);
    const uint64_t literal_length = kLiteralLengthBase[ll_code] + b.read(kLiteralLengthBits[ll_code]);
    if (b.overflowed()) return false;
    uint64_t offset;
    if (offset_value > 3) {
      offset = offset_value - 3;
      d.repeat[2] = d.repeat[1];
      d.repeat[1] = d.repeat[0];
      d.repeat[0] = offset;
    } else {
      const uint64_t index = offset_value - 1 + (literal_length == 0 ? 1 : 0);  // 0..3
      if (index == 0) {
        offset = d.repeat[0];
      } else {
        offset = index == 3 ? d.repeat[0] - 1 : d.repeat[index];
        if (offset == 0) return false;
        if (index != 1) d.repeat[2] = d.repeat[1];
        d.repeat[1] = d.repeat[0];
        d.repeat[0] = offset;
      }
    }
    if (literal_length > literals.size() - literal_position) return false;
    if (!out.literals(literals.subview(literal_position, literal_length))) return false;
    literal_position += literal_length;
    if (!out.match(offset, match_length)) return false;
    if (n + 1 < count) {
      ll.update(b);
      ml.update(b);
      of.update(b);
      if (b.overflowed()) return false;
    }
  }
  if (!b.finished()) return false;
  return out.literals(literals.subview(literal_position));
}

[[nodiscard]] bool decode_frame(Decoder& d, ConstByteView in, uint64_t* position, ByteBuffer* out, uint64_t out_start, uint64_t max_output,
                                ByteBuffer& literal_buffer) {
  uint64_t at = *position;
  uint8_t descriptor;
  if (!in.try_get(at++, &descriptor)) return false;
  const uint32_t size_flag = descriptor >> 6;
  const bool single_segment = (descriptor >> 5) & 1;
  const bool has_checksum = (descriptor >> 2) & 1;
  const uint32_t dictionary_flag = descriptor & 3;
  if (descriptor & 8) return false;  // reserved bit
  uint64_t window_size = 0;
  if (!single_segment) {
    uint8_t w;
    if (!in.try_get(at++, &w)) return false;
    const uint64_t base = uint64_t{1} << (10 + (w >> 3));
    window_size = base + (base / 8) * (w & 7);
  }
  const uint32_t dictionary_bytes = dictionary_flag == 3 ? 4 : dictionary_flag;
  if (at > in.size() || dictionary_bytes > in.size() - at) return false;
  if (load_le(in, at, dictionary_bytes) != 0) return false;  // dictionaries are not supported
  at += dictionary_bytes;
  const uint32_t size_bytes = size_flag == 0 ? (single_segment ? 1 : 0) : size_flag == 1 ? 2 : size_flag == 2 ? 4 : 8;
  if (size_bytes > in.size() - at) return false;
  uint64_t content_size = load_le(in, at, size_bytes);
  if (size_bytes == 2) content_size += 256;
  const bool has_content_size = size_bytes != 0;
  at += size_bytes;
  if (single_segment) window_size = content_size;
  const uint64_t block_max = window_size < kMaxBlockSize ? window_size : kMaxBlockSize;

  d.reset();
  const uint64_t frame_start = out->size();
  for (;;) {
    if (in.size() - at < 3) return false;
    const uint32_t header = static_cast<uint32_t>(load_le(in, at, 3));
    at += 3;
    const bool last = header & 1;
    const uint32_t type = (header >> 1) & 3;
    const uint64_t size = header >> 3;
    const uint64_t produced = out->size() - out_start;
    const uint64_t budget = max_output - produced;
    if (type == 0) {  // raw
      ConstByteView raw;
      if (size > block_max || size > budget || !in.try_subview(at, size, &raw) || !out->append(raw)) return false;
      at += size;
    } else if (type == 1) {  // RLE
      uint8_t byte;
      if (size > block_max || size > budget || !in.try_get(at, &byte)) return false;
      const uint64_t start = out->size();
      if (!out->put_zeros(size)) return false;
      out->writable_view().subview(start).fill(byte);
      at += 1;
    } else if (type == 2) {  // compressed
      ConstByteView block;
      if (size > block_max || !in.try_subview(at, size, &block)) return false;
      at += size;
      ConstByteView literals;
      uint64_t literal_bytes = 0;
      if (!read_literals(d, block, block_max, literal_buffer, &literals, &literal_bytes)) return false;
      const uint64_t space = block_max < budget ? block_max : budget;
      const uint64_t start = out->size();
      if (!out->put_zeros(space)) return false;
      BlockOutput target{out->writable_view(), frame_start, start, start + space};
      const bool ok = execute_sequences(d, block.subview(literal_bytes), literals, target);
      out->truncate(target.position);
      if (!ok) return false;
    } else {
      return false;
    }
    if (has_content_size && out->size() - frame_start > content_size) return false;
    if (last) break;
  }
  if (has_content_size && out->size() - frame_start != content_size) return false;
  if (has_checksum) {
    if (in.size() - at < 4) return false;
    const uint32_t want = static_cast<uint32_t>(load_le(in, at, 4));
    at += 4;
    if (static_cast<uint32_t>(xxh64(out->view().subview(frame_start))) != want) return false;
  }
  *position = at;
  return true;
}

}  // namespace

bool zstd_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output) {
  const uint64_t out_start = out->size();
  if (in.empty()) return false;
  UniquePointer<Decoder> decoder = UniquePointer<Decoder>::make(out->allocator());
  if (!decoder) return false;
  ByteBuffer literal_buffer(out->allocator());
  uint64_t at = 0;
  while (at < in.size()) {
    if (in.size() - at < 4) {
      out->truncate(out_start);
      return false;
    }
    const uint32_t magic = static_cast<uint32_t>(load_le(in, at, 4));
    at += 4;
    if ((magic & 0xFFFFFFF0u) == kSkippableMagic) {
      if (in.size() - at < 4) {
        out->truncate(out_start);
        return false;
      }
      const uint64_t skip = load_le(in, at, 4);
      at += 4;
      if (skip > in.size() - at) {
        out->truncate(out_start);
        return false;
      }
      at += skip;
      continue;
    }
    if (magic != kFrameMagic || !decode_frame(*decoder, in, &at, out, out_start, max_output, literal_buffer)) {
      out->truncate(out_start);
      return false;
    }
  }
  return true;
}

}  // namespace media
