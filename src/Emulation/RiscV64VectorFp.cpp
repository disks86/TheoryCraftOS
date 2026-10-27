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
#include "Emulation/RiscV64FpOps.hpp"
#include "Emulation/RiscV64VectorUnit.hpp"

namespace emulation {

using namespace softfloat;
using namespace rvfp;

namespace {

constexpr int64_t sext(uint64_t v, uint32_t bits) { return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }

// The 7-bit estimate tables of vfrec7 and vfrsqrt7 (RISC-V V 1.0, section 13.9 and 13.10), indexed by the top 7 significand bits and, for the square root, by {exponent LSB, top 6 bits}.
const uint8_t kRec7[128] = {
    127, 125, 123, 121, 119, 117, 116, 114, 112, 110, 109, 107, 105, 104, 102, 100,
    99, 97, 96, 94, 93, 91, 90, 88, 87, 85, 84, 83, 81, 80, 79, 77,
    76, 75, 74, 72, 71, 70, 69, 68, 66, 65, 64, 63, 62, 61, 60, 59,
    58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43,
    42, 41, 40, 40, 39, 38, 37, 36, 35, 35, 34, 33, 32, 31, 31, 30,
    29, 28, 28, 27, 26, 25, 25, 24, 23, 23, 22, 21, 21, 20, 19, 19,
    18, 17, 17, 16, 15, 15, 14, 14, 13, 12, 12, 11, 11, 10, 9, 9,
    8, 8, 7, 7, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0,
};
const uint8_t kRsqrt7[128] = {
    52, 51, 50, 48, 47, 46, 44, 43, 42, 41, 40, 39, 38, 36, 35, 34,
    33, 32, 31, 30, 30, 29, 28, 27, 26, 25, 24, 23, 23, 22, 21, 20,
    19, 19, 18, 17, 16, 16, 15, 14, 14, 13, 12, 12, 11, 10, 10, 9,
    9, 8, 7, 7, 6, 6, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0,
    127, 125, 123, 121, 119, 118, 116, 114, 113, 111, 109, 108, 106, 105, 103, 102,
    100, 99, 97, 96, 95, 93, 92, 91, 90, 88, 87, 86, 85, 84, 83, 82,
    80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 70, 69, 68, 67, 66,
    65, 64, 63, 63, 62, 61, 60, 59, 59, 58, 57, 56, 56, 55, 54, 53,
};

Format format_of(uint32_t bits) { return bits == 32 ? Format::F32 : Format::F64; }
bool format_bits_unsupported(uint32_t bits) { return bits != 32 && bits != 64; }

uint64_t integer_to_float(Format f, uint64_t bits, uint32_t width, bool is_signed, Env& env) {
  uint64_t v = is_signed ? static_cast<uint64_t>(sext(bits, width)) : bits;
  bool negative = false;
  if (is_signed && static_cast<int64_t>(v) < 0) {
    negative = true;
    v = 0 - v;
  }
  return canonical(f, from_integer(f, v, negative, 0, env));
}

uint64_t float_to_integer(Format f, uint64_t bits, uint32_t width, bool is_signed, Rounding mode, Env& env) {
  if (is_nan(f, bits)) {      // a NaN converts to the largest value
    env.flags |= kInvalid;
    return is_signed ? (uint64_t{1} << (width - 1)) - 1 : (width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1);
  }
  return to_integer_saturating(f, bits, mode, width, !is_signed, 0, env);
}

}  // namespace

RiscV64Cpu::Step VectorUnit::floating() {
  if (sew != 32 && sew != 64 && !(funct6 == 18 && sew == 16)) return undefined();      // (half precision is Zvfh; only the 16-bit integer sides of conversions are allowed)
  const bool vf = funct3 == 5;
  if (funct6 == 18) return floating_convert();
  const Format f = format_of(sew);
  const uint32_t regs = regs_of(lmul_log);
  const uint64_t mask = sew_mask();
  Env env;
  env.arm = true;
  env.tininess_after = true;
  const bool rounding_ok = c.rounding_mode(7, &env.mode);
  const uint64_t scalar = vf ? (sew == 32 ? c.read_single(vs1) : s.f[vs1]) : 0;
  const uint64_t sign = sign_bit_of(f);

  auto finish = [&](Step r) {
    if (r == Step::Ok) c.fp_finish(env);
    return r;
  };
  auto arithmetic = [&](auto&& fn) -> Step {
    if (!rounding_ok) return undefined();
    return finish(binary_loop(*this, !vf, scalar, fn));
  };

  switch (funct6) {
    case 0: return arithmetic([&](uint64_t a, uint64_t b, uint64_t) { return canonical(f, add(f, a, b, env)); });
    case 2: return arithmetic([&](uint64_t a, uint64_t b, uint64_t) { return canonical(f, softfloat::sub(f, a, b, env)); });
    case 4: return finish(binary_loop(*this, !vf, scalar, [&](uint64_t a, uint64_t b, uint64_t) { return minmax(f, a, b, false, env); }));
    case 6: return finish(binary_loop(*this, !vf, scalar, [&](uint64_t a, uint64_t b, uint64_t) { return minmax(f, a, b, true, env); }));
    case 8: return binary_loop(*this, !vf, scalar, [&](uint64_t a, uint64_t b, uint64_t) { return (a & ~sign) | (b & sign); });
    case 9: return binary_loop(*this, !vf, scalar, [&](uint64_t a, uint64_t b, uint64_t) { return (a & ~sign) | (~b & sign); });
    case 10: return binary_loop(*this, !vf, scalar, [&](uint64_t a, uint64_t b, uint64_t) { return a ^ (b & sign); });
    case 32: return arithmetic([&](uint64_t a, uint64_t b, uint64_t) { return canonical(f, softfloat::div(f, a, b, env)); });
    case 36: return arithmetic([&](uint64_t a, uint64_t b, uint64_t) { return canonical(f, mul(f, a, b, env)); });
    case 33:
      if (!vf) return undefined();
      return arithmetic([&](uint64_t a, uint64_t b, uint64_t) { return canonical(f, softfloat::div(f, b, a, env)); });
    case 39:
      if (!vf) return undefined();
      return arithmetic([&](uint64_t a, uint64_t b, uint64_t) { return canonical(f, softfloat::sub(f, b, a, env)); });
    case 40: return arithmetic([&](uint64_t a, uint64_t b, uint64_t d) { return canonical(f, fused_multiply_add(f, b, d, a, env)); });      // vd = vs1 * vd + vs2
    case 41: return arithmetic([&](uint64_t a, uint64_t b, uint64_t d) { return canonical(f, fused_multiply_add(f, negate(f, b), d, negate(f, a), env)); });
    case 42: return arithmetic([&](uint64_t a, uint64_t b, uint64_t d) { return canonical(f, fused_multiply_add(f, b, d, negate(f, a), env)); });
    case 43: return arithmetic([&](uint64_t a, uint64_t b, uint64_t d) { return canonical(f, fused_multiply_add(f, negate(f, b), d, a, env)); });
    case 44: return arithmetic([&](uint64_t a, uint64_t b, uint64_t d) { return canonical(f, fused_multiply_add(f, b, a, d, env)); });      // vd = vs1 * vs2 + vd
    case 45: return arithmetic([&](uint64_t a, uint64_t b, uint64_t d) { return canonical(f, fused_multiply_add(f, negate(f, b), a, negate(f, d), env)); });
    case 46: return arithmetic([&](uint64_t a, uint64_t b, uint64_t d) { return canonical(f, fused_multiply_add(f, b, a, negate(f, d), env)); });
    case 47: return arithmetic([&](uint64_t a, uint64_t b, uint64_t d) { return canonical(f, fused_multiply_add(f, negate(f, b), a, d, env)); });
    case 14: case 15: {      // vfslide1up, vfslide1down
      if (!vf || !group_ok(vd, regs) || !group_ok(vs2, regs) || (!vm && vd == 0) || (funct6 == 14 && vd == vs2)) return undefined();
      if (s.vstart >= vl) return ok();
      for (uint64_t i = s.vstart; i < vl; ++i) {
        if (!active(i)) {
          if (ma) ones(vd, sew_bytes, i, i + 1);
          continue;
        }
        uint64_t v;
        if (funct6 == 14) v = i == 0 ? scalar : get(vs2, sew_bytes, i - 1);
        else v = i + 1 == vl ? scalar : get(vs2, sew_bytes, i + 1);
        put(vd, sew_bytes, i, v);
      }
      tail(vd, regs, sew_bytes, vl);
      return ok();
    }
    case 16: {
      if (!vf) {      // vfmv.f.s
        if (vs1 != 0) return undefined();
        const uint64_t v = get(vs2, sew_bytes, 0);
        if (sew == 32) c.write_single(vd, v);
        else s.f[vd] = v;
        return ok();
      }
      if (vs2 != 0) return undefined();      // vfmv.s.f
      if (s.vstart >= vl) return ok();
      put(vd, sew_bytes, 0, scalar);
      tail(vd, 1, sew_bytes, 1);
      return ok();
    }
    case 23: {      // vfmerge.vfm, vfmv.v.f
      if (!vf || !group_ok(vd, regs) || (vm && vs2 != 0) || (!vm && (!group_ok(vs2, regs) || vd == 0))) return undefined();
      if (s.vstart >= vl) return ok();
      for (uint64_t i = s.vstart; i < vl; ++i) put(vd, sew_bytes, i, vm || mask_bit(0, i) ? scalar : get(vs2, sew_bytes, i));
      tail(vd, regs, sew_bytes, vl);
      return ok();
    }
    case 24: case 25: case 27: case 28: case 29: case 31: {      // vmfeq, vmfle, vmflt, vmfne, vmfgt, vmfge
      if ((funct6 == 29 || funct6 == 31) && !vf) return undefined();
      if (!group_ok(vs2, regs) || (!vf && !group_ok(vs1, regs))) return undefined();
      if (s.vstart >= vl) return ok();
      for (uint64_t i = s.vstart; i < vl; ++i) {
        if (!active(i)) {
          if (ma) set_mask_bit(vd, i, true);
          continue;
        }
        const uint64_t a = get(vs2, sew_bytes, i), b = vf ? scalar : get(vs1, sew_bytes, i);
        const Order order = compare(f, a, b, funct6 != 24 && funct6 != 28, env);
        bool r;
        switch (funct6) {
          case 24: r = order == Order::Equal; break;
          case 25: r = order == Order::Less || order == Order::Equal; break;
          case 27: r = order == Order::Less; break;
          case 28: r = order != Order::Equal; break;
          case 29: r = order == Order::Greater; break;
          default: r = order == Order::Greater || order == Order::Equal; break;
        }
        set_mask_bit(vd, i, r);
      }
      mask_tail(vd);
      return finish(ok());
    }
    case 19: {      // vfsqrt.v, vfclass.v (vfrsqrt7 and vfrec7 below)
      if (vf) return undefined();
      if (vs1 == 0) {
        if (!rounding_ok) return undefined();
        return finish(binary_loop(*this, false, 0, [&](uint64_t a, uint64_t, uint64_t) { return canonical(f, sqrt(f, a, env)); }));
      }
      if (vs1 == 16) return binary_loop(*this, false, 0, [&](uint64_t a, uint64_t, uint64_t) { return classify(f, a); });
      if (vs1 == 4 || vs1 == 5) {
        const bool reciprocal = vs1 == 5;
        return finish(binary_loop(*this, false, 0, [&](uint64_t a, uint64_t, uint64_t) { return estimate7(f, a, reciprocal, env); }));
      }
      return undefined();
    }
    case 18: return floating_convert();
    case 1: case 3: case 5: case 7: case 49: case 51: return float_reduction();
    case 48: case 50: case 52: case 54: case 56: case 60: case 61: case 62: case 63: return float_wide();
    default: return undefined();
  }
}

RiscV64Cpu::Step VectorUnit::float_reduction() {
  const bool widening = funct6 == 49 || funct6 == 51;
  if (funct3 != 1 || s.vstart != 0) return undefined();
  if (widening && sew != 32) return undefined();
  Env env;
  env.arm = true;
  env.tininess_after = true;
  if (!c.rounding_mode(7, &env.mode)) return undefined();
  const uint32_t regs = regs_of(lmul_log);
  if (!group_ok(vs2, regs)) return undefined();
  if (vl == 0) return ok();
  const Format f = format_of(sew), acc_format = widening ? Format::F64 : f;
  const uint32_t acc_bytes = widening ? sew_bytes * 2 : sew_bytes;
  uint64_t acc = get(vs1, acc_bytes, 0);
  for (uint64_t i = 0; i < vl; ++i) {
    if (!active(i)) continue;
    uint64_t x = get(vs2, sew_bytes, i);
    if (widening) x = convert(Format::F32, Format::F64, x, env);
    switch (funct6) {
      case 5: acc = minmax(acc_format, acc, x, false, env); break;
      case 7: acc = minmax(acc_format, acc, x, true, env); break;
      default: acc = canonical(acc_format, add(acc_format, acc, x, env)); break;
    }
  }
  put(vd, acc_bytes, 0, acc);
  tail(vd, 1, acc_bytes, 1);
  c.fp_finish(env);
  return ok();
}


uint64_t VectorUnit::estimate7(Format f, uint64_t a, bool reciprocal, Env& env) {
  const uint32_t fraction_bits = f == Format::F32 ? 23 : 52, exponent_bits = f == Format::F32 ? 8 : 11;
  const int64_t bias = (int64_t{1} << (exponent_bits - 1)) - 1, exponent_max = (int64_t{1} << exponent_bits) - 1;
  const uint64_t fraction_mask = (uint64_t{1} << fraction_bits) - 1;
  const bool negative = (a >> (fraction_bits + exponent_bits)) & 1;
  const int64_t biased = static_cast<int64_t>((a >> fraction_bits) & static_cast<uint64_t>(exponent_max));
  uint64_t fraction = a & fraction_mask;
  const uint64_t sign = static_cast<uint64_t>(negative) << (fraction_bits + exponent_bits);
  const uint64_t infinity = static_cast<uint64_t>(exponent_max) << fraction_bits;
  const uint64_t nan = f == Format::F32 ? kCanonicalSingle : kCanonicalDouble;
  if (biased == exponent_max && fraction != 0) {
    if (!((fraction >> (fraction_bits - 1)) & 1)) env.flags |= kInvalid;
    return nan;
  }
  if (biased == exponent_max) {      // an infinity
    if (reciprocal) return sign;
    if (negative) {
      env.flags |= kInvalid;
      return nan;
    }
    return 0;
  }
  if (biased == 0 && fraction == 0) {      // a zero
    env.flags |= kDivideByZero;
    return sign | infinity;
  }
  if (!reciprocal && negative) {
    env.flags |= kInvalid;
    return nan;
  }
  int64_t exponent = biased;
  if (biased == 0) {      // a subnormal: normalize it (the exponent goes to 0 or below)
    uint32_t top = fraction_bits - 1;
    while (((fraction >> top) & 1) == 0) --top;
    if (reciprocal && top < fraction_bits - 2) {      // too small to invert: the result overflows
      env.flags |= kInexact | kOverflow;
      const bool largest = env.mode == Rounding::TowardZero || (env.mode == Rounding::Down && !negative) || (env.mode == Rounding::Up && negative);
      return sign | (largest ? infinity - 1 : infinity);
    }
    exponent = static_cast<int64_t>(top) + 1 - fraction_bits;
    fraction = (fraction << (fraction_bits - top)) & fraction_mask;
  }
  if (reciprocal) {
    int64_t out_exponent = 2 * bias - 1 - exponent;
    uint64_t out_fraction = static_cast<uint64_t>(kRec7[fraction >> (fraction_bits - 7)]) << (fraction_bits - 7);
    if (out_exponent == 0 || out_exponent == -1) {      // the result is a subnormal
      out_fraction = (out_fraction >> 1) | (uint64_t{1} << (fraction_bits - 1));
      if (out_exponent == -1) out_fraction >>= 1;
      out_exponent = 0;
    }
    return sign | (static_cast<uint64_t>(out_exponent) << fraction_bits) | out_fraction;
  }
  const uint32_t index = static_cast<uint32_t>(((exponent & 1) << 6) | (fraction >> (fraction_bits - 6)));
  const int64_t out_exponent = (3 * bias - 1 - exponent) >> 1;
  return (static_cast<uint64_t>(out_exponent) << fraction_bits) | (static_cast<uint64_t>(kRsqrt7[index]) << (fraction_bits - 7));
}

RiscV64Cpu::Step VectorUnit::float_wide() {
  if (sew != 32 || lmul_log > 2) return undefined();
  const bool vf = funct3 == 5;
  const bool wide_source = funct6 == 52 || funct6 == 54;
  Env env;
  env.arm = true;
  env.tininess_after = true;
  if (!c.rounding_mode(7, &env.mode)) return undefined();
  const uint32_t regs = regs_of(lmul_log), wide_regs = regs_of(lmul_log + 1);
  if (!group_ok(vd, wide_regs) || !group_ok(vs2, wide_source ? wide_regs : regs) || (!vf && !group_ok(vs1, regs)) || (!vm && vd == 0)) return undefined();
  if (s.vstart >= vl) return ok();
  const Format d = Format::F64;
  const uint64_t single = vf ? c.read_single(vs1) : 0;
  for (uint64_t i = s.vstart; i < vl; ++i) {
    if (!active(i)) {
      if (ma) ones(vd, 8, i, i + 1);
      continue;
    }
    const uint64_t a = wide_source ? get(vs2, 8, i) : convert(Format::F32, d, get(vs2, 4, i), env);
    const uint64_t b = convert(Format::F32, d, vf ? single : get(vs1, 4, i), env);
    const uint64_t old = get(vd, 8, i);
    uint64_t r;
    switch (funct6) {
      case 48: case 52: r = add(d, a, b, env); break;
      case 50: case 54: r = softfloat::sub(d, a, b, env); break;
      case 56: r = mul(d, a, b, env); break;
      case 60: r = fused_multiply_add(d, b, a, old, env); break;
      case 61: r = fused_multiply_add(d, negate(d, b), a, negate(d, old), env); break;
      case 62: r = fused_multiply_add(d, b, a, negate(d, old), env); break;
      default: r = fused_multiply_add(d, negate(d, b), a, old, env); break;
    }
    put(vd, 8, i, canonical(d, r));
  }
  tail(vd, wide_regs, 8, vl);
  c.fp_finish(env);
  return ok();
}

RiscV64Cpu::Step VectorUnit::floating_convert() {
  if (funct3 != 1) return undefined();
  const uint32_t sel = vs1, kind = sel & 7;
  const bool widening = sel >= 8 && sel < 16, narrowing = sel >= 16 && sel < 24;
  if (sel >= 24) return undefined();
  if (!widening && !narrowing && (sel > 7 || sel == 4 || sel == 5)) return undefined();
  if (widening && kind == 5) return undefined();
  const bool float_to_int = kind == 0 || kind == 1 || kind == 6 || kind == 7;
  const bool int_to_float = kind == 2 || kind == 3;
  const bool float_to_float = kind == 4 || (narrowing && kind == 5);
  const bool is_signed = kind & 1;
  uint32_t src_bits = sew, dst_bits = sew;
  if (widening) {
    dst_bits = sew * 2;
    if (dst_bits > 64) return undefined();
  } else if (narrowing) {
    src_bits = sew * 2;
    if (src_bits > 64) return undefined();
  }
  // half-precision values (Zvfh) are not supported: a float operand has to be 32 or 64 bits
  if ((float_to_int || float_to_float) && format_bits_unsupported(src_bits)) return undefined();
  if ((int_to_float || float_to_float) && format_bits_unsupported(dst_bits)) return undefined();
  const uint32_t src_bytes = src_bits / 8, dst_bytes = dst_bits / 8;
  const int32_t src_emul = lmul_log + (narrowing ? 1 : 0), dst_emul = lmul_log + (widening ? 1 : 0);
  if (src_emul > 3 || dst_emul > 3) return undefined();
  const uint32_t src_regs = regs_of(src_emul), dst_regs = regs_of(dst_emul);
  if (!group_ok(vd, dst_regs) || !group_ok(vs2, src_regs) || (!vm && vd == 0)) return undefined();
  Env env;
  env.arm = true;
  env.tininess_after = true;
  const bool uses_frm = !(kind == 6 || kind == 7 || sel == 21);
  if (!c.rounding_mode(7, &env.mode) && uses_frm) return undefined();
  if (!uses_frm) env.mode = Rounding::TowardZero;
  if (s.vstart >= vl) return ok();
  for (uint64_t i = s.vstart; i < vl; ++i) {
    if (!active(i)) {
      if (ma) ones(vd, dst_bytes, i, i + 1);
      continue;
    }
    const uint64_t x = get(vs2, src_bytes, i);
    uint64_t r;
    if (float_to_int) {
      r = float_to_integer(format_of(src_bits), x, dst_bits, is_signed, env.mode, env);
    } else if (int_to_float) {
      r = integer_to_float(format_of(dst_bits), x, src_bits, is_signed, env);
    } else {
      const uint32_t accumulated = env.flags;
      env.flags = 0;
      r = canonical(format_of(dst_bits), convert(format_of(src_bits), format_of(dst_bits), x, env));
      if (sel == 21 && (env.flags & kInexact) && !is_nan(format_of(dst_bits), r) && (r & 0x7FFFFFFFu) != 0x7F800000u) r |= 1;      // round to odd
      env.flags |= accumulated;
    }
    put(vd, dst_bytes, i, r);
  }
  tail(vd, dst_regs, dst_bytes, vl);
  c.fp_finish(env);
  return ok();
}

}  // namespace emulation
