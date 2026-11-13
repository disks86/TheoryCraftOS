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

// .xz and .lzma decoding: the LZMA range decoder, LZMA2 chunks, the .xz container (stream header and footer, block
// headers, index) and the BCJ and Delta filters. Everything is decoded straight into the output buffer, which then serves
// as the LZMA dictionary: matches are copied from what was written earlier, checked against the start of the dictionary
// (the last dictionary reset) and its size. Filters run in place over a finished block, which gives the same result as
// liblzma's streaming filters because those also leave the last few unprocessable bytes of a block as they are.

#include "Media/Xz.hpp"
#include "Crypto/Crypto.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Crc32.hpp"

namespace media {

namespace {

struct Crc64Table {
  uint64_t value[256];
};

constexpr Crc64Table make_crc64_table() {
  Crc64Table table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint64_t crc = i;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xC96C5795D7870F42ull & (0ull - (crc & 1)));
    table.value[i] = crc;
  }
  return table;
}

constexpr Crc64Table kCrc64 = make_crc64_table();

}  // namespace

uint64_t crc64(ConstByteView data, uint64_t crc) {
  crc = ~crc;
  for (uint8_t byte : data) crc = kCrc64.value[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Output window: the output buffer as the LZ dictionary.

struct Window {
  ByteBuffer* out;
  uint64_t limit;      // out->size() may not grow past this
  uint64_t base;       // the dictionary starts here (the last dictionary reset)
  uint64_t dict_size;  // matches reach back at most this far

  uint64_t size() const { return out->size(); }
  uint64_t position() const { return out->size() - base; }
  uint64_t available() const {
    const uint64_t have = out->size() - base;
    return have < dict_size ? have : dict_size;
  }
  [[nodiscard]] bool put(uint8_t byte) { return out->size() < limit && out->put8(byte); }
  // The byte `distance` (1 = the last one) back, if it is inside the dictionary.
  [[nodiscard]] bool back(uint64_t distance, uint32_t* byte) const {
    if (distance == 0 || distance > available()) return false;
    *byte = out->view()[out->size() - distance];
    return true;
  }
  // Repeats `length` bytes starting `distance` back (the copy may overlap what it writes).
  [[nodiscard]] bool copy(uint64_t distance, uint64_t length) {
    if (distance == 0 || distance > available() || length > limit - out->size()) return false;
    const uint64_t at = out->size();
    if (!out->put_zeros(length)) return false;
    ByteView data = out->writable_view();
    for (uint64_t i = 0; i < length; ++i) data[at + i] = data[at + i - distance];
    return true;
  }
};

// ---------------------------------------------------------------------------------------------------------------------
// Range decoder.

struct RangeDecoder {
  ConstByteView in;
  uint64_t position = 0;
  uint32_t range = 0xFFFFFFFF;
  uint32_t code = 0;
  bool failed = false;

  explicit RangeDecoder(ConstByteView data) : in(data) {}

  [[nodiscard]] bool init() {
    if (in.size() < 5 || in[0] != 0) return false;
    code = (uint32_t{in[1]} << 24) | (uint32_t{in[2]} << 16) | (uint32_t{in[3]} << 8) | in[4];
    position = 5;
    return true;
  }
  uint8_t next() {
    if (position >= in.size()) {
      failed = true;
      return 0;
    }
    return in[position++];
  }
  void normalize() {
    if (range < (1u << 24)) {
      range <<= 8;
      code = (code << 8) | next();
    }
  }
  uint32_t bit(uint16_t& probability) {
    normalize();
    const uint32_t bound = (range >> 11) * probability;
    if (code < bound) {
      range = bound;
      probability = static_cast<uint16_t>(probability + ((2048 - probability) >> 5));
      return 0;
    }
    range -= bound;
    code -= bound;
    probability = static_cast<uint16_t>(probability - (probability >> 5));
    return 1;
  }
  uint32_t direct(uint32_t count) {
    uint32_t result = 0;
    while (count--) {
      normalize();
      range >>= 1;
      const uint32_t bit = code >= range ? 1 : 0;
      if (bit) code -= range;
      result = (result << 1) | bit;
    }
    return result;
  }
  // A `bits` wide symbol coded most significant bit first with the tree at probs[offset + 1 ...].
  uint32_t tree(ArrayView<uint16_t> probs, uint64_t offset, uint32_t bits) {
    uint32_t symbol = 1;
    for (uint32_t i = 0; i < bits; ++i) symbol = (symbol << 1) | bit(probs[offset + symbol]);
    return symbol - (1u << bits);
  }
  // The same, least significant bit first.
  uint32_t reverse_tree(ArrayView<uint16_t> probs, uint64_t offset, uint32_t bits) {
    uint32_t m = 1, symbol = 0;
    for (uint32_t i = 0; i < bits; ++i) {
      const uint32_t b = bit(probs[offset + m]);
      m = (m << 1) | b;
      symbol |= b << i;
    }
    return symbol;
  }
};

// ---------------------------------------------------------------------------------------------------------------------
// LZMA.

// Offsets into the one probability array: the fixed models, then the literal coders.
constexpr uint64_t kStates = 12, kPosStatesMax = 16;
constexpr uint64_t kIsMatch = 0;
constexpr uint64_t kIsRep = kIsMatch + kStates * kPosStatesMax;
constexpr uint64_t kIsRep0 = kIsRep + kStates;
constexpr uint64_t kIsRep1 = kIsRep0 + kStates;
constexpr uint64_t kIsRep2 = kIsRep1 + kStates;
constexpr uint64_t kIsRep0Long = kIsRep2 + kStates;
constexpr uint64_t kDistSlot = kIsRep0Long + kStates * kPosStatesMax;
constexpr uint64_t kDistSpecial = kDistSlot + 4 * 64;
constexpr uint64_t kDistAlign = kDistSpecial + 114;
// A length coder: choice, choice2, low[16][8], mid[16][8], high[256].
constexpr uint64_t kLenChoice = 0, kLenChoice2 = 1, kLenLow = 2, kLenMid = kLenLow + 16 * 8, kLenHigh = kLenMid + 16 * 8, kLenSize = kLenHigh + 256;
constexpr uint64_t kMatchLen = kDistAlign + 16;
constexpr uint64_t kRepLen = kMatchLen + kLenSize;
constexpr uint64_t kLiteral = kRepLen + kLenSize;
constexpr uint32_t kEndMarker = 0xFFFFFFFF;

enum class LzmaResult : uint8_t { Ok, EndMarker, Error };

class LzmaDecoder {
 public:
  explicit LzmaDecoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~LzmaDecoder() { release_view(allocator_, probs_); }
  LzmaDecoder(const LzmaDecoder&) = delete;
  LzmaDecoder& operator=(const LzmaDecoder&) = delete;

  // Room for literal coders of lc + lp up to `max_lc_lp` bits.
  [[nodiscard]] bool allocate(uint32_t max_lc_lp) {
    max_lc_lp_ = max_lc_lp;
    probs_ = allocate_view<uint16_t>(allocator_, kLiteral + (uint64_t{0x300} << max_lc_lp));
    return !probs_.empty();
  }
  // lc, lp and pb from their packed byte ((pb * 5 + lp) * 9 + lc).
  [[nodiscard]] bool set_properties(uint8_t properties) {
    if (properties >= 9 * 5 * 5) return false;
    lc_ = properties % 9;
    lp_ = (properties / 9) % 5;
    pb_ = properties / 45;
    return lc_ + lp_ <= max_lc_lp_;
  }
  void reset() {
    probs_.first(kLiteral + (uint64_t{0x300} << (lc_ + lp_))).fill(1024);
    state_ = 0;
    for (uint32_t& r : rep_) r = 0;
  }

  // Decodes until the output reaches `end` bytes or, if `allow_end_marker`, an end marker comes.
  LzmaResult decode(RangeDecoder& rc, Window& w, uint64_t end, bool allow_end_marker) {
    const uint32_t pb_mask = (1u << pb_) - 1;
    while (w.size() < end) {
      if (rc.failed) return LzmaResult::Error;
      const uint64_t position = w.position();
      const uint32_t pos_state = static_cast<uint32_t>(position) & pb_mask;
      if (!rc.bit(probs_[kIsMatch + state_ * kPosStatesMax + pos_state])) {
        if (!literal(rc, w, position)) return LzmaResult::Error;
        continue;
      }
      uint64_t length;
      if (!rc.bit(probs_[kIsRep + state_])) {
        length = decode_length(rc, kMatchLen, pos_state);
        state_ = state_ < 7 ? 7 : 10;
        rep_[3] = rep_[2];
        rep_[2] = rep_[1];
        rep_[1] = rep_[0];
        rep_[0] = decode_distance(rc, static_cast<uint32_t>(length < 6 ? length - 2 : 3));
        if (rep_[0] == kEndMarker) return allow_end_marker && !rc.failed ? LzmaResult::EndMarker : LzmaResult::Error;
      } else {
        if (!rc.bit(probs_[kIsRep0 + state_])) {
          if (!rc.bit(probs_[kIsRep0Long + state_ * kPosStatesMax + pos_state])) {  // a single byte at rep0
            state_ = state_ < 7 ? 9 : 11;
            if (!w.copy(uint64_t{rep_[0]} + 1, 1)) return LzmaResult::Error;
            continue;
          }
        } else {
          uint32_t distance;
          if (!rc.bit(probs_[kIsRep1 + state_])) {
            distance = rep_[1];
          } else {
            if (!rc.bit(probs_[kIsRep2 + state_])) {
              distance = rep_[2];
            } else {
              distance = rep_[3];
              rep_[3] = rep_[2];
            }
            rep_[2] = rep_[1];
          }
          rep_[1] = rep_[0];
          rep_[0] = distance;
        }
        length = decode_length(rc, kRepLen, pos_state);
        state_ = state_ < 7 ? 8 : 11;
      }
      if (rc.failed || length > end - w.size() || !w.copy(uint64_t{rep_[0]} + 1, length)) return LzmaResult::Error;
    }
    return rc.failed ? LzmaResult::Error : LzmaResult::Ok;
  }

 private:
  [[nodiscard]] bool literal(RangeDecoder& rc, Window& w, uint64_t position) {
    uint32_t previous = 0;
    if (position > 0 && !w.back(1, &previous)) return false;
    const uint64_t coder = kLiteral + 0x300ull * (((position & ((1u << lp_) - 1)) << lc_) + (previous >> (8 - lc_)));
    uint32_t symbol = 1;
    if (state_ < 7) {
      while (symbol < 0x100) symbol = (symbol << 1) | rc.bit(probs_[coder + symbol]);
    } else {  // after a match: the byte at rep0 steers the coder until the first differing bit
      uint32_t match_byte;
      if (!w.back(uint64_t{rep_[0]} + 1, &match_byte)) return false;
      uint32_t offset = 0x100;
      while (symbol < 0x100) {
        match_byte <<= 1;
        const uint32_t match_bit = match_byte & offset;
        const uint32_t b = rc.bit(probs_[coder + offset + match_bit + symbol]);
        symbol = (symbol << 1) | b;
        offset &= b ? match_bit : ~match_bit;
      }
    }
    state_ = state_ < 4 ? 0 : state_ < 10 ? state_ - 3 : state_ - 6;
    return w.put(static_cast<uint8_t>(symbol));
  }

  uint64_t decode_length(RangeDecoder& rc, uint64_t coder, uint32_t pos_state) {
    if (!rc.bit(probs_[coder + kLenChoice])) return 2 + rc.tree(probs_, coder + kLenLow + pos_state * 8 - 1, 3);
    if (!rc.bit(probs_[coder + kLenChoice2])) return 2 + 8 + rc.tree(probs_, coder + kLenMid + pos_state * 8 - 1, 3);
    return 2 + 16 + rc.tree(probs_, coder + kLenHigh - 1, 8);
  }

  // The distance minus one; kEndMarker is the end marker.
  uint32_t decode_distance(RangeDecoder& rc, uint32_t length_state) {
    const uint32_t slot = rc.tree(probs_, kDistSlot + length_state * 64 - 1, 6);
    if (slot < 4) return slot;
    const uint32_t direct_bits = (slot >> 1) - 1;
    uint32_t distance = (2 | (slot & 1)) << direct_bits;
    if (slot < 14) return distance + rc.reverse_tree(probs_, kDistSpecial + distance - slot - 1, direct_bits);
    distance += rc.direct(direct_bits - 4) << 4;
    return distance + rc.reverse_tree(probs_, kDistAlign, 4);
  }

  allocators::IAllocator& allocator_;
  ArrayView<uint16_t> probs_;
  uint32_t max_lc_lp_ = 0;
  uint32_t lc_ = 0, lp_ = 0, pb_ = 0;
  uint32_t state_ = 0;
  uint32_t rep_[4] = {0, 0, 0, 0};
};

// Dictionary sizes are rounded the way liblzma allocates them (at least 4 KiB, a multiple of 16), so a stream it accepts
// is accepted here too.
uint64_t effective_dictionary(uint64_t dict_size) {
  if (dict_size < 4096) return 4096;
  return (dict_size + 15) & ~uint64_t{15};
}

// LZMA2: a run of chunks (uncompressed or LZMA, with dictionary, state and property resets) ending with a zero byte.
// Decodes into `w` up to `end`; `consumed` gets the bytes used, end byte included.
[[nodiscard]] bool lzma2_decode(ConstByteView in, Window& w, uint64_t end, uint64_t* consumed) {
  LzmaDecoder lzma(w.out->allocator());
  if (!lzma.allocate(4)) return false;
  ByteReader r(in);
  bool need_dictionary_reset = true, need_properties = true;
  for (;;) {
    const uint8_t control = r.u8();
    if (!r.ok()) return false;
    if (control == 0x00) {
      *consumed = r.position();
      return true;
    }
    if (control >= 0xE0 || control == 0x01) {
      need_properties = true;
      need_dictionary_reset = false;
      w.base = w.size();
    } else if (need_dictionary_reset) {
      return false;
    }
    if (control >= 0x80) {
      const uint64_t unpacked = (uint64_t{control & 0x1Fu} << 16) + r.be16() + 1;
      const uint64_t packed = uint64_t{r.be16()} + 1;
      if (control >= 0xC0) {
        if (!lzma.set_properties(r.u8())) return false;
        need_properties = false;
        lzma.reset();
      } else if (need_properties) {
        return false;
      } else if (control >= 0xA0) {
        lzma.reset();
      }
      const ConstByteView chunk = r.take(packed);
      if (!r.ok() || unpacked > end - w.size()) return false;
      RangeDecoder rc(chunk);
      if (!rc.init()) return false;
      if (lzma.decode(rc, w, w.size() + unpacked, false) != LzmaResult::Ok) return false;
      rc.normalize();
      if (rc.failed || rc.position != chunk.size() || rc.code != 0) return false;
    } else {
      if (control > 0x02) return false;
      const ConstByteView data = r.take(uint64_t{r.be16()} + 1);
      if (!r.ok() || data.size() > end - w.size() || !w.out->append(data)) return false;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// BCJ and Delta filters (decoding direction), in place over a whole block. `now` is the start offset.

inline uint32_t read_le32(ByteView b, uint64_t i) {
  return uint32_t{b[i]} | (uint32_t{b[i + 1]} << 8) | (uint32_t{b[i + 2]} << 16) | (uint32_t{b[i + 3]} << 24);
}
inline uint32_t read_be32(ByteView b, uint64_t i) {
  return (uint32_t{b[i]} << 24) | (uint32_t{b[i + 1]} << 16) | (uint32_t{b[i + 2]} << 8) | uint32_t{b[i + 3]};
}
inline void write_le32(ByteView b, uint64_t i, uint32_t v) {
  for (int k = 0; k < 4; ++k) b[i + k] = static_cast<uint8_t>(v >> (8 * k));
}
inline void write_be32(ByteView b, uint64_t i, uint32_t v) {
  for (int k = 0; k < 4; ++k) b[i + k] = static_cast<uint8_t>(v >> (24 - 8 * k));
}

void bcj_x86(ByteView buf, uint32_t now) {
  static constexpr bool kAllowed[8] = {true, true, true, false, true, false, false, false};
  static constexpr uint32_t kBitNumber[8] = {0, 1, 2, 2, 3, 3, 3, 3};
  auto ms_byte = [](uint32_t b) { return b == 0 || b == 0xFF; };
  if (buf.size() < 5) return;
  uint32_t prev_mask = 0, prev_pos = now - 5;
  const uint64_t limit = buf.size() - 5;
  uint64_t i = 0;
  while (i <= limit) {
    uint32_t b = buf[i];
    if (b != 0xE8 && b != 0xE9) {
      ++i;
      continue;
    }
    const uint32_t here = now + static_cast<uint32_t>(i);
    const uint32_t offset = here - prev_pos;
    prev_pos = here;
    if (offset > 5) {
      prev_mask = 0;
    } else {
      for (uint32_t k = 0; k < offset; ++k) {
        prev_mask &= 0x77;
        prev_mask <<= 1;
      }
    }
    b = buf[i + 4];
    if (ms_byte(b) && kAllowed[(prev_mask >> 1) & 7] && (prev_mask >> 1) < 0x10) {
      uint32_t src = read_le32(buf, i + 1);
      uint32_t dest;
      for (;;) {
        dest = src - (here + 5);
        if (prev_mask == 0) break;
        const uint32_t n = kBitNumber[(prev_mask >> 1) & 7];  // (prev_mask >> 1) < 8 here: bit 4 is never shifted in
        b = static_cast<uint8_t>(dest >> (24 - n * 8));
        if (!ms_byte(b)) break;
        src = dest ^ ((1u << (32 - n * 8)) - 1);
      }
      buf[i + 4] = static_cast<uint8_t>(~(((dest >> 24) & 1) - 1));
      buf[i + 3] = static_cast<uint8_t>(dest >> 16);
      buf[i + 2] = static_cast<uint8_t>(dest >> 8);
      buf[i + 1] = static_cast<uint8_t>(dest);
      i += 5;
      prev_mask = 0;
    } else {
      ++i;
      prev_mask |= 1;
      if (ms_byte(b)) prev_mask |= 0x10;
    }
  }
}

void bcj_powerpc(ByteView buf, uint32_t now) {
  for (uint64_t i = 0; buf.size() >= 4 && i <= buf.size() - 4; i += 4) {
    if ((buf[i] >> 2) != 0x12 || (buf[i + 3] & 3) != 1) continue;
    const uint32_t src = ((uint32_t{buf[i]} & 3) << 24) | (uint32_t{buf[i + 1]} << 16) | (uint32_t{buf[i + 2]} << 8) | (uint32_t{buf[i + 3]} & ~3u);
    const uint32_t dest = src - (now + static_cast<uint32_t>(i));
    buf[i] = static_cast<uint8_t>(0x48 | ((dest >> 24) & 0x03));
    buf[i + 1] = static_cast<uint8_t>(dest >> 16);
    buf[i + 2] = static_cast<uint8_t>(dest >> 8);
    buf[i + 3] = static_cast<uint8_t>((buf[i + 3] & 0x03) | (dest & ~3u));
  }
}

void bcj_ia64(ByteView buf, uint32_t now) {
  static constexpr uint32_t kBranchTable[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
  for (uint64_t i = 0; buf.size() >= 16 && i <= buf.size() - 16; i += 16) {
    const uint32_t mask = kBranchTable[buf[i] & 0x1F];
    uint32_t bit_pos = 5;
    for (uint32_t slot = 0; slot < 3; ++slot, bit_pos += 41) {
      if (((mask >> slot) & 1) == 0) continue;
      const uint64_t byte_pos = bit_pos >> 3;
      const uint32_t bit_res = bit_pos & 7;
      uint64_t instruction = 0;
      for (uint32_t j = 0; j < 6; ++j) instruction |= uint64_t{buf[i + j + byte_pos]} << (8 * j);
      uint64_t norm = instruction >> bit_res;
      if (((norm >> 37) & 0xF) != 0x5 || ((norm >> 9) & 0x7) != 0) continue;
      uint32_t src = static_cast<uint32_t>((norm >> 13) & 0xFFFFF);
      src |= static_cast<uint32_t>((norm >> 36) & 1) << 20;
      src <<= 4;
      uint32_t dest = src - (now + static_cast<uint32_t>(i));
      dest >>= 4;
      norm &= ~(uint64_t{0x8FFFFF} << 13);
      norm |= uint64_t{dest & 0xFFFFF} << 13;
      norm |= uint64_t{dest & 0x100000} << (36 - 20);
      instruction &= (uint64_t{1} << bit_res) - 1;
      instruction |= norm << bit_res;
      for (uint32_t j = 0; j < 6; ++j) buf[i + j + byte_pos] = static_cast<uint8_t>(instruction >> (8 * j));
    }
  }
}

void bcj_arm(ByteView buf, uint32_t now) {
  for (uint64_t i = 0; buf.size() >= 4 && i <= buf.size() - 4; i += 4) {
    if (buf[i + 3] != 0xEB) continue;
    const uint32_t src = ((uint32_t{buf[i + 2]} << 16) | (uint32_t{buf[i + 1]} << 8) | uint32_t{buf[i]}) << 2;
    const uint32_t dest = (src - (now + static_cast<uint32_t>(i) + 8)) >> 2;
    buf[i + 2] = static_cast<uint8_t>(dest >> 16);
    buf[i + 1] = static_cast<uint8_t>(dest >> 8);
    buf[i] = static_cast<uint8_t>(dest);
  }
}

void bcj_arm_thumb(ByteView buf, uint32_t now) {
  for (uint64_t i = 0; buf.size() >= 4 && i <= buf.size() - 4; i += 2) {
    if ((buf[i + 1] & 0xF8) != 0xF0 || (buf[i + 3] & 0xF8) != 0xF8) continue;
    const uint32_t src = (((uint32_t{buf[i + 1]} & 7) << 19) | (uint32_t{buf[i]} << 11) | ((uint32_t{buf[i + 3]} & 7) << 8) | uint32_t{buf[i + 2]}) << 1;
    const uint32_t dest = (src - (now + static_cast<uint32_t>(i) + 4)) >> 1;
    buf[i + 1] = static_cast<uint8_t>(0xF0 | ((dest >> 19) & 0x7));
    buf[i] = static_cast<uint8_t>(dest >> 11);
    buf[i + 3] = static_cast<uint8_t>(0xF8 | ((dest >> 8) & 0x7));
    buf[i + 2] = static_cast<uint8_t>(dest);
    i += 2;
  }
}

void bcj_sparc(ByteView buf, uint32_t now) {
  for (uint64_t i = 0; buf.size() >= 4 && i <= buf.size() - 4; i += 4) {
    if (!((buf[i] == 0x40 && (buf[i + 1] & 0xC0) == 0x00) || (buf[i] == 0x7F && (buf[i + 1] & 0xC0) == 0xC0))) continue;
    const uint32_t src = read_be32(buf, i) << 2;
    uint32_t dest = (src - (now + static_cast<uint32_t>(i))) >> 2;
    dest = (((0u - ((dest >> 22) & 1)) << 22) & 0x3FFFFFFF) | (dest & 0x3FFFFF) | 0x40000000;
    write_be32(buf, i, dest);
  }
}

void bcj_arm64(ByteView buf, uint32_t now) {
  for (uint64_t i = 0; buf.size() >= 4 && i <= buf.size() - 4; i += 4) {
    uint32_t pc = now + static_cast<uint32_t>(i);
    uint32_t instruction = read_le32(buf, i);
    if ((instruction >> 26) == 0x25) {  // BL
      const uint32_t src = instruction;
      pc >>= 2;
      instruction = 0x94000000 | ((src - pc) & 0x03FFFFFF);
      write_le32(buf, i, instruction);
    } else if ((instruction & 0x9F000000) == 0x90000000) {  // ADRP
      const uint32_t src = ((instruction >> 29) & 3) | ((instruction >> 3) & 0x001FFFFC);
      if ((src + 0x00020000) & 0x001C0000) continue;  // only addresses within +-512 MiB are converted
      instruction &= 0x9000001F;
      pc >>= 12;
      const uint32_t dest = src - pc;
      instruction |= (dest & 3) << 29;
      instruction |= (dest & 0x0003FFFC) << 3;
      instruction |= (0u - (dest & 0x00020000)) & 0x00E00000;
      write_le32(buf, i, instruction);
    }
  }
}

void bcj_riscv(ByteView buf, uint32_t now) {
  if (buf.size() < 8) return;
  const uint64_t last = buf.size() - 8;
  for (uint64_t i = 0; i <= last; i += 2) {
    uint32_t instruction = buf[i];
    if (instruction == 0xEF) {  // JAL
      const uint32_t b1 = buf[i + 1];
      if ((b1 & 0x0D) != 0) continue;
      const uint32_t b2 = buf[i + 2];
      const uint32_t b3 = buf[i + 3];
      const uint32_t pc = now + static_cast<uint32_t>(i);
      uint32_t address = ((b1 & 0xF0) << 13) | (b2 << 9) | (b3 << 1);
      address -= pc;
      buf[i + 1] = static_cast<uint8_t>((b1 & 0x0F) | ((address >> 8) & 0xF0));
      buf[i + 2] = static_cast<uint8_t>(((address >> 16) & 0x0F) | ((address >> 7) & 0x10) | ((address << 4) & 0xE0));
      buf[i + 3] = static_cast<uint8_t>(((address >> 4) & 0x7F) | ((address >> 13) & 0x80));
      i += 4 - 2;
    } else if ((instruction & 0x7F) == 0x17) {  // AUIPC
      instruction |= uint32_t{buf[i + 1]} << 8;
      instruction |= uint32_t{buf[i + 2]} << 16;
      instruction |= uint32_t{buf[i + 3]} << 24;
      uint32_t second;
      if (instruction & 0xE80) {  // rd is neither x0 nor x2: a pair the encoder left alone, kept in its "fake" form
        second = read_le32(buf, i + 4);
        if (((instruction << 8) ^ (second - 3)) & 0xF8003) {
          i += 6 - 2;
          continue;
        }
        uint32_t address = instruction & 0xFFFFF000;
        address += second >> 20;
        instruction = 0x17 | (2 << 7) | (second << 12);
        second = address;
      } else {  // rd is x0 or x2: the encoder's converted pair
        const uint32_t second_rs1 = instruction >> 27;
        if (((instruction - 0x3117) << 18) >= (second_rs1 & 0x1D)) {
          i += 4 - 2;
          continue;
        }
        uint32_t address = read_be32(buf, i + 4);
        address -= now + static_cast<uint32_t>(i);
        second = (instruction >> 12) | (address << 20);
        instruction = 0x17 | (second_rs1 << 7) | ((address + 0x800) & 0xFFFFF000);
      }
      write_le32(buf, i, instruction);
      write_le32(buf, i + 4, second);
      i += 8 - 2;
    }
  }
}

void delta_decode(ByteView buf, uint32_t distance) {
  for (uint64_t i = distance; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(buf[i] + buf[i - distance]);
}

// ---------------------------------------------------------------------------------------------------------------------
// The .xz container.

constexpr uint8_t kStreamMagic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
constexpr uint64_t kFilterDelta = 0x03, kFilterX86 = 0x04, kFilterPowerPc = 0x05, kFilterIa64 = 0x06, kFilterArm = 0x07,
                   kFilterArmThumb = 0x08, kFilterSparc = 0x09, kFilterArm64 = 0x0A, kFilterRiscV = 0x0B, kFilterLzma2 = 0x21;
constexpr uint32_t kCheckNone = 0, kCheckCrc32 = 1, kCheckCrc64 = 4, kCheckSha256 = 10;

uint32_t check_size(uint32_t type) {
  if (type == 0) return 0;
  return 4u << ((type - 1) / 3);  // 4, 8, 16, 32, 64 bytes in groups of three types
}

[[nodiscard]] bool read_varint(ByteReader& r, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t i = 0; i < 9; ++i) {
    const uint8_t byte = r.u8();
    if (!r.ok()) return false;
    result |= uint64_t{byte & 0x7Fu} << (7 * i);
    if ((byte & 0x80) == 0) {
      if (byte == 0 && i > 0) return false;  // not the shortest encoding
      *value = result;
      return true;
    }
  }
  return false;
}

struct Filter {
  uint64_t id = 0;
  uint32_t parameter = 0;  // the start offset of a BCJ filter, the distance of Delta, the dictionary size of LZMA2
};

void apply_filter(const Filter& f, ByteView data) {
  switch (f.id) {
    case kFilterDelta: delta_decode(data, f.parameter); break;
    case kFilterX86: bcj_x86(data, f.parameter); break;
    case kFilterPowerPc: bcj_powerpc(data, f.parameter); break;
    case kFilterIa64: bcj_ia64(data, f.parameter); break;
    case kFilterArm: bcj_arm(data, f.parameter); break;
    case kFilterArmThumb: bcj_arm_thumb(data, f.parameter); break;
    case kFilterSparc: bcj_sparc(data, f.parameter); break;
    case kFilterArm64: bcj_arm64(data, f.parameter); break;
    case kFilterRiscV: bcj_riscv(data, f.parameter); break;
    default: break;
  }
}

// The running summary of the blocks of a stream that the index must agree with.
struct IndexSummary {
  uint64_t count = 0;
  uint64_t unpadded = 0;
  uint64_t uncompressed = 0;
  uint32_t crc = 0;  // over each record's two sizes

  [[nodiscard]] bool add(uint64_t unpadded_size, uint64_t uncompressed_size) {
    if (unpadded_size > UINT64_MAX - unpadded || uncompressed_size > UINT64_MAX - uncompressed) return false;
    ++count;
    unpadded += unpadded_size;
    uncompressed += uncompressed_size;
    uint8_t record[16];
    for (int i = 0; i < 8; ++i) {
      record[i] = static_cast<uint8_t>(unpadded_size >> (8 * i));
      record[8 + i] = static_cast<uint8_t>(uncompressed_size >> (8 * i));
    }
    crc = crc32(ConstByteView(record), crc);
    return true;
  }
  bool operator==(const IndexSummary& o) const {
    return count == o.count && unpadded == o.unpadded && uncompressed == o.uncompressed && crc == o.crc;
  }
};

[[nodiscard]] bool verify_check(uint32_t type, ConstByteView data, ConstByteView stored) {
  switch (type) {
    case kCheckNone: return true;
    case kCheckCrc32: {
      const uint32_t crc = crc32(data);
      for (int i = 0; i < 4; ++i)
        if (stored[i] != static_cast<uint8_t>(crc >> (8 * i))) return false;
      return true;
    }
    case kCheckCrc64: {
      const uint64_t crc = crc64(data);
      for (int i = 0; i < 8; ++i)
        if (stored[i] != static_cast<uint8_t>(crc >> (8 * i))) return false;
      return true;
    }
    case kCheckSha256: {
      uint8_t digest[32];
      crypto::hash(crypto::HashKind::Sha256, data, digest);
      return ConstByteView(digest).equals(stored);
    }
    default: return true;  // a check this decoder does not know is skipped, as liblzma does on request
  }
}

// One block, its header starting at `*pos` (header size byte already known to be non-zero).
[[nodiscard]] bool decode_block(ConstByteView in, uint64_t* pos, Window& w, uint32_t check_type, IndexSummary* summary) {
  uint8_t size_byte;
  if (!in.try_get(*pos, &size_byte)) return false;
  const uint64_t header_size = (uint64_t{size_byte} + 1) * 4;
  ConstByteView header;
  if (!in.try_subview(*pos, header_size, &header)) return false;
  ByteReader crc_reader(header.last(4));
  if (crc32(header.first(header_size - 4)) != crc_reader.le32()) return false;

  ByteReader hr(header.first(header_size - 4));
  hr.skip(1);
  const uint8_t flags = hr.u8();
  if (flags & 0x3C) return false;
  const uint32_t filter_count = (flags & 3u) + 1;
  uint64_t compressed_size = 0, uncompressed_size = 0;
  if ((flags & 0x40) && (!read_varint(hr, &compressed_size) || compressed_size == 0)) return false;
  if ((flags & 0x80) && !read_varint(hr, &uncompressed_size)) return false;
  Filter filters[4];
  for (uint32_t i = 0; i < filter_count; ++i) {
    uint64_t properties_size;
    if (!read_varint(hr, &filters[i].id) || !read_varint(hr, &properties_size)) return false;
    const ConstByteView properties = hr.take(properties_size);
    if (!hr.ok()) return false;
    const bool last = i + 1 == filter_count;
    const uint64_t id = filters[i].id;
    if (last != (id == kFilterLzma2)) return false;  // LZMA2 comes last and only last
    if (id == kFilterLzma2) {
      if (properties.size() != 1 || properties[0] > 40) return false;
      const uint32_t bits = properties[0];
      filters[i].parameter = bits == 40 ? 0xFFFFFFFF : (2u | (bits & 1)) << (bits / 2 + 11);
    } else if (id == kFilterDelta) {
      if (properties.size() != 1) return false;
      filters[i].parameter = uint32_t{properties[0]} + 1;
    } else if (id >= kFilterX86 && id <= kFilterRiscV) {
      if (properties.size() == 4) filters[i].parameter = ByteReader(properties).le32();
      else if (!properties.empty()) return false;
    } else {
      return false;
    }
  }
  for (uint8_t padding : hr.rest())
    if (padding != 0) return false;

  ConstByteView data = in.subview(*pos + header_size);
  if ((flags & 0x40) && !data.try_subview(0, compressed_size, &data)) return false;
  const uint64_t block_start = w.size();
  uint64_t end = w.limit;
  if ((flags & 0x80) && uncompressed_size < end - block_start) end = block_start + uncompressed_size;
  w.base = block_start;
  w.dict_size = effective_dictionary(filters[filter_count - 1].parameter);
  uint64_t consumed = 0;
  if (!lzma2_decode(data, w, end, &consumed)) return false;
  const uint64_t produced = w.size() - block_start;
  if ((flags & 0x40) && consumed != compressed_size) return false;
  if ((flags & 0x80) && produced != uncompressed_size) return false;
  for (uint32_t i = filter_count - 1; i-- > 0;) apply_filter(filters[i], w.out->writable_view().subview(block_start));

  // Block padding to a multiple of four, then the check.
  ByteReader tail(in.subview(*pos + header_size + consumed));
  const uint64_t padding = (4 - (consumed & 3)) & 3;
  for (uint64_t i = 0; i < padding; ++i)
    if (tail.u8() != 0) return false;
  const uint32_t check_bytes = check_size(check_type);
  const ConstByteView stored = tail.take(check_bytes);
  if (!tail.ok() || !verify_check(check_type, w.out->view().subview(block_start), stored)) return false;
  if (!summary->add(header_size + consumed + check_bytes, produced)) return false;
  *pos += header_size + consumed + tail.position();
  return true;
}

// One stream starting at `*pos`.
[[nodiscard]] bool decode_stream(ConstByteView in, uint64_t* pos, Window& w) {
  ConstByteView header;
  if (!in.try_subview(*pos, 12, &header)) return false;
  if (!header.first(6).equals(ConstByteView(kStreamMagic))) return false;
  if (crc32(header.subview(6, 2)) != ByteReader(header.subview(8, 4)).le32()) return false;
  if (header[6] != 0 || (header[7] & 0xF0) != 0) return false;
  const uint32_t check_type = header[7] & 0x0F;
  *pos += 12;

  IndexSummary blocks;
  for (;;) {
    uint8_t size_byte;
    if (!in.try_get(*pos, &size_byte)) return false;
    if (size_byte == 0) break;  // the index indicator
    if (!decode_block(in, pos, w, check_type, &blocks)) return false;
  }

  // The index.
  const uint64_t index_start = *pos;
  ByteReader ir(in.subview(index_start));
  ir.skip(1);
  uint64_t count;
  if (!read_varint(ir, &count) || count != blocks.count) return false;
  IndexSummary records;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t unpadded, uncompressed;
    if (!read_varint(ir, &unpadded) || !read_varint(ir, &uncompressed) || unpadded == 0) return false;
    if (!records.add(unpadded, uncompressed)) return false;
  }
  if (!(records == blocks)) return false;
  while (ir.position() & 3)
    if (ir.u8() != 0 || !ir.ok()) return false;
  const uint64_t index_body = ir.position();
  if (crc32(in.subview(index_start, index_body)) != ir.le32() || !ir.ok()) return false;
  const uint64_t index_size = ir.position();
  *pos = index_start + index_size;

  // The footer.
  ConstByteView footer;
  if (!in.try_subview(*pos, 12, &footer)) return false;
  ByteReader fr(footer);
  const uint32_t footer_crc = fr.le32();
  const uint64_t backward_size = (uint64_t{fr.le32()} + 1) * 4;
  if (crc32(footer.subview(4, 6)) != footer_crc || backward_size != index_size) return false;
  if (footer[8] != header[6] || footer[9] != header[7] || footer[10] != 'Y' || footer[11] != 'Z') return false;
  *pos += 12;
  return true;
}

}  // namespace

bool xz_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output) {
  const uint64_t start = out->size();
  Window w{out, max_output > UINT64_MAX - start ? UINT64_MAX : start + max_output, start, 0};
  uint64_t pos = 0;
  bool ok = decode_stream(in, &pos, w);
  while (ok && pos < in.size()) {
    // Stream padding (zero bytes in groups of four), then the end or another stream.
    ConstByteView group;
    if (in.try_subview(pos, 4, &group) && group[0] == 0 && group[1] == 0 && group[2] == 0 && group[3] == 0) {
      pos += 4;
      continue;
    }
    ok = decode_stream(in, &pos, w);
  }
  if (!ok) out->truncate(start);
  return ok;
}

bool lzma_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output) {
  ByteReader r(in);
  const uint8_t properties = r.u8();
  const uint32_t dict_size = r.le32();
  const uint64_t size = r.le64();
  if (!r.ok()) return false;
  const uint64_t start = out->size();
  const uint64_t limit = max_output > UINT64_MAX - start ? UINT64_MAX : start + max_output;
  const bool known = size != UINT64_MAX;
  if (known && size > limit - start) return false;
  if (properties >= 9 * 5 * 5) return false;
  LzmaDecoder lzma(out->allocator());  // literal coders for exactly this lc + lp (up to 12 bits: 6 MiB of probabilities)
  if (!lzma.allocate(properties % 9 + (properties / 9) % 5) || !lzma.set_properties(properties)) return false;
  lzma.reset();
  Window w{out, limit, start, effective_dictionary(dict_size)};
  RangeDecoder rc(r.rest());
  bool ok = rc.init();
  if (ok) {
    const LzmaResult result = lzma.decode(rc, w, known ? start + size : UINT64_MAX, true);  // unknown: the end marker may follow a full output
    ok = known ? result == LzmaResult::Ok : result == LzmaResult::EndMarker;
  }
  if (!ok) out->truncate(start);
  return ok;
}

}  // namespace media
