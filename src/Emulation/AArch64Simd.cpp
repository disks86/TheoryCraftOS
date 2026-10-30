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
#include "Emulation/Wide.hpp"

namespace emulation {

using namespace softfloat;

namespace {

using Vec = AArch64Cpu::Vec;

constexpr uint32_t bits_of(uint32_t insn, uint32_t hi, uint32_t lo) { return (insn >> lo) & ((1u << (hi - lo + 1)) - 1); }
constexpr uint64_t ones(uint32_t n) { return n >= 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1; }
constexpr int64_t sext(uint64_t v, uint32_t bits) { return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }

uint64_t gl(const Vec& v, uint32_t w, uint32_t i) { return lane_get(v.lo, v.hi, w, i); }
void sl(Vec* v, uint32_t w, uint32_t i, uint64_t x) { lane_set(&v->lo, &v->hi, w, i, x); }
Vec half_of(const Vec& v, bool upper) {      // one 64-bit half as a vector
  Vec r;
  r.lo = upper ? v.hi : v.lo;
  return r;
}

uint32_t popcount8(uint64_t v) {
  uint32_t n = 0;
  for (v &= 0xFF; v; v &= v - 1) ++n;
  return n;
}

uint64_t reverse_bits(uint64_t v, uint32_t w) {
  uint64_t r = 0;
  for (uint32_t i = 0; i < w; ++i) r |= ((v >> i) & 1) << (w - 1 - i);
  return r;
}

uint32_t leading_zeros(uint64_t v, uint32_t w) {
  uint32_t n = 0;
  for (int i = static_cast<int>(w) - 1; i >= 0 && !((v >> i) & 1); --i) ++n;
  return n;
}

// The saturated value of a signed or unsigned result in `w` bits.
uint64_t saturate_signed(__int128 v, uint32_t w) {
  const __int128 hi = (static_cast<__int128>(1) << (w - 1)) - 1, lo = -(static_cast<__int128>(1) << (w - 1));
  return static_cast<uint64_t>(v > hi ? hi : v < lo ? lo : v);
}
uint64_t saturate_unsigned(__int128 v, uint32_t w) {
  const __int128 hi = (static_cast<__int128>(1) << w) - 1;
  return static_cast<uint64_t>(v < 0 ? 0 : v > hi ? hi : v);
}

}  // namespace

AArch64Cpu::Step AArch64Cpu::simd(uint32_t insn) {
  {
    Step crypto_result;
    if (crypto(insn, &crypto_result)) return crypto_result;
    if (fp8_neon(insn, &crypto_result)) return crypto_result;
    if (neon_shift_immediate(insn, &crypto_result)) return crypto_result;
    if (neon_three_same(insn, &crypto_result)) return crypto_result;
    if (neon_by_element(insn, &crypto_result)) return crypto_result;
    if (neon_two_register(insn, &crypto_result)) return crypto_result;
    if (neon_three_different(insn, &crypto_result)) return crypto_result;
  }
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const bool q = (insn >> 30) & 1, u = (insn >> 29) & 1;
  const uint32_t size = bits_of(insn, 23, 22);
  const bool scalar = (insn >> 28) & 1 && ((insn >> 24) & 0x1F) == 0x1E;      // 01 U 11110: the scalar forms
  Env env = fp_env();
  const Vec a = s_.v[rn], b = s_.v[rm];
  Vec r;
  const uint32_t datasize = q ? 128 : 64;
  bool qc = false;      // an integer saturated: FPSR.QC
  auto finish = [&](const Vec& result, bool keep_upper_zero = true) {
    s_.v[rd] = result;
    if (qc) s_.fpsr |= 1u << 27;
    if (keep_upper_zero && !q && !scalar) s_.v[rd].hi = 0;
    fp_finish(env);
    return Step::Ok;
  };

  // --- copy: DUP, INS, UMOV, SMOV ---
  if ((insn & 0x9FE08400u) == 0x0E000400u || (insn & 0xDFE08400u) == 0x5E000400u) {
    const uint32_t imm5 = bits_of(insn, 20, 16), imm4 = bits_of(insn, 14, 11);
    if ((imm5 & 15) == 0) return undefined();
    uint32_t lg = 0;
    while (!((imm5 >> lg) & 1)) ++lg;
    const uint32_t w = 8u << lg, index = imm5 >> (lg + 1);
    const bool op = (insn >> 29) & 1;
    if (scalar) {      // DUP (element) to a scalar
      if (imm4 != 0 || op) return undefined();
      Vec v;
      v.lo = gl(a, w, index);
      s_.v[rd] = v;
      return Step::Ok;
    }
    if (op) {      // INS (element)
      if (!q) return undefined();
      Vec v = s_.v[rd];
      sl(&v, w, index, gl(a, w, imm4 >> lg));
      s_.v[rd] = v;
      return Step::Ok;
    }
    switch (imm4) {
      case 0: case 1: {      // DUP element / general
        if (lg == 3 && !q) return undefined();
        const uint64_t value = imm4 == 0 ? gl(a, w, index) : reg(rn, true);
        const uint32_t n = datasize / w;
        for (uint32_t i = 0; i < n; ++i) sl(&r, w, i, value);
        s_.v[rd] = r;
        return Step::Ok;
      }
      case 3: {      // INS (general)
        Vec v = s_.v[rd];
        sl(&v, w, index, reg(rn, true));
        s_.v[rd] = v;
        return Step::Ok;
      }
      case 5: case 7: {      // SMOV, UMOV
        if (imm4 == 7 ? (w == 64) != q : (w == 64 || (w == 32 && !q))) return undefined();
        uint64_t value = gl(a, w, index);
        if (imm4 == 5) value = static_cast<uint64_t>(sext(value, w));
        set_reg(rd, value, q);
        return Step::Ok;
      }
      default: return undefined();
    }
  }

  // --- modified immediate ---
  if ((insn & 0x9FF80400u) == 0x0F000400u) {
    const uint32_t cmode = bits_of(insn, 15, 12);
    const uint64_t imm8 = (uint64_t{bits_of(insn, 18, 16)} << 5) | bits_of(insn, 9, 5);
    if ((insn >> 11) & 1) {      // bit 11 (o2) is only set for FMOV (vector, half precision)
      if (cmode != 15 || u) return undefined();
      const uint64_t half = ((imm8 & 0x80) << 8) | ((imm8 & 0x40) ? 0x3000ull : 0x4000ull) | ((imm8 & 0x30) << 6) | ((imm8 & 0x0F) << 6);
      Vec v;
      v.lo = half * 0x0001000100010001ull;
      v.hi = q ? v.lo : 0;
      s_.v[rd] = v;
      return Step::Ok;
    }
    uint64_t imm;
    bool is_orr_bic = false;
    uint32_t w = 32;
    if (cmode < 8) {
      imm = imm8 << (8 * (cmode >> 1));
      is_orr_bic = cmode & 1;
    } else if (cmode < 12) {
      imm = imm8 << (8 * ((cmode >> 1) & 1));
      is_orr_bic = cmode & 1;
      w = 16;
    } else if (cmode < 14) {
      imm = (imm8 << (8 * ((cmode & 1) + 1))) | ones(8 * ((cmode & 1) + 1));
    } else if (cmode == 14) {
      if (!u) {
        imm = imm8 * 0x0101010101010101ull;
        w = 64;
      } else {
        imm = 0;
        for (uint32_t i = 0; i < 8; ++i)
          if ((imm8 >> i) & 1) imm |= uint64_t{0xFF} << (8 * i);
        w = 64;
      }
    } else {      // 15: FMOV
      if (!u) imm = ((imm8 & 0x80) << 24) | ((imm8 & 0x40) ? 0x3E000000ull : 0x40000000ull) | ((imm8 & 0x3F) << 19);
      else imm = ((imm8 & 0x80) << 56) | ((imm8 & 0x40) ? 0x3FC0000000000000ull : 0x4000000000000000ull) | ((imm8 & 0x3F) << 48), w = 64;
      if (u && !q) return undefined();
    }
    if (w == 32 || w == 16) {
      const uint64_t pattern = w == 32 ? imm | (imm << 32) : imm | (imm << 16) | (imm << 32) | (imm << 48);
      Vec v;
      v.lo = v.hi = pattern;
      Vec cur = s_.v[rd];
      if (is_orr_bic) {
        if (u) { v.lo = cur.lo & ~pattern; v.hi = cur.hi & ~pattern; }      // BIC
        else { v.lo = cur.lo | pattern; v.hi = cur.hi | pattern; }          // ORR
      } else if (u && cmode < 14) {      // MVNI
        v.lo = ~pattern; v.hi = ~pattern;
      }
      if (!q) v.hi = 0;
      s_.v[rd] = v;
      return Step::Ok;
    }
    Vec v;
    v.lo = imm;
    v.hi = q ? imm : 0;
    if (cmode == 15 && !u && !q) v.hi = 0;
    if (cmode == 15 && !u) v.lo = imm | (imm << 32), v.hi = q ? v.lo : 0;
    s_.v[rd] = v;
    return Step::Ok;
  }

  // --- three registers, same type ---
  if ((insn & 0x9F200400u) == 0x0E200400u || (insn & 0xDF200400u) == 0x5E200400u) {
    const uint32_t opcode = bits_of(insn, 15, 11);
    const uint32_t w = 8u << size;
    uint32_t n = scalar ? 1 : datasize / w;
    if (opcode >= 0x18) {      // floating point
      const bool sz = size & 1, high = size >> 1;
      const Format f = sz ? Format::F64 : Format::F32;
      const uint32_t fw = sz ? 64 : 32;
      if (sz && !q && !scalar) return undefined();
      n = scalar ? 1 : datasize / fw;
      Vec v = s_.v[rd];
      const bool pairwise = u && (opcode == 0x18 || opcode == 0x1A || opcode == 0x1E) && !(opcode == 0x1A && high);
      for (uint32_t i = 0; i < n; ++i) {
        uint64_t x, y;
        if (pairwise && !scalar) {
          const uint32_t half_n = n / 2;
          const Vec& src = i < half_n ? a : b;
          const uint32_t j = (i < half_n ? i : i - half_n) * 2;
          x = gl(src, fw, j);
          y = gl(src, fw, j + 1);
        } else {
          x = gl(a, fw, i);
          y = gl(b, fw, i);
        }
        uint64_t res = 0;
        const uint64_t all = ones(fw);
        const uint32_t key = (u ? 0x20u : 0) | opcode;
        switch (key) {
          case 0x18: res = arm_minmax(f, x, y, !high, true, env); break;      // FMAXNM, FMINNM
          case 0x38: res = arm_minmax(f, x, y, !high, true, env); break;      // pairwise
          case 0x19: {      // FMLA, FMLS
            const uint64_t acc = gl(v, fw, i);
            res = fused_multiply_add(f, high ? negate(f, x) : x, y, acc, env);
            break;
          }
          case 0x1A: res = high ? softfloat::sub(f, x, y, env) : add(f, x, y, env); break;
          case 0x3A:
            if (high) {      // FABD
              res = absolute(f, softfloat::sub(f, x, y, env));
            } else {
              res = add(f, x, y, env);      // FADDP
            }
            break;
          case 0x1B: res = mul(f, x, y, env); break;      // FMULX (the 0 * inf case is not special-cased)
          case 0x3B: res = mul(f, x, y, env); break;      // FMUL
          case 0x1C: res = compare(f, x, y, false, env) == Order::Equal ? all : 0; break;      // FCMEQ
          case 0x3C: {      // FCMGE, FCMGT
            const Order o = compare(f, x, y, true, env);
            res = (high ? o == Order::Greater : (o == Order::Greater || o == Order::Equal)) ? all : 0;
            break;
          }
          case 0x3D: {      // FACGE, FACGT
            const Order o = compare(f, absolute(f, x), absolute(f, y), true, env);
            res = (high ? o == Order::Greater : (o == Order::Greater || o == Order::Equal)) ? all : 0;
            break;
          }
          case 0x1E: res = arm_minmax(f, x, y, !high, false, env); break;
          case 0x3E: res = arm_minmax(f, x, y, !high, false, env); break;
          case 0x3F: res = softfloat::div(f, x, y, env); break;      // FDIV
          default: return undefined();
        }
        sl(&r, fw, i, res);
        (void)pairwise;
      }
      if (scalar) {
        Vec out;
        out.lo = r.lo & ones(fw);
        return finish(out);
      }
      return finish(r);
    }
    if (scalar && w != 64 && opcode != 0x16) {
      if (opcode != 0x01 && opcode != 0x05) return undefined();
    }
    Vec acc = s_.v[rd];
    switch (opcode) {
      case 0x00: case 0x02: case 0x04:      // SHADD/UHADD, SRHADD/URHADD, SHSUB/UHSUB
        for (uint32_t i = 0; i < n; ++i) {
          const __int128 x = u ? static_cast<__int128>(gl(a, w, i)) : sext(gl(a, w, i), w);
          const __int128 y = u ? static_cast<__int128>(gl(b, w, i)) : sext(gl(b, w, i), w);
          __int128 t = opcode == 0x04 ? x - y : x + y + (opcode == 0x02 ? 1 : 0);
          sl(&r, w, i, static_cast<uint64_t>(t >> 1));
        }
        return finish(r);
      case 0x01: case 0x05:      // SQADD/UQADD, SQSUB/UQSUB
        for (uint32_t i = 0; i < n; ++i) {
          const __int128 x = u ? static_cast<__int128>(gl(a, w, i)) : sext(gl(a, w, i), w);
          const __int128 y = u ? static_cast<__int128>(gl(b, w, i)) : sext(gl(b, w, i), w);
          const __int128 t = opcode == 0x01 ? x + y : x - y;
          const uint64_t saturated = u ? saturate_unsigned(t, w) : saturate_signed(t, w);
          if (static_cast<__int128>(u ? saturated : static_cast<uint64_t>(sext(saturated, w))) != (u ? t : t) && (u ? static_cast<__int128>(saturated) != t : sext(saturated, w) != t)) qc = true;
          sl(&r, w, i, saturated);
        }
        return finish(r);
      case 0x03: {      // logical operations
        const uint32_t kind = (u ? 4u : 0) | size;
        for (uint32_t k = 0; k < 2; ++k) {
          const uint64_t x = k ? a.hi : a.lo, y = k ? b.hi : b.lo, d = k ? acc.hi : acc.lo;
          uint64_t res;
          switch (kind) {
            case 0: res = x & y; break;
            case 1: res = x & ~y; break;
            case 2: res = x | y; break;
            case 3: res = x | ~y; break;
            case 4: res = x ^ y; break;
            case 5: res = (d & x) | (~d & y); break;      // BSL
            case 6: res = (d & ~y) | (x & y); break;      // BIT: insert x where y is set
            default: res = (d & y) | (x & ~y); break;     // BIF: insert x where y is clear
          }
          (k ? r.hi : r.lo) = res;
        }
        if (kind == 5) { r.lo = (acc.lo & a.lo) | (~acc.lo & b.lo); r.hi = (acc.hi & a.hi) | (~acc.hi & b.hi); }
        if (kind == 6) { r.lo = (acc.lo & ~b.lo) | (a.lo & b.lo); r.hi = (acc.hi & ~b.hi) | (a.hi & b.hi); }
        if (kind == 7) { r.lo = (acc.lo & b.lo) | (a.lo & ~b.lo); r.hi = (acc.hi & b.hi) | (a.hi & ~b.hi); }
        return finish(r);
      }
      case 0x06: case 0x07:      // CMGT/CMHI, CMGE/CMHS
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t x = gl(a, w, i), y = gl(b, w, i);
          bool t;
          if (u) t = opcode == 0x06 ? x > y : x >= y;
          else t = opcode == 0x06 ? sext(x, w) > sext(y, w) : sext(x, w) >= sext(y, w);
          sl(&r, w, i, t ? ones(w) : 0);
        }
        return finish(r);
      case 0x08:      // SSHL, USHL
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t x = gl(a, w, i);
          const int64_t amount = sext(gl(b, w, i) & 0xFF, 8);
          uint64_t res;
          if (amount >= 0) res = amount >= static_cast<int64_t>(w) ? 0 : x << amount;
          else if (u) res = -amount >= static_cast<int64_t>(w) ? 0 : x >> -amount;
          else res = static_cast<uint64_t>(sext(x, w) >> (-amount >= static_cast<int64_t>(w) ? w - 1 : -amount));
          sl(&r, w, i, res);
        }
        return finish(r);
      case 0x0C: case 0x0D:      // SMAX/UMAX, SMIN/UMIN
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t x = gl(a, w, i), y = gl(b, w, i);
          const bool x_greater = u ? x > y : sext(x, w) > sext(y, w);
          sl(&r, w, i, (opcode == 0x0C) == x_greater ? x : y);
        }
        return finish(r);
      case 0x0E: case 0x0F:      // SABD/UABD, SABA/UABA
        for (uint32_t i = 0; i < n; ++i) {
          const __int128 x = u ? static_cast<__int128>(gl(a, w, i)) : sext(gl(a, w, i), w);
          const __int128 y = u ? static_cast<__int128>(gl(b, w, i)) : sext(gl(b, w, i), w);
          const __int128 d = x > y ? x - y : y - x;
          sl(&r, w, i, static_cast<uint64_t>(d) + (opcode == 0x0F ? gl(acc, w, i) : 0));
        }
        return finish(r);
      case 0x10:      // ADD, SUB
        for (uint32_t i = 0; i < n; ++i) sl(&r, w, i, u ? gl(a, w, i) - gl(b, w, i) : gl(a, w, i) + gl(b, w, i));
        return finish(r);
      case 0x11:      // CMTST, CMEQ
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t x = gl(a, w, i), y = gl(b, w, i);
          sl(&r, w, i, (u ? x == y : (x & y) != 0) ? ones(w) : 0);
        }
        return finish(r);
      case 0x12:      // MLA, MLS
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t p = gl(a, w, i) * gl(b, w, i);
          sl(&r, w, i, u ? gl(acc, w, i) - p : gl(acc, w, i) + p);
        }
        return finish(r);
      case 0x13:      // MUL (PMUL is not implemented)
        if (u) return undefined();
        for (uint32_t i = 0; i < n; ++i) sl(&r, w, i, gl(a, w, i) * gl(b, w, i));
        return finish(r);
      case 0x14: case 0x15: case 0x17: {      // SMAXP/UMAXP, SMINP/UMINP, ADDP
        if (opcode == 0x17 && u) return undefined();
        const uint32_t half_n = n / 2;
        for (uint32_t i = 0; i < n; ++i) {
          const Vec& src = i < half_n ? a : b;
          const uint32_t j = (i < half_n ? i : i - half_n) * 2;
          const uint64_t x = gl(src, w, j), y = gl(src, w, j + 1);
          uint64_t res;
          if (opcode == 0x17) res = x + y;
          else {
            const bool x_greater = u ? x > y : sext(x, w) > sext(y, w);
            res = (opcode == 0x14) == x_greater ? x : y;
          }
          sl(&r, w, i, res);
        }
        if (scalar) {      // ADDP (scalar) reads one vector, handled below
          return undefined();
        }
        return finish(r);
      }
      default:
        return undefined();
    }
  }

  // --- scalar pairwise: ADDP, FADDP, FMAXP... ---
  if ((insn & 0xDF3E0C00u) == 0x5E300800u) {
    const uint32_t opcode = bits_of(insn, 16, 12);
    Vec out;
    if (opcode == 0x1B && !u) {      // ADDP Dd, Vn.2D
      if (size != 3) return undefined();
      out.lo = a.lo + a.hi;
      s_.v[rd] = out;
      return Step::Ok;
    }
    if (u && (opcode == 0x0D || opcode == 0x0C || opcode == 0x0F)) {
      const bool sz = size & 1;
      const Format f = sz ? Format::F64 : Format::F32;
      const uint32_t fw = sz ? 64 : 32;
      const uint64_t x = gl(a, fw, 0), y = gl(a, fw, 1);
      uint64_t res;
      if (opcode == 0x0D) res = add(f, x, y, env);
      else if (opcode == 0x0C) res = arm_minmax(f, x, y, !(size >> 1), true, env);
      else res = arm_minmax(f, x, y, !(size >> 1), false, env);
      out.lo = res;
      return finish(out);
    }
    return undefined();
  }

  // --- three registers, different types ---
  if ((insn & 0x9F200C00u) == 0x0E200000u) {
    const uint32_t opcode = bits_of(insn, 15, 12);
    const uint32_t w = 8u << size;
    if (size == 3 || w > 32) return undefined();
    const uint32_t n = 64 / w;      // result lanes (twice as wide)
    const Vec an = half_of(a, q), bn = half_of(b, q);
    Vec acc = s_.v[rd];
    auto widen = [&](const Vec& v, uint32_t i) -> __int128 {
      const uint64_t x = gl(v, w, i);
      return u ? static_cast<__int128>(x) : static_cast<__int128>(sext(x, w));
    };
    auto wide_lane = [&](const Vec& v, uint32_t i) -> __int128 {
      const uint64_t x = gl(v, 2 * w, i);
      return u ? static_cast<__int128>(x) : static_cast<__int128>(sext(x, 2 * w));
    };
    for (uint32_t i = 0; i < n; ++i) {
      __int128 res;
      switch (opcode) {
        case 0: res = widen(an, i) + widen(bn, i); break;      // SADDL, UADDL
        case 1: res = wide_lane(a, i) + widen(bn, i); break;   // SADDW, UADDW
        case 2: res = widen(an, i) - widen(bn, i); break;
        case 3: res = wide_lane(a, i) - widen(bn, i); break;
        case 5: { const __int128 x = widen(an, i), y = widen(bn, i); res = (x > y ? x - y : y - x) + wide_lane(acc, i); break; }      // SABAL
        case 7: { const __int128 x = widen(an, i), y = widen(bn, i); res = x > y ? x - y : y - x; break; }                            // SABDL
        case 8: res = widen(an, i) * widen(bn, i) + wide_lane(acc, i); break;      // SMLAL
        case 10: res = wide_lane(acc, i) - widen(an, i) * widen(bn, i); break;     // SMLSL
        case 12: res = widen(an, i) * widen(bn, i); break;                         // SMULL
        default: return undefined();
      }
      sl(&r, 2 * w, i, static_cast<uint64_t>(res));
    }
    s_.v[rd] = r;
    return Step::Ok;
  }

  // --- two-register miscellaneous ---
  if ((insn & 0x9F3E0C00u) == 0x0E200800u || (insn & 0xDF3E0C00u) == 0x5E200800u) {
    const uint32_t opcode = bits_of(insn, 16, 12);
    const uint32_t w = opcode == 0x05 ? 8u : 8u << size;      // (CNT, NOT and RBIT always work on bytes: `size` only picks the instruction)
    const uint32_t n = scalar ? 1 : datasize / w;
    Vec acc = s_.v[rd];
    switch (opcode) {
      case 0x00: case 0x01: {      // REV64, REV32 (U), REV16
        const uint32_t group = opcode == 0x01 ? 16 : (u ? 32 : 64);
        if (w >= group) return undefined();
        const uint32_t per = group / w;
        for (uint32_t i = 0; i < n; ++i) sl(&r, w, (i / per) * per + (per - 1 - i % per), gl(a, w, i));
        return finish(r);
      }
      case 0x02: case 0x06: {      // SADDLP/UADDLP, SADALP/UADALP
        if (size == 3) return undefined();
        const uint32_t m = datasize / (2 * w);
        for (uint32_t i = 0; i < m; ++i) {
          const __int128 x = u ? static_cast<__int128>(gl(a, w, 2 * i)) : sext(gl(a, w, 2 * i), w);
          const __int128 y = u ? static_cast<__int128>(gl(a, w, 2 * i + 1)) : sext(gl(a, w, 2 * i + 1), w);
          sl(&r, 2 * w, i, static_cast<uint64_t>(x + y) + (opcode == 0x06 ? gl(acc, 2 * w, i) : 0));
        }
        return finish(r);
      }
      case 0x04:      // CLS, CLZ
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t x = gl(a, w, i);
          if (u) sl(&r, w, i, leading_zeros(x, w));
          else {
            const bool top = (x >> (w - 1)) & 1;
            uint32_t c = 0;
            for (int k = static_cast<int>(w) - 2; k >= 0 && (((x >> k) & 1) == (top ? 1u : 0u)); --k) ++c;
            sl(&r, w, i, c);
          }
        }
        return finish(r);
      case 0x05:      // CNT, NOT, RBIT
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t x = gl(a, w, i);
          if (!u) { if (size != 0) return undefined(); sl(&r, 8, i, popcount8(x)); }
          else if (size == 0) sl(&r, 8, i, ~x);
          else if (size == 1) sl(&r, 8, i, reverse_bits(x, 8));
          else return undefined();
        }
        return finish(r);
      case 0x08: case 0x09: case 0x0A:      // compare with zero: CMGT/CMGE, CMEQ/CMLE, CMLT
        for (uint32_t i = 0; i < n; ++i) {
          const int64_t x = sext(gl(a, w, i), w);
          bool t;
          if (opcode == 0x08) t = u ? x >= 0 : x > 0;
          else if (opcode == 0x09) t = u ? x <= 0 : x == 0;
          else { if (u) return undefined(); t = x < 0; }
          sl(&r, w, i, t ? ones(w) : 0);
        }
        return finish(r);
      case 0x0B:      // ABS, NEG
        for (uint32_t i = 0; i < n; ++i) {
          const int64_t x = sext(gl(a, w, i), w);
          sl(&r, w, i, u ? static_cast<uint64_t>(-x) : static_cast<uint64_t>(x < 0 ? -x : x));
        }
        return finish(r);
      case 0x12:      // XTN, XTN2 (SQXTUN is not implemented)
        if (u || size == 3) return undefined();
        {
          const Vec dst = s_.v[rd];
          for (uint32_t i = 0; i < 64 / w; ++i) sl(&r, w, i, gl(a, 2 * w, i));
          if (q) { Vec v = dst; v.hi = r.lo; s_.v[rd] = v; }
          else { r.hi = 0; s_.v[rd] = r; }
          return Step::Ok;
        }
      case 0x13: {      // SHLL, SHLL2
        if (!u || size == 3) return undefined();
        const Vec src = half_of(a, q);
        for (uint32_t i = 0; i < 64 / w; ++i) sl(&r, 2 * w, i, gl(src, w, i) << w);
        s_.v[rd] = r;
        return Step::Ok;
      }
      default:
        break;
    }
    // floating point forms (size bit 0 picks single or double; bit 1 picks the variant)
    const bool sz = size & 1, variant = size >> 1;
    const Format f = sz ? Format::F64 : Format::F32;
    const uint32_t fw = sz ? 64 : 32;
    const uint32_t fn = scalar ? 1 : datasize / fw;
    if (sz && !q && !scalar && opcode != 0x16 && opcode != 0x17) return undefined();
    switch (opcode) {
      case 0x16: case 0x17: {      // FCVTN(2) (double -> single), FCVTL(2) (single -> double)
        if (opcode == 0x16) {
          if (!sz) return undefined();
          for (uint32_t i = 0; i < 2; ++i) sl(&r, 32, i, convert(Format::F64, Format::F32, gl(a, 64, i), env));
          if (q) { Vec v = s_.v[rd]; v.hi = r.lo; r = v; } else r.hi = 0;
          s_.v[rd] = r;
          fp_finish(env);
          return Step::Ok;
        }
        if (!sz) return undefined();
        const Vec src = half_of(a, q);
        for (uint32_t i = 0; i < 2; ++i) sl(&r, 64, i, convert(Format::F32, Format::F64, gl(src, 32, i), env));
        s_.v[rd] = r;
        fp_finish(env);
        return Step::Ok;
      }
      default: break;
    }
    for (uint32_t i = 0; i < fn; ++i) {
      const uint64_t x = gl(a, fw, i);
      uint64_t res;
      switch (opcode) {
        case 0x0C: case 0x0D: case 0x0E: {      // FCMGT/FCMGE, FCMEQ/FCMLE, FCMLT with zero
          if (!variant) return undefined();
          const Order o = compare(f, x, 0, !(opcode == 0x0D && !u), env);      // (FCMEQ is a quiet comparison)
          bool t;
          if (opcode == 0x0C) t = u ? (o == Order::Greater || o == Order::Equal) : o == Order::Greater;
          else if (opcode == 0x0D) t = u ? (o == Order::Less || o == Order::Equal) : o == Order::Equal;
          else t = o == Order::Less;
          res = t ? ones(fw) : 0;
          break;
        }
        case 0x0F: if (!variant) return undefined(); res = u ? negate(f, x) : absolute(f, x); break;      // FABS, FNEG
        case 0x1F: if (!variant || !u) return undefined(); res = sqrt(f, x, env); break;      // FSQRT
        case 0x1D:      // SCVTF, UCVTF
          if (variant) return undefined();
          {
            const bool negative = !u && sext(x, fw) < 0;
            const uint64_t mag = u ? x : (negative ? static_cast<uint64_t>(-sext(x, fw)) : x);
            res = from_integer(f, mag, negative, 0, env);
          }
          break;
        case 0x1A: case 0x1B: {      // FCVTNS/NU or FCVTPS/PU (A); FCVTMS/MU or FCVTZS/ZU (B)
          const Rounding mode = opcode == 0x1A ? (variant ? Rounding::Up : Rounding::NearestEven) : (variant ? Rounding::TowardZero : Rounding::Down);
          res = to_integer_saturating(f, x, mode, fw, u, 0, env);
          break;
        }
        case 0x1C: if (variant) return undefined(); res = to_integer_saturating(f, x, Rounding::TieAway, fw, u, 0, env); break;      // FCVTAS, FCVTAU
        case 0x18: case 0x19: {      // FRINTN/FRINTM... (variant 0), FRINTP/FRINTZ (variant 1)
          Rounding mode;
          bool exact = false;
          if (opcode == 0x18) mode = variant ? Rounding::Up : (u ? Rounding::TieAway : Rounding::NearestEven);
          else if (!variant) { mode = Rounding::Down; if (u) { mode = env.mode; exact = true; } }
          else { mode = Rounding::TowardZero; if (u) { mode = env.mode; exact = false; } }
          res = round_to_integral(f, x, mode, exact, env);
          break;
        }
        default: return undefined();
      }
      sl(&r, fw, i, res);
    }
    if (scalar) { Vec out; out.lo = r.lo & ones(fw); return finish(out); }
    return finish(r);
  }

  // --- across lanes ---
  if ((insn & 0x9F3E0C00u) == 0x0E300800u) {
    const uint32_t opcode = bits_of(insn, 16, 12);
    const uint32_t w = 8u << size;
    if (size == 3) return undefined();
    const uint32_t n = datasize / w;
    if (w == 32 && !q) return undefined();
    __int128 acc = 0;
    uint64_t res;
    switch (opcode) {
      case 0x03:      // SADDLV, UADDLV
        for (uint32_t i = 0; i < n; ++i) acc += u ? static_cast<__int128>(gl(a, w, i)) : sext(gl(a, w, i), w);
        res = static_cast<uint64_t>(acc);
        r.lo = res & ones(2 * w);
        s_.v[rd] = r;
        return Step::Ok;
      case 0x1B:      // ADDV
        if (u) return undefined();
        for (uint32_t i = 0; i < n; ++i) acc += gl(a, w, i);
        r.lo = static_cast<uint64_t>(acc) & ones(w);
        s_.v[rd] = r;
        return Step::Ok;
      case 0x0A: case 0x1A: {      // SMAXV/UMAXV, SMINV/UMINV
        uint64_t best = gl(a, w, 0);
        for (uint32_t i = 1; i < n; ++i) {
          const uint64_t x = gl(a, w, i);
          const bool greater = u ? x > best : sext(x, w) > sext(best, w);
          const bool less = u ? x < best : sext(x, w) < sext(best, w);
          if (opcode == 0x0A ? greater : less) best = x;
        }
        r.lo = best;
        s_.v[rd] = r;
        return Step::Ok;
      }
      default: return undefined();
    }
  }

  // --- permute: UZP, TRN, ZIP ---
  if ((insn & 0xBF208C00u) == 0x0E000800u) {
    const uint32_t opcode = bits_of(insn, 14, 12), w = 8u << size, n = datasize / w;
    if (w == 64 && !q) return undefined();
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t value;
      switch (opcode) {
        case 1: case 5: {      // UZP1, UZP2
          const uint32_t j = 2 * (i % (n / 2)) + (opcode == 5 ? 1 : 0);
          value = i < n / 2 ? gl(a, w, j) : gl(b, w, j);
          break;
        }
        case 2: case 6: {      // TRN1, TRN2
          const uint32_t j = (i & ~1u) + (opcode == 6 ? 1 : 0);
          value = (i & 1) ? gl(b, w, j) : gl(a, w, j);
          break;
        }
        case 3: case 7: {      // ZIP1, ZIP2
          const uint32_t j = (opcode == 7 ? n / 2 : 0) + i / 2;
          value = (i & 1) ? gl(b, w, j) : gl(a, w, j);
          break;
        }
        default: return undefined();
      }
      sl(&r, w, i, value);
    }
    return finish(r);
  }

  // --- EXT ---
  if ((insn & 0xBF208400u) == 0x2E000000u) {
    const uint32_t pos = bits_of(insn, 14, 11);
    if (!q && pos >= 8) return undefined();
    const uint32_t bytes = datasize / 8;
    for (uint32_t i = 0; i < bytes; ++i) {
      const uint32_t j = i + pos;
      sl(&r, 8, i, j < bytes ? gl(a, 8, j) : gl(b, 8, j - bytes));
    }
    return finish(r);
  }

  // --- TBL, TBX ---
  if ((insn & 0xBF208C00u) == 0x0E000000u) {
    const uint32_t len = bits_of(insn, 14, 13) + 1;
    const bool extension = (insn >> 12) & 1;
    const uint32_t bytes = datasize / 8;
    Vec dst = s_.v[rd];
    for (uint32_t i = 0; i < bytes; ++i) {
      const uint64_t index = gl(b, 8, i);
      uint64_t value = extension ? gl(dst, 8, i) : 0;
      if (index < 16 * len) value = gl(s_.v[(rn + index / 16) & 31], 8, static_cast<uint32_t>(index % 16));
      sl(&r, 8, i, value);
    }
    return finish(r);
  }

  // --- shift by immediate ---
  if ((insn & 0x9F800400u) == 0x0F000400u || (insn & 0xDF800400u) == 0x5F000400u) {
    const uint32_t immh = bits_of(insn, 22, 19), immb = bits_of(insn, 18, 16), opcode = bits_of(insn, 15, 11);
    if (immh == 0) return undefined();
    const uint32_t hb = (immh << 3) | immb;
    const uint32_t lg = immh >= 8 ? 3 : immh >= 4 ? 2 : immh >= 2 ? 1 : 0;
    const uint32_t w = 8u << lg;
    const uint32_t right = 2 * w - hb, left = hb - w;
    const uint32_t n = scalar ? 1 : datasize / w;
    Vec acc = s_.v[rd];
    switch (opcode) {
      case 0x00: case 0x02: case 0x08: case 0x0A: {      // SSHR/USHR, SSRA/USRA, SRI, SHL/SLI
        if (w == 64 && !q && !scalar) return undefined();
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t x = gl(a, w, i);
          uint64_t res;
          if (opcode == 0x0A) {
            res = x << left;
            if (u) { const uint64_t keep = ones(left); res |= gl(acc, w, i) & keep; }      // SLI
          } else {
            const uint64_t shifted = u ? (right >= 64 ? 0 : x >> right) : static_cast<uint64_t>(sext(x, w) >> (right >= w ? w - 1 : right));
            if (opcode == 0x00) res = shifted;
            else if (opcode == 0x02) res = gl(acc, w, i) + shifted;
            else {      // SRI
              const uint64_t keep = right >= w ? ~uint64_t{0} : ~(ones(w) >> right);
              res = (gl(acc, w, i) & keep) | shifted;
            }
          }
          sl(&r, w, i, res);
        }
        if (scalar) { Vec out; out.lo = r.lo; s_.v[rd] = out; return Step::Ok; }
        return finish(r);
      }
      case 0x10: {      // SHRN, SHRN2 (U = 0)
        if (u || lg == 3) return undefined();
        const uint32_t dw = w;      // destination lane width is w; sources are 2w
        const uint32_t sr = 2 * w - hb;
        (void)dw;
        for (uint32_t i = 0; i < 64 / w; ++i) sl(&r, w, i, gl(a, 2 * w, i) >> sr);
        if (q) { Vec v = acc; v.hi = r.lo; s_.v[rd] = v; }
        else { r.hi = 0; s_.v[rd] = r; }
        return Step::Ok;
      }
      case 0x14: {      // SSHLL, USHLL (and the 2 forms)
        if (lg == 3) return undefined();
        const Vec src = half_of(a, q);
        const uint32_t sh = hb - w;
        for (uint32_t i = 0; i < 64 / w; ++i) {
          const uint64_t x = gl(src, w, i);
          sl(&r, 2 * w, i, u ? x << sh : static_cast<uint64_t>(sext(x, w)) << sh);
        }
        s_.v[rd] = r;
        return Step::Ok;
      }
      default:
        return undefined();
    }
  }

  // --- vector by indexed element ---
  if ((insn & 0x9F000400u) == 0x0F000000u || (insn & 0xDF000400u) == 0x5F000000u) {
    const uint32_t opcode = bits_of(insn, 15, 12);
    const uint32_t l = (insn >> 21) & 1, mbit = (insn >> 20) & 1, h = (insn >> 11) & 1;
    const bool fp_form = opcode == 0x1 || opcode == 0x5 || opcode == 0x9;
    if (fp_form && size >= 2) {
      const bool sz = size & 1;
      const Format f = sz ? Format::F64 : Format::F32;
      const uint32_t fw = sz ? 64 : 32;
      const uint32_t index = sz ? h : (h << 1) | l;
      const uint32_t m_reg = (mbit << 4) | rm;
      if (sz && (l || (!q && !scalar))) return undefined();
      const uint64_t y = gl(s_.v[m_reg], fw, index);
      const uint32_t n = scalar ? 1 : datasize / fw;
      Vec acc = s_.v[rd];
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = gl(a, fw, i);
        uint64_t res;
        if (opcode == 0x9) res = mul(f, x, y, env);
        else res = fused_multiply_add(f, opcode == 0x5 ? negate(f, x) : x, y, gl(acc, fw, i), env);
        sl(&r, fw, i, res);
      }
      if (scalar) { Vec out; out.lo = r.lo & ones(fw); return finish(out); }
      return finish(r);
    }
    if (size == 0 || size == 3) return undefined();
    const uint32_t w = 8u << size;
    uint32_t index, m_reg;
    if (size == 1) { index = (h << 2) | (l << 1) | mbit; m_reg = rm & 15; }
    else { index = (h << 1) | l; m_reg = (mbit << 4) | rm; }
    const uint64_t y = gl(s_.v[m_reg], w, index);
    Vec acc = s_.v[rd];
    if (opcode == 0x8 || opcode == 0x0 || opcode == 0x4) {      // MUL, MLA, MLS
      if ((opcode != 0x8 && !u) || (opcode == 0x8 && u)) return undefined();
      const uint32_t n = datasize / w;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t p = gl(a, w, i) * y;
        sl(&r, w, i, opcode == 0x8 ? p : opcode == 0x0 ? gl(acc, w, i) + p : gl(acc, w, i) - p);
      }
      return finish(r);
    }
    if (opcode == 0xA || opcode == 0x2 || opcode == 0x6) {      // SMULL/UMULL, SMLAL/UMLAL, SMLSL/UMLSL by element
      const Vec src = half_of(a, q);
      for (uint32_t i = 0; i < 64 / w; ++i) {
        const __int128 x = u ? static_cast<__int128>(gl(src, w, i)) : sext(gl(src, w, i), w);
        const __int128 yy = u ? static_cast<__int128>(y) : sext(y, w);
        const __int128 p = x * yy;
        const uint64_t cur = gl(acc, 2 * w, i);
        sl(&r, 2 * w, i, opcode == 0xA ? static_cast<uint64_t>(p) : opcode == 0x2 ? cur + static_cast<uint64_t>(p) : cur - static_cast<uint64_t>(p));
      }
      s_.v[rd] = r;
      return Step::Ok;
    }
    return undefined();
  }

  return undefined();
}

