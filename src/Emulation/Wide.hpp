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

namespace emulation {

// 64x64 -> 128-bit multiply and 128/64 -> 64 divide built from 64-bit operations, so that no compiler runtime routine (__multi3, __udivti3) is needed on any CPU.
inline void multiply_unsigned(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
  const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
  const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  const uint64_t middle = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
  *lo = (p00 & 0xFFFFFFFFu) | (middle << 32);
  *hi = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

// hi:lo / divisor; false if the divisor is zero or the quotient does not fit 64 bits (hi >= divisor). Remainder is optional-free: both outputs are written.
inline bool divide_unsigned(uint64_t hi, uint64_t lo, uint64_t divisor, uint64_t* quotient, uint64_t* remainder) {
  if (divisor == 0 || hi >= divisor) return false;
  uint64_t rem = hi, quo = 0;
  for (int i = 63; i >= 0; --i) {
    const bool carry = (rem >> 63) != 0;
    rem = (rem << 1) | ((lo >> i) & 1);
    quo <<= 1;
    if (carry || rem >= divisor) {
      rem -= divisor;
      quo |= 1;
    }
  }
  *quotient = quo;
  *remainder = rem;
  return true;
}

// The lanes of a 128-bit register held as two words, `width` bits each (8, 16, 32 or 64).
// The lanes of an XMM register, `width` bits each.
inline uint64_t lane_get(const uint64_t lo, const uint64_t hi, uint32_t width, uint32_t index) {
  const uint32_t bit = index * width;
  const uint64_t word = bit < 64 ? lo : hi;
  const uint32_t shift = bit & 63;
  return width == 64 ? word : (word >> shift) & ((uint64_t{1} << width) - 1);
}
inline void lane_set(uint64_t* lo, uint64_t* hi, uint32_t width, uint32_t index, uint64_t value) {
  const uint32_t bit = index * width;
  uint64_t& word = bit < 64 ? *lo : *hi;
  const uint32_t shift = bit & 63;
  if (width == 64) {
    word = value;
    return;
  }
  const uint64_t m = ((uint64_t{1} << width) - 1) << shift;
  word = (word & ~m) | ((value << shift) & m);
}

}  // namespace emulation
