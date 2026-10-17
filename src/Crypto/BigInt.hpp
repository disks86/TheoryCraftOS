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

namespace crypto {

// Fixed-size unsigned integers with 32-bit limbs (least significant first) and Montgomery modular arithmetic, for RSA and elliptic curves.
// `N` is the capacity in limbs; the modulus decides how many limbs are used. Montgomery multiplication, addition and subtraction take
// the same time for all values of the same length. pow() branches on the exponent's bits: use it for public exponents (RSA verification)
// and for inversion by a public modulus.
template <uint32_t N>
struct BigNum {
  uint32_t w[N] = {};
  bool operator==(const BigNum& o) const {
    uint32_t d = 0;
    for (uint32_t i = 0; i < N; ++i) d |= w[i] ^ o.w[i];
    return d == 0;
  }
  bool is_zero() const {
    uint32_t d = 0;
    for (uint32_t i = 0; i < N; ++i) d |= w[i];
    return d == 0;
  }
  bool bit(uint32_t i) const { return (w[i >> 5] >> (i & 31)) & 1; }
};

template <uint32_t N>
class Mont {
 public:
  using Num = BigNum<N>;
  // The modulus as big-endian bytes (leading zeros allowed). It must be odd, at least 3 and fit N limbs. False otherwise.
  bool init(ConstByteView modulus_be);
  uint32_t limbs() const { return len_; }
  uint32_t bits() const;
  uint32_t byte_size() const { return (bits() + 7) / 8; }
  const Num& modulus() const { return m_; }

  // Big-endian bytes to a number; false if it does not fit the modulus' limb count. No range check against the modulus.
  bool load(ConstByteView be, Num* out) const;
  // The number as exactly `out.size()` big-endian bytes (left padded); false if it does not fit.
  bool store(const Num& a, ByteView out) const;
  bool below_modulus(const Num& a) const;   // a < m
  void reduce_once(Num* a) const;            // a - m if a >= m (for a < 2m)

  void to_mont(const Num& a, Num* out) const { mul(a, r2_, out); }
  void from_mont(const Num& a, Num* out) const;
  const Num& one() const { return one_; }    // 1 in the Montgomery domain
  void mul(const Num& a, const Num& b, Num* out) const;
  void sqr(const Num& a, Num* out) const { mul(a, a, out); }
  void add(const Num& a, const Num& b, Num* out) const;
  void sub(const Num& a, const Num& b, Num* out) const;
  // out = base^exp for a big-endian exponent (Montgomery in, Montgomery out).
  void pow(const Num& base, ConstByteView exponent_be, Num* out) const;
  // a^(m-2): the inverse when the modulus is prime (and a is not 0).
  void invert_prime(const Num& a, Num* out) const;

 private:
  Num m_, r2_, one_;
  uint32_t len_ = 0;
  uint32_t n0_ = 0;   // -m^-1 mod 2^32
};

using MontRsa = Mont<130>;   // up to 4160-bit moduli
using MontEc = Mont<12>;     // up to 384 bits

}  // namespace crypto