// --- Advanced SIMD structure loads and stores ----------------------------------------------------------------------------------------------

AArch64Cpu::Step AArch64Cpu::simd_load_store_multiple(GuestMemory& m, uint32_t insn) {
  const bool q = (insn >> 30) & 1, load_bit = (insn >> 22) & 1, post = (insn >> 23) & 1;
  const uint32_t opcode = bits_of(insn, 15, 12), size = bits_of(insn, 11, 10), rt = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  uint32_t regs, selem;
  switch (opcode) {
    case 0: regs = 4; selem = 4; break;
    case 2: regs = 4; selem = 1; break;
    case 4: regs = 3; selem = 3; break;
    case 6: regs = 3; selem = 1; break;
    case 7: regs = 1; selem = 1; break;
    case 8: regs = 2; selem = 2; break;
    case 10: regs = 2; selem = 1; break;
    default: return undefined();
  }
  if (size == 3 && !q && selem != 1) return undefined();
  const uint32_t ebytes = 1u << size, datasize = q ? 16 : 8, per_reg = datasize / ebytes;
  const uint64_t base = reg_sp(rn, true);
  for (uint32_t k = 0; k < regs; ++k) {
    Vec v = load_bit ? Vec() : s_.v[(rt + k) & 31];
    for (uint32_t e = 0; e < per_reg; ++e) {
      const uint64_t address = selem == 1 ? base + k * datasize + e * ebytes : base + (e * selem + k) * ebytes;
      if (load_bit) {
        uint64_t x;
        if (!load(m, address, ebytes, &x)) return Step::Fault;
        sl(&v, ebytes * 8, e, x);
      } else if (!store(m, address, ebytes, gl(v, ebytes * 8, e))) {
        return Step::Fault;
      }
    }
    if (load_bit) s_.v[(rt + k) & 31] = v;
  }
  if (post) set_reg_sp(rn, base + (rm == 31 ? regs * datasize : reg(rm, true)), true);
  return Step::Ok;
}

