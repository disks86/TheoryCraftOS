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
#include "Emulation/AArch64Cpu.hpp"

namespace emulation {

using namespace softfloat;

namespace {
constexpr uint32_t bits_of(uint32_t insn, uint32_t hi, uint32_t lo) { return (insn >> lo) & ((1u << (hi - lo + 1)) - 1); }
}  // namespace

Env AArch64Cpu::fp_env() const {
  Env env;
  env.arm = true;
  switch ((s_.fpcr >> 22) & 3) {
    case 0: env.mode = Rounding::NearestEven; break;
    case 1: env.mode = Rounding::Up; break;
    case 2: env.mode = Rounding::Down; break;
    default: env.mode = Rounding::TowardZero; break;
  }
  return env;
}

void AArch64Cpu::fp_finish(const Env& env) {
  uint32_t f = 0;
  if (env.flags & kInvalid) f |= 1;
  if (env.flags & kDivideByZero) f |= 2;
  if (env.flags & kOverflow) f |= 4;
  if (env.flags & kUnderflow) f |= 8;
  if (env.flags & kInexact) f |= 16;
  s_.fpsr |= f;
}

void AArch64Cpu::fp_write(uint32_t r, Format f, uint64_t bits) {
  s_.v[r].lo = f == Format::F32 ? bits & 0xFFFFFFFFu : bits;
  s_.v[r].hi = 0;
}

// FPCR.DN: every NaN result is the default NaN.
uint64_t AArch64Cpu::fp_default_nan_fix(Format f, uint64_t bits) const {
  if (!(s_.fpcr & (1u << 25)) || !is_nan(f, bits)) return bits;
  return f == Format::F32 ? 0x7FC00000u : 0x7FF8000000000000ull;
}

AArch64Cpu::Step AArch64Cpu::floating_point(uint32_t insn) {
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const uint32_t ftype = bits_of(insn, 23, 22);
  Env env = fp_env();
  auto format_of = [](uint32_t t, Format* f) {
    if (t == 0) { *f = Format::F32; return true; }
    if (t == 1) { *f = Format::F64; return true; }
    return false;
  };
  Format f = Format::F32;

  if ((insn >> 24) & 1) {      // 3 sources: FMADD FMSUB FNMADD FNMSUB
    if ((insn >> 31) || (insn >> 29 & 1) || !format_of(ftype, &f)) return undefined();
    const uint32_t ra = bits_of(insn, 14, 10);
    const bool o1 = (insn >> 21) & 1, o0 = (insn >> 15) & 1;
    uint64_t a = fp_read(rn, f), b = fp_read(rm, f), c = fp_read(ra, f);
    if (o1) c = negate(f, c);      // FNMADD, FNMSUB: -Ra
    if (o1 != o0) a = negate(f, a);      // FMSUB, FNMADD: -Rn * Rm
    // FMSUB: Ra - n*m = fma(-n, m, a); FNMADD: -Ra - n*m = fma(-n, m, -a); FNMSUB: n*m - Ra = fma(n, m, -a)
    const uint64_t r = fp_default_nan_fix(f, fused_multiply_add(f, a, b, c, env));
    fp_write(rd, f, r);
    fp_finish(env);
    return Step::Ok;
  }

  if (!((insn >> 21) & 1)) {      // fixed-point conversions
    const uint32_t opcode = bits_of(insn, 18, 16), rmode = bits_of(insn, 20, 19);
    const bool sf = insn >> 31;
    const uint32_t fbits = 64 - bits_of(insn, 15, 10);
    if (!format_of(ftype, &f) || (!sf && bits_of(insn, 15, 10) < 32)) return undefined();
    if (rmode == 0 && (opcode == 2 || opcode == 3)) {      // SCVTF, UCVTF
      uint64_t v = rn == 31 ? 0 : s_.x[rn];
      bool negative = false;
      if (!sf) v &= 0xFFFFFFFFu;
      if (opcode == 2) {
        const int64_t sv = sf ? static_cast<int64_t>(v) : static_cast<int32_t>(v);
        negative = sv < 0;
        v = negative ? ~static_cast<uint64_t>(sv) + 1 : static_cast<uint64_t>(sv);
      }
      fp_write(rd, f, from_integer(f, v, negative, fbits, env));
      fp_finish(env);
      return Step::Ok;
    }
    if (rmode == 3 && (opcode == 0 || opcode == 1)) {      // FCVTZS, FCVTZU
      env.mode = Rounding::TowardZero;
      const uint64_t r = to_integer_saturating(f, fp_read(rn, f), Rounding::TowardZero, sf ? 64 : 32, opcode == 1, fbits, env);
      if (rd != 31) s_.x[rd] = sf ? r : r & 0xFFFFFFFFu;
      fp_finish(env);
      return Step::Ok;
    }
    return undefined();
  }

  const uint32_t low = bits_of(insn, 15, 10);
  if (low == 0) {      // conversions between floating point and integer
    const uint32_t opcode = bits_of(insn, 18, 16), rmode = bits_of(insn, 20, 19);
    const bool sf = insn >> 31;
    if (opcode == 6 || opcode == 7) {      // FMOV
      if (rmode == 1 && ftype == 2 && sf) {      // the top half of a 128-bit register
        if (opcode == 6) set_reg(rd, s_.v[rn].hi, true);
        else s_.v[rd].hi = reg(rn, true);
        return Step::Ok;
      }
      if (rmode != 0 || !((ftype == 0 && !sf) || (ftype == 1 && sf))) return undefined();
      if (opcode == 6) {
        set_reg(rd, ftype == 0 ? s_.v[rn].lo & 0xFFFFFFFFu : s_.v[rn].lo, sf);
      } else {
        s_.v[rd].lo = reg(rn, sf);
        s_.v[rd].hi = 0;
      }
      return Step::Ok;
    }
    if (!format_of(ftype, &f)) return undefined();
    if (rmode == 0 && (opcode == 2 || opcode == 3)) {      // SCVTF, UCVTF
      uint64_t v = reg(rn, sf);
      bool negative = false;
      if (opcode == 2) {
        const int64_t sv = sf ? static_cast<int64_t>(v) : static_cast<int32_t>(v);
        negative = sv < 0;
        v = negative ? ~static_cast<uint64_t>(sv) + 1 : static_cast<uint64_t>(sv);
      }
      fp_write(rd, f, from_integer(f, v, negative, 0, env));
      fp_finish(env);
      return Step::Ok;
    }
    Rounding mode;
    bool is_unsigned = opcode & 1;
    if (opcode <= 1) {
      static const Rounding by_rmode[4] = {Rounding::NearestEven, Rounding::Up, Rounding::Down, Rounding::TowardZero};
      mode = by_rmode[rmode];
    } else if ((opcode == 4 || opcode == 5) && rmode == 0) {
      mode = Rounding::TieAway;
    } else {
      return undefined();
    }
    const uint64_t r = to_integer_saturating(f, fp_read(rn, f), mode, sf ? 64 : 32, is_unsigned, 0, env);
    set_reg(rd, r, sf);
    fp_finish(env);
    return Step::Ok;
  }

  if ((insn >> 31) || ((insn >> 29) & 1) || !format_of(ftype, &f)) {
    if (ftype == 3) return undefined();      // half precision
    return undefined();
  }
  if (bits_of(insn, 14, 10) == 0x10) {      // 1 source
    const uint32_t opcode = bits_of(insn, 20, 15);
    const uint64_t a = fp_read(rn, f);
    uint64_t r;
    switch (opcode) {
      case 0: fp_write(rd, f, a); return Step::Ok;      // FMOV
      case 1: fp_write(rd, f, absolute(f, a)); return Step::Ok;
      case 2: fp_write(rd, f, negate(f, a)); return Step::Ok;
      case 3: r = fp_default_nan_fix(f, sqrt(f, a, env)); break;
      case 4: case 5: {      // FCVT between single and double
        const Format to = opcode == 4 ? Format::F32 : Format::F64;
        if (to == f) return undefined();
        r = fp_default_nan_fix(to, convert(f, to, a, env));
        fp_write(rd, to, r);
        fp_finish(env);
        return Step::Ok;
      }
      case 8: r = round_to_integral(f, a, Rounding::NearestEven, false, env); break;
      case 9: r = round_to_integral(f, a, Rounding::Up, false, env); break;
      case 10: r = round_to_integral(f, a, Rounding::Down, false, env); break;
      case 11: r = round_to_integral(f, a, Rounding::TowardZero, false, env); break;
      case 12: r = round_to_integral(f, a, Rounding::TieAway, false, env); break;
      case 14: r = round_to_integral(f, a, env.mode, true, env); break;
      case 15: r = round_to_integral(f, a, env.mode, false, env); break;
      default: return undefined();
    }
    fp_write(rd, f, r);
    fp_finish(env);
    return Step::Ok;
  }
  if (bits_of(insn, 13, 10) == 8) {      // FCMP, FCMPE
    if (bits_of(insn, 15, 14) != 0 || (insn & 7) != 0) return undefined();
    const bool zero = (insn >> 3) & 1, signaling = (insn >> 4) & 1;
    const uint64_t a = fp_read(rn, f), b = zero ? 0 : fp_read(rm, f);
    const Order o = compare(f, a, b, signaling, env);
    s_.nzcv = o == Order::Equal ? (kZ | kC) : o == Order::Less ? kN : o == Order::Greater ? kC : (kC | kV);
    fp_finish(env);
    return Step::Ok;
  }
  if (bits_of(insn, 12, 10) == 4) {      // FMOV immediate
    if (bits_of(insn, 9, 5) != 0) return undefined();
    const uint32_t imm = bits_of(insn, 20, 13);
    const uint64_t sign = imm >> 7, b6 = (imm >> 6) & 1, hi2 = (imm >> 4) & 3, lo4 = imm & 15;
    uint64_t bits;
    if (f == Format::F32) bits = (sign << 31) | ((b6 ? 0 : 1) << 30) | ((b6 ? 0x1Fu : 0) << 25) | (hi2 << 23) | (lo4 << 19);
    else bits = (sign << 63) | ((b6 ? 0 : uint64_t{1}) << 62) | ((b6 ? 0xFFull : 0) << 54) | (hi2 << 52) | (lo4 << 48);
    fp_write(rd, f, bits);
    return Step::Ok;
  }
  switch (bits_of(insn, 11, 10)) {
    case 1: {      // FCCMP, FCCMPE
      const uint32_t cond = bits_of(insn, 15, 12);
      if (condition(cond)) {
        const Order o = compare(f, fp_read(rn, f), fp_read(rm, f), (insn >> 4) & 1, env);
        s_.nzcv = o == Order::Equal ? (kZ | kC) : o == Order::Less ? kN : o == Order::Greater ? kC : (kC | kV);
        fp_finish(env);
      } else {
        s_.nzcv = (insn & 15) << 28;
      }
      return Step::Ok;
    }
    case 3: {      // FCSEL
      fp_write(rd, f, condition(bits_of(insn, 15, 12)) ? fp_read(rn, f) : fp_read(rm, f));
      return Step::Ok;
    }
    case 2: {      // 2 sources
      const uint64_t a = fp_read(rn, f), b = fp_read(rm, f);
      uint64_t r;
      switch (bits_of(insn, 15, 12)) {
        case 0: r = mul(f, a, b, env); break;
        case 1: r = softfloat::div(f, a, b, env); break;
        case 2: r = add(f, a, b, env); break;
        case 3: r = softfloat::sub(f, a, b, env); break;
        case 4: r = arm_minmax(f, a, b, true, false, env); break;
        case 5: r = arm_minmax(f, a, b, false, false, env); break;
        case 6: r = arm_minmax(f, a, b, true, true, env); break;
        case 7: r = arm_minmax(f, a, b, false, true, env); break;
        case 8: r = negate(f, mul(f, a, b, env)); break;
        default: return undefined();
      }
      fp_write(rd, f, fp_default_nan_fix(f, r));
      fp_finish(env);
      return Step::Ok;
    }
    default:
      return undefined();
  }
}

}  // namespace emulation
