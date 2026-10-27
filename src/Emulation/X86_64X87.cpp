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
#include "Emulation/X86_64Cpu.hpp"

// The x87 floating-point unit: opcodes D8..DF. The arithmetic is in SoftFloatExt.cpp; this file is the register stack, the control and status words, the memory formats and the
// instruction decoding. All exceptions are masked (a fault never happens; the status word records what occurred).
namespace emulation {

using namespace softfloat;
using namespace softfloat::ext;

namespace {
constexpr uint16_t kC0 = 0x0100, kC1 = 0x0200, kC2 = 0x0400, kC3 = 0x4000, kStackFault = 0x0040;

Ext make_constant(uint64_t mant, int32_t exp) {
  Ext e;
  e.mant = mant;
  e.se = static_cast<uint16_t>(exp + 16383);
  return e;
}
}  // namespace

void X86_64Cpu::x87_set_cc(bool c0, bool c1, bool c2, bool c3) {
  x87_c1_set_ = true;      // (the instruction decided C1; the "rounded up" rule does not override it)
  s_.fpu_status = static_cast<uint16_t>((s_.fpu_status & ~(kC0 | kC1 | kC2 | kC3)) | (c0 ? kC0 : 0) | (c1 ? kC1 : 0) | (c2 ? kC2 : 0) | (c3 ? kC3 : 0));
}

void X86_64Cpu::x87_reset() {
  s_.fpu_control = 0x37F;
  s_.fpu_status = 0;
  s_.fpu_valid = 0;
}

// ST(i); an empty register is a stack underflow (invalid operation, stack fault): the masked response is the real indefinite.
Ext X86_64Cpu::x87_read(uint32_t stack_index, Env& env) {
  if (x87_empty(stack_index)) {
    x87_underflowed_ = true;
    env.flags |= kInvalid;
    s_.fpu_status = static_cast<uint16_t>((s_.fpu_status | kStackFault) & ~kC1);
    x87_c1_set_ = true;
    return indefinite();
  }
  return x87_phys(stack_index);
}

// Pushes: TOP moves down and the new ST(0) must have been empty (else a stack overflow: invalid operation, and the real indefinite is pushed).
bool X86_64Cpu::x87_push(const Ext& value, Env& env) {
  x87_set_top(x87_top() - 1);
  const bool overflow = !x87_empty(0);
  if (overflow) {
    env.flags |= kInvalid;
    s_.fpu_status = static_cast<uint16_t>(s_.fpu_status | kStackFault | kC1);
    x87_c1_set_ = true;
  }
  x87_phys(0) = overflow ? indefinite() : value;
  s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << x87_top()));
  return !overflow;
}

void X86_64Cpu::x87_pop() {
  s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid & ~(1u << x87_top()));
  x87_set_top(x87_top() + 1);
}

// The full tag word: two bits per physical register (00 valid, 01 zero, 10 special, 11 empty).
uint16_t X86_64Cpu::x87_tag_word() const {
  uint16_t tags = 0;
  for (uint32_t p = 0; p < 8; ++p) {
    uint32_t tag = 3;
    if ((s_.fpu_valid >> p) & 1) {
      switch (classify(s_.st[p])) {
        case Class::Zero: tag = 1; break;
        case Class::Normal: tag = 0; break;
        default: tag = 2; break;
      }
    }
    tags = static_cast<uint16_t>(tags | (tag << (2 * p)));
  }
  return tags;
}

bool X86_64Cpu::x87_load_ext(GuestMemory& m, uint64_t address, Ext* out) {
  uint8_t raw[10];
  if (!m.read(address, ByteView(raw, 10))) {
    fail(14, address, 0);
    return false;
  }
  uint64_t mant = 0;
  for (uint32_t i = 0; i < 8; ++i) mant |= uint64_t{raw[i]} << (8 * i);
  out->mant = mant;
  out->se = static_cast<uint16_t>(raw[8] | (raw[9] << 8));
  return true;
}

bool X86_64Cpu::x87_store_ext(GuestMemory& m, uint64_t address, const Ext& value) {
  uint8_t raw[10];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(value.mant >> (8 * i));
  raw[8] = static_cast<uint8_t>(value.se);
  raw[9] = static_cast<uint8_t>(value.se >> 8);
  if (!m.write(address, ConstByteView(raw, 10))) {
    fail(14, address, 2);
    return false;
  }
  return true;
}

