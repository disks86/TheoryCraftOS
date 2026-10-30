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
#include "Emulation/AArch64Cpu.hpp"
#include "Emulation/Wide.hpp"

// Helpers shared by the Advanced SIMD files (AArch64Neon*.cpp). 128-bit integers are used only for additions, subtractions, shifts and comparisons; a product is formed in 64 bits first (no
// compiler runtime call is needed on the Arm and RISC-V kernels).
namespace emulation {
namespace neon {

using Vec = AArch64Cpu::Vec;
using I128 = __int128;

constexpr uint32_t bits_of(uint32_t insn, uint32_t hi, uint32_t lo) { return (insn >> lo) & ((1u << (hi - lo + 1)) - 1); }
constexpr uint64_t ones(uint32_t n) { return n >= 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1; }
constexpr int64_t sext(uint64_t v, uint32_t bits) { return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }

inline uint64_t lane(const Vec& v, uint32_t w, uint32_t i) { return lane_get(v.lo, v.hi, w, i); }
inline void set_lane(Vec* v, uint32_t w, uint32_t i, uint64_t x) { lane_set(&v->lo, &v->hi, w, i, x); }

inline I128 signed_of(uint64_t v, uint32_t w) { return static_cast<I128>(sext(v, w)); }
inline I128 unsigned_of(uint64_t v, uint32_t w) { return static_cast<I128>(v & ones(w)); }

// The two's complement bits of `v` in `w` bits after saturating to the signed or unsigned range; *sat is set when it had to.
inline uint64_t saturate_to_signed(I128 v, uint32_t w, bool* sat) {
  const I128 hi = (static_cast<I128>(1) << (w - 1)) - 1, lo = -(static_cast<I128>(1) << (w - 1));
  if (v > hi) { *sat = true; return static_cast<uint64_t>(hi) & ones(w); }
  if (v < lo) { *sat = true; return static_cast<uint64_t>(lo) & ones(w); }
  return static_cast<uint64_t>(v) & ones(w);
}
inline uint64_t saturate_to_unsigned(I128 v, uint32_t w, bool* sat) {
  const I128 hi = (static_cast<I128>(1) << w) - 1;
  if (v > hi) { *sat = true; return ones(w); }
  if (v < 0) { *sat = true; return 0; }
  return static_cast<uint64_t>(v);
}

// v >> n, arithmetic, with the shifted-out half added first when `round` is set (n >= 1).
inline I128 shift_right(I128 v, uint32_t n, bool round) {
  if (n >= 127) return v < 0 && !round ? -1 : 0;
  if (round) v += static_cast<I128>(1) << (n - 1);
  return v >> n;
}

}  // namespace neon
}  // namespace emulation

