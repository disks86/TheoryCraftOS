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

// IEEE 754 binary32 and binary64 arithmetic in integer operations, with x86 SSE semantics (default NaN, NaN selection, MXCSR rounding modes and exception flags).
// The kernel cannot assume a floating-point unit (the AArch64 build forbids it), and the host's own float would not be bit-exact with an x86 anyway.
namespace emulation {
namespace softfloat {

enum class Format : uint8_t { F32, F64 };
enum class Rounding : uint8_t { NearestEven = 0, Down = 1, Up = 2, TowardZero = 3, TieAway = 4 };      // 0..3 are the MXCSR.RC encoding; TieAway for conversions to integers and, with RISC-V's RMM, for arithmetic
// MXCSR exception flag bits.
constexpr uint32_t kInvalid = 1, kDenormal = 2, kDivideByZero = 4, kOverflow = 8, kUnderflow = 16, kInexact = 32;

struct Env {
  Rounding mode = Rounding::NearestEven;
  uint32_t flags = 0;      // OR-ed in as exceptions happen (all exceptions are treated as masked)
  bool rounded_up = false; // x87: the last inexact result was rounded away from zero (the status word's C1)
  bool arm = false;        // AArch64 rules: the default NaN is positive, and a signaling NaN operand is chosen before a quiet one
  bool tininess_after = false;      // RISC-V: underflow is detected after rounding (x86 and AArch64: before); TieAway then also rounds arithmetic results
};

// Values are bit patterns: a binary32 in the low 32 bits of the word.
uint64_t add(Format f, uint64_t a, uint64_t b, Env& env);
uint64_t sub(Format f, uint64_t a, uint64_t b, Env& env);
uint64_t mul(Format f, uint64_t a, uint64_t b, Env& env);
uint64_t div(Format f, uint64_t a, uint64_t b, Env& env);
uint64_t sqrt(Format f, uint64_t a, Env& env);
// binary32 <-> binary64.
uint64_t convert(Format from, Format to, uint64_t a, Env& env);
// Signed integer (already sign-extended into `value`) to float.
uint64_t from_int(Format f, int64_t value, Env& env);
// Float to a signed integer of `bits` (32 or 64), rounding per the environment or toward zero. On overflow, NaN or infinity: the "integer indefinite" (only the sign bit
// set) and the invalid flag.
int64_t to_int(Format f, uint64_t a, uint32_t bits, bool truncate, Env& env);

enum class Order : uint8_t { Less, Equal, Greater, Unordered };
// `signaling`: any NaN raises invalid (COMIS, CMPLT...); otherwise only a signaling NaN does (UCOMIS, CMPEQ...).
Order compare(Format f, uint64_t a, uint64_t b, bool signaling, Env& env);
// MINSS/MAXSS: when the operands are unordered or equal the second one is the result.
uint64_t minimum(Format f, uint64_t a, uint64_t b, Env& env);
uint64_t maximum(Format f, uint64_t a, uint64_t b, Env& env);

// --- what AArch64 needs beyond SSE ---------------------------------------------------------------------------------------------------------------
// a * b + c with one rounding.
uint64_t fused_multiply_add(Format f, uint64_t a, uint64_t b, uint64_t c, Env& env);
// Float to an integer of `bits` bits (up to 64), signed or not, after scaling by 2^fbits, rounded per `mode` (any of the five). Out of range and infinities saturate; a NaN
// gives 0; both raise invalid. Returns the two's complement bits.
uint64_t to_integer_saturating(Format f, uint64_t a, Rounding mode, uint32_t bits, bool is_unsigned, uint32_t fbits, Env& env);
// An integer (magnitude and sign) times 2^-fbits to float.
uint64_t from_integer(Format f, uint64_t magnitude, bool negative, uint32_t fbits, Env& env);
// FRINT*: the value rounded to an integral value of the same format. `exact` raises inexact when the value changed.
uint64_t round_to_integral(Format f, uint64_t a, Rounding mode, bool exact, Env& env);
// FMAX/FMIN (a NaN operand gives a NaN) and FMAXNM/FMINNM (a quiet NaN loses to a number); +0 is above -0.
uint64_t arm_minmax(Format f, uint64_t a, uint64_t b, bool is_max, bool number, Env& env);
uint64_t negate(Format f, uint64_t a);      // flips the sign bit (also of a NaN)
uint64_t absolute(Format f, uint64_t a);
bool is_nan(Format f, uint64_t a);

}  // namespace softfloat
}  // namespace emulation
