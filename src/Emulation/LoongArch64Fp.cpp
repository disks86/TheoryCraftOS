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
#include "Emulation/LoongArch64Cpu.hpp"

namespace emulation {

using namespace softfloat;

namespace {

constexpr int64_t sext(uint64_t v, uint32_t bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
constexpr uint32_t field(uint32_t v, uint32_t hi, uint32_t lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

// The layout of a format: its fraction and exponent widths.
struct Layout {
  uint32_t fraction, exponent;
  uint64_t sign() const { return uint64_t{1} << (fraction + exponent); }
  uint64_t fraction_mask() const { return (uint64_t{1} << fraction) - 1; }
  uint64_t exponent_max() const { return (uint64_t{1} << exponent) - 1; }
  int64_t bias() const { return static_cast<int64_t>(exponent_max() >> 1); }
  uint64_t quiet() const { return uint64_t{1} << (fraction - 1); }
};
constexpr Layout layout(Format f) { return f == Format::F32 ? Layout{23, 8} : Layout{52, 11}; }

uint64_t default_nan(Format f) {
  const Layout l = layout(f);
  return (l.exponent_max() << l.fraction) | l.quiet();
}

// FCLASS: one bit for each of the ten classes.
uint64_t classify(Format f, uint64_t bits) {
  const Layout l = layout(f);
  const bool negative = (bits & l.sign()) != 0;
  const uint64_t exponent = (bits >> l.fraction) & l.exponent_max(), fraction = bits & l.fraction_mask();
  if (exponent == l.exponent_max()) {
    if (fraction != 0) return (fraction & l.quiet()) ? 1u << 1 : 1u << 0;      // quiet, signaling
    return negative ? 1u << 2 : 1u << 6;
  }
  if (exponent == 0) {
    if (fraction == 0) return negative ? 1u << 5 : 1u << 9;
    return negative ? 1u << 4 : 1u << 8;
  }
  return negative ? 1u << 3 : 1u << 7;
}

// A power of two as a value of the format (down to the smallest subnormal).
uint64_t power_of_two(Format f, int64_t power) {
  const Layout l = layout(f);
  if (power >= 1 - l.bias()) return static_cast<uint64_t>(power + l.bias()) << l.fraction;
  return uint64_t{1} << (power + l.bias() - 1 + l.fraction);
}

// FSCALEB: x * 2^n, rounded once.
uint64_t scale(Format f, uint64_t x, int64_t n, Env& env) {
  const Layout l = layout(f);
  const uint64_t one = static_cast<uint64_t>(l.bias()) << l.fraction;
  uint64_t exponent = (x >> l.fraction) & l.exponent_max(), fraction = x & l.fraction_mask();
  if (exponent == l.exponent_max() || (exponent == 0 && fraction == 0)) return mul(f, x, one, env);      // (a NaN is made quiet; infinities and zeros stay)
  int64_t e = static_cast<int64_t>(exponent);
  if (exponent == 0) {      // a subnormal: brought into the form of a normal number first
    e = 1;
    while (!(fraction & (uint64_t{1} << l.fraction))) {
      fraction <<= 1;
      --e;
    }
    fraction &= l.fraction_mask();
  }
  const int64_t limit = 2 * static_cast<int64_t>(l.exponent_max()) + l.fraction + 8;      // (beyond this every result is an overflow or an underflow anyway)
  if (n > limit) n = limit;
  if (n < -limit) n = -limit;
  e += n;
  const uint64_t sign = x & l.sign();
  const int64_t top = static_cast<int64_t>(l.exponent_max()) - 1;
  if (e >= 1 && e <= top) return sign | (static_cast<uint64_t>(e) << l.fraction) | fraction;
  if (e > top) return mul(f, sign | (static_cast<uint64_t>(top) << l.fraction) | fraction, power_of_two(f, 1), env);      // overflows, with the rounding mode's result
  // Too small for a normal number: the smallest normal with this fraction, times a power of two, is the one multiplication that rounds.
  int64_t power = e - 1;
  const int64_t smallest = 1 - l.bias() - static_cast<int64_t>(l.fraction);
  if (power < smallest - 2) power = smallest - 2;
  if (power >= smallest) return mul(f, sign | (uint64_t{1} << l.fraction) | fraction, power_of_two(f, power), env);
  // (below the smallest subnormal's half: two steps, the first of which is exact)
  const uint64_t tiny = mul(f, sign | (uint64_t{1} << l.fraction) | 0, power_of_two(f, smallest), env);      // the smallest subnormal, with the sign
  return mul(f, tiny, power_of_two(f, -1), env);
}

// FLOGB: the exponent of x as a value of the format.
uint64_t exponent_of(Format f, uint64_t x, Env& env) {
  const Layout l = layout(f);
  const uint64_t one = static_cast<uint64_t>(l.bias()) << l.fraction;
  const uint64_t exponent = (x >> l.fraction) & l.exponent_max();
  uint64_t fraction = x & l.fraction_mask();
  if (exponent == l.exponent_max() && fraction != 0) return mul(f, x, one, env);
  if (exponent == 0 && fraction == 0) {
    env.flags |= kDivideByZero;
    return l.sign() | (l.exponent_max() << l.fraction);
  }
  if (x & l.sign()) {
    env.flags |= kInvalid;
    return default_nan(f);
  }
  if (exponent == l.exponent_max()) return x;
  int64_t e = static_cast<int64_t>(exponent) - l.bias();
  if (exponent == 0) {
    e = 1 - l.bias();
    while (!(fraction & (uint64_t{1} << l.fraction))) {
      fraction <<= 1;
      --e;
    }
  }
  Env exact;
  return from_int(f, e, exact);
}

uint64_t magnitude(Format f, uint64_t x) { return x & ~layout(f).sign(); }

}  // namespace

Env LoongArch64Cpu::fp_env() const {
  Env env;
  env.arm = true;      // (a positive default NaN; a signaling NaN operand is chosen before a quiet one)
  env.tininess_after = true;
  switch ((s_.fcsr >> 8) & 3) {
    case 0: env.mode = Rounding::NearestEven; break;
    case 1: env.mode = Rounding::TowardZero; break;
    case 2: env.mode = Rounding::Up; break;
    default: env.mode = Rounding::Down; break;
  }
  return env;
}

bool LoongArch64Cpu::fp_finish(const Env& env, uint32_t ignore) {
  uint32_t cause = 0;
  if (env.flags & kInexact) cause |= 1;
  if (env.flags & kUnderflow) cause |= 2;
  if (env.flags & kOverflow) cause |= 4;
  if (env.flags & kDivideByZero) cause |= 8;
  if (env.flags & kInvalid) cause |= 16;
  cause &= ~ignore;
  s_.fcsr = (s_.fcsr & ~0x1F000000u) | (cause << 24);
  if (cause & s_.fcsr & 0x1F) {      // an enabled exception: the instruction traps (SIGFPE) and has no result
    fail(16);
    return false;
  }
  s_.fcsr |= cause << 16;
  return true;
}

LoongArch64Cpu::Step LoongArch64Cpu::floating(uint32_t insn) {
  const uint32_t rd = field(insn, 4, 0), rj = field(insn, 9, 5), rk = field(insn, 14, 10);
  const uint32_t op17 = insn >> 15, op22 = insn >> 10;
  Env env = fp_env();

  if (op17 >= 0x200 && op17 < 0x228) {      // two operands: fadd .. fcopysign, each for single (1) and double (2)
    const uint32_t kind = (op17 - 0x200) >> 2, size = op17 & 3;
    if (size != 1 && size != 2) return undefined();
    const Format f = size == 1 ? Format::F32 : Format::F64;
    const uint64_t mask = size == 1 ? 0xFFFFFFFFu : ~uint64_t{0};
    const uint64_t x = s_.f[rj] & mask, y = s_.f[rk] & mask;
    uint64_t r;
    switch (kind) {
      case 0: r = add(f, x, y, env); break;
      case 1: r = softfloat::sub(f, x, y, env); break;
      case 2: r = mul(f, x, y, env); break;
      case 3: r = softfloat::div(f, x, y, env); break;
      case 4: r = arm_minmax(f, x, y, true, true, env); break;       // fmax: a number beats a quiet NaN
      case 5: r = arm_minmax(f, x, y, false, true, env); break;      // fmin
      case 6: case 7: {      // fmaxa, fmina: by magnitude; the plain maximum or minimum where the magnitudes are the same
        const bool is_max = kind == 6;
        if (is_nan(f, x) || is_nan(f, y) || magnitude(f, x) == magnitude(f, y)) r = arm_minmax(f, x, y, is_max, true, env);
        else r = (magnitude(f, x) > magnitude(f, y)) == is_max ? x : y;
        break;
      }
      case 8: {      // fscaleb: fj * 2^fk, fk an integer
        const int64_t n = size == 1 ? sext(y, 32) : static_cast<int64_t>(y);
        r = scale(f, x, n, env);
        break;
      }
      default: {     // fcopysign: no exceptions, a NaN is not touched (and of a single, the upper half is fk's)
        const uint64_t sign = layout(f).sign();
        s_.f[rd] = (x & ~sign) | (s_.f[rk] & ~(sign - 1));
        return Step::Ok;
      }
    }
    if (!fp_finish(env)) return Step::Fault;
    if (size == 1) write_single(rd, r);
    else s_.f[rd] = r;
    return Step::Ok;
  }

  if (op22 >= 0x4501 && op22 <= 0x451A) {      // one operand: fabs, fneg, flogb, fclass, fsqrt, frecip, frsqrt
    const uint32_t kind = (op22 - 0x4500) >> 2, size = op22 & 3;
    if (size != 1 && size != 2) return undefined();
    const Format f = size == 1 ? Format::F32 : Format::F64;
    const uint64_t x = size == 1 ? s_.f[rj] & 0xFFFFFFFFu : s_.f[rj];
    const uint64_t one = static_cast<uint64_t>(layout(f).bias()) << layout(f).fraction;
    uint64_t r;
    switch (kind) {
      case 0: r = absolute(f, x); break;
      case 1: r = negate(f, x); break;
      case 2: r = exponent_of(f, x, env); break;
      case 3: s_.f[rd] = classify(f, x); return Step::Ok;
      case 4: r = softfloat::sqrt(f, x, env); break;
      case 5: r = softfloat::div(f, one, x, env); break;
      default: r = softfloat::div(f, one, softfloat::sqrt(f, x, env), env); break;      // (two roundings, as the processors this follows do it)
    }
    if (kind >= 2 && !fp_finish(env, kind == 2 ? 1 : 0)) return Step::Fault;      // (flogb's result is exact)
    if (size == 1) write_single(rd, r);
    else s_.f[rd] = r;
    return Step::Ok;
  }

  switch (op22) {
    case 0x4525: write_single(rd, s_.f[rj]); return Step::Ok;                                          // fmov.s
    case 0x4526: s_.f[rd] = s_.f[rj]; return Step::Ok;                                                 // fmov.d
    case 0x4529: s_.f[rd] = (s_.f[rd] & 0xFFFFFFFF00000000ull) | (s_.r[rj] & 0xFFFFFFFFu); return Step::Ok;      // movgr2fr.w
    case 0x452A: s_.f[rd] = s_.r[rj]; return Step::Ok;                                                 // movgr2fr.d
    case 0x452B: s_.f[rd] = (s_.f[rd] & 0xFFFFFFFFu) | (s_.r[rj] << 32); return Step::Ok;              // movgr2frh.w
    case 0x452D: set_r(rd, static_cast<uint64_t>(sext(s_.f[rj], 32))); return Step::Ok;                // movfr2gr.s
    case 0x452E: set_r(rd, s_.f[rj]); return Step::Ok;                                                 // movfr2gr.d
    case 0x452F: set_r(rd, static_cast<uint64_t>(sext(s_.f[rj] >> 32, 32))); return Step::Ok;          // movfrh2gr.s
    case 0x4530: case 0x4532: {      // movgr2fcsr, movfcsr2gr: fcsr0, or one of its three parts (1 the enables, 2 the cause and flags, 3 the rounding mode)
      const uint32_t which = (op22 == 0x4530 ? rd : rj);
      if (which > 3) return undefined();
      const uint32_t part = which == 0 ? kFcsrBits : which == 1 ? 0x1Fu : which == 2 ? 0x1F1F0000u : 0x300u;
      if (op22 == 0x4530) s_.fcsr = (s_.fcsr & ~part) | (static_cast<uint32_t>(s_.r[rj]) & part);
      else set_r(rd, s_.fcsr & part);
      return Step::Ok;
    }
    case 0x4534: if (rd > 7) return undefined(); s_.fcc[rd] = s_.f[rj] & 1; return Step::Ok;           // movfr2cf
    case 0x4535: if (rj > 7) return undefined(); s_.f[rd] = s_.fcc[rj]; return Step::Ok;               // movcf2fr
    case 0x4536: if (rd > 7) return undefined(); s_.fcc[rd] = s_.r[rj] & 1; return Step::Ok;           // movgr2cf
    case 0x4537: if (rj > 7) return undefined(); set_r(rd, s_.fcc[rj]); return Step::Ok;               // movcf2gr
    case 0x4646: {      // fcvt.s.d
      const uint64_t r = convert(Format::F64, Format::F32, s_.f[rj], env);
      if (!fp_finish(env)) return Step::Fault;
      write_single(rd, r);
      return Step::Ok;
    }
    case 0x4649: {      // fcvt.d.s
      const uint64_t r = convert(Format::F32, Format::F64, s_.f[rj] & 0xFFFFFFFFu, env);
      if (!fp_finish(env)) return Step::Fault;
      s_.f[rd] = r;
      return Step::Ok;
    }
    case 0x4744: case 0x4746: case 0x4748: case 0x474A: {      // ffint.s.w, ffint.s.l, ffint.d.w, ffint.d.l
      const bool to_double = op22 >= 0x4748, from_long = (op22 & 2) != 0;
      const int64_t value = from_long ? static_cast<int64_t>(s_.f[rj]) : sext(s_.f[rj], 32);
      const uint64_t r = from_int(to_double ? Format::F64 : Format::F32, value, env);
      if (!fp_finish(env)) return Step::Fault;
      if (to_double) s_.f[rd] = r;
      else write_single(rd, r);
      return Step::Ok;
    }
    case 0x4791: case 0x4792: {      // frint: to an integral value, in the rounding mode
      const Format f = op22 == 0x4791 ? Format::F32 : Format::F64;
      const uint64_t r = round_to_integral(f, f == Format::F32 ? s_.f[rj] & 0xFFFFFFFFu : s_.f[rj], env.mode, true, env);
      if (!fp_finish(env)) return Step::Fault;
      s_.f[rd] = r;      // (a single with a zero upper half)
      return Step::Ok;
    }
    default: break;
  }

  if (op22 >= 0x4681 && op22 <= 0x46CA) {      // ftintrm, ftintrp, ftintrz, ftintrne, ftint: to a word or a long, from a single or a double
    const uint32_t group = (op22 - 0x4680) >> 4, low = op22 & 0xF;
    if (low != 1 && low != 2 && low != 9 && low != 0xA) return undefined();
    const bool to_long = low >= 8;
    const Format f = (low & 3) == 1 ? Format::F32 : Format::F64;
    const Rounding mode = group == 0 ? Rounding::Down : group == 1 ? Rounding::Up : group == 2 ? Rounding::TowardZero : group == 3 ? Rounding::NearestEven : env.mode;
    const uint64_t x = f == Format::F32 ? s_.f[rj] & 0xFFFFFFFFu : s_.f[rj];
    // (out of range saturates, a NaN gives zero; both are invalid operations)
    const uint64_t r = to_integer_saturating(f, x, mode, to_long ? 64 : 32, false, 0, env);
    if (!fp_finish(env)) return Step::Fault;
    // (the upper half of a word result is not defined; it is the sign's copies here, which is what most of the conversions give under qemu too)
    s_.f[rd] = to_long ? r : static_cast<uint64_t>(sext(r, 32));
    return Step::Ok;
  }
  return undefined();
}

LoongArch64Cpu::Step LoongArch64Cpu::floating_fused(uint32_t insn) {
  const uint32_t rd = field(insn, 4, 0), rj = field(insn, 9, 5), rk = field(insn, 14, 10), ra = field(insn, 19, 15);
  const uint32_t op12 = insn >> 20;
  Env env = fp_env();

  if (op12 >= 0x081 && op12 <= 0x08E) {      // fmadd, fmsub, fnmadd, fnmsub: fj * fk + fa, with fa or the whole negated
    const uint32_t kind = (op12 - 0x080) >> 2, size = op12 & 3;
    if (size != 1 && size != 2) return undefined();
    const Format f = size == 1 ? Format::F32 : Format::F64;
    const uint64_t mask = size == 1 ? 0xFFFFFFFFu : ~uint64_t{0};
    const uint64_t x = s_.f[rj] & mask, y = s_.f[rk] & mask;
    uint64_t z = s_.f[ra] & mask;
    if ((kind & 1) && !is_nan(f, z)) z = negate(f, z);      // (a NaN keeps its sign on the way through)
    uint64_t r = fused_multiply_add(f, x, y, z, env);
    // Infinity times zero with a quiet NaN to add: an invalid operation whose result is that NaN, not the default one.
    const uint64_t huge = layout(f).exponent_max() << layout(f).fraction;
    const bool x_infinite = magnitude(f, x) == huge, y_infinite = magnitude(f, y) == huge;
    if (is_nan(f, z) && (z & layout(f).quiet()) && ((x_infinite && magnitude(f, y) == 0) || (y_infinite && magnitude(f, x) == 0))) r = z;
    if ((kind & 2) && !is_nan(f, r)) r = negate(f, r);
    if (!fp_finish(env)) return Step::Fault;
    if (size == 1) write_single(rd, r);
    else s_.f[rd] = r;
    return Step::Ok;
  }
  if (op12 == 0x0C1 || op12 == 0x0C2) {      // fcmp: the condition says which of less, equal, unordered and "less or greater" make it true, and whether a quiet NaN is an invalid operation
    const uint32_t condition = ra;
    if (rd > 7) return undefined();
    const uint32_t what = condition >> 1;
    if (what > 8 && what != 10 && what != 12) return undefined();
    const Format f = op12 == 0x0C1 ? Format::F32 : Format::F64;
    const uint64_t mask = f == Format::F32 ? 0xFFFFFFFFu : ~uint64_t{0};
    const Order order = compare(f, s_.f[rj] & mask, s_.f[rk] & mask, (condition & 1) != 0, env);
    const bool result = ((what & 1) && order == Order::Less) || ((what & 2) && order == Order::Equal) || ((what & 4) && order == Order::Unordered) ||
                        ((what & 8) && (order == Order::Less || order == Order::Greater));
    if (!fp_finish(env)) return Step::Fault;
    s_.fcc[rd] = result;
    return Step::Ok;
  }
  if ((insn >> 18) == 0x340) {      // fsel: fk where the flag is set, else fj
    s_.f[rd] = s_.fcc[field(insn, 17, 15)] ? s_.f[rk] : s_.f[rj];
    return Step::Ok;
  }
  return undefined();
}

}  // namespace emulation
