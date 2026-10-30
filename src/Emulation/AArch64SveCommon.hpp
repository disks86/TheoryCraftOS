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
#include "Emulation/AArch64NeonCommon.hpp"
#include "Emulation/Wide.hpp"
#include "Emulation/SoftFloat.hpp"

// Helpers shared by the SVE files (AArch64Sve*.cpp). The vector length is fixed at 128 bits, so a Z register is the low and high word of the V register of the same number, a
// predicate register is 16 bits (one per byte of the vector), and an element of size 2^esz bytes is active when the predicate bit at (index << esz) is set.
namespace emulation {
namespace sve {

using namespace neon;

constexpr uint32_t kVectorBytes = 16;

inline uint32_t element_count(uint32_t esz) { return kVectorBytes >> esz; }
inline uint32_t element_bits(uint32_t esz) { return 8u << esz; }
inline bool active(uint16_t pg, uint32_t index, uint32_t esz) { return ((pg >> (index << esz)) & 1) != 0; }
inline uint16_t element_mask(uint32_t esz) {      // the predicate bits an element of this size can use
  uint16_t m = 0;
  for (uint32_t i = 0; i < kVectorBytes; i += 1u << esz) m = static_cast<uint16_t>(m | (1u << i));
  return m;
}
inline uint64_t get(const Vec& v, uint32_t esz, uint32_t i) { return lane(v, element_bits(esz), i); }
inline void put(Vec* v, uint32_t esz, uint32_t i, uint64_t x) { set_lane(v, element_bits(esz), i, x); }

// The element count a predicate pattern (as in PTRUE and CNTx) selects for a vector of `elements` elements.
inline uint32_t pattern_count(uint32_t pattern, uint32_t elements) {
  switch (pattern) {
    case 0: {
      uint32_t n = 1;
      while (n * 2 <= elements) n *= 2;
      return n;
    }
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: return pattern <= elements ? pattern : 0;
    case 9: return 16 <= elements ? 16 : 0;
    case 10: return 32 <= elements ? 32 : 0;
    case 11: return 64 <= elements ? 64 : 0;
    case 12: return 128 <= elements ? 128 : 0;
    case 13: return 256 <= elements ? 256 : 0;
    case 29: return elements - elements % 4;
    case 30: return elements - elements % 3;
    case 31: return elements;
    default: return 0;
  }
}

// The all-ones value of an element and its sign bit.
inline uint64_t element_ones(uint32_t esz) { return ones(element_bits(esz)); }

// Rotate `x` right by `n` within `w` bits.
inline uint64_t rotate_right(uint64_t x, uint32_t n, uint32_t w) {
  n %= w;
  x &= ones(w);
  return n == 0 ? x : ((x >> n) | (x << (w - n))) & ones(w);
}

// Predicate-as-counter registers (SVE2.1 / SME2): bits 3:0 hold a marker (the lowest set bit gives the element size, 0 = byte .. 3 = doubleword), bits 6:marker+1 the element count and bit 15
// inverts the meaning. The predicate for vector `segment` of the group, at element size `esz` (a counter made for a wider element keeps that element's spacing).
inline uint16_t counter_to_predicate(uint16_t pn, uint32_t esz, uint32_t segment) {
  if ((pn & 15) == 0) return 0;
  uint32_t marker = 0;
  while (!((pn >> marker) & 1)) ++marker;
  const bool invert = (pn >> 15) & 1;
  const uint32_t count = (pn & 0x7Fu) >> (marker + 1);      // (only bits 6:marker+1 count; the higher bits are ignored)
  const uint32_t es = esz > marker ? esz : marker;
  const uint32_t elements = ((count << marker) + (1u << es) - 1) >> es, per_vector = 16u >> es;
  uint16_t r = 0;
  for (uint32_t i = 0; i < per_vector; ++i) {
    const bool on = (segment * per_vector + i < elements) != invert;
    if (on) r = static_cast<uint16_t>(r | (1u << (i << es)));
  }
  return r;
}

// A counter for `run` leading active elements out of `total` (ascending) or trailing ones (descending, which the inverted form expresses).
inline uint16_t make_counter(uint32_t run, uint32_t total, uint32_t esz, bool ascending) {
  const uint16_t marker = static_cast<uint16_t>(1u << esz);
  if (run >= total) return static_cast<uint16_t>(0x8000u | marker);
  if (run == 0) return 0;
  return ascending ? static_cast<uint16_t>(marker | (run << (esz + 1))) : static_cast<uint16_t>(0x8000u | marker | ((total - run) << (esz + 1)));
}

// A logical immediate (N:immr:imms) as the 64-bit pattern it replicates; false for a reserved encoding.
inline bool decode_logical_immediate(uint32_t imm13, uint64_t* out) {
  const uint32_t n = (imm13 >> 12) & 1, immr = (imm13 >> 6) & 63, imms = imm13 & 63;
  const uint32_t combined = (n << 6) | (~imms & 0x3Fu);
  if (combined == 0) return false;
  uint32_t len = 6;
  while (!((combined >> len) & 1)) --len;
  if (len < 1) return false;
  const uint32_t esize = 1u << len, levels = esize - 1;
  if ((imms & levels) == levels) return false;
  const uint32_t s = imms & levels, r = immr & levels;
  const uint64_t element = rotate_right(ones(s + 1), r, esize);
  uint64_t value = 0;
  for (uint32_t i = 0; i < 64; i += esize) value |= element << i;
  *out = value;
  return true;
}

// The low 64 bits of a product (wrapping, so a 64-bit element times itself is not signed overflow).
inline uint64_t product_bits(int64_t a, int64_t b) { return static_cast<uint64_t>(a) * static_cast<uint64_t>(b); }

// A signed 64 x 64 product as a 128-bit integer (built from the unsigned product, so that no compiler runtime routine is needed).
inline I128 multiply_signed(int64_t a, int64_t b) {
  const bool negative = (a < 0) != (b < 0);
  uint64_t hi, lo;
  multiply_unsigned(a < 0 ? uint64_t{0} - static_cast<uint64_t>(a) : static_cast<uint64_t>(a), b < 0 ? uint64_t{0} - static_cast<uint64_t>(b) : static_cast<uint64_t>(b), &hi, &lo);
  const I128 magnitude = (static_cast<I128>(hi) << 64) | lo;
  return negative ? -magnitude : magnitude;
}

// (acc * 2^w + 2 * prod + round) >> w, evaluated as acc + ((prod + 2^(w-2)) >> (w-1)) so that nothing exceeds 128 bits even for 64-bit elements.
inline I128 doubling_high(I128 acc, I128 prod, uint32_t w, bool rounding) {
  const I128 half_unit = rounding ? (static_cast<I128>(1) << (w - 2)) : 0;
  return acc + ((prod + half_unit) >> (w - 1));
}

// The BFloat16 dot product step used by BFDOT, BFMMLA and BFMOPA: exact products, every add rounded to odd, denormals flushed, the default NaN, no exception flags. `sum` is single precision,
// the four others are BFloat16 values in the low 16 bits.
inline uint64_t bfloat_odd_op(bool is_mul, uint64_t x, uint64_t y, const softfloat::Env& base) {
  using namespace softfloat;
  if (is_nan(Format::F32, x) || is_nan(Format::F32, y)) return 0x7FC00000u;
  Env e = base;
  e.mode = Rounding::TowardZero;
  e.flags = 0;
  e.daz = true;
  e.ftz = true;
  uint64_t r = is_mul ? mul(Format::F32, x, y, e) : add(Format::F32, x, y, e);
  if (is_nan(Format::F32, r)) return 0x7FC00000u;
  if (e.flags & kOverflow) return (r & 0x80000000u) | 0x7F800000u;      // (an overflow gives an infinity)
  if ((e.flags & kInexact) && (r & 0x7FFFFFFFu) != 0x7F800000u) r |= 1;
  return r;
}
inline uint64_t bfloat_dot_add(uint64_t sum, uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1, const softfloat::Env& base) {
  const uint64_t p1 = bfloat_odd_op(true, (a0 & 0xFFFF) << 16, (b0 & 0xFFFF) << 16, base), p2 = bfloat_odd_op(true, (a1 & 0xFFFF) << 16, (b1 & 0xFFFF) << 16, base);
  return bfloat_odd_op(false, sum, bfloat_odd_op(false, p1, p2, base), base);
}

// The high half of a 64 x 64 product, signed or unsigned.
uint64_t multiply_high(uint64_t a, uint64_t b, bool is_signed);

// FEXPA: the exponential-approximation helper (AArch64SveFloat.cpp).
uint64_t sve_expa(uint32_t esz, uint64_t x);

}  // namespace sve
}  // namespace emulation
