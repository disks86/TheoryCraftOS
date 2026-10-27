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

// The RISC-V floating-point rules the scalar (F, D) and vector instructions share.
namespace emulation {
namespace rvfp {

using namespace softfloat;

constexpr uint64_t kCanonicalSingle = 0x7FC00000u, kCanonicalDouble = 0x7FF8000000000000ull;

// Every NaN a RISC-V floating-point operation computes is the canonical NaN, whatever the payload of its operands.
inline uint64_t canonical(Format f, uint64_t bits) { return is_nan(f, bits) ? (f == Format::F32 ? kCanonicalSingle : kCanonicalDouble) : bits; }

inline uint64_t sign_bit_of(Format f) { return f == Format::F32 ? 0x80000000u : 0x8000000000000000ull; }

// FCLASS: one bit of the result for each of the ten classes.
inline uint64_t classify(Format f, uint64_t bits) {
  const bool single = f == Format::F32;
  const uint32_t man_bits = single ? 23 : 52, exp_bits = single ? 8 : 11;
  const bool negative = (bits >> (man_bits + exp_bits)) & 1;
  const uint64_t exponent = (bits >> man_bits) & ((uint64_t{1} << exp_bits) - 1), mantissa = bits & ((uint64_t{1} << man_bits) - 1);
  if (exponent == (uint64_t{1} << exp_bits) - 1) {
    if (mantissa == 0) return negative ? 1 : 1u << 7;
    return (mantissa >> (man_bits - 1)) & 1 ? 1u << 9 : 1u << 8;
  }
  if (exponent == 0) {
    if (mantissa == 0) return negative ? 1u << 3 : 1u << 4;
    return negative ? 1u << 2 : 1u << 5;
  }
  return negative ? 1u << 1 : 1u << 6;
}

// FMIN / FMAX: a number beats a NaN, two NaNs give the canonical NaN, -0 is below +0, and a signaling NaN raises invalid.
inline uint64_t minmax(Format f, uint64_t x, uint64_t y, bool is_max, Env& env) {
  const bool nx = is_nan(f, x), ny = is_nan(f, y);
  const uint64_t quiet = f == Format::F32 ? 0x400000u : 0x8000000000000ull;
  if ((nx && !(x & quiet)) || (ny && !(y & quiet))) env.flags |= kInvalid;
  if (nx && ny) return f == Format::F32 ? kCanonicalSingle : kCanonicalDouble;
  if (nx) return y;
  if (ny) return x;
  const Order order = compare(f, x, y, false, env);
  const uint64_t sign = sign_bit_of(f);
  bool x_is_less;
  if (order == Order::Equal) x_is_less = (x & sign) != 0;
  else x_is_less = order == Order::Less;
  const bool pick_x = !is_max ? x_is_less : (order == Order::Equal ? !(x & sign) : !x_is_less);
  return pick_x ? x : y;
}

}  // namespace rvfp
}  // namespace emulation
