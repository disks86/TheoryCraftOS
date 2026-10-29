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
#include "Emulation/X86_64EvexInternal.hpp"
#include "Emulation/SoftFloat.hpp"

namespace emulation {

using namespace softfloat;

namespace {

// Conversion operand types
enum Type : uint32_t { tF32, tF64, tI32, tU32, tI64, tU64, tF16, tI16, tU16 };
uint32_t type_bits(uint32_t t) { return (t == tF16 || t == tI16 || t == tU16) ? 16 : (t == tF32 || t == tI32 || t == tU32) ? 32 : 64; }
bool type_is_float(uint32_t t) { return t == tF32 || t == tF64 || t == tF16; }
Format format_of(uint32_t bits) { return bits == 16 ? Format::F16 : bits == 32 ? Format::F32 : Format::F64; }

uint64_t quiet_bit_of(uint32_t w) { return w == 64 ? uint64_t{1} << 51 : w == 32 ? uint64_t{1} << 22 : uint64_t{1} << 9; }

// One element of a conversion; `bits` is the width of the integer side.
uint64_t convert_element(uint32_t src, uint32_t dst, uint64_t v, bool truncate, Env& env) {
  if (type_is_float(src) && type_is_float(dst)) return convert(format_of(type_bits(src)), format_of(type_bits(dst)), v, env);
  if (type_is_float(src)) {
    const Format f = format_of(type_bits(src));
    const uint32_t bits = type_bits(dst);
    if (dst == tI16 || dst == tI32 || dst == tI64) return static_cast<uint64_t>(to_int(f, v, bits, truncate, env));
    // unsigned: out of range, NaN and negative values give all ones
    Env e2 = env;
    e2.flags = 0;
    if (is_nan(f, v)) {
      env.flags |= kInvalid;
      return width_mask(bits);
    }
    const uint64_t r = to_integer_saturating(f, v, truncate ? Rounding::TowardZero : env.mode, bits, true, 0, e2);
    env.flags |= e2.flags;
    return (e2.flags & kInvalid) ? width_mask(bits) : r;
  }
  const Format f = format_of(type_bits(dst));
  const uint32_t bits = type_bits(src);
  if (src == tI16 || src == tI32 || src == tI64) return from_int(f, signed_of(v, bits), env);
  return from_integer(f, v & width_mask(bits), false, 0, env);
}


struct FloatFields {
  uint32_t mant_bits, exp_bits;
  int32_t bias;
  uint64_t mant_mask, exp_max;
};
FloatFields fields_of(uint32_t elem) {
  if (elem == 16) return FloatFields{10, 5, 15, (uint64_t{1} << 10) - 1, 0x1F};
  return elem == 64 ? FloatFields{52, 11, 1023, (uint64_t{1} << 52) - 1, 0x7FF} : FloatFields{23, 8, 127, (uint64_t{1} << 23) - 1, 0xFF};
}
uint64_t build(uint32_t elem, bool negative, uint64_t exponent, uint64_t mantissa) {
  const FloatFields f = fields_of(elem);
  return (uint64_t{negative} << (f.mant_bits + f.exp_bits)) | (exponent << f.mant_bits) | mantissa;
}
uint64_t default_nan(uint32_t elem) { return elem == 64 ? 0xFFF8000000000000ull : elem == 32 ? 0xFFC00000u : 0xFE00u; }
// 2^e as a bit pattern (e down to the smallest denormal)
uint64_t power_of_two(uint32_t elem, int32_t e) {
  const FloatFields f = fields_of(elem);
  if (e + f.bias >= 1) return build(elem, false, static_cast<uint64_t>(e + f.bias), 0);
  return uint64_t{1} << (e + f.bias - 1 + static_cast<int32_t>(f.mant_bits));
}

// x * 2^k with at most one rounding in the common cases
uint64_t scale_by_power_of_two(Format fmt, uint32_t elem, uint64_t x, int64_t k, Env& env) {
  const FloatFields f = fields_of(elem);
  auto power = [&](int32_t e) { return build(elem, false, static_cast<uint64_t>(e + f.bias), 0); };
  if (k > 0) {
    while (k > 0) {
      const int32_t step = k > f.bias ? f.bias : static_cast<int32_t>(k);
      x = mul(fmt, x, power(step), env);
      k -= step;
    }
    return x;
  }
  const int32_t exponent_field = static_cast<int32_t>((x >> f.mant_bits) & f.exp_max);
  if (exponent_field > 1) {      // bring a normal number down to the smallest exponent without rounding
    int64_t room = exponent_field - 1;
    while (room > 0 && k < 0) {
      int64_t piece = -k < room ? -k : room;
      if (piece > f.bias - 1) piece = f.bias - 1;
      x = mul(fmt, x, power(static_cast<int32_t>(-piece)), env);
      k += piece;
      room -= piece;
    }
  }
  while (k < 0) {
    const int32_t step = -k > f.bias - 1 ? f.bias - 1 : static_cast<int32_t>(-k);
    x = mul(fmt, x, power(-step), env);
    k += step;
  }
  return x;
}

}  // namespace

X86_64Cpu::Step X86_64Cpu::evex_fp(GuestMemory& m, const EvexOp& defn) {
  const EvexOp* def = &defn;
  const uint32_t reg = ev_.reg, vvvv = ev_.vvvv, aaa = ev_.aaa, rm_reg = ev_.rm;
  const bool memory = ev_.memory, z = ev_.z, w = ev_.w, er = ev_.er;
  uint32_t vl = ev_.vl;
  const uint32_t elem = def->elem ? def->elem : (w ? 64 : 32);
  const Format fmt = format_of(elem);
  const bool scalar = def->kind == kFpScalar || def->kind == kCvtScalar || def->kind == kCvtToGpr || def->kind == kCvtFromGpr || (def->kind == kFpCmp && def->sub == 1) || def->kind == kComi ||
                      (def->sub == 1 && (def->kind == kRound || def->kind == kGetExp || def->kind == kScalef || def->kind == kGetMant || def->kind == kFixup || def->kind == kReduce || def->kind == kRange || def->kind == kFpClass || def->kind == kRcp14 || def->kind == kRsqrt14)) || (def->kind == kFma && (def->opcode & 15) % 2 == 1 && (def->opcode & 15) >= 9) || (def->kind == kFma4 && (def->sub & 1));
  if (scalar) vl = 128;
  Env env;
  // EVEX.b with a register operand is an embedded rounding mode for the arithmetic forms but only "suppress all exceptions" (the rounding comes from MXCSR or the immediate) for the
  // minimum / maximum, comparisons, rndscale / reduce / getexp / getmant / range / fixup, and the truncating or exact conversions.
  const bool sae_only = def->kind == kFpCmp || def->kind == kComi || def->kind == kRound || def->kind == kReduce || def->kind == kGetExp || def->kind == kGetMant || def->kind == kRange ||
                        def->kind == kFixup || (def->kind == kFpBin && (def->sub == 0x5D || def->sub == 0x5F)) || (def->kind == kFpScalar && (def->sub == 0x5D || def->sub == 0x5F)) ||
                        (def->kind == kCvtToGpr && (def->sub & 0x40)) ||
                        ((def->kind == kCvt || def->kind == kCvtScalar) && ((def->sub & 0x40) || (type_is_float(def->sub & 7) && type_is_float((def->sub >> 3) & 7) && type_bits(def->sub & 7) < type_bits((def->sub >> 3) & 7))));
  env.mode = er && !sae_only ? static_cast<Rounding>(ev_.rc) : static_cast<Rounding>((s_.mxcsr >> 13) & 3);
  env.daz = (s_.mxcsr & 0x40) != 0;
  env.ftz = (s_.mxcsr & 0x8000) != 0;
  auto active = [&](uint32_t i) { return ev_active(i); };
  auto commit = [&]() {
    if (!er) s_.mxcsr |= env.flags;
    s_.rip += d_.position;
    return Step::Ok;
  };
  // flags of one element: an invalid operation takes precedence over the denormal flag
  auto fold = [&](Env& le) {
    if (le.flags & kInvalid || elem == 16) le.flags &= ~kDenormal;      // (FP16 has no denormal exception)
    env.flags |= le.flags;
  };
  const uint32_t count = scalar ? 1 : vl / elem;
  Vec a = get_vec(vvvv), b, result;
  const uint32_t saved_vl = ev_.vl;
  ev_.vl = vl;

  switch (def->kind) {
    case kComi: {      // vucomiss / vcomiss and the double forms
      uint64_t v;
      if (memory) {
        if (!load(m, memory_address(), elem / 8, &v)) return Step::Fault;
      } else {
        v = element(get_vec(rm_reg), elem, 0);
      }
      const uint64_t x = element(get_vec(reg), elem, 0);
      Env le = env;
      le.flags = 0;
      const Order order = compare(fmt, x, v, def->sub == 1, le);
      uint64_t flags = 0;
      switch (order) {
        case Order::Unordered: flags = kZero | kParity | kCarry; break;
        case Order::Less: flags = kCarry; break;
        case Order::Equal: flags = kZero; break;
        default: break;
      }
      s_.rflags = (s_.rflags & ~(kZero | kParity | kCarry | kOverflow | kAdjust | kSign)) | flags;
      fold(le);
      return commit();
    }
    case kFpBin: case kFpScalar: case kFpUnary: {
      if (scalar) {
        if (memory) {
          uint64_t v;
          if (!load(m, memory_address(), elem / 8, &v)) return Step::Fault;
          b.q[0] = v;
        } else {
          b = get_vec(rm_reg);
        }
      } else if (!ev_read_rm(m, &b, elem, true, true)) {
        return Step::Fault;
      }
      result = scalar ? a : Vec();
      for (uint32_t i = 0; i < count; ++i) {
        if (aaa != 0 && !active(i)) {
          set_element(result, elem, i, scalar ? element(ev_.old_dest, elem, 0) : (z ? 0 : element(ev_.old_dest, elem, i)));
          if (scalar && z) set_element(result, elem, 0, 0);
          continue;
        }
        const uint64_t x = element(a, elem, i), y = element(b, elem, i);
        Env le = env;
        le.flags = 0;
        uint64_t r;
        switch (def->sub) {
          case 0x51: r = sqrt(fmt, y, le); break;
          case 0x58: r = add(fmt, x, y, le); break;
          case 0x59: r = mul(fmt, x, y, le); break;
          case 0x5C: r = softfloat::sub(fmt, x, y, le); break;
          case 0x5D: r = minimum(fmt, x, y, le); break;
          case 0x5E: r = softfloat::div(fmt, x, y, le); break;
          default: r = maximum(fmt, x, y, le); break;
        }
        fold(le);
        set_element(result, elem, i, r);
      }
      set_vec(reg, result, vl);
      if (!scalar && false) {}
      return commit();
    }
    case kFma: {
      const uint32_t low = def->opcode & 15;
      const uint32_t order = def->opcode >> 4;      // 9: 132, A: 213, B: 231
      if (scalar) {
        if (memory) {
          uint64_t v;
          if (!load(m, memory_address(), elem / 8, &v)) return Step::Fault;
          b.q[0] = v;
        } else {
          b = get_vec(rm_reg);
        }
      } else if (!ev_read_rm(m, &b, elem, true, true)) {
        return Step::Fault;
      }
      const Vec dest = ev_.old_dest;
      result = dest;
      const bool negate_product = low == 0xC || low == 0xD || low == 0xE || low == 0xF;
      const bool negate_addend = low == 0xA || low == 0xB || low == 0xE || low == 0xF;
      const uint64_t qb = quiet_bit_of(elem);
      for (uint32_t i = 0; i < count; ++i) {
        if (aaa != 0 && !active(i)) {
          if (z) set_element(result, elem, i, 0);
          continue;
        }
        const uint64_t A = element(dest, elem, i), B = element(a, elem, i), C = element(b, elem, i);
        uint64_t p, q, r;
        if (order == 9) { p = A; q = C; r = B; }
        else if (order == 0xA) { p = B; q = A; r = C; }
        else { p = B; q = C; r = A; }
        const uint64_t vals[3] = {p, q, r};
        int pick = -1;
        bool any_signaling = false;
        for (int k = 0; k < 3; ++k) {
          if (!is_nan(fmt, vals[k])) continue;
          if (!(vals[k] & qb)) any_signaling = true;
          if (pick < 0) pick = k;
        }
        if (pick >= 0) {
          if (any_signaling) env.flags |= kInvalid;
          set_element(result, elem, i, vals[pick] | qb);
          continue;
        }
        bool neg_p = negate_product, neg_r = negate_addend;
        if (low == 6) neg_r = (i & 1) == 0;
        if (low == 7) neg_r = (i & 1) == 1;
        if (neg_p && !is_nan(fmt, p)) p = negate(fmt, p);
        if (neg_r && !is_nan(fmt, r)) r = negate(fmt, r);
        Env le = env;
        le.flags = 0;
        set_element(result, elem, i, fused_multiply_add(fmt, p, q, r, le));
        fold(le);
      }
      set_vec(reg, result, vl);
      return commit();
    }
    case kFma4: {      // Xeon Phi 4FMAPS: dest += four products of the register group (vvvv & ~3 ..+3) with the four floats of the m128 operand, one fused multiply-add each
      if (!memory || ev_.bcst || (!scalar && saved_vl != 512)) return fail(6);
      uint64_t weights[4];
      for (uint32_t j = 0; j < 4; ++j) {
        if (!load(m, memory_address() + 4 * j, 4, &weights[j])) return Step::Fault;
      }
      const bool negate_product = def->sub & 2;
      const uint64_t qb = quiet_bit_of(elem);
      const Vec dest = ev_.old_dest;
      Vec group[4];
      for (uint32_t j = 0; j < 4; ++j) group[j] = get_vec((vvvv & ~3u) + j);
      result = dest;
      for (uint32_t i = 0; i < count; ++i) {
        if (aaa != 0 && !active(i)) {
          if (z) set_element(result, elem, i, 0);
          continue;
        }
        uint64_t acc = element(dest, elem, i);
        for (uint32_t j = 0; j < 4; ++j) {
          uint64_t p = element(group[j], elem, i), q = weights[j];
          const uint64_t vals[3] = {p, q, acc};
          int pick = -1;
          bool any_signaling = false;
          for (int k = 0; k < 3; ++k) {
            if (!is_nan(fmt, vals[k])) continue;
            if (!(vals[k] & qb)) any_signaling = true;
            if (pick < 0) pick = k;
          }
          if (pick >= 0) {
            if (any_signaling) env.flags |= kInvalid;
            acc = vals[pick] | qb;
            continue;
          }
          if (negate_product) p = negate(fmt, p);
          Env le = env;
          le.flags = 0;
          acc = fused_multiply_add(fmt, p, q, acc, le);
          fold(le);
        }
        set_element(result, elem, i, acc);
      }
      set_vec(reg, result, vl);
      return commit();
    }
    case kFpCmp: {
      if (scalar) {
        if (memory) {
          uint64_t v;
          if (!load(m, memory_address(), elem / 8, &v)) return Step::Fault;
          b.q[0] = v;
        } else {
          b = get_vec(rm_reg);
        }
      } else if (!ev_read_rm(m, &b, elem, true, false)) {
        return Step::Fault;
      }
      const uint32_t pred = ev_.imm;
      if (pred > 31) return fail(6);
      uint64_t bits = 0;
      for (uint32_t i = 0; i < count; ++i) {
        if (aaa != 0 && !active(i)) continue;
        const bool signaling = (pred & 3) == 1 || (pred & 3) == 2 ? !(pred & 0x10) : (pred & 0x10) != 0;
        Env le = env;
        le.flags = 0;
        const Order order = compare(fmt, element(a, elem, i), element(b, elem, i), signaling, le);
        fold(le);
        bool r;
        switch (pred & 15) {
          case 0: r = order == Order::Equal; break;
          case 1: r = order == Order::Less; break;
          case 2: r = order == Order::Less || order == Order::Equal; break;
          case 3: r = order == Order::Unordered; break;
          case 4: r = order != Order::Equal; break;
          case 5: r = order != Order::Less; break;
          case 6: r = order != Order::Less && order != Order::Equal; break;
          case 7: r = order != Order::Unordered; break;
          case 8: r = order == Order::Equal || order == Order::Unordered; break;
          case 9: r = order == Order::Less || order == Order::Unordered; break;
          case 10: r = order == Order::Less || order == Order::Equal || order == Order::Unordered; break;
          case 11: r = false; break;
          case 12: r = order != Order::Equal && order != Order::Unordered; break;
          case 13: r = order == Order::Greater || order == Order::Equal; break;
          case 14: r = order == Order::Greater; break;
          default: r = true; break;
        }
        if (r) bits |= uint64_t{1} << i;
      }
      s_.k[reg & 7] = bits;
      return commit();
    }
    case kRound: {      // vrndscale
      if (scalar) {
        if (memory) {
          uint64_t v;
          if (!load(m, memory_address(), elem / 8, &v)) return Step::Fault;
          b.q[0] = v;
        } else {
          b = get_vec(rm_reg);
        }
      } else if (!ev_read_rm(m, &b, elem, true, true)) {
        return Step::Fault;
      }
      const uint32_t imm = ev_.imm, scale = imm >> 4;
      Rounding mode = (imm & 4) ? static_cast<Rounding>((s_.mxcsr >> 13) & 3) : static_cast<Rounding>(imm & 3);
      result = scalar ? a : Vec();
      for (uint32_t i = 0; i < count; ++i) {
        if (aaa != 0 && !active(i)) {
          set_element(result, elem, i, (scalar || !z) ? element(ev_.old_dest, elem, i) : 0);
          if (scalar && z) set_element(result, elem, 0, 0);
          continue;
        }
        Env le = env;
        le.flags = 0;
        uint64_t x = element(b, elem, i);
        uint64_t r;
        if (is_nan(fmt, x)) {
          if (!(x & quiet_bit_of(elem))) le.flags |= kInvalid;
          r = x | quiet_bit_of(elem);
        } else if (scale == 0) {
          r = round_to_integral(fmt, x, mode, !(imm & 8), le);
        } else if (static_cast<int32_t>((x >> fields_of(elem).mant_bits) & fields_of(elem).exp_max) - fields_of(elem).bias >= static_cast<int32_t>(fields_of(elem).mant_bits) - static_cast<int32_t>(scale)) {
          r = x;      // no bit below the 2^-scale position: already a multiple of it
        } else {
          // scale by 2^scale, round, scale back (exact for the ranges involved)
          const uint64_t p2 = power_of_two(elem, static_cast<int32_t>(scale));
          const uint64_t p2inv = power_of_two(elem, -static_cast<int32_t>(scale));
          Env e2 = le;
          e2.flags = 0;
          e2.ftz = false;      // (the scaling steps are internal: nothing is flushed or flagged until the final value)
          const uint64_t scaled = mul(fmt, x, p2, e2);
          Env e3 = le;
          e3.flags = 0;
          e3.daz = false;
          const uint64_t rounded = round_to_integral(fmt, scaled, mode, !(imm & 8), e3);
          Env e4 = le;
          e4.flags = 0;
          e4.ftz = false;
          e4.daz = false;
          r = mul(fmt, rounded, p2inv, e4);
          le.flags |= e3.flags;
        }
        fold(le);
        set_element(result, elem, i, r);
      }
      set_vec(reg, result, vl);
      return commit();
    }
    case kCvt: {
      uint32_t src = def->sub & 7, dst = (def->sub >> 3) & 7;
      if (def->sub & 0x80) {      // the 16-bit integer side is unsigned (vcvtph2uw, vcvtuw2ph)
        if (src == tI16) src = tU16;
        if (dst == tI16) dst = tU16;
      }
      const bool truncate = def->sub & 0x40;
      const uint32_t sw = type_bits(src), dw = type_bits(dst);
      const uint32_t n = vl / (sw > dw ? sw : dw);
      ev_.vl = n * sw;
      if (ev_.vl < 32) ev_.vl = 32;
      if (!ev_read_rm(m, &b, sw, true, false)) {
        ev_.vl = saved_vl;
        return Step::Fault;
      }
      ev_.vl = saved_vl;
      for (uint32_t i = 0; i < n; ++i) {
        if (aaa != 0 && !active(i)) {
          set_element(result, dw, i, z ? 0 : element(ev_.old_dest, dw, i));
          continue;
        }
        Env le = env;
        le.flags = 0;
        const uint64_t r = convert_element(src, dst, element(b, sw, i), truncate, le);
        if (!type_is_float(dst) || !type_is_float(src) || src == tF16) le.flags &= ~kDenormal;
        fold(le);
        set_element(result, dw, i, r);
      }
      set_vec(reg, result, n * dw < 128 ? 128 : n * dw);
      return commit();
    }
    case kCvtScalar: {
      const uint32_t src = def->sub & 7, dst = (def->sub >> 3) & 7;
      uint64_t v;
      if (memory) {
        if (!load(m, memory_address(), type_bits(src) / 8, &v)) return Step::Fault;
      } else {
        v = get_vec(rm_reg).q[0] & width_mask(type_bits(src));
      }
      result = a;
      const bool lane = aaa == 0 || active(0);
      if (lane) {
        Env le = env;
        le.flags = 0;
        const uint64_t r = convert_element(src, dst, v, false, le);
        if (src == tF16) le.flags &= ~kDenormal;
        fold(le);
        set_element(result, type_bits(dst), 0, r);
      } else {
        set_element(result, type_bits(dst), 0, z ? 0 : element(ev_.old_dest, type_bits(dst), 0));
      }
      set_vec(reg, result, 128);
      return commit();
    }
    case kCvtToGpr: {
      const uint32_t src = def->sub & 7;
      const bool truncate = def->sub & 0x40, is_unsigned = def->sub & 0x80;
      uint64_t v;
      if (memory) {
        if (!load(m, memory_address(), type_bits(src) / 8, &v)) return Step::Fault;
      } else {
        v = get_vec(rm_reg).q[0] & width_mask(type_bits(src));
      }
      const uint32_t dst_bits = w ? 64 : 32;
      Env le = env;
      le.flags = 0;
      const uint64_t r = convert_element(src, is_unsigned ? (w ? tU64 : tU32) : (w ? tI64 : tI32), v, truncate, le);
      le.flags &= ~kDenormal;
      fold(le);
      set_reg(d_.reg, dst_bits / 8, r);
      return commit();
    }
    case kCvtFromGpr: {
      const uint32_t dst = def->sub & 7;
      const bool is_unsigned = def->sub & 0x80;
      uint64_t v;
      const uint32_t src_bits = w ? 64 : 32;
      if (memory) {
        if (!load(m, memory_address(), src_bits / 8, &v)) return Step::Fault;
      } else {
        v = get_reg(d_.rm, src_bits / 8);
      }
      result = a;
      Env le = env;
      le.flags = 0;
      const uint64_t r = convert_element(is_unsigned ? (w ? tU64 : tU32) : (w ? tI64 : tI32), dst, v, false, le);
      le.flags &= ~kDenormal;
      fold(le);
      set_element(result, type_bits(dst), 0, r);
      set_vec(reg, result, 128);
      return commit();
    }
    case kGetExp: case kGetMant: case kReduce: case kRcp14: case kRsqrt14: case kFpClass: case kScalef: case kRange: case kFixup: {
      const bool binary = def->kind == kScalef || def->kind == kRange || def->kind == kFixup;
      if (scalar) {
        if (memory) {
          uint64_t v;
          if (!load(m, memory_address(), elem / 8, &v)) return Step::Fault;
          b.q[0] = v;
        } else {
          b = get_vec(rm_reg);
        }
      } else if (!ev_read_rm(m, &b, elem, true, true)) {
        return Step::Fault;
      }
      const FloatFields f = fields_of(elem);
      const uint64_t qb = quiet_bit_of(elem);
      const uint32_t imm = ev_.imm;
      uint64_t class_bits = 0;
      result = scalar ? a : Vec();
      if (def->kind == kFixup && !scalar) result = ev_.old_dest;
      for (uint32_t i = 0; i < count; ++i) {
        if (aaa != 0 && !active(i)) {
          if (def->kind == kFpClass) continue;
          if (def->kind == kFixup) {
            if (scalar) set_element(result, elem, 0, z ? 0 : element(ev_.old_dest, elem, 0));
            else if (z) set_element(result, elem, i, 0);
            continue;
          }
          set_element(result, elem, i, scalar ? element(ev_.old_dest, elem, 0) : (z ? 0 : element(ev_.old_dest, elem, i)));
          if (scalar && z) set_element(result, elem, 0, 0);
          continue;
        }
        Env le = env;
        le.flags = 0;
        const uint64_t x = flush_denormal(fmt, element(binary ? a : b, elem, i), env), y = def->kind == kFixup ? element(b, elem, i) : flush_denormal(fmt, element(b, elem, i), env);      // (MXCSR.DAZ; the table of vfixupimm is integers)
        const bool negative = (x >> (f.mant_bits + f.exp_bits)) & 1;
        const uint64_t exponent = (x >> f.mant_bits) & f.exp_max, mantissa = x & f.mant_mask;
        const bool x_nan = exponent == f.exp_max && mantissa, x_inf = exponent == f.exp_max && !mantissa, x_zero = exponent == 0 && !mantissa, x_denormal = exponent == 0 && mantissa;
        const bool x_snan = x_nan && !(x & qb);
        uint64_t r = 0;
        switch (def->kind) {
          case kGetExp:
            if (x_nan) { if (x_snan) le.flags |= kInvalid; r = x | qb; }
            else if (x_inf) r = build(elem, false, f.exp_max, 0);
            else if (x_zero) r = build(elem, true, f.exp_max, 0);
            else {
              int32_t e;
              if (x_denormal) {
                uint32_t top = f.mant_bits - 1;
                while (!((mantissa >> top) & 1)) --top;
                e = static_cast<int32_t>(top) + 1 - f.bias - static_cast<int32_t>(f.mant_bits);
              } else {
                e = static_cast<int32_t>(exponent) - f.bias;
              }
              r = from_int(fmt, e, le);
            }
            break;
          case kRcp14: case kRsqrt14: {
            if (x_nan) { if (x_snan) le.flags |= kInvalid; r = x | qb; }
            else if (x_zero) r = build(elem, negative, f.exp_max, 0);
            else if (def->kind == kRcp14 && x_inf) r = build(elem, negative, 0, 0);
            else if (def->kind == kRsqrt14 && negative) { le.flags |= kInvalid; r = default_nan(elem); }
            else if (def->kind == kRsqrt14 && x_inf) r = 0;
            else {
              Env e2 = le;
              e2.mode = Rounding::NearestEven;
              e2.flags = 0;
              const uint64_t one = build(elem, false, static_cast<uint64_t>(f.bias), 0);
              uint64_t v;
              if (elem == 16 && def->kind == kRsqrt14) {      // 1/sqrt in double precision, then one rounding to half (two roundings in half would be off by an ulp now and then)
                const uint64_t x64 = convert(Format::F16, Format::F64, x, e2);
                const uint64_t q = softfloat::div(Format::F64, 0x3FF0000000000000ull, sqrt(Format::F64, x64, e2), e2);
                v = convert(Format::F64, Format::F16, q, e2);
              } else {
                v = def->kind == kRcp14 ? softfloat::div(fmt, one, x, e2) : softfloat::div(fmt, one, sqrt(fmt, x, e2), e2);
              }
              if (elem != 16) {      // keep 14 bits of the significand (the half-precision forms keep the correctly rounded value, which is within their 2^-11 bound)
                const uint32_t drop = f.mant_bits - 14;
                if (((v >> f.mant_bits) & f.exp_max) != 0 && ((v >> f.mant_bits) & f.exp_max) != f.exp_max) v &= ~((uint64_t{1} << drop) - 1);
              }
              r = v;
              le.flags |= e2.flags & kOverflow;
            }
            break;
          }
          case kFpClass: {
            const bool qnan = x_nan && (x & qb), pos_zero = x_zero && !negative, neg_zero = x_zero && negative, pos_inf = x_inf && !negative, neg_inf = x_inf && negative;
            const bool fin_neg = negative && !x_nan && !x_inf && !x_zero;
            const bool hit = ((imm & 1) && qnan) || ((imm & 2) && pos_zero) || ((imm & 4) && neg_zero) || ((imm & 8) && pos_inf) || ((imm & 16) && neg_inf) || ((imm & 32) && x_denormal) ||
                             ((imm & 64) && fin_neg) || ((imm & 128) && x_snan);
            if (hit) class_bits |= uint64_t{1} << i;
            continue;
          }
          case kScalef: {
            const uint64_t y2 = y;
            const bool y_negative = (y2 >> (f.mant_bits + f.exp_bits)) & 1;
            const uint64_t ye = (y2 >> f.mant_bits) & f.exp_max, ym = y2 & f.mant_mask;
            const bool y_nan = ye == f.exp_max && ym, y_inf = ye == f.exp_max && !ym, y_snan = y_nan && !(y2 & qb);
            if (x_nan || y_nan) {
              if (x_snan || y_snan) le.flags |= kInvalid;
              r = (x_nan ? x : y2) | qb;
            } else if (x_zero) {
              if (y_inf && !y_negative) { le.flags |= kInvalid; r = default_nan(elem); } else r = x;
            } else if (x_inf) {
              if (y_inf && y_negative) { le.flags |= kInvalid; r = default_nan(elem); } else r = x;
            } else if (y_inf) {
              r = y_negative ? build(elem, negative, 0, 0) : build(elem, negative, f.exp_max, 0);
            } else {
              Env e2 = le;
              e2.flags = 0;
              e2.ftz = false;      // (one scaling: MXCSR.FTZ looks at the final result only,
              e2.daz = false;      // and a denormal made on the way is still a number: the input was flushed above)
              int64_t k;
              const int32_t y_unbiased = ye == 0 ? -f.bias : static_cast<int32_t>(ye) - f.bias;
              if (y_unbiased >= (elem == 16 ? 7 : 20)) k = y_negative ? -(1 << 20) : (1 << 20);
              else {
                Env e3 = le;
                e3.flags = 0;
                k = to_int(fmt, y2, 64, false, e3);
                // floor: to_int rounded per the mode; redo with Down
                Env e4;
                e4.mode = Rounding::Down;
                k = to_int(fmt, y2, 64, false, e4);
              }
              r = scale_by_power_of_two(fmt, elem, x, k, e2);
              le.flags |= e2.flags;
              if (env.ftz && ((r >> f.mant_bits) & f.exp_max) == 0 && (r & ((uint64_t{1} << (f.mant_bits + f.exp_bits)) - 1)) != 0) {      // a tiny result (denormal; an exact one too) is flushed
                le.flags |= kUnderflow | kInexact;
                r = r & (uint64_t{1} << (f.mant_bits + f.exp_bits));
              }
            }
            break;
          }
          case kGetMant: {
            const uint32_t interval = imm & 3, sign_ctl = (imm >> 2) & 3;
            const bool out_negative = (sign_ctl & 1) ? false : negative;
            if (x_nan) { if (x_snan) le.flags |= kInvalid; r = x | qb; }
            else if ((sign_ctl & 2) && negative && !x_zero) { le.flags |= kInvalid; r = default_nan(elem); }
            else if (x_zero || x_inf) r = build(elem, out_negative, static_cast<uint64_t>(f.bias), 0);
            else {
              uint64_t man = mantissa;
              uint64_t exp_field = exponent;
              if (x_denormal) {      // normalize
                uint32_t top = f.mant_bits - 1;
                while (!((man >> top) & 1)) --top;
                const uint32_t shift = f.mant_bits - top;
                man = (man << shift) & f.mant_mask;
                exp_field = 1 - shift;      // (may wrap; only its low bit matters below)
              }
              uint64_t out_exp;
              switch (interval) {
                case 0: out_exp = static_cast<uint64_t>(f.bias); break;
                case 1: out_exp = (exp_field & 1) ? static_cast<uint64_t>(f.bias) - 1 + 1 - 1 + 0 : 0; out_exp = (exp_field & 1) ? static_cast<uint64_t>(f.bias) : static_cast<uint64_t>(f.bias) - 1; break;
                case 2: out_exp = static_cast<uint64_t>(f.bias) - 1; break;
                default: out_exp = ((man >> (f.mant_bits - 1)) & 1) ? static_cast<uint64_t>(f.bias) - 1 : static_cast<uint64_t>(f.bias); break;
              }
              r = build(elem, out_negative, out_exp, man);
              if (x_denormal) le.flags |= kDenormal;
            }
            break;
          }
          case kReduce: {
            const uint32_t scale = imm >> 4;
            Rounding mode = (imm & 4) ? static_cast<Rounding>((s_.mxcsr >> 13) & 3) : static_cast<Rounding>(imm & 3);
            if (x_nan) { if (x_snan) le.flags |= kInvalid; r = x | qb; break; }
            if (x_inf) { r = 0; break; }
            uint64_t rounded;
            Env e2 = le;
            e2.flags = 0;
            e2.ftz = false;      // (the hardware flushes the final difference only, and without an underflow flag)
            if (x_zero || static_cast<int32_t>(exponent) - f.bias >= static_cast<int32_t>(f.mant_bits) - static_cast<int32_t>(scale)) {
              rounded = x;      // nothing below the 2^-scale position
            } else if (scale == 0) {
              rounded = round_to_integral(fmt, x, mode, false, e2);
            } else {
              const uint64_t p2 = power_of_two(elem, static_cast<int32_t>(scale)), p2inv = power_of_two(elem, -static_cast<int32_t>(scale));
              Env e3 = e2;
              const uint64_t scaled = mul(fmt, x, p2, e3);
              e2.daz = false;      // (x was flushed above; what follows works on the intermediate values)
              Env e4 = e2;
              const uint64_t r2 = round_to_integral(fmt, scaled, mode, false, e4);
              rounded = mul(fmt, r2, p2inv, e4);
            }
            Env e5 = le;
            e5.flags = 0;
            e5.mode = mode;      // (the subtraction rounds the way the reduction does)
            e5.ftz = false;
            r = softfloat::sub(fmt, x, rounded, e5);
            if (env.ftz && ((r >> f.mant_bits) & f.exp_max) == 0 && (r & ((uint64_t{1} << (f.mant_bits + f.exp_bits)) - 1)) != 0) {      // (a tiny difference is flushed: inexact, but no underflow flag)
              r &= uint64_t{1} << (f.mant_bits + f.exp_bits);
              e5.flags |= kInexact;
            }
            if (!(imm & 8) && !x_zero && e2.flags == 0 && false) {}
            le.flags |= e5.flags;
            if (imm & 8) le.flags &= ~kInexact;
            break;
          }
          case kRange: {
            const uint32_t op = imm & 3, sign_ctl = (imm >> 2) & 3;
            const bool y_negative = (y >> (f.mant_bits + f.exp_bits)) & 1;
            const uint64_t ye = (y >> f.mant_bits) & f.exp_max, ym = y & f.mant_mask;
            const bool y_nan = ye == f.exp_max && ym, y_snan = y_nan && !(y & qb);
            if (x_snan || y_snan) {      // a signaling NaN wins (quieted); a quiet NaN only if the other is one too
              le.flags |= kInvalid;
              r = (x_snan ? x : y) | qb;
              break;
            }
            if (x_nan && y_nan) {
              r = x;
              break;
            }
            const bool one_nan = x_nan || y_nan;
            const bool abs_op = op >= 2;
            const uint64_t ax = x & ~(uint64_t{1} << (f.mant_bits + f.exp_bits)), ay = y & ~(uint64_t{1} << (f.mant_bits + f.exp_bits));
            const uint64_t cx = abs_op ? ax : x, cy = abs_op ? ay : y;
            Env e2 = le;
            e2.flags = 0;
            const Order order = compare(fmt, cx, cy, false, e2);
            bool pick_x;
            const bool is_min = (op & 1) == 0;
            if (order == Order::Equal) {
              // equal magnitudes: for min the negative one, for max the positive one (for the signed forms this also separates -0 and +0)
              pick_x = is_min ? negative : !negative;
              if (abs_op) pick_x = is_min ? negative : !negative;
              if (!abs_op && x == y) pick_x = true;
            } else {
              pick_x = is_min ? order == Order::Less : order == Order::Greater;
            }
            if (one_nan) pick_x = !x_nan;      // a NaN loses to a number
            const uint64_t chosen = pick_x ? x : y;
            const bool chosen_negative = (chosen >> (f.mant_bits + f.exp_bits)) & 1;
            uint64_t mag = chosen & ~(uint64_t{1} << (f.mant_bits + f.exp_bits));
            bool out_negative;
            switch (sign_ctl) {
              case 0: out_negative = negative; break;
              case 1: out_negative = abs_op ? chosen_negative : chosen_negative; break;
              case 2: out_negative = false; break;
              default: out_negative = true; break;
            }
            r = mag | (uint64_t{out_negative} << (f.mant_bits + f.exp_bits));
            break;
          }
          default: {      // kFixup: x is the value (vvvv), y the table entry, the old destination the "keep" answer
            const uint64_t table = y;
            uint32_t token;
            if (x_nan) token = (x & qb) ? 0 : 1;
            else if (x_zero) token = 2;
            else if (x == build(elem, false, static_cast<uint64_t>(f.bias), 0)) token = 3;
            else if (x_inf) token = negative ? 4 : 5;
            else token = negative ? 6 : 7;
            const uint32_t response = static_cast<uint32_t>((table >> (4 * token)) & 15);
            const uint64_t keep = element(ev_.old_dest, elem, i);
            const uint64_t max_float = build(elem, false, f.exp_max - 1, f.mant_mask);
            switch (response) {
              case 0: r = keep; break;
              case 1: r = x; break;
              case 2: r = x | qb | (f.exp_max << f.mant_bits); break;
              case 3: r = default_nan(elem); break;
              case 4: r = build(elem, true, f.exp_max, 0); break;
              case 5: r = build(elem, false, f.exp_max, 0); break;
              case 6: r = build(elem, negative, f.exp_max, 0); break;
              case 7: r = build(elem, true, 0, 0); break;
              case 8: r = 0; break;
              case 9: r = build(elem, true, static_cast<uint64_t>(f.bias), 0); break;
              case 10: r = build(elem, false, static_cast<uint64_t>(f.bias), 0); break;
              case 11: r = build(elem, false, static_cast<uint64_t>(f.bias) - 1, 0); break;
              case 12: r = elem == 64 ? 0x4056800000000000ull : 0x42B40000u; break;
              case 13: r = elem == 64 ? 0x3FF921FB54442D18ull : 0x3FC90FDBu; break;
              case 14: r = max_float; break;
              default: r = max_float | (uint64_t{1} << (f.mant_bits + f.exp_bits)); break;
            }
            // exception reporting is selected by the immediate, per token class
            switch (token) {
              case 2: if (imm & 1) le.flags |= kDivideByZero; if (imm & 2) le.flags |= kInvalid; break;
              case 3: if (imm & 4) le.flags |= kDivideByZero; if (imm & 8) le.flags |= kInvalid; break;
              case 1: if (imm & 16) le.flags |= kInvalid; break;
              case 4: if (imm & 32) le.flags |= kInvalid; break;
              case 6: if (imm & 64) le.flags |= kInvalid; break;
              case 5: if (imm & 128) le.flags |= kInvalid; break;
              default: break;
            }
            break;
          }
        }
        fold(le);
        set_element(result, elem, i, r);
      }
      if (def->kind == kFpClass) {
        ev_write_k(class_bits, count);
        return commit();
      }
      set_vec(reg, result, vl);
      return commit();
    }
    default: return fail(6);
  }
}

}  // namespace emulation
