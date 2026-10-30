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

// Advanced SIMD "three registers, same type", vector and scalar: the integer operations (halving, saturating and rounding adds and subtracts, compares, shifts by register, min / max / absolute
// difference, multiplies, pairwise operations, SQDMULH / SQRDMULH), the floating-point operations in single, double and half precision (arithmetic, fused multiply-add, compares, FMULX, FRECPS,
// FRSQRTS, pairwise, FAMAX / FAMIN) and the "extra" group (SQRDMLAH / SQRDMLSH, dot products, FCADD, FCMLA, matrix multiply-accumulate, the half-to-single long multiply-adds).
#include "Emulation/AArch64NeonCommon.hpp"

namespace emulation {

using namespace neon;
using namespace softfloat;

namespace {

// A shift of `v` by the signed amount in the low byte of the second operand (SSHL / USHL and their rounding and saturating forms).
uint64_t shift_by_register(I128 v, int32_t sh, uint32_t w, bool round, bool saturate, bool is_unsigned, bool* sat) {
  if (sh >= 0) {
    if (saturate) {
      if (v == 0) return 0;
      const I128 shifted = v << (sh > 64 ? 64 : sh);
      return is_unsigned ? saturate_to_unsigned(shifted, w, sat) : saturate_to_signed(shifted, w, sat);
    }
    return sh >= static_cast<int32_t>(w) ? 0 : (static_cast<uint64_t>(v) << sh) & ones(w);
  }
  return static_cast<uint64_t>(shift_right(v, static_cast<uint32_t>(-sh), round)) & ones(w);
}

// One element of an integer three-same operation; `acc` is the old destination element.
uint64_t integer_lane(uint32_t opc, bool u, uint32_t w, uint64_t x, uint64_t y, uint64_t acc, bool* sat) {
  const I128 sx = signed_of(x, w), sy = signed_of(y, w), ux = unsigned_of(x, w), uy = unsigned_of(y, w);
  const I128 vx = u ? ux : sx, vy = u ? uy : sy;
  const uint64_t m = ones(w);
  switch (opc) {
    case 0x00: return static_cast<uint64_t>((vx + vy) >> 1) & m;
    case 0x01: return u ? saturate_to_unsigned(ux + uy, w, sat) : saturate_to_signed(sx + sy, w, sat);
    case 0x02: return static_cast<uint64_t>((vx + vy + 1) >> 1) & m;
    case 0x04: return static_cast<uint64_t>((vx - vy) >> 1) & m;
    case 0x05: return u ? saturate_to_unsigned(ux - uy, w, sat) : saturate_to_signed(sx - sy, w, sat);
    case 0x06: return vx > vy ? m : 0;
    case 0x07: return vx >= vy ? m : 0;
    case 0x08: return shift_by_register(vx, static_cast<int8_t>(y & 0xFF), w, false, false, u, sat);
    case 0x09: return shift_by_register(vx, static_cast<int8_t>(y & 0xFF), w, false, true, u, sat);
    case 0x0A: return shift_by_register(vx, static_cast<int8_t>(y & 0xFF), w, true, false, u, sat);
    case 0x0B: return shift_by_register(vx, static_cast<int8_t>(y & 0xFF), w, true, true, u, sat);
    case 0x0C: case 0x14: return static_cast<uint64_t>(vx > vy ? vx : vy) & m;
    case 0x0D: case 0x15: return static_cast<uint64_t>(vx < vy ? vx : vy) & m;
    case 0x0E: return static_cast<uint64_t>(vx > vy ? vx - vy : vy - vx) & m;
    case 0x0F: return (acc + static_cast<uint64_t>(vx > vy ? vx - vy : vy - vx)) & m;
    case 0x10: return (u ? x - y : x + y) & m;
    case 0x11: return u ? (x == y ? m : 0) : ((x & y) != 0 ? m : 0);
    case 0x12: return (u ? acc - x * y : acc + x * y) & m;
    case 0x13: {
      if (!u) return (x * y) & m;
      uint64_t r = 0;      // PMUL: polynomial multiply, the low byte
      for (uint32_t i = 0; i < 8; ++i)
        if ((y >> i) & 1) r ^= x << i;
      return r & m;
    }
    case 0x16: {
      const int64_t product = sext(x, w) * sext(y, w);
      I128 doubled = static_cast<I128>(product) * 2;
      if (u) doubled += static_cast<I128>(1) << (w - 1);
      return saturate_to_signed(doubled >> w, w, sat);
    }
    case 0x17: return (x + y) & m;
    default: return 0;
  }
}

uint64_t fp_two(Format f) { return f == Format::F64 ? 0x4000000000000000ull : f == Format::F32 ? 0x40000000u : 0x4000u; }
uint64_t fp_three(Format f) { return f == Format::F64 ? 0x4008000000000000ull : f == Format::F32 ? 0x40400000u : 0x4200u; }
uint64_t fp_half(Format f) { return f == Format::F64 ? 0x3FE0000000000000ull : f == Format::F32 ? 0x3F000000u : 0x3800u; }
uint64_t fp_one_and_half(Format f) { return f == Format::F64 ? 0x3FF8000000000000ull : f == Format::F32 ? 0x3FC00000u : 0x3E00u; }
bool fp_is_infinity(Format f, uint64_t a) {
  const uint64_t magnitude = absolute(f, a);
  return f == Format::F64 ? magnitude == 0x7FF0000000000000ull : f == Format::F32 ? magnitude == 0x7F800000u : magnitude == 0x7C00u;
}
bool fp_is_zero(Format f, uint64_t a) { return absolute(f, a) == 0; }

}  // namespace

// The floating-point three-same operations. `op` is the Arm opcode (0x18..0x1F), a = bit 23.
static uint64_t fp_three_same_lane(uint32_t op, bool u, bool a_bit, Format f, uint64_t x, uint64_t y, uint64_t acc, Env& env, bool* valid) {
  *valid = true;
  switch (op) {
    case 0x18: return arm_minmax(f, x, y, !a_bit, true, env);      // FMAXNM / FMINNM and the pairwise forms
    case 0x19: return a_bit ? fused_multiply_add(f, negate(f, x), y, acc, env) : fused_multiply_add(f, x, y, acc, env);      // FMLA, FMLS
    case 0x1A: return u && a_bit ? absolute(f, softfloat::sub(f, x, y, env)) : a_bit ? softfloat::sub(f, x, y, env) : add(f, x, y, env);      // FADD / FSUB / FADDP / FABD
    case 0x1B: {
      if (u) return mul(f, x, y, env);      // FMUL
      if (is_nan(f, x) || is_nan(f, y)) return mul(f, x, y, env);
      if ((fp_is_infinity(f, x) && fp_is_zero(f, y)) || (fp_is_zero(f, x) && fp_is_infinity(f, y))) {
        const uint64_t two = fp_two(f);
        return ((x ^ y) & (f == Format::F64 ? 0x8000000000000000ull : f == Format::F32 ? 0x80000000u : 0x8000u)) ? negate(f, two) : two;
      }
      return mul(f, x, y, env);      // FMULX
    }
    case 0x1C: {
      const Order o = compare(f, x, y, u, env);      // FCMEQ is quiet, FCMGE and FCMGT signal on any NaN
      const bool t = !u ? o == Order::Equal : (a_bit ? o == Order::Greater : (o == Order::Greater || o == Order::Equal));
      return t ? ~uint64_t{0} : 0;
    }
    case 0x1D: {      // FACGE, FACGT
      const Order o = compare(f, absolute(f, x), absolute(f, y), true, env);
      return (a_bit ? o == Order::Greater : (o == Order::Greater || o == Order::Equal)) ? ~uint64_t{0} : 0;
    }
    case 0x1E: return arm_minmax(f, x, y, !a_bit, false, env);      // FMAX / FMIN and the pairwise forms
    case 0x1F: {
      if (u) return softfloat::div(f, x, y, env);      // FDIV
      const bool inf_zero = (fp_is_infinity(f, x) && fp_is_zero(f, y)) || (fp_is_zero(f, x) && fp_is_infinity(f, y));
      if (!a_bit) {      // FRECPS: 2 - x * y
        if (!(is_nan(f, x) || is_nan(f, y)) && inf_zero) return fp_two(f);
        return fused_multiply_add(f, negate(f, x), y, fp_two(f), env);
      }
      if (!(is_nan(f, x) || is_nan(f, y)) && inf_zero) return fp_one_and_half(f);      // FRSQRTS: (3 - x * y) / 2
      return mul(f, fused_multiply_add(f, negate(f, x), y, fp_three(f), env), fp_half(f), env);
    }
    default: *valid = false; return 0;
  }
}

bool AArch64Cpu::neon_three_same(uint32_t insn, Step* result) {
  const bool vec_a = (insn & 0x9F200400u) == 0x0E200400u, sc_a = (insn & 0xDF200400u) == 0x5E200400u;
  const bool vec_h = (insn & 0x9F60C400u) == 0x0E400400u, sc_h = (insn & 0xDF60C400u) == 0x5E400400u;
  const bool vec_x = (insn & 0x9F208400u) == 0x0E008400u, sc_x = (insn & 0xDF208400u) == 0x5E008400u;
  if (!vec_a && !sc_a && !vec_h && !sc_h && !vec_x && !sc_x) return false;
  const bool scalar = sc_a || sc_h || sc_x;
  const bool q = !scalar && ((insn >> 30) & 1), u = (insn >> 29) & 1;
  const uint32_t size = bits_of(insn, 23, 22), rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const Vec a = s_.v[rn], b = s_.v[rm], old = s_.v[rd];
  Env env = fp_env();
  bool sat = false;
  Vec r;
  auto bad = [&]() { *result = undefined(); return true; };
  auto finish = [&](const Vec& v) {
    s_.v[rd] = v;
    if (sat) s_.fpsr |= 1u << 27;
    fp_finish(env);
    *result = Step::Ok;
    return true;
  };

  if (vec_a || sc_a) {
    const uint32_t opc = bits_of(insn, 15, 11);
    if (opc < 0x18) {
      if (opc == 0x03) {      // AND, BIC, ORR, ORN, EOR, BSL, BIT, BIF: bitwise on the whole register
        if (scalar) return bad();
        const uint64_t da = a.lo, ha = a.hi, db = b.lo, hb = b.hi;
        auto bitwise = [&](uint64_t x, uint64_t y, uint64_t o) -> uint64_t {
          if (!u) return size == 0 ? (x & y) : size == 1 ? (x & ~y) : size == 2 ? (x | y) : (x | ~y);
          return size == 0 ? (x ^ y) : size == 1 ? ((x & o) | (y & ~o)) : size == 2 ? (o ^ ((o ^ x) & y)) : (o ^ ((o ^ x) & ~y));
        };
        // BSL: d = (n & d) | (m & ~d); BIT: d = d ^ ((d ^ n) & m); BIF: d = d ^ ((d ^ n) & ~m)
        r.lo = bitwise(da, db, old.lo);
        r.hi = q ? bitwise(ha, hb, old.hi) : 0;
        return finish(r);
      }
      uint32_t w = 8u << size;
      if (scalar) {
        const bool any_size = opc == 0x01 || opc == 0x05 || opc == 0x09 || opc == 0x0B || opc == 0x16;
        const bool d_only = opc == 0x06 || opc == 0x07 || opc == 0x08 || opc == 0x0A || opc == 0x10 || opc == 0x11;
        if (!any_size && !d_only) return bad();
        if (d_only && size != 3) return bad();
        if (opc == 0x16 && (size == 0 || size == 3)) return bad();
      } else {
        if (size == 3 && !q && opc != 0x03) return bad();
        const bool no_d = opc == 0x00 || opc == 0x02 || opc == 0x04 || opc == 0x0C || opc == 0x0D || opc == 0x0E || opc == 0x0F || opc == 0x12 || opc == 0x13 || opc == 0x14 || opc == 0x15 || opc == 0x16;
        if (size == 3 && no_d) return bad();
        if (opc == 0x16 && size == 0) return bad();
        if (opc == 0x13 && u && size != 0) return bad();
        if (opc == 0x17 && u) return bad();
      }
      const uint32_t n = scalar ? 1 : (q ? 128 : 64) / w;
      const bool pairwise = opc == 0x14 || opc == 0x15 || opc == 0x17;
      for (uint32_t i = 0; i < n; ++i) {
        uint64_t x, y;
        if (pairwise) {
          const Vec& src = i < n / 2 ? a : b;
          const uint32_t j = (i % (n / 2)) * 2;
          x = lane(src, w, j);
          y = lane(src, w, j + 1);
        } else {
          x = lane(a, w, i);
          y = lane(b, w, i);
        }
        set_lane(&r, w, i, integer_lane(opc, u, w, x, y, lane(old, w, i), &sat));
      }
      return finish(r);
    }
    // floating point
    const uint32_t fop = opc;
    const bool a_bit = (size >> 1) & 1, sz = size & 1;
    if (opc == 0x1B && a_bit) {      // FAMAX (U = 0), FAMIN (U = 1): FEAT_FAMINMAX
      const Format f = sz ? Format::F64 : Format::F32;
      if (scalar) return bad();
      const uint32_t w = sz ? 64 : 32, n = (q ? 128 : 64) / w;
      if (sz && !q) return bad();
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, w, i), y = lane(b, w, i);
        const bool nan = is_nan(f, x) || is_nan(f, y);
        set_lane(&r, w, i, arm_minmax(f, nan ? x : absolute(f, x), nan ? y : absolute(f, y), !u, false, env));
      }
      return finish(r);
    }
    if (scalar) {
      const bool ok = (fop == 0x1B && !u && !a_bit) || (fop == 0x1C) || (fop == 0x1D && u) || (fop == 0x1F && !u) || (fop == 0x1A && u && a_bit);
      if (!ok) return bad();
    }
    if (fop == 0x1D && !u) {      // FMLAL / FMLSL (half to single, fused)
      if (sz) return bad();
      const uint32_t n = q ? 4 : 2;
      for (uint32_t i = 0; i < n; ++i) {
        uint64_t x = lane(a, 16, i), y = lane(b, 16, i);
        x = convert(Format::F16, Format::F32, x, env);
        y = convert(Format::F16, Format::F32, y, env);
        if (a_bit) x = negate(Format::F32, x);
        set_lane(&r, 32, i, fp_default_nan_fix(Format::F32, fused_multiply_add(Format::F32, x, y, lane(old, 32, i), env)));
      }
      return finish(r);
    }
    if (fop == 0x19 && u) {      // FMLAL2 / FMLSL2
      if (sz) return bad();
      const uint32_t n = q ? 4 : 2, base = q ? 4 : 2;
      for (uint32_t i = 0; i < n; ++i) {
        uint64_t x = lane(a, 16, base + i), y = lane(b, 16, base + i);
        x = convert(Format::F16, Format::F32, x, env);
        y = convert(Format::F16, Format::F32, y, env);
        if (a_bit) x = negate(Format::F32, x);
        set_lane(&r, 32, i, fp_default_nan_fix(Format::F32, fused_multiply_add(Format::F32, x, y, lane(old, 32, i), env)));
      }
      return finish(r);
    }
    if (fop == 0x19 && u) return bad();
    const Format f = sz ? Format::F64 : Format::F32;
    const uint32_t w = sz ? 64 : 32;
    if (!scalar && sz && !q) return bad();
    const bool pairwise = u && (fop == 0x18 || fop == 0x1A && !a_bit || fop == 0x1E);
    if (u && (fop == 0x19 || (fop == 0x1B && a_bit))) return bad();
    const uint32_t n = scalar ? 1 : (q ? 128 : 64) / w;
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t x, y;
      if (pairwise) {
        const Vec& src = i < n / 2 ? a : b;
        const uint32_t j = (i % (n / 2)) * 2;
        x = lane(src, w, j);
        y = lane(src, w, j + 1);
      } else {
        x = lane(a, w, i);
        y = lane(b, w, i);
      }
      bool valid;
      const uint64_t out = fp_three_same_lane(fop, u, a_bit, f, x, y, lane(old, w, i), env, &valid);
      if (!valid) return bad();
      set_lane(&r, w, i, fop >= 0x1C && fop <= 0x1D ? out & ones(w) : fp_default_nan_fix(f, out) & ones(w));
    }
    return finish(r);
  }

  if (vec_h || sc_h) {      // half-precision floating point
    const uint32_t fop = 0x18 | bits_of(insn, 13, 11);
    const bool a_bit = (insn >> 23) & 1;
    if (fop == 0x1B && a_bit) {      // FAMAX (U = 0), FAMIN (U = 1) on halves
      if (scalar) return bad();
      const uint32_t n = (q ? 128 : 64) / 16;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, 16, i), y = lane(b, 16, i);
        const bool nan = is_nan(Format::F16, x) || is_nan(Format::F16, y);
        set_lane(&r, 16, i, arm_minmax(Format::F16, nan ? x : absolute(Format::F16, x), nan ? y : absolute(Format::F16, y), !u, false, env));
      }
      return finish(r);
    }
    if (scalar) {
      const bool ok = (fop == 0x1B && !u && !a_bit) || (fop == 0x1C) || (fop == 0x1D && u) || (fop == 0x1F && !u) || (fop == 0x1A && u && a_bit);
      if (!ok) return bad();
    }
    const bool pairwise = u && (fop == 0x18 || (fop == 0x1A && !a_bit) || fop == 0x1E);
    if (u && fop == 0x19) return bad();
    if (u && fop == 0x1B && a_bit) return bad();
    const uint32_t n = scalar ? 1 : (q ? 128 : 64) / 16;
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t x, y;
      if (pairwise) {
        const Vec& src = i < n / 2 ? a : b;
        const uint32_t j = (i % (n / 2)) * 2;
        x = lane(src, 16, j);
        y = lane(src, 16, j + 1);
      } else {
        x = lane(a, 16, i);
        y = lane(b, 16, i);
      }
      bool valid;
      const uint64_t out = fp_three_same_lane(fop, u, a_bit, Format::F16, x, y, lane(old, 16, i), env, &valid);
      if (!valid) return bad();
      set_lane(&r, 16, i, fop >= 0x1C && fop <= 0x1D ? out & 0xFFFF : fp_default_nan_fix(Format::F16, out) & 0xFFFF);
    }
    return finish(r);
  }

  // the "extra" group: bit 15 is set, the operation is in bits 14:11
  const uint32_t opc4 = bits_of(insn, 14, 11);
  if (scalar) {      // SQRDMLAH, SQRDMLSH on a half or a single
    if (!u || opc4 > 1 || (size != 1 && size != 2)) return bad();
    const uint32_t w = 8u << size;
    const int64_t product = sext(lane(a, w, 0), w) * sext(lane(b, w, 0), w);
    I128 sum = (static_cast<I128>(sext(lane(old, w, 0), w)) << w) + (static_cast<I128>(product) * 2 * (opc4 ? -1 : 1)) + (static_cast<I128>(1) << (w - 1));
    set_lane(&r, w, 0, saturate_to_signed(sum >> w, w, &sat));
    return finish(r);
  }
  if (u && opc4 <= 1) {      // SQRDMLAH, SQRDMLSH
    if (size != 1 && size != 2) return bad();
    const uint32_t w = 8u << size, n = (q ? 128 : 64) / w;
    for (uint32_t i = 0; i < n; ++i) {
      const int64_t product = sext(lane(a, w, i), w) * sext(lane(b, w, i), w);
      const I128 sum = (static_cast<I128>(sext(lane(old, w, i), w)) << w) + (static_cast<I128>(product) * 2 * (opc4 ? -1 : 1)) + (static_cast<I128>(1) << (w - 1));
      set_lane(&r, w, i, saturate_to_signed(sum >> w, w, &sat));
    }
    return finish(r);
  }
  if (opc4 == 2 && size == 2) {      // SDOT, UDOT: four byte products into each 32-bit lane
    const uint32_t n = q ? 4 : 2;
    for (uint32_t i = 0; i < n; ++i) {
      int64_t sum = static_cast<int64_t>(lane(old, 32, i));
      for (uint32_t k = 0; k < 4; ++k) {
        const uint64_t x = lane(a, 8, 4 * i + k), y = lane(b, 8, 4 * i + k);
        sum += u ? static_cast<int64_t>(x * y) : sext(x, 8) * sext(y, 8);
      }
      set_lane(&r, 32, i, static_cast<uint64_t>(sum) & 0xFFFFFFFFu);
    }
    return finish(r);
  }
  if (opc4 == 3 && size == 2 && !u) {      // USDOT: unsigned bytes of the first source, signed bytes of the second
    const uint32_t n = q ? 4 : 2;
    for (uint32_t i = 0; i < n; ++i) {
      int64_t sum = static_cast<int64_t>(lane(old, 32, i));
      for (uint32_t k = 0; k < 4; ++k) sum += static_cast<int64_t>(lane(a, 8, 4 * i + k)) * sext(lane(b, 8, 4 * i + k), 8);
      set_lane(&r, 32, i, static_cast<uint64_t>(sum) & 0xFFFFFFFFu);
    }
    return finish(r);
  }
  if (opc4 == 4 && size == 2 && q) {      // SMMLA, UMMLA: 2x8 by 8x2 byte matrices into a 2x2 matrix of 32-bit sums
    for (uint32_t i = 0; i < 2; ++i)
      for (uint32_t j = 0; j < 2; ++j) {
        int64_t sum = static_cast<int64_t>(lane(old, 32, 2 * i + j));
        for (uint32_t k = 0; k < 8; ++k) {
          const uint64_t x = lane(a, 8, 8 * i + k), y = lane(b, 8, 8 * j + k);
          sum += u ? static_cast<int64_t>(x * y) : sext(x, 8) * sext(y, 8);
        }
        set_lane(&r, 32, 2 * i + j, static_cast<uint64_t>(sum) & 0xFFFFFFFFu);
      }
    return finish(r);
  }
  if (opc4 == 5 && size == 2 && q && !u) {      // USMMLA
    for (uint32_t i = 0; i < 2; ++i)
      for (uint32_t j = 0; j < 2; ++j) {
        int64_t sum = static_cast<int64_t>(lane(old, 32, 2 * i + j));
        for (uint32_t k = 0; k < 8; ++k) sum += static_cast<int64_t>(lane(a, 8, 8 * i + k)) * sext(lane(b, 8, 8 * j + k), 8);
        set_lane(&r, 32, 2 * i + j, static_cast<uint64_t>(sum) & 0xFFFFFFFFu);
      }
    return finish(r);
  }
  if (u && (opc4 & 0xC) == 0x8) {      // FCMLA: opc4 = 10 rot
    const Format f = size == 1 ? Format::F16 : size == 2 ? Format::F32 : Format::F64;
    if (size == 0 || (size == 3 && !q)) return bad();
    const uint32_t w = 8u << size, rot = opc4 & 3, pairs = (q ? 128 : 64) / (2 * w);
    for (uint32_t p = 0; p < pairs; ++p) {
      const uint64_t re_a = lane(a, w, 2 * p), im_a = lane(a, w, 2 * p + 1), re_b = lane(b, w, 2 * p), im_b = lane(b, w, 2 * p + 1);
      uint64_t e1, e2, e3, e4;
      switch (rot) {
        case 0: e1 = re_a; e2 = re_b; e3 = re_a; e4 = im_b; break;
        case 1: e1 = negate(f, im_a); e2 = im_b; e3 = im_a; e4 = re_b; break;
        case 2: e1 = negate(f, re_a); e2 = re_b; e3 = negate(f, re_a); e4 = im_b; break;
        default: e1 = im_a; e2 = im_b; e3 = negate(f, im_a); e4 = re_b; break;
      }
      // the real part accumulates e1 * e2, the imaginary part e3 * e4
      set_lane(&r, w, 2 * p, fp_default_nan_fix(f, fused_multiply_add(f, e2, e1, lane(old, w, 2 * p), env)));
      set_lane(&r, w, 2 * p + 1, fp_default_nan_fix(f, fused_multiply_add(f, e4, e3, lane(old, w, 2 * p + 1), env)));
    }
    return finish(r);
  }
  if (u && (opc4 & 0xD) == 0xC) {      // FCADD: opc4 = 11 rot 0
    const Format f = size == 1 ? Format::F16 : size == 2 ? Format::F32 : Format::F64;
    if (size == 0 || (size == 3 && !q)) return bad();
    const uint32_t w = 8u << size, pairs = (q ? 128 : 64) / (2 * w);
    const bool rot270 = (opc4 >> 1) & 1;
    for (uint32_t p = 0; p < pairs; ++p) {
      const uint64_t re_a = lane(a, w, 2 * p), im_a = lane(a, w, 2 * p + 1), re_b = lane(b, w, 2 * p), im_b = lane(b, w, 2 * p + 1);
      uint64_t re, im;
      if (!rot270) { re = softfloat::sub(f, re_a, im_b, env); im = add(f, im_a, re_b, env); }
      else { re = add(f, re_a, im_b, env); im = softfloat::sub(f, im_a, re_b, env); }
      set_lane(&r, w, 2 * p, fp_default_nan_fix(f, re));
      set_lane(&r, w, 2 * p + 1, fp_default_nan_fix(f, im));
    }
    return finish(r);
  }
  return bad();
}

}  // namespace emulation
