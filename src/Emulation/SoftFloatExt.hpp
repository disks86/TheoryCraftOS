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
#include "Emulation/SoftFloat.hpp"

// x87 double-extended precision (80 bits: a 64-bit significand with an explicit integer bit, a 15-bit exponent, a sign) in integer operations. Results follow the x87:
// rounding per the control word's RC field, the significand rounded to the PC field's 24, 53 or 64 bits (the exponent range stays that of the extended format), the default
// "real indefinite" NaN, and the status word's exception flags (kInvalid .. kInexact are in the same bit positions as in the MXCSR).
namespace emulation {
namespace softfloat {

struct Ext {
  uint64_t mant = 0;
  uint16_t se = 0;      // sign in bit 15, biased exponent in bits 0..14
  bool sign() const { return (se & 0x8000) != 0; }
  uint32_t exponent() const { return se & 0x7FFF; }
};

namespace ext {

// How many significand bits results are rounded to: the control word's PC field (0 = 24, 2 = 53, 3 = 64).
inline uint32_t precision_bits(uint32_t pc_field) { return pc_field == 0 ? 24 : pc_field == 2 ? 53 : 64; }

enum class Class : uint8_t { Zero, Denormal, Normal, Infinity, QuietNan, SignalingNan, Unsupported };
Class classify(const Ext& x);
inline Ext indefinite() { Ext e; e.mant = 0xC000000000000000ull; e.se = 0xFFFF; return e; }      // the real indefinite: a negative quiet NaN
inline Ext zero(bool negative) { Ext e; e.se = negative ? 0x8000 : 0; return e; }
inline Ext infinity(bool negative) { Ext e; e.mant = 0x8000000000000000ull; e.se = negative ? 0xFFFF : 0x7FFF; return e; }
inline Ext one() { Ext e; e.mant = 0x8000000000000000ull; e.se = 0x3FFF; return e; }
inline Ext negated(Ext x) { x.se ^= 0x8000; return x; }
inline Ext absolute(Ext x) { x.se &= 0x7FFF; return x; }

// 32-bit and 64-bit floats and integers to extended (exact), and back (rounded per env.mode).
Ext from_single(uint32_t bits, Env& env);
Ext from_double(uint64_t bits, Env& env);
Ext from_integer(int64_t value);
uint32_t to_single(const Ext& x, Env& env);
uint64_t to_double(const Ext& x, Env& env);
// Rounds to an integer of `bits` (16, 32 or 64). False (and the invalid flag) if the value does not fit or is not a number: the caller stores the integer indefinite.
bool to_integer(const Ext& x, uint32_t bits, Rounding mode, Env& env, int64_t* out);

Ext add(const Ext& a, const Ext& b, uint32_t precision, Env& env);
Ext sub(const Ext& a, const Ext& b, uint32_t precision, Env& env);
Ext mul(const Ext& a, const Ext& b, uint32_t precision, Env& env);
Ext div(const Ext& a, const Ext& b, uint32_t precision, Env& env);
Ext sqrt(const Ext& a, uint32_t precision, Env& env);
Ext round_to_integer(const Ext& a, Rounding mode, Env& env);      // FRNDINT
Ext scale(const Ext& a, const Ext& b, Env& env);      // FSCALE: a * 2^trunc(b)
// FPREM / FPREM1: the partial remainder of a / b. Returns the new value; *complete is false if the exponents were too far apart for one step (C2 set: run it again) and
// *quotient_bits then is not meaningful, otherwise it holds the low three bits of the quotient (C0 = bit 2, C3 = bit 1, C1 = bit 0).
Ext remainder(const Ext& a, const Ext& b, bool nearest, bool* complete, uint32_t* quotient_bits, Env& env);
// FXTRACT: *significand and *exponent from a.
void extract(const Ext& a, Ext* significand, Ext* exponent, Env& env);

// -1, 0, 1 for less, equal, greater; 2 for unordered. `ordered_only`: an unordered result from a quiet NaN raises invalid too (FCOM, as opposed to FUCOM).
int compare(const Ext& a, const Ext& b, bool ordered_only, Env& env);

// The transcendental instructions. They work in the extended format with a few extra guard bits' worth of care and are accurate to a unit or two in the last place (the
// hardware's own results are not exactly rounded either). Out-of-range operands: *ok false (the instruction does nothing, C2 is set).
Ext f2xm1(const Ext& x, Env& env);                              // 2^x - 1, |x| <= 1
Ext fyl2x(const Ext& x, const Ext& y, Env& env);               // y * log2(x), x > 0
Ext fyl2xp1(const Ext& x, const Ext& y, Env& env);             // y * log2(x + 1)
Ext fpatan(const Ext& y, const Ext& x, Env& env);              // atan2(y, x)
bool sin_cos(const Ext& x, Ext* sine, Ext* cosine, Env& env);  // false: |x| >= 2^63
bool tangent(const Ext& x, Ext* tan, Env& env);

}  // namespace ext
}  // namespace softfloat
}  // namespace emulation
