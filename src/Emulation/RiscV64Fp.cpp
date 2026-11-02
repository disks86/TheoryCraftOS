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
#include "Emulation/RiscV64Cpu.hpp"
#include "Emulation/RiscV64FpOps.hpp"

namespace emulation {

using namespace softfloat;
using namespace rvfp;

namespace {

constexpr int64_t sext(uint64_t v, uint32_t bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
constexpr uint32_t field(uint32_t v, uint32_t hi, uint32_t lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

}  // namespace

bool RiscV64Cpu::rounding_mode(uint32_t rm, Rounding* out) const {
  if (rm == 7) rm = (s_.fcsr >> 5) & 7;
  switch (rm) {
    case 0: *out = Rounding::NearestEven; return true;
    case 1: *out = Rounding::TowardZero; return true;
    case 2: *out = Rounding::Down; return true;
    case 3: *out = Rounding::Up; return true;
    case 4: *out = Rounding::TieAway; return true;
    default: return false;
  }
}

void RiscV64Cpu::fp_finish(const Env& env) {
  uint32_t f = 0;
  if (env.flags & kInexact) f |= 1;
  if (env.flags & kUnderflow) f |= 2;
  if (env.flags & kOverflow) f |= 4;
  if (env.flags & kDivideByZero) f |= 8;
  if (env.flags & kInvalid) f |= 16;
  s_.fcsr |= f;
}

uint64_t RiscV64Cpu::read_single(uint32_t r) const { return (s_.f[r] >> 32) == 0xFFFFFFFFu ? (s_.f[r] & 0xFFFFFFFFu) : kCanonicalSingle; }

RiscV64Cpu::Step RiscV64Cpu::floating(GuestMemory& m, uint32_t insn) {
  const uint32_t opcode = insn & 0x7F, rd = field(insn, 11, 7), funct3 = field(insn, 14, 12), rs1 = field(insn, 19, 15), rs2 = field(insn, 24, 20), fmt = field(insn, 26, 25), funct5 = insn >> 27;

  if (opcode == 0x07) {      // flh, flw, fld
    if (funct3 != 1 && funct3 != 2 && funct3 != 3) return undefined();
    uint64_t v;
    if (!load(m, s_.x[rs1] + static_cast<uint64_t>(sext(insn >> 20, 12)), funct3 == 1 ? 2 : funct3 == 2 ? 4 : 8, &v)) return Step::Fault;
    if (funct3 == 1) write_half(rd, v);
    else if (funct3 == 2) write_single(rd, v);
    else s_.f[rd] = v;
    return Step::Ok;
  }
  if (opcode == 0x27) {      // fsh, fsw, fsd
    if (funct3 != 1 && funct3 != 2 && funct3 != 3) return undefined();
    const int64_t off = sext(((insn >> 25) << 5) | rd, 12);
    const uint64_t address = s_.x[rs1] + static_cast<uint64_t>(off);
    if (!store(m, address, funct3 == 1 ? 2 : funct3 == 2 ? 4 : 8, s_.f[rs2])) return Step::Fault;
    return Step::Ok;
  }
  if (fmt == 3) return undefined();
  const Format f = fmt == 0 ? Format::F32 : fmt == 1 ? Format::F64 : Format::F16;
  auto read = [&](uint32_t r) { return f == Format::F32 ? read_single(r) : f == Format::F16 ? read_half(r) : s_.f[r]; };
  auto write = [&](uint64_t bits) {
    if (f == Format::F32) write_single(rd, bits);
    else if (f == Format::F16) write_half(rd, bits);
    else s_.f[rd] = bits;
  };
  Env env;
  env.arm = true;
  env.tininess_after = true;

  if (opcode >= 0x43 && opcode <= 0x4F) {      // fmadd, fmsub, fnmsub, fnmadd
    if (!rounding_mode(funct3, &env.mode)) return undefined();
    const uint32_t rs3 = funct5;
    uint64_t x = read(rs1), y = read(rs2), z = read(rs3);
    if (opcode == 0x47 || opcode == 0x4F) z = negate(f, z);      // fmsub, fnmadd: - c
    if (opcode == 0x4B || opcode == 0x4F) x = negate(f, x);      // fnmsub, fnmadd: - (a * b)
    write(canonical(f, fused_multiply_add(f, x, y, z, env)));
    fp_finish(env);
    return Step::Ok;
  }

  switch (funct5) {
    case 0: case 1: case 2: case 3: {      // fadd, fsub, fmul, fdiv
      if (!rounding_mode(funct3, &env.mode)) return undefined();
      const uint64_t x = read(rs1), y = read(rs2);
      uint64_t r;
      switch (funct5) {
        case 0: r = add(f, x, y, env); break;
        case 1: r = softfloat::sub(f, x, y, env); break;
        case 2: r = mul(f, x, y, env); break;
        default: r = softfloat::div(f, x, y, env); break;
      }
      write(canonical(f, r));
      fp_finish(env);
      return Step::Ok;
    }
    case 0x0B: {      // fsqrt
      if (rs2 != 0 || !rounding_mode(funct3, &env.mode)) return undefined();
      write(canonical(f, sqrt(f, read(rs1), env)));
      fp_finish(env);
      return Step::Ok;
    }
    case 4: {      // fsgnj, fsgnjn, fsgnjx
      if (funct3 > 2) return undefined();
      const uint64_t x = read(rs1), y = read(rs2), sign = sign_bit_of(f);
      const uint64_t s = funct3 == 0 ? (y & sign) : funct3 == 1 ? (~y & sign) : ((x ^ y) & sign);
      write((x & ~sign) | s);
      return Step::Ok;
    }
    case 5: {      // fmin, fmax, and Zfa's fminm, fmaxm (a NaN operand gives the canonical NaN)
      if (funct3 > 3) return undefined();
      const uint64_t x = read(rs1), y = read(rs2);
      uint64_t r = minmax(f, x, y, funct3 & 1, env);
      if (funct3 >= 2 && (is_nan(f, x) || is_nan(f, y))) r = canonical_nan(f);
      write(r);
      fp_finish(env);
      return Step::Ok;
    }
    case 8: {      // fcvt between the formats (rs2 names the source), fround / froundnx (Zfa), and the BFloat16 conversions of Zfbfmin
      if (!rounding_mode(funct3, &env.mode)) return undefined();
      if (rs2 == 4 || rs2 == 5) {      // fround, froundnx: round to an integral value in the same format
        const uint64_t x = read(rs1);
        if (is_nan(f, x)) { if (!(x & (f == Format::F32 ? 0x400000u : f == Format::F64 ? 0x8000000000000ull : 0x200u))) env.flags |= kInvalid; write(canonical_nan(f)); }
        else write(round_to_integral(f, x, env.mode, rs2 == 5, env));
        fp_finish(env);
        return Step::Ok;
      }
      Format from;
      if ((fmt == 2 && rs2 == 8) || (fmt == 0 && rs2 == 6)) {      // fcvt.bf16.s and fcvt.s.bf16
        from = Format::F32;
        if (fmt == 2) {
          const uint64_t r = canonical(Format::BF16, convert(Format::F32, Format::BF16, read_single(rs1), env));
          write_half(rd, r);
        } else {
          write_single(rd, canonical(Format::F32, convert(Format::BF16, Format::F32, (s_.f[rs1] >> 16) == 0xFFFFFFFFFFFFull ? (s_.f[rs1] & 0xFFFFu) : 0x7FC0u, env)));
        }
        fp_finish(env);
        return Step::Ok;
      }
      if (rs2 == 0) from = Format::F32; else if (rs2 == 1) from = Format::F64; else if (rs2 == 2) from = Format::F16; else return undefined();
      if (from == f) return undefined();
      const uint64_t source = from == Format::F32 ? read_single(rs1) : from == Format::F16 ? read_half(rs1) : s_.f[rs1];
      write(canonical(f, convert(from, f, source, env)));
      fp_finish(env);
      return Step::Ok;
    }
    case 0x14: {      // feq, flt, fle
      if (funct3 > 5 || funct3 == 3) return undefined();
      const uint64_t x = read(rs1), y = read(rs2);
      const Order order = compare(f, x, y, funct3 == 0 || funct3 == 1, env);      // (feq, fleq and fltq are quiet comparisons)
      bool result;
      switch (funct3) {
        case 2: result = order == Order::Equal; break;
        case 1: case 5: result = order == Order::Less; break;
        default: result = order == Order::Less || order == Order::Equal; break;
      }
      if (rd != 0) s_.x[rd] = result;
      fp_finish(env);
      return Step::Ok;
    }
    case 0x18: {      // fcvt.w.s, fcvt.wu.s, fcvt.l.s, fcvt.lu.s (and .d, .h), and Zfa's fcvtmod.w.d
      if (rs2 == 8 && fmt == 1 && funct3 == 1) {      // fcvtmod.w.d: the value truncated toward zero, modulo 2^32, with invalid for a NaN, an infinity or a value too large to be exact
        const uint64_t x = s_.f[rs1];
        uint64_t result = 0;
        const uint32_t exponent = static_cast<uint32_t>((x >> 52) & 0x7FF);
        if (exponent == 0x7FF) env.flags |= kInvalid;
        else if (exponent >= 1023) {
          const int32_t shift = static_cast<int32_t>(exponent) - 1075;      // value = mantissa * 2^shift
          const uint64_t mantissa = (x & 0xFFFFFFFFFFFFFull) | (uint64_t{1} << 52);
          uint64_t magnitude;
          bool too_big = false;
          if (shift >= 0) { too_big = shift > 11; magnitude = too_big ? 0 : mantissa << shift; }
          else { magnitude = mantissa >> -shift; if (mantissa & ((uint64_t{1} << -shift) - 1)) env.flags |= kInexact; }
          const bool negative = static_cast<int64_t>(x) < 0;
          if (too_big || magnitude > (negative ? uint64_t{1} << 31 : (uint64_t{1} << 31) - 1)) { env.flags |= kInvalid; env.flags &= ~kInexact; }      // (outside the range of a 32 bit integer, though the result is the value modulo 2^32)
          result = shift >= 0 && shift < 64 && !too_big ? magnitude : (shift < 0 ? magnitude : 0);
          if (shift >= 0 && shift > 11) result = shift >= 64 ? 0 : mantissa << shift;
          if (negative) result = 0 - result;
        } else if ((x & 0x7FFFFFFFFFFFFFFFull) != 0) env.flags |= kInexact;
        if (rd != 0) s_.x[rd] = static_cast<uint64_t>(sext(result, 32));
        fp_finish(env);
        return Step::Ok;
      }
      if (rs2 > 3 || !rounding_mode(funct3, &env.mode)) return undefined();
      const bool is_unsigned = rs2 & 1, is_long = rs2 >= 2;
      const uint32_t bits = is_long ? 64 : 32;
      const uint64_t x = read(rs1);
      uint64_t r;
      if (is_nan(f, x)) {      // a NaN converts to the largest value
        env.flags |= kInvalid;
        r = is_unsigned ? (is_long ? ~uint64_t{0} : 0xFFFFFFFFu) : (is_long ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFu);
      } else {
        r = to_integer_saturating(f, x, env.mode, bits, is_unsigned, 0, env);
      }
      if (!is_long) r = static_cast<uint64_t>(sext(r, 32));
      if (rd != 0) s_.x[rd] = r;
      fp_finish(env);
      return Step::Ok;
    }
    case 0x1A: {      // fcvt.s.w, fcvt.s.wu, fcvt.s.l, fcvt.s.lu (and .d)
      if (rs2 > 3 || !rounding_mode(funct3, &env.mode)) return undefined();
      const bool is_unsigned = rs2 & 1, is_long = rs2 >= 2;
      uint64_t v = s_.x[rs1];
      bool negative = false;
      if (!is_long) v = is_unsigned ? (v & 0xFFFFFFFFu) : static_cast<uint64_t>(sext(v, 32));
      if (!is_unsigned && static_cast<int64_t>(v) < 0) {
        negative = true;
        v = 0 - v;
      }
      write(from_integer(f, v, negative, 0, env));
      fp_finish(env);
      return Step::Ok;
    }
    case 0x1C: {      // fmv.x.w / fmv.x.d, fclass
      if (rs2 != 0) return undefined();
      if (funct3 == 0) {
        if (rd != 0) s_.x[rd] = f == Format::F32 ? static_cast<uint64_t>(sext(s_.f[rs1], 32)) : f == Format::F16 ? static_cast<uint64_t>(sext(s_.f[rs1], 16)) : s_.f[rs1];
      } else if (funct3 == 1) {
        if (rd != 0) s_.x[rd] = classify(f, read(rs1));
      } else {
        return undefined();
      }
      return Step::Ok;
    }
    case 0x1E: {      // fmv.w.x / fmv.d.x / fmv.h.x, and Zfa's fli
      if (funct3 != 0) return undefined();
      if (rs2 == 1) {      // fli: one of 32 constants, named by rs1
        static const uint64_t kFli[30] = {0xbff0000000000000ull, 0x0000000000000000ull, 0x3ef0000000000000ull, 0x3f00000000000000ull, 0x3f70000000000000ull, 0x3f80000000000000ull, 0x3fb0000000000000ull, 0x3fc0000000000000ull, 0x3fd0000000000000ull, 0x3fd4000000000000ull, 0x3fd8000000000000ull, 0x3fdc000000000000ull, 0x3fe0000000000000ull, 0x3fe4000000000000ull, 0x3fe8000000000000ull, 0x3fec000000000000ull, 0x3ff0000000000000ull, 0x3ff4000000000000ull, 0x3ff8000000000000ull, 0x3ffc000000000000ull, 0x4000000000000000ull, 0x4004000000000000ull, 0x4008000000000000ull, 0x4010000000000000ull, 0x4020000000000000ull, 0x4030000000000000ull, 0x4060000000000000ull, 0x4070000000000000ull, 0x40e0000000000000ull, 0x40f0000000000000ull};      // (binary64 patterns; converting them is exact in every format)
        uint64_t bits;
        if (rs1 == 1) bits = f == Format::F32 ? 0x00800000u : f == Format::F64 ? 0x0010000000000000ull : 0x0400u;      // the smallest normal number
        else if (rs1 == 30) bits = f == Format::F32 ? 0x7F800000u : f == Format::F64 ? 0x7FF0000000000000ull : 0x7C00u;
        else if (rs1 == 31) bits = canonical_nan(f);
        else {
          Env exact;
          bits = convert(Format::F64, f, kFli[rs1], exact);
        }
        write(bits);
        return Step::Ok;
      }
      if (rs2 != 0) return undefined();
      if (f == Format::F32) write_single(rd, s_.x[rs1]);
      else if (f == Format::F16) write_half(rd, s_.x[rs1]);
      else s_.f[rd] = s_.x[rs1];
      return Step::Ok;
    }
    default: return undefined();
  }
}

}  // namespace emulation