X86_64Cpu::Step X86_64Cpu::step_x87(GuestMemory& m, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  auto advance = [&]() { s_.rip += d_.position; };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  const uint32_t reg = d_.reg & 7;
  x87_underflowed_ = false;
  x87_c1_set_ = false;
  const bool memory = d_.mod != 3;
  const uint32_t i = d_.rm & 7;      // ST(i) of the register forms
  Env env;
  env.mode = static_cast<Rounding>((s_.fpu_control >> 10) & 3);
  const uint32_t precision = precision_bits((s_.fpu_control >> 8) & 3);
  // The destination of a computed result: the real indefinite if an operand was an empty register.
  auto result = [&](const Ext& v) -> Ext { return x87_underflowed_ ? indefinite() : v; };
  auto done = [&]() {
    x87_finish(env);
    advance();
    return Step::Ok;
  };
  // the binary operations: kind is the "reg" field of D8/DC/DA/DE (0 add, 1 mul, 4 sub, 5 subr, 6 div, 7 divr)
  auto arith = [&](uint32_t kind, const Ext& a, const Ext& b) -> Ext {
    switch (kind) {
      case 0: return add(a, b, precision, env);
      case 1: return mul(a, b, precision, env);
      case 4: return sub(a, b, precision, env);
      case 5: return sub(b, a, precision, env);
      case 6: return softfloat::ext::div(a, b, precision, env);
      default: return softfloat::ext::div(b, a, precision, env);
    }
  };
  // the compare instructions set C0, C2, C3
  auto set_compare = [&](int order) { x87_set_cc(order == -1 || order == 2, false, order == 2, order == 0 || order == 2); };
  // the compares that set EFLAGS (FCOMI, FUCOMI)
  auto set_eflags = [&](int order) {
    uint64_t f = s_.rflags & ~(kZero | kParity | kCarry | kOverflow | kSign | kAdjust);
    if (order == 2) f |= kZero | kParity | kCarry;
    else if (order == -1) f |= kCarry;
    else if (order == 0) f |= kZero;
    s_.rflags = f;
  };
  // Operations that write ST(0) with an operand loaded from memory in some format.
  // A denormal single or double operand sets DE -- but only if no NaN or other invalid operation took over, so the flag is applied after the operation (apply_denormal).
  bool memory_denormal = false;
  auto load_operand = [&](uint32_t format, Ext* b) -> bool {      // 0 = single, 1 = int32, 2 = double, 3 = int16
    uint64_t v = 0;
    Env quiet;
    switch (format) {
      case 0:
        if (!load(m, memory_address(), 4, &v)) return false;
        *b = from_single(static_cast<uint32_t>(v), quiet);
        memory_denormal = (quiet.flags & kDenormal) != 0;
        env.flags |= quiet.flags & kInvalid;
        return true;
      case 1: if (!load(m, memory_address(), 4, &v)) return false; *b = from_integer(static_cast<int32_t>(v)); return true;
      case 2:
        if (!load(m, memory_address(), 8, &v)) return false;
        *b = from_double(v, quiet);
        memory_denormal = (quiet.flags & kDenormal) != 0;
        env.flags |= quiet.flags & kInvalid;
        return true;
      default: if (!load(m, memory_address(), 2, &v)) return false; *b = from_integer(static_cast<int16_t>(v)); return true;
    }
  };
  auto apply_denormal = [&](const Ext& other) {
    const Class c = classify(other);
    if (memory_denormal && !(env.flags & (kInvalid | kDivideByZero)) && c != Class::QuietNan && c != Class::SignalingNan) env.flags |= kDenormal;
  };

  switch (op) {
    case 0xD8: case 0xDC: case 0xDA: case 0xDE: {      // arithmetic and compares
      const uint32_t format = op == 0xD8 ? 0 : op == 0xDC ? 2 : op == 0xDA ? 1 : 3;
      if (memory) {
        Ext b;
        if (!load_operand(format, &b)) return Step::Fault;
        const Ext a = x87_read(0, env);
        if (reg == 2 || reg == 3) {      // fcom, fcomp (and ficom, ficomp)
          set_compare(compare(a, b, true, env));
          apply_denormal(a);
          if (reg == 3) x87_pop();
          return done();
        }
        x87_phys(0) = result(arith(reg, a, b));
        apply_denormal(a);
        s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << x87_top()));
        return done();
      }
      if (op == 0xDA) {      // fcmovcc, fucompp
        if (reg == 5 && i == 1) {      // fucompp
          set_compare(compare(x87_read(0, env), x87_read(1, env), false, env));
          x87_pop();
          x87_pop();
          return done();
        }
        if (reg > 3) return fail(6);
        const bool cf = s_.rflags & kCarry, zf = s_.rflags & kZero, pf = s_.rflags & kParity;
        const bool condition = reg == 0 ? cf : reg == 1 ? zf : reg == 2 ? (cf || zf) : pf;
        if (condition) {
          x87_phys(0) = x87_read(i, env);
          s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << x87_top()));
        }
        return done();
      }
      if (op == 0xD8) {
        const Ext a = x87_read(0, env), b = x87_read(i, env);
        if (reg == 2 || reg == 3) {
          set_compare(compare(a, b, true, env));
          if (reg == 3) x87_pop();
          return done();
        }
        x87_phys(0) = result(arith(reg, a, b));
        s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << x87_top()));
        return done();
      }
      // DC and DE register forms: ST(i) = ST(i) op ST(0), with the subtract and divide encodings swapped (reg 4 is FSUBR, 5 FSUB, 6 FDIVR, 7 FDIV)
      if (op == 0xDE && reg == 3) {      // fcompp (DE D9)
        if (i != 1) return fail(6);
        set_compare(compare(x87_read(0, env), x87_read(1, env), true, env));
        x87_pop();
        x87_pop();
        return done();
      }
      if (reg == 2 || reg == 3) {      // DC D0/D8: not instructions; DE D0.. likewise
        return fail(6);
      }
      const Ext st0 = x87_read(0, env), sti = x87_read(i, env);
      const uint32_t kind = reg >= 4 ? (reg ^ 1) : reg;      // 4 <-> 5, 6 <-> 7
      x87_phys(i) = result(arith(kind, sti, st0));
      s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << ((x87_top() + i) & 7)));
      if (op == 0xDE) x87_pop();
      return done();
    }

    case 0xD9: {
      if (memory) {
        switch (reg) {
          case 0: {      // fld m32
            uint64_t v;
            if (!load(m, memory_address(), 4, &v)) return Step::Fault;
            x87_push(from_single(static_cast<uint32_t>(v), env), env);
            return done();
          }
          case 2: case 3: {      // fst m32, fstp m32
            const Ext a = x87_read(0, env);
            if (!store(m, memory_address(), 4, to_single(a, env))) return Step::Fault;
            if (reg == 3) x87_pop();
            return done();
          }
          case 4: {      // fldenv: the 28-byte environment (32-bit protected mode layout)
            uint8_t raw[28];
            if (!m.read(memory_address(), ByteView(raw, 28))) return fail(14, memory_address(), 0);
            s_.fpu_control = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
            s_.fpu_status = static_cast<uint16_t>(raw[4] | (raw[5] << 8));
            const uint16_t tags = static_cast<uint16_t>(raw[8] | (raw[9] << 8));
            s_.fpu_valid = 0;
            for (uint32_t p = 0; p < 8; ++p) if (((tags >> (2 * p)) & 3) != 3) s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << p));
            advance();
            return Step::Ok;
          }
          case 5: {      // fldcw
            uint64_t v;
            if (!load(m, memory_address(), 2, &v)) return Step::Fault;
            s_.fpu_control = static_cast<uint16_t>(v);
            advance();
            return Step::Ok;
          }
          case 6: {      // fnstenv (it then masks all exceptions)
            uint8_t raw[28] = {};
            const uint16_t tags = x87_tag_word();
            raw[0] = static_cast<uint8_t>(s_.fpu_control); raw[1] = static_cast<uint8_t>(s_.fpu_control >> 8);
            raw[4] = static_cast<uint8_t>(s_.fpu_status); raw[5] = static_cast<uint8_t>(s_.fpu_status >> 8);
            raw[8] = static_cast<uint8_t>(tags); raw[9] = static_cast<uint8_t>(tags >> 8);
            if (!m.write(memory_address(), ConstByteView(raw, 28))) return fail(14, memory_address(), 2);
            s_.fpu_control |= 0x3F;
            advance();
            return Step::Ok;
          }
          case 7: {      // fnstcw
            if (!store(m, memory_address(), 2, s_.fpu_control)) return Step::Fault;
            advance();
            return Step::Ok;
          }
          default: return fail(6);
        }
      }
      switch (reg) {
        case 0: {      // fld st(i)
          const Ext v = x87_read(i, env);
          x87_push(v, env);
          return done();
        }
        case 1: {      // fxch st(i)
          const Ext a = x87_read(0, env), b = x87_read(i, env);
          x87_phys(0) = b;
          x87_phys(i) = a;
          s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << x87_top()) | (1u << ((x87_top() + i) & 7)));
          x87_set_cc(false, false, (s_.fpu_status & kC2) != 0, (s_.fpu_status & kC3) != 0);
          s_.fpu_status = static_cast<uint16_t>(s_.fpu_status & ~kC1);
          return done();
        }
        case 2:      // fnop (D9 D0)
          if (i != 0) return fail(6);
          advance();
          return Step::Ok;
        case 4:
          switch (i) {
            case 0: { Ext a = x87_read(0, env); x87_phys(0) = result(negated(a)); s_.fpu_status = static_cast<uint16_t>(s_.fpu_status & ~kC1); return done(); }      // fchs
            case 1: { Ext a = x87_read(0, env); x87_phys(0) = result(absolute(a)); s_.fpu_status = static_cast<uint16_t>(s_.fpu_status & ~kC1); return done(); }      // fabs
            case 4: {      // ftst
              Env quiet = env;
              Ext zero_value;
              set_compare(compare(x87_read(0, env), zero_value, true, quiet));
              env.flags |= quiet.flags;
              return done();
            }
            case 5: {      // fxam
              const bool empty = x87_empty(0);
              const Ext a = x87_phys(0);
              bool c0 = false, c2 = false, c3 = false;
              if (empty) { c0 = true; c3 = true; }
              else switch (classify(a)) {
                case Class::Zero: c3 = true; break;
                case Class::Normal: c2 = true; break;
                case Class::Denormal: c2 = true; c3 = true; break;
                case Class::Infinity: c2 = true; c0 = true; break;
                case Class::QuietNan: case Class::SignalingNan: c0 = true; break;
                default: break;
              }
              x87_set_cc(c0, a.sign(), c2, c3);
              advance();
              return Step::Ok;
            }
            default: return fail(6);
          }
        case 5: {      // constants
          const bool down = env.mode == Rounding::Down || env.mode == Rounding::TowardZero;
          Ext value;
          switch (i) {
            case 0: value = one(); break;
            case 1: value = make_constant(env.mode == Rounding::Up ? 0xD49A784BCD1B8AFFull : 0xD49A784BCD1B8AFEull, 1); break;      // log2(10)
            case 2: value = make_constant(down ? 0xB8AA3B295C17F0BBull : 0xB8AA3B295C17F0BCull, 0); break;      // log2(e)
            case 3: value = make_constant(down ? 0xC90FDAA22168C234ull : 0xC90FDAA22168C235ull, 1); break;      // pi
            case 4: value = make_constant(down ? 0x9A209A84FBCFF798ull : 0x9A209A84FBCFF799ull, -2); break;      // log10(2)
            case 5: value = make_constant(down ? 0xB17217F7D1CF79ABull : 0xB17217F7D1CF79ACull, -1); break;      // ln(2)
            case 6: value = zero(false); break;
            default: return fail(6);
          }
          x87_push(value, env);
          return done();
        }
        case 6:
          switch (i) {
            case 0: {      // f2xm1
              const Ext r = f2xm1(x87_read(0, env), env);
              x87_phys(0) = result(r);
              return done();
            }
            case 1: {      // fyl2x: ST(1) = ST(1) * log2(ST(0)), pop
              const Ext r = fyl2x(x87_read(0, env), x87_read(1, env), env);
              x87_phys(1) = result(r);
              s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << ((x87_top() + 1) & 7)));
              x87_pop();
              return done();
            }
            case 2: {      // fptan: ST(0) = tan, push 1.0
              const Ext x = x87_read(0, env);
              Ext t;
              if (!tangent(x, &t, env)) { x87_set_cc(false, false, true, false); advance(); return Step::Ok; }
              x87_phys(0) = t;
              x87_push(one(), env);
              s_.fpu_status = static_cast<uint16_t>(s_.fpu_status & ~kC2);
              return done();
            }
            case 3: {      // fpatan: ST(1) = atan2(ST(1), ST(0)), pop
              const Ext r = fpatan(x87_read(1, env), x87_read(0, env), env);
              x87_phys(1) = result(r);
              s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << ((x87_top() + 1) & 7)));
              x87_pop();
              return done();
            }
            case 4: {      // fxtract: ST(0) = exponent, push the significand
              Ext significand, exponent;
              extract(x87_read(0, env), &significand, &exponent, env);
              x87_phys(0) = exponent;
              x87_push(significand, env);
              return done();
            }
            case 5: {      // fprem1
              bool complete;
              uint32_t quotient;
              const Ext r = remainder(x87_read(0, env), x87_read(1, env), true, &complete, &quotient, env);
              x87_phys(0) = result(r);
              if (complete) x87_set_cc((quotient & 4) != 0, (quotient & 1) != 0, false, (quotient & 2) != 0);
              else x87_set_cc(false, false, true, false);
              return done();
            }
            case 6: x87_set_top(x87_top() - 1); advance(); return Step::Ok;      // fdecstp
            default: x87_set_top(x87_top() + 1); advance(); return Step::Ok;     // fincstp
          }
        default: break;
      }
      if (reg == 7) {
        switch (i) {
          case 0: {      // fprem
            bool complete;
            uint32_t quotient;
            const Ext r = remainder(x87_read(0, env), x87_read(1, env), false, &complete, &quotient, env);
            x87_phys(0) = result(r);
            if (complete) x87_set_cc((quotient & 4) != 0, (quotient & 1) != 0, false, (quotient & 2) != 0);
            else x87_set_cc(false, false, true, false);
            return done();
          }
          case 1: {      // fyl2xp1
            const Ext r = fyl2xp1(x87_read(0, env), x87_read(1, env), env);
            x87_phys(1) = result(r);
            s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << ((x87_top() + 1) & 7)));
            x87_pop();
            return done();
          }
          case 2: { const Ext r = softfloat::ext::sqrt(x87_read(0, env), precision, env); x87_phys(0) = result(r); return done(); }      // fsqrt
          case 3: {      // fsincos: ST(0) = sin, push cos
            const Ext x = x87_read(0, env);
            Ext sn, cs;
            if (!sin_cos(x, &sn, &cs, env)) { x87_set_cc(false, false, true, false); advance(); return Step::Ok; }
            x87_phys(0) = sn;
            x87_push(cs, env);
            s_.fpu_status = static_cast<uint16_t>(s_.fpu_status & ~kC2);
            return done();
          }
          case 4: { const Ext r = round_to_integer(x87_read(0, env), env.mode, env); x87_phys(0) = result(r); return done(); }      // frndint
          case 5: { const Ext r = scale(x87_read(0, env), x87_read(1, env), env); x87_phys(0) = result(r); return done(); }      // fscale
          case 6: case 7: {      // fsin, fcos
            const Ext x = x87_read(0, env);
            Ext sn, cs;
            if (!sin_cos(x, &sn, &cs, env)) { x87_set_cc(false, false, true, false); advance(); return Step::Ok; }
            x87_phys(0) = result(i == 6 ? sn : cs);
            s_.fpu_status = static_cast<uint16_t>(s_.fpu_status & ~kC2);
            return done();
          }
          default: return fail(6);
        }
      }
      return fail(6);
    }

    case 0xDB: {
      if (memory) {
        switch (reg) {
          case 0: {      // fild m32
            uint64_t v;
            if (!load(m, memory_address(), 4, &v)) return Step::Fault;
            x87_push(from_integer(static_cast<int32_t>(v)), env);
            return done();
          }
          case 1: case 2: case 3: {      // fisttp, fist, fistp m32
            const Ext a = x87_read(0, env);
            int64_t out = 0;
            const bool ok = to_integer(a, 32, reg == 1 ? Rounding::TowardZero : env.mode, env, &out);
            if (!store(m, memory_address(), 4, static_cast<uint64_t>(ok ? out : INT32_MIN))) return Step::Fault;
            if (reg != 2) x87_pop();
            return done();
          }
          case 5: {      // fld m80
            Ext v;
            if (!x87_load_ext(m, memory_address(), &v)) return Step::Fault;
            x87_push(v, env);
            return done();
          }
          case 7: {      // fstp m80
            const Ext a = x87_read(0, env);
            if (!x87_store_ext(m, memory_address(), a)) return Step::Fault;
            x87_pop();
            return done();
          }
          default: return fail(6);
        }
      }
      if (reg < 4) {      // fcmovnb, fcmovne, fcmovnbe, fcmovnu
        const bool cf = s_.rflags & kCarry, zf = s_.rflags & kZero, pf = s_.rflags & kParity;
        const bool condition = reg == 0 ? !cf : reg == 1 ? !zf : reg == 2 ? !(cf || zf) : !pf;
        if (condition) {
          x87_phys(0) = x87_read(i, env);
          s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << x87_top()));
        }
        return done();
      }
      if (reg == 4) {
        if (i == 2) { s_.fpu_status = static_cast<uint16_t>(s_.fpu_status & 0x7F00); advance(); return Step::Ok; }      // fnclex
        if (i == 3) { x87_reset(); advance(); return Step::Ok; }      // fninit
        if (i == 0 || i == 1 || i == 4) { advance(); return Step::Ok; }      // fneni, fndisi, fnsetpm: no-ops
        return fail(6);
      }
      if (reg == 5 || reg == 6) {      // fucomi, fcomi
        set_eflags(compare(x87_read(0, env), x87_read(i, env), reg == 6, env));
        return done();
      }
      return fail(6);
    }

    case 0xDD: {
      if (memory) {
        switch (reg) {
          case 0: {      // fld m64
            uint64_t v;
            if (!load(m, memory_address(), 8, &v)) return Step::Fault;
            x87_push(from_double(v, env), env);
            return done();
          }
          case 1: {      // fisttp m64
            const Ext a = x87_read(0, env);
            int64_t out = 0;
            const bool ok = to_integer(a, 64, Rounding::TowardZero, env, &out);
            if (!store(m, memory_address(), 8, static_cast<uint64_t>(ok ? out : INT64_MIN))) return Step::Fault;
            x87_pop();
            return done();
          }
          case 2: case 3: {      // fst m64, fstp m64
            const Ext a = x87_read(0, env);
            if (!store(m, memory_address(), 8, to_double(a, env))) return Step::Fault;
            if (reg == 3) x87_pop();
            return done();
          }
          case 4: {      // frstor: the 28-byte environment, then ST(0)..ST(7)
            uint8_t raw[108];
            if (!m.read(memory_address(), ByteView(raw, 108))) return fail(14, memory_address(), 0);
            s_.fpu_control = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
            s_.fpu_status = static_cast<uint16_t>(raw[4] | (raw[5] << 8));
            const uint16_t tags = static_cast<uint16_t>(raw[8] | (raw[9] << 8));
            s_.fpu_valid = 0;
            for (uint32_t p = 0; p < 8; ++p) if (((tags >> (2 * p)) & 3) != 3) s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << p));
            for (uint32_t k = 0; k < 8; ++k) {
              Ext& e = x87_phys(k);
              uint64_t mant = 0;
              for (uint32_t b = 0; b < 8; ++b) mant |= uint64_t{raw[28 + 10 * k + b]} << (8 * b);
              e.mant = mant;
              e.se = static_cast<uint16_t>(raw[28 + 10 * k + 8] | (raw[28 + 10 * k + 9] << 8));
            }
            advance();
            return Step::Ok;
          }
          case 6: {      // fnsave: the environment and the registers, then fninit
            uint8_t raw[108] = {};
            const uint16_t tags = x87_tag_word();
            raw[0] = static_cast<uint8_t>(s_.fpu_control); raw[1] = static_cast<uint8_t>(s_.fpu_control >> 8);
            raw[4] = static_cast<uint8_t>(s_.fpu_status); raw[5] = static_cast<uint8_t>(s_.fpu_status >> 8);
            raw[8] = static_cast<uint8_t>(tags); raw[9] = static_cast<uint8_t>(tags >> 8);
            for (uint32_t k = 0; k < 8; ++k) {
              const Ext& e = x87_phys(k);
              for (uint32_t b = 0; b < 8; ++b) raw[28 + 10 * k + b] = static_cast<uint8_t>(e.mant >> (8 * b));
              raw[28 + 10 * k + 8] = static_cast<uint8_t>(e.se);
              raw[28 + 10 * k + 9] = static_cast<uint8_t>(e.se >> 8);
            }
            if (!m.write(memory_address(), ConstByteView(raw, 108))) return fail(14, memory_address(), 2);
            x87_reset();
            advance();
            return Step::Ok;
          }
          case 7: {      // fnstsw m16
            if (!store(m, memory_address(), 2, s_.fpu_status)) return Step::Fault;
            advance();
            return Step::Ok;
          }
          default: return fail(6);
        }
      }
      switch (reg) {
        case 0: s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid & ~(1u << ((x87_top() + i) & 7))); advance(); return Step::Ok;      // ffree
        case 2: case 3: {      // fst st(i), fstp st(i)
          const Ext a = x87_read(0, env);
          x87_phys(i) = a;
          s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid | (1u << ((x87_top() + i) & 7)));
          if (reg == 3) x87_pop();
          return done();
        }
        case 4: case 5: {      // fucom, fucomp
          set_compare(compare(x87_read(0, env), x87_read(i, env), false, env));
          if (reg == 5) x87_pop();
          return done();
        }
        default: return fail(6);
      }
    }

    default: {      // 0xDF
      if (memory) {
        switch (reg) {
          case 0: {      // fild m16
            uint64_t v;
            if (!load(m, memory_address(), 2, &v)) return Step::Fault;
            x87_push(from_integer(static_cast<int16_t>(v)), env);
            return done();
          }
          case 1: case 2: case 3: {      // fisttp, fist, fistp m16
            const Ext a = x87_read(0, env);
            int64_t out = 0;
            const bool ok = to_integer(a, 16, reg == 1 ? Rounding::TowardZero : env.mode, env, &out);
            if (!store(m, memory_address(), 2, static_cast<uint64_t>(ok ? out : INT16_MIN))) return Step::Fault;
            if (reg != 2) x87_pop();
            return done();
          }
          case 5: {      // fild m64
            uint64_t v;
            if (!load(m, memory_address(), 8, &v)) return Step::Fault;
            x87_push(from_integer(static_cast<int64_t>(v)), env);
            return done();
          }
          case 7: {      // fistp m64
            const Ext a = x87_read(0, env);
            int64_t out = 0;
            const bool ok = to_integer(a, 64, env.mode, env, &out);
            if (!store(m, memory_address(), 8, static_cast<uint64_t>(ok ? out : INT64_MIN))) return Step::Fault;
            x87_pop();
            return done();
          }
          default: return fail(6);      // fbld and fbstp (packed decimal) are not emulated
        }
      }
      switch (reg) {
        case 0: s_.fpu_valid = static_cast<uint8_t>(s_.fpu_valid & ~(1u << ((x87_top() + i) & 7))); x87_pop(); advance(); return Step::Ok;      // ffreep
        case 4:
          if (i != 0) return fail(6);
          set_reg(0, 2, s_.fpu_status);      // fnstsw ax
          advance();
          return Step::Ok;
        case 5: case 6:      // fucomip, fcomip
          set_eflags(compare(x87_read(0, env), x87_read(i, env), reg == 6, env));
          x87_pop();
          return done();
        default: return fail(6);
      }
    }
  }
}

}  // namespace emulation
