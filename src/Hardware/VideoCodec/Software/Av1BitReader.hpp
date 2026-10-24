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

// The bit reader of the AV1 OBU syntax (dav1d's getbits.c: f(n), su(n), leb128, ns(n), the variable length code and the sub-exponential code).
// Reading past the end of the data sets a sticky error flag and yields zero bits; callers check error() at the end of each header section.

namespace hardware {
namespace av1 {

class BitReader {
 public:
  explicit BitReader(ConstByteView data) : data_(data) {}

  unsigned bit() {
    const uint64_t byte = pos_ >> 3;
    if (byte >= data_.size()) {
      error_ = true;
      return 0;
    }
    const unsigned b = (data_[byte] >> (7 - (pos_ & 7))) & 1u;
    ++pos_;
    return b;
  }

  // f(n), n <= 32.
  unsigned bits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | bit();
    return v;
  }

  // su(n): n bits as a two's complement value.
  int sbits(int n) {
    const uint32_t v = bits(n);
    if (n <= 0 || n >= 32) return static_cast<int>(v);
    const uint32_t sign = uint32_t{1} << (n - 1);
    return static_cast<int>((v ^ sign) - sign);
  }

  // leb128(); more than 32 bits of value or more than 8 bytes is an error.
  unsigned uleb128() {
    uint64_t val = 0;
    unsigned i = 0, more;
    do {
      const unsigned v = bits(8);
      more = v & 0x80;
      val |= static_cast<uint64_t>(v & 0x7f) << i;
      i += 7;
    } while (more && i < 56);
    if (val > 0xffffffffull || more) {
      error_ = true;
      return 0;
    }
    return static_cast<unsigned>(val);
  }

  // ns(max): a value in [0, max - 1]; nothing is read for max <= 1.
  unsigned uniform(unsigned max) {
    if (max <= 1) return 0;
    int l = 0;
    while ((max >> l) > 1) ++l;  // floor(log2(max))
    ++l;
    const unsigned m = (1u << l) - max;
    const unsigned v = bits(l - 1);
    return v < m ? v : (v << 1) - m + bit();
  }

  // The unary-prefixed code of num_ticks_per_picture_minus_1; 0xffffffff if it is longer than 32 bits.
  unsigned vlc() {
    if (bit()) return 0;
    int n_bits = 0;
    do {
      if (++n_bits == 32) return 0xffffffffu;
    } while (!bit());
    return ((1u << n_bits) - 1) + bits(n_bits);
  }

  // decode_signed_subexp_with_ref() with the range [-(1 << n), (1 << n)).
  int subexp(int ref, unsigned n) {
    return static_cast<int>(subexp_u(static_cast<unsigned>(ref) + (1u << n), 2u << n) - (1u << n));
  }

  // Skips to the next byte boundary.
  void byte_align() { pos_ = (pos_ + 7) & ~uint64_t{7}; }
  // The number of whole bytes read so far (the offset of the next byte once aligned).
  uint64_t byte_position() const { return (pos_ + 7) >> 3; }
  // True if the remaining bits of the current byte are zero (they are the trailing bits' zero padding).
  bool rest_of_byte_is_zero() const {
    const uint64_t byte = pos_ >> 3;
    if ((pos_ & 7) == 0 || byte >= data_.size()) return true;
    return (data_[byte] & (0xffu >> (pos_ & 7))) == 0;
  }
  ConstByteView data() const { return data_; }
  bool error() const { return error_; }
  void set_error() { error_ = true; }

 private:
  static unsigned inv_recenter(unsigned r, unsigned v) {
    if (v > (r << 1)) return v;
    if ((v & 1) == 0) return (v >> 1) + r;
    return r - ((v + 1) >> 1);
  }

  unsigned subexp_u(unsigned ref, unsigned n) {
    unsigned v = 0;
    for (int i = 0;; i++) {
      const int b = i ? 3 + i - 1 : 3;
      if (n < v + 3u * (1u << b)) {
        v += uniform(n - v + 1);
        break;
      }
      if (!bit()) {
        v += bits(b);
        break;
      }
      v += 1u << b;
    }
    return ref * 2 <= n ? inv_recenter(ref, v) : n - inv_recenter(n - ref, v);
  }

  ConstByteView data_;
  uint64_t pos_ = 0;
  bool error_ = false;
};

}  // namespace av1
}  // namespace hardware