AArch64Cpu::Step AArch64Cpu::simd_load_store_single(GuestMemory& m, uint32_t insn) {
  const bool q = (insn >> 30) & 1, load_bit = (insn >> 22) & 1, post = (insn >> 23) & 1;
  const uint32_t selem = ((((insn >> 13) & 1) << 1) | ((insn >> 21) & 1)) + 1;
  const uint32_t scale = bits_of(insn, 15, 14), s_bit = (insn >> 12) & 1, size = bits_of(insn, 11, 10);
  const uint32_t rt = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  uint32_t ebytes, index = 0;
  bool replicate = false;
  switch (scale) {
    case 0: ebytes = 1; index = (uint32_t{q} << 3) | (s_bit << 2) | size; break;
    case 1: if (size & 1) return undefined(); ebytes = 2; index = (uint32_t{q} << 2) | (s_bit << 1) | (size >> 1); break;
    case 2:
      if (size == 0) { ebytes = 4; index = (uint32_t{q} << 1) | s_bit; }
      else if (size == 1 && !s_bit) { ebytes = 8; index = q; }
      else return undefined();
      break;
    default:
      if (!load_bit || s_bit) return undefined();
      replicate = true;
      ebytes = 1u << size;
      break;
  }
  const uint64_t base = reg_sp(rn, true);
  for (uint32_t k = 0; k < selem; ++k) {
    const uint64_t address = base + k * ebytes;
    Vec v = s_.v[(rt + k) & 31];
    if (replicate) {
      uint64_t x;
      if (!load(m, address, ebytes, &x)) return Step::Fault;
      Vec out;
      for (uint32_t e = 0; e < (q ? 16u : 8u) / ebytes; ++e) sl(&out, ebytes * 8, e, x);
      s_.v[(rt + k) & 31] = out;
    } else if (load_bit) {
      uint64_t x;
      if (!load(m, address, ebytes, &x)) return Step::Fault;
      sl(&v, ebytes * 8, index, x);
      s_.v[(rt + k) & 31] = v;
    } else if (!store(m, address, ebytes, gl(v, ebytes * 8, index))) {
      return Step::Fault;
    }
  }
  if (post) set_reg_sp(rn, base + (rm == 31 ? selem * ebytes : reg(rm, true)), true);
  return Step::Ok;
}

}  // namespace emulation
