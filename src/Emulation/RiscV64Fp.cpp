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

  if (opcode == 0x07) {      // flw, fld
    if (funct3 != 2 && funct3 != 3) return undefined();
    uint64_t v;
    if (!load(m, s_.x[rs1] + static_cast<uint64_t>(sext(insn >> 20, 12)), funct3 == 2 ? 4 : 8, &v)) return Step::Fault;
    if (funct3 == 2) write_single(rd, v);
    else s_.f[rd] = v;
    return Step::Ok;
  }
  if (opcode == 0x27) {      // fsw, fsd
    if (funct3 != 2 && funct3 != 3) return undefined();
    const int64_t off = sext(((insn >> 25) << 5) | rd, 12);
    const uint64_t address = s_.x[rs1] + static_cast<uint64_t>(off);
    if (!store(m, address, funct3 == 2 ? 4 : 8, s_.f[rs2])) return Step::Fault;
    return Step::Ok;
  }
  if (fmt > 1) return undefined();
  const Format f = fmt == 0 ? Format::F32 : Format::F64;
  auto read = [&](uint32_t r) { return f == Format::F32 ? read_single(r) : s_.f[r]; };
  auto write = [&](uint64_t bits) {
    if (f == Format::F32) write_single(rd, bits);
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
    case 5: {      // fmin, fmax
      if (funct3 > 1) return undefined();
      const uint64_t x = read(rs1), y = read(rs2);
      const uint64_t r = minmax(f, x, y, funct3 == 1, env);
      write(r);
      fp_finish(env);
      return Step::Ok;
    }
    case 8: {      // fcvt.s.d, fcvt.d.s
      if (!rounding_mode(funct3, &env.mode)) return undefined();
      if (fmt == 0) {
        if (rs2 != 1) return undefined();
        write(canonical(Format::F32, convert(Format::F64, Format::F32, s_.f[rs1], env)));
      } else {
        if (rs2 != 0) return undefined();
        write(canonical(Format::F64, convert(Format::F32, Format::F64, read_single(rs1), env)));
      }
      fp_finish(env);
      return Step::Ok;
    }
    case 0x14: {      // feq, flt, fle
      if (funct3 > 2) return undefined();
      const uint64_t x = read(rs1), y = read(rs2);
      const Order order = compare(f, x, y, funct3 != 2, env);
      bool result;
      switch (funct3) {
        case 2: result = order == Order::Equal; break;
        case 1: result = order == Order::Less; break;
        default: result = order == Order::Less || order == Order::Equal; break;
      }
      if (rd != 0) s_.x[rd] = result;
      fp_finish(env);
      return Step::Ok;
    }
    case 0x18: {      // fcvt.w.s, fcvt.wu.s, fcvt.l.s, fcvt.lu.s (and .d)
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
        if (rd != 0) s_.x[rd] = f == Format::F32 ? static_cast<uint64_t>(sext(s_.f[rs1], 32)) : s_.f[rs1];
      } else if (funct3 == 1) {
        if (rd != 0) s_.x[rd] = classify(f, read(rs1));
      } else {
        return undefined();
      }
      return Step::Ok;
    }
    case 0x1E: {      // fmv.w.x / fmv.d.x
      if (rs2 != 0 || funct3 != 0) return undefined();
      if (f == Format::F32) write_single(rd, s_.x[rs1]);
      else s_.f[rd] = s_.x[rs1];
      return Step::Ok;
    }
    default: return undefined();
  }
}

}  // namespace emulation
