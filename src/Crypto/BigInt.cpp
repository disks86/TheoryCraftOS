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

#include "Crypto/BigInt.hpp"

namespace crypto {

template <uint32_t N>
bool Mont<N>::load(ConstByteView be, Num* out) const {
  *out = Num();
  uint64_t size = be.size();
  uint64_t skip = 0;
  while (skip < size && be[skip] == 0) ++skip;   // leading zeros do not count
  if (size - skip > 4ull * len_) return false;
  for (uint64_t i = skip; i < size; ++i) {
    const uint64_t pos = size - 1 - i;   // byte index from the least significant end
    out->w[pos / 4] |= static_cast<uint32_t>(be[i]) << (8 * (pos % 4));
  }
  return true;
}

template <uint32_t N>
bool Mont<N>::store(const Num& a, ByteView out) const {
  for (uint64_t i = 0; i < out.size(); ++i) out[i] = 0;
  for (uint32_t i = 0; i < N; ++i) {
    for (int k = 0; k < 4; ++k) {
      const uint8_t b = static_cast<uint8_t>(a.w[i] >> (8 * k));
      const uint64_t pos = 4ull * i + k;   // from the least significant end
      if (pos < out.size()) out[out.size() - 1 - pos] = b;
      else if (b) return false;
    }
  }
  return true;
}

template <uint32_t N>
uint32_t Mont<N>::bits() const {
  for (uint32_t i = len_; i > 0; --i)
    if (m_.w[i - 1]) {
      uint32_t b = 32;
      while (!((m_.w[i - 1] >> (b - 1)) & 1)) --b;
      return (i - 1) * 32 + b;
    }
  return 0;
}

// a >= m as 0/1, in constant time over the limbs.
namespace {
template <uint32_t N>
uint32_t geq_mask(const uint32_t* a, const uint32_t* m, uint32_t len) {
  uint32_t borrow = 0;
  for (uint32_t i = 0; i < len; ++i) {
    const uint64_t d = static_cast<uint64_t>(a[i]) - m[i] - borrow;
    borrow = static_cast<uint32_t>(d >> 63);
  }
  return borrow ^ 1;   // no borrow means a >= m
}
}  // namespace

template <uint32_t N>
bool Mont<N>::below_modulus(const Num& a) const { return geq_mask<N>(a.w, m_.w, len_) == 0; }

template <uint32_t N>
void Mont<N>::reduce_once(Num* a) const {
  const uint32_t take = geq_mask<N>(a->w, m_.w, len_);
  uint32_t borrow = 0;
  const uint32_t mask = 0 - take;
  for (uint32_t i = 0; i < len_; ++i) {
    const uint64_t d = static_cast<uint64_t>(a->w[i]) - (m_.w[i] & mask) - borrow;
    a->w[i] = static_cast<uint32_t>(d);
    borrow = static_cast<uint32_t>(d >> 63);
  }
}

template <uint32_t N>
void Mont<N>::add(const Num& a, const Num& b, Num* out) const {
  uint32_t t[N + 1];
  uint64_t carry = 0;
  for (uint32_t i = 0; i < len_; ++i) {
    const uint64_t s = static_cast<uint64_t>(a.w[i]) + b.w[i] + carry;
    t[i] = static_cast<uint32_t>(s);
    carry = s >> 32;
  }
  // subtract m if the sum overflowed or is >= m
  const uint32_t take = static_cast<uint32_t>(carry) | geq_mask<N>(t, m_.w, len_);
  const uint32_t mask = 0 - take;
  uint32_t borrow = 0;
  for (uint32_t i = 0; i < len_; ++i) {
    const uint64_t d = static_cast<uint64_t>(t[i]) - (m_.w[i] & mask) - borrow;
    out->w[i] = static_cast<uint32_t>(d);
    borrow = static_cast<uint32_t>(d >> 63);
  }
  for (uint32_t i = len_; i < N; ++i) out->w[i] = 0;
}

template <uint32_t N>
void Mont<N>::sub(const Num& a, const Num& b, Num* out) const {
  uint32_t t[N];
  uint32_t borrow = 0;
  for (uint32_t i = 0; i < len_; ++i) {
    const uint64_t d = static_cast<uint64_t>(a.w[i]) - b.w[i] - borrow;
    t[i] = static_cast<uint32_t>(d);
    borrow = static_cast<uint32_t>(d >> 63);
  }
  const uint32_t mask = 0 - borrow;   // add m back if it went negative
  uint64_t carry = 0;
  for (uint32_t i = 0; i < len_; ++i) {
    const uint64_t s = static_cast<uint64_t>(t[i]) + (m_.w[i] & mask) + carry;
    out->w[i] = static_cast<uint32_t>(s);
    carry = s >> 32;
  }
  for (uint32_t i = len_; i < N; ++i) out->w[i] = 0;
}

// Coarsely integrated operand scanning (CIOS) Montgomery multiplication.
template <uint32_t N>
void Mont<N>::mul(const Num& a, const Num& b, Num* out) const {
  uint32_t t[N + 2] = {};
  for (uint32_t i = 0; i < len_; ++i) {
    uint64_t carry = 0;
    for (uint32_t j = 0; j < len_; ++j) {
      const uint64_t s = static_cast<uint64_t>(t[j]) + static_cast<uint64_t>(a.w[j]) * b.w[i] + carry;
      t[j] = static_cast<uint32_t>(s);
      carry = s >> 32;
    }
    uint64_t s = static_cast<uint64_t>(t[len_]) + carry;
    t[len_] = static_cast<uint32_t>(s);
    t[len_ + 1] = static_cast<uint32_t>(s >> 32);
    const uint32_t q = t[0] * n0_;
    s = static_cast<uint64_t>(t[0]) + static_cast<uint64_t>(q) * m_.w[0];
    carry = s >> 32;
    for (uint32_t j = 1; j < len_; ++j) {
      s = static_cast<uint64_t>(t[j]) + static_cast<uint64_t>(q) * m_.w[j] + carry;
      t[j - 1] = static_cast<uint32_t>(s);
      carry = s >> 32;
    }
    s = static_cast<uint64_t>(t[len_]) + carry;
    t[len_ - 1] = static_cast<uint32_t>(s);
    t[len_] = t[len_ + 1] + static_cast<uint32_t>(s >> 32);
  }
  // t (len_ + 1 limbs) < 2m: subtract m if t >= m
  const uint32_t take = t[len_] | geq_mask<N>(t, m_.w, len_);
  const uint32_t mask = 0 - take;
  uint32_t borrow = 0;
  for (uint32_t i = 0; i < len_; ++i) {
    const uint64_t d = static_cast<uint64_t>(t[i]) - (m_.w[i] & mask) - borrow;
    out->w[i] = static_cast<uint32_t>(d);
    borrow = static_cast<uint32_t>(d >> 63);
  }
  for (uint32_t i = len_; i < N; ++i) out->w[i] = 0;
}

template <uint32_t N>
void Mont<N>::from_mont(const Num& a, Num* out) const {
  Num one;
  one.w[0] = 1;
  mul(a, one, out);
}

template <uint32_t N>
bool Mont<N>::init(ConstByteView modulus_be) {
  *this = Mont();
  uint64_t skip = 0;
  while (skip < modulus_be.size() && modulus_be[skip] == 0) ++skip;
  const uint64_t bytes = modulus_be.size() - skip;
  if (bytes == 0 || bytes > 4ull * N) return false;
  len_ = static_cast<uint32_t>((bytes + 3) / 4);
  if (!load(modulus_be, &m_)) return false;
  if ((m_.w[0] & 1) == 0 || (len_ == 1 && m_.w[0] < 3)) return false;
  // n0 = -m^-1 mod 2^32 by Newton iteration
  uint32_t inv = m_.w[0];
  for (int i = 0; i < 5; ++i) inv *= 2 - m_.w[0] * inv;
  n0_ = 0 - inv;
  // R mod m and R^2 mod m by doubling 1 for 32 * len and then another 32 * len times
  Num x;
  x.w[0] = 1;
  auto double_mod = [&] {
    uint32_t carry = 0;
    for (uint32_t i = 0; i < len_; ++i) {
      const uint32_t next = x.w[i] >> 31;
      x.w[i] = (x.w[i] << 1) | carry;
      carry = next;
    }
    const uint32_t take = carry | geq_mask<N>(x.w, m_.w, len_);
    const uint32_t mask = 0 - take;
    uint32_t borrow = 0;
    for (uint32_t i = 0; i < len_; ++i) {
      const uint64_t d = static_cast<uint64_t>(x.w[i]) - (m_.w[i] & mask) - borrow;
      x.w[i] = static_cast<uint32_t>(d);
      borrow = static_cast<uint32_t>(d >> 63);
    }
  };
  // 1 may already be >= m only for m = 1, which was refused above
  for (uint32_t i = 0; i < 32 * len_; ++i) double_mod();
  one_ = x;
  for (uint32_t i = 0; i < 32 * len_; ++i) double_mod();
  r2_ = x;
  return true;
}

template <uint32_t N>
void Mont<N>::pow(const Num& base, ConstByteView exponent_be, Num* out) const {
  Num result = one_;
  for (uint64_t i = 0; i < exponent_be.size(); ++i)
    for (int bit = 7; bit >= 0; --bit) {
      sqr(result, &result);
      if ((exponent_be[i] >> bit) & 1) mul(result, base, &result);
    }
  *out = result;
}

template <uint32_t N>
void Mont<N>::invert_prime(const Num& a, Num* out) const {
  uint8_t exponent[4 * N];
  Num e = m_;
  // m - 2 (m is odd, so only the low limb changes unless it is 1, which init() refuses)
  e.w[0] -= 2;
  if (m_.w[0] < 2) {
    for (uint32_t i = 1; i < len_; ++i)
      if (e.w[i]-- != 0) break;
  }
  store(e, ByteView(exponent, 4 * len_));
  pow(a, ConstByteView(exponent, 4 * len_), out);
}

template class Mont<130>;
template class Mont<12>;

}  // namespace crypto