// --- floating-point helpers shared by the scalar and Advanced SIMD code ---------------------------------------------------------------------------------------
#include "Emulation/SoftFloat.hpp"
namespace emulation {
namespace neon {

inline uint64_t fp_default_nan(softfloat::Format f) {
  return f == softfloat::Format::F64 ? 0x7FF8000000000000ull : f == softfloat::Format::F32 ? 0x7FC00000u : f == softfloat::Format::BF16 ? 0x7FC0u : 0x7E00u;
}

struct FpShape {
  uint32_t exp_bits, frac_bits;
};
inline FpShape shape_of(softfloat::Format f) {
  return f == softfloat::Format::F64 ? FpShape{11, 52} : f == softfloat::Format::F32 ? FpShape{8, 23} : f == softfloat::Format::BF16 ? FpShape{8, 7} : FpShape{5, 10};
}

// The integer cores of the reciprocal and reciprocal square root estimates (Arm pseudocode RecipEstimate / RecipSqrtEstimate): 9-bit fixed-point in and out.
inline uint32_t recip_estimate(uint32_t a) {
  a = a * 2 + 1;
  const uint32_t b = (1u << 19) / a;
  return (b + 1) / 2;
}
inline uint32_t recip_sqrt_estimate(uint32_t a) {
  if (a < 256) a = a * 2 + 1;
  else a = (((a >> 1) << 1) + 1) * 2;
  uint64_t b = 512;
  while (static_cast<uint64_t>(a) * (b + 1) * (b + 1) < (uint64_t{1} << 28)) ++b;
  return static_cast<uint32_t>((b + 1) / 2);
}

// FRECPE: the reciprocal estimate of one element. `flush` is FPCR.FZ.
inline uint64_t fp_recip_estimate(softfloat::Format f, uint64_t x, softfloat::Env& env, bool flush) {
  using namespace softfloat;
  const FpShape s = shape_of(f);
  const uint32_t E = s.exp_bits, M = s.frac_bits;
  const int64_t bias = (int64_t{1} << (E - 1)) - 1;
  const uint64_t sign_bit = uint64_t{1} << (E + M), sign = x & sign_bit;
  int64_t exp = static_cast<int64_t>((x >> M) & ones(E));
  uint64_t frac = x & ones(M);
  const uint64_t inf = ones(E) << M;
  if (exp == static_cast<int64_t>(ones(E)) && frac != 0) {      // NaN
    if (!((frac >> (M - 1)) & 1)) env.flags |= kInvalid;
    return x | (uint64_t{1} << (M - 1));
  }
  if (exp == static_cast<int64_t>(ones(E))) return sign;      // infinity -> zero
  if (exp == 0 && frac == 0) { env.flags |= kDivideByZero; return sign | inf; }
  if (exp == 0 && (frac >> (M - 2)) == 0) {      // too small: the reciprocal overflows
    env.flags |= kOverflow | kInexact;
    const bool negative = sign != 0;
    bool to_inf;
    switch (env.mode) {
      case Rounding::Up: to_inf = !negative; break;
      case Rounding::Down: to_inf = negative; break;
      case Rounding::TowardZero: to_inf = false; break;
      default: to_inf = true; break;
    }
    return to_inf ? (sign | inf) : (sign | ((ones(E) - 1) << M) | ones(M));
  }
  if (flush && exp >= (int64_t{1} << E) - 3) {      // a huge operand whose reciprocal would be a denormal
    env.flags |= kUnderflow;
    return sign;
  }
  if (exp == 0) {
    if (!((frac >> (M - 1)) & 1)) { exp = -1; frac = (frac << 2) & ones(M); }
    else frac = (frac << 1) & ones(M);
  }
  const uint32_t scaled = (1u << 8) | static_cast<uint32_t>(frac >> (M - 8));
  int64_t result_exp = 2 * bias - 1 - exp;
  const uint32_t estimate = recip_estimate(scaled);
  uint64_t fraction = static_cast<uint64_t>(estimate & 0xFF) << (M - 8);
  if (result_exp == 0) fraction = (uint64_t{1} << (M - 1)) | (fraction >> 1);
  else if (result_exp == -1) { fraction = (uint64_t{1} << (M - 2)) | (fraction >> 2); result_exp = 0; }
  return sign | (static_cast<uint64_t>(result_exp) << M) | fraction;
}

// FRSQRTE: the reciprocal square root estimate of one element.
inline uint64_t fp_recip_sqrt_estimate(softfloat::Format f, uint64_t x, softfloat::Env& env, uint64_t default_nan) {
  using namespace softfloat;
  const FpShape s = shape_of(f);
  const uint32_t E = s.exp_bits, M = s.frac_bits;
  const int64_t bias = (int64_t{1} << (E - 1)) - 1;
  const uint64_t sign_bit = uint64_t{1} << (E + M), sign = x & sign_bit;
  int64_t exp = static_cast<int64_t>((x >> M) & ones(E));
  uint64_t frac = x & ones(M);
  const uint64_t inf = ones(E) << M;
  if (exp == static_cast<int64_t>(ones(E)) && frac != 0) {
    if (!((frac >> (M - 1)) & 1)) env.flags |= kInvalid;
    return x | (uint64_t{1} << (M - 1));
  }
  if (exp == 0 && frac == 0) { env.flags |= kDivideByZero; return sign | inf; }
  if (sign) { env.flags |= kInvalid; return default_nan; }
  if (exp == static_cast<int64_t>(ones(E))) return 0;
  if (exp == 0) {
    while (!((frac >> (M - 1)) & 1)) { frac = (frac << 1) & ones(M); --exp; }
    frac = (frac << 1) & ones(M);
  }
  uint32_t scaled;
  if (!(exp & 1)) scaled = (1u << 8) | static_cast<uint32_t>(frac >> (M - 8));
  else scaled = (1u << 7) | static_cast<uint32_t>(frac >> (M - 7));
  const int64_t result_exp = (3 * bias - 1 - exp) / 2;
  const uint32_t estimate = recip_sqrt_estimate(scaled);
  return (static_cast<uint64_t>(result_exp) << M) | (static_cast<uint64_t>(estimate & 0xFF) << (M - 8));
}

// FRINT32Z / FRINT32X / FRINT64Z / FRINT64X: round to an integral value that fits in `width` bits; anything else (and NaN) becomes -2^(width-1) with the invalid flag.
inline uint64_t frint_to_width(softfloat::Format f, uint64_t a, uint32_t width, softfloat::Rounding mode, bool exact, softfloat::Env& env) {
  using namespace softfloat;
  Env local = env;
  local.flags = 0;
  Env range = env;
  range.flags = 0;
  bool bad = is_nan(f, a);
  uint64_t rounded = a;
  if (!bad) {
    rounded = round_to_integral(f, a, mode, exact, local);
    to_integer_saturating(f, rounded, Rounding::TowardZero, width, false, 0, range);
    bad = (range.flags & kInvalid) != 0;
  }
  if (bad) {
    env.flags |= kInvalid;
    return from_integer(f, uint64_t{1} << (width - 1), true, 0, range);
  }
  env.flags |= local.flags;
  return rounded;
}

}  // namespace neon
}  // namespace emulation
