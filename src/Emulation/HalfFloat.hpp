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
#include "Emulation/SoftFloat.hpp"

// IEEE half precision (the F16C and AVX-512 conversions).
namespace emulation {
namespace half {

using softfloat::Rounding;
using softfloat::kDenormal;
using softfloat::kInexact;
using softfloat::kInvalid;
using softfloat::kOverflow;
using softfloat::kUnderflow;

inline uint32_t half_to_single(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 0x3FF;
  if (exp == 0) {
    if (man == 0) return sign << 31;
    uint32_t e = 113, m = man;      // a denormal: normalize
    while (!(m & 0x400)) { m <<= 1; --e; }
    return (sign << 31) | (e << 23) | ((m & 0x3FF) << 13);
  }
  if (exp == 31) return (sign << 31) | 0x7F800000u | (man ? 0x400000u | (man << 13) : 0);
  return (sign << 31) | ((exp + 112) << 23) | (man << 13);
}

inline uint16_t single_to_half(uint32_t bits, Rounding mode, uint32_t* flags) {
  const uint32_t sign = bits >> 31, exp = (bits >> 23) & 255, man = bits & 0x7FFFFF;
  const uint16_t s = static_cast<uint16_t>(sign << 15);
  if (exp == 255) {
    if (man && !(man & 0x400000)) *flags |= kInvalid;
    return static_cast<uint16_t>(s | 0x7C00 | (man ? 0x200 | (man >> 13) : 0));
  }
  if (exp == 0 && man == 0) return s;
  if (exp == 0) *flags |= kDenormal;
  const int32_t e = static_cast<int32_t>(exp) - 127;
  uint32_t m = man | (exp ? 0x800000u : 0);      // with the integer bit
  int32_t he = e + 15;
  uint32_t shift = 13;
  if (he <= 0) {      // the half result is a denormal
    shift = static_cast<uint32_t>(13 + 1 - he);
    he = 0;
  }
  if (shift > 31) {      // far below the smallest denormal
    *flags |= kInexact | kUnderflow;
    return static_cast<uint16_t>(s | (((mode == Rounding::Up && !sign) || (mode == Rounding::Down && sign)) ? 1 : 0));
  }
  uint32_t kept = m >> shift;
  const uint32_t rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
  if (rem) *flags |= kInexact;
  if (rem && he == 0) *flags |= kUnderflow;
  bool inc = false;
  switch (mode) {
    case Rounding::NearestEven: inc = rem > half || (rem == half && (kept & 1)); break;
    case Rounding::Down: inc = sign && rem; break;
    case Rounding::Up: inc = !sign && rem; break;
    default: break;
  }
  kept += inc ? 1 : 0;
  uint32_t result = he == 0 ? kept : ((static_cast<uint32_t>(he) - 1) << 10) + kept;
  if (he != 0 && (result >> 10) >= 31) { *flags |= kOverflow | kInexact; return static_cast<uint16_t>(s | ((mode == Rounding::TowardZero || (mode == Rounding::Down && !sign) || (mode == Rounding::Up && sign)) ? 0x7BFF : 0x7C00)); }
  return static_cast<uint16_t>(s | result);
}

}  // namespace half
}  // namespace emulation
