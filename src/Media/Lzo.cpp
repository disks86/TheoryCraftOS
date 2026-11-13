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

// LZO1X decoding. The format is a run of instructions, each a token byte (sometimes with length bytes and a two byte
// distance) that copies literals or a match; the low two bits of the instruction's last distance byte say how many
// literals (0 to 3) follow it before the next token. Token ranges:
//   0..15    after a match with no trailing literals: a run of 3+ literals (0 = longer run in bytes)
//            after 1..3 trailing literals: a 2 byte match within 1 KiB (M1)
//            right after a literal run: a 3 byte match 2 KiB further back (M1 variant)
//   16..31   M4: 3+ bytes from 16 KiB to 48 KiB back; distance 0 is the end of the stream
//   32..63   M3: 3+ bytes up to 16 KiB back
//   64..255  M2: 3 to 8 bytes up to 2 KiB back
// The first byte may also be 18..255: 1 to 238 literals.

#include "Media/Lzo.hpp"

namespace media {

namespace {

class LzoDecoder {
 public:
  LzoDecoder(ConstByteView in, ByteBuffer* out, uint64_t limit) : in_(in), out_(out), start_(out->size()), limit_(limit) {}

  [[nodiscard]] bool run() {
    enum class Next : uint8_t { Token, AfterLiterals, Match };
    Next next = Next::Token;
    uint32_t t = 0;
    uint8_t first;
    if (!in_.try_get(0, &first)) return false;
    if (first > 17) {
      ip_ = 1;
      t = first - 17u;
      if (t < 4) {
        if (!literals(t)) return false;
        if (!byte(&t)) return false;
        next = Next::Match;
      } else {
        if (!literals(t)) return false;
        next = Next::AfterLiterals;
      }
    }
    for (;;) {
      if (next == Next::Token) {
        if (!byte(&t)) return false;
        if (t >= 16) {
          next = Next::Match;
          continue;
        }
        uint64_t run = t;
        if (run == 0 && !extended(&run, 15)) return false;
        if (!literals(run + 3)) return false;
        next = Next::AfterLiterals;
        continue;
      }
      if (next == Next::AfterLiterals) {
        if (!byte(&t)) return false;
        if (t >= 16) {
          next = Next::Match;
          continue;
        }
        uint32_t high;
        if (!byte(&high) || !match(1 + 0x800 + (t >> 2) + (uint64_t{high} << 2), 3)) return false;
      } else if (t >= 64) {  // M2
        uint32_t high;
        if (!byte(&high) || !match(1 + ((t >> 2) & 7) + (uint64_t{high} << 3), (t >> 5) + 1)) return false;
      } else if (t >= 32) {  // M3
        uint64_t length = t & 31;
        if (length == 0 && !extended(&length, 31)) return false;
        uint32_t low, high;
        if (!byte(&low) || !byte(&high) || !match(1 + (low >> 2) + (uint64_t{high} << 6), length + 2)) return false;
      } else if (t >= 16) {  // M4
        uint64_t length = t & 7;
        if (length == 0 && !extended(&length, 7)) return false;
        uint32_t low, high;
        if (!byte(&low) || !byte(&high)) return false;
        const uint64_t distance = (uint64_t{t & 8} << 11) + (low >> 2) + (uint64_t{high} << 6);
        if (distance == 0) return length == 1 && ip_ == in_.size();  // the end marker 11 00 00, and nothing after it
        if (!match(distance + 0x4000, length + 2)) return false;
      } else {  // M1
        uint32_t high;
        if (!byte(&high) || !match(1 + (t >> 2) + (uint64_t{high} << 2), 2)) return false;
      }
      // The two low bits of the instruction's first distance byte (two bytes back) count the literals that follow.
      const uint32_t trailing = in_[ip_ - 2] & 3u;
      if (trailing == 0) {
        next = Next::Token;
        continue;
      }
      if (!literals(trailing) || !byte(&t)) return false;
      next = Next::Match;
    }
  }

 private:
  [[nodiscard]] bool byte(uint32_t* value) {
    uint8_t b;
    if (!in_.try_get(ip_, &b)) return false;
    ++ip_;
    *value = b;
    return true;
  }
  // A zero length field continues in the following bytes: each zero adds 255, the first non-zero byte ends it.
  [[nodiscard]] bool extended(uint64_t* length, uint32_t base) {
    uint32_t b;
    uint64_t total = 0;
    for (;;) {
      if (!byte(&b)) return false;
      if (b != 0) break;
      total += 255;  // cannot wrap: at most 255 per input byte
    }
    *length = total + base + b;
    return true;
  }
  [[nodiscard]] bool literals(uint64_t count) {
    ConstByteView run;
    if (!in_.try_subview(ip_, count, &run) || count > limit_ - out_->size() || !out_->append(run)) return false;
    ip_ += count;
    return true;
  }
  [[nodiscard]] bool match(uint64_t distance, uint64_t length) {
    const uint64_t at = out_->size();
    if (distance > at - start_ || length > limit_ - at || !out_->put_zeros(length)) return false;
    ByteView data = out_->writable_view();
    for (uint64_t i = 0; i < length; ++i) data[at + i] = data[at + i - distance];
    return true;
  }

  ConstByteView in_;
  uint64_t ip_ = 0;
  ByteBuffer* out_;
  uint64_t start_;
  uint64_t limit_;
};

}  // namespace

bool lzo1x_decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output) {
  const uint64_t start = out->size();
  const uint64_t limit = max_output > UINT64_MAX - start ? UINT64_MAX : start + max_output;
  LzoDecoder decoder(in, out, limit);
  if (decoder.run()) return true;
  out->truncate(start);
  return false;
}

}  // namespace media
