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

// Advanced SIMD "two-register miscellaneous", vector and scalar, with the half-precision variants: byte and element reversal, pairwise widening adds, saturating accumulates, counts, compares with
// zero, ABS / NEG and their saturating forms, the narrowing and widening moves, the floating-point conversions (FCVTN, FCVTL, FCVTXN, BFCVTN, FCVT* to integer, SCVTF, UCVTF), FRINT*, the
// reciprocal and reciprocal square root estimates (FP and integer), FSQRT, FABS, FNEG and the compares with zero.
#include "Emulation/AArch64NeonCommon.hpp"

namespace emulation {

using namespace neon;
using namespace softfloat;

bool AArch64Cpu::neon_two_register(uint32_t insn, Step* result) {
  const bool vec_i = (insn & 0x9F3E0C00u) == 0x0E200800u, sc_i = (insn & 0xDF3E0C00u) == 0x5E200800u;
  const bool vec_h = (insn & 0x9F7E0C00u) == 0x0E780800u, sc_h = (insn & 0xDF7E0C00u) == 0x5E780800u;
  if (!vec_i && !sc_i && !vec_h && !sc_h) return false;
  const bool scalar = sc_i || sc_h, half = vec_h || sc_h;
  const bool q = !scalar && ((insn >> 30) & 1), u = (insn >> 29) & 1;
  const uint32_t size = bits_of(insn, 23, 22), opcode = bits_of(insn, 16, 12), rd = insn & 31, rn = (insn >> 5) & 31;
  const bool a_bit = (insn >> 23) & 1, sz = (insn >> 22) & 1;
  const Vec a = s_.v[rn], old = s_.v[rd];
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

  // ---- the floating-point operations (also the half-precision class, whose operation is in the same opcode field) ----
  const bool fp_class = half || opcode >= 0x18 || opcode == 0x16 || opcode == 0x17 || (opcode >= 0x0C && opcode <= 0x0F && size >= 2);
  if (fp_class) {
    Format f;
    uint32_t w;
    if (half) { f = Format::F16; w = 16; }
    else if (opcode == 0x16 || opcode == 0x17) { f = sz ? Format::F64 : Format::F32; w = 0; }
    else { f = sz ? Format::F64 : Format::F32; w = sz ? 64 : 32; }
    // conversions between precisions
    if (!half && (opcode == 0x16 || opcode == 0x17)) {
      if (opcode == 0x17) {      // FCVTL
        if (scalar || u) return bad();
        const uint32_t src = sz ? 32 : 16, dst = sz ? 64 : 32;
        const Format from = sz ? Format::F32 : Format::F16, to = sz ? Format::F64 : Format::F32;
        const Vec source = q ? Vec{a.hi, 0} : Vec{a.lo, 0};
        for (uint32_t i = 0; i < 64 / src; ++i) set_lane(&r, dst, i, fp_default_nan_fix(to, convert(from, to, lane(source, src, i), env)));
        return finish(r);
      }
      // FCVTN (U = 0, size 0x), FCVTXN (U = 1, size 0x), BFCVTN (U = 0, size 10)
      const bool bfloat = !u && size == 2;
      if (scalar && !(u && !a_bit)) return bad();      // only FCVTXN has a scalar form
      if (a_bit && !bfloat) return bad();
      if (size == 3) return bad();
      if (bfloat && scalar) return bad();
      const uint32_t dst = bfloat ? 16 : (sz ? 32 : 16), src = bfloat ? 32 : (sz ? 64 : 32);
      const Format from = bfloat ? Format::F32 : (sz ? Format::F64 : Format::F32), to = bfloat ? Format::BF16 : (sz ? Format::F32 : Format::F16);
      Vec narrowed;
      const uint32_t n = scalar ? 1 : 64 / dst;
      for (uint32_t i = 0; i < n; ++i) {
        uint64_t out;
        if (u) {      // FCVTXN: round to odd
          Env odd = env;
          odd.mode = Rounding::TowardZero;
          odd.flags = 0;
          out = convert(from, to, lane(a, src, i), odd);
          if ((odd.flags & kInexact) && !is_nan(to, out) && (out & 0x7FFFFFFFu) != 0x7F800000u) out |= 1;
          env.flags |= odd.flags;
        } else {
          out = convert(from, to, lane(a, src, i), env);
        }
        set_lane(&narrowed, dst, i, fp_default_nan_fix(to, out));
      }
      if (scalar || !q) { r.lo = narrowed.lo; r.hi = 0; }
      else { r.lo = old.lo; r.hi = narrowed.lo; }
      return finish(r);
    }
    if (!half && w == 0) return bad();
    if (!scalar && !half && sz && !q) return bad();
    // opcode groups that exist only with the "a" bit set or clear
    uint32_t op = opcode;
    const uint32_t n = scalar ? 1 : (q ? 128 : 64) / w;
    // validity of the combination (u, a, opcode)
    bool valid = true;
    switch (op) {
      case 0x0C: case 0x0D: valid = a_bit; break;           // FCMGT / FCMGE / FCMEQ / FCMLE with zero
      case 0x0E: valid = a_bit && !u; break;                // FCMLT with zero
      case 0x0F: valid = a_bit; break;                      // FABS, FNEG
      case 0x18: case 0x19: valid = !(u && a_bit && op == 0x18) && !(scalar); break;      // FRINTN / FRINTP / FRINTA, FRINTM / FRINTZ / FRINTX / FRINTI
      case 0x1A: case 0x1B: case 0x1C: case 0x1D: break;
      case 0x1E: valid = !half && !scalar; break;           // FRINT32Z .. FRINT64X
      case 0x1F: valid = a_bit && (u ? !scalar : true); break;      // FSQRT (U = 1), FRECPX (scalar, U = 0)
      default: valid = false; break;
    }
    if (!valid) return bad();
    if (op == 0x1F && !u) {      // FRECPX (scalar)
      if (!scalar) return bad();
      const FpShape s = shape_of(f);
      const uint64_t x = lane(a, w, 0);
      uint64_t out;
      if (is_nan(f, x)) { if (!((x >> (s.frac_bits - 1)) & 1)) env.flags |= kInvalid; out = x | (uint64_t{1} << (s.frac_bits - 1)); }
      else out = (x & (uint64_t{1} << (s.exp_bits + s.frac_bits))) | ((((x >> s.frac_bits) & ones(s.exp_bits)) == 0 ? ones(s.exp_bits) - 1 : (~(x >> s.frac_bits) & ones(s.exp_bits))) << s.frac_bits);
      set_lane(&r, w, 0, fp_default_nan_fix(f, out) & ones(w));
      return finish(r);
    }
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t x = lane(a, w, i);
      uint64_t out = 0;
      bool nan_fix = true;
      auto compare_zero = [&](bool signaling, bool less, bool equal, bool greater) {
        const Order o = compare(f, x, 0, signaling, env);
        const bool t = (o == Order::Less && less) || (o == Order::Equal && equal) || (o == Order::Greater && greater);
        nan_fix = false;
        return t ? ones(w) : uint64_t{0};
      };
      switch (op) {
        case 0x0C: out = u ? compare_zero(true, false, true, true) : compare_zero(true, false, false, true); break;      // FCMGE0 / FCMGT0
        case 0x0D: out = u ? compare_zero(true, true, true, false) : compare_zero(false, false, true, false); break;      // FCMLE0 / FCMEQ0
        case 0x0E: out = compare_zero(true, true, false, false); break;      // FCMLT0
        case 0x0F: out = u ? negate(f, x) : absolute(f, x); nan_fix = false; break;
        case 0x18: case 0x19: {
          // (U, a) -> FRINTN (0,0), FRINTP (0,1), FRINTA (1,0) | FRINTM (0,0), FRINTZ (0,1), FRINTX (1,0), FRINTI (1,1)
          Rounding mode = Rounding::NearestEven;
          bool exact = false;
          if (op == 0x18) mode = !u ? (a_bit ? Rounding::Up : Rounding::NearestEven) : Rounding::TieAway;
          else {
            if (!u) mode = a_bit ? Rounding::TowardZero : Rounding::Down;
            else { mode = env.mode; exact = !a_bit; }
          }
          out = round_to_integral(f, x, mode, exact, env);
          break;
        }
        case 0x1A: case 0x1B: case 0x1C: {
          // FCVTNS/PS/NU/PU (0x1A), FCVTMS/ZS/MU/ZU (0x1B), FCVTAS/AU (0x1C, a = 0) and URECPE / URSQRTE (0x1C, a = 1: integer)
          if (op == 0x1C && a_bit) {      // URECPE, URSQRTE
            if (half || sz || scalar) return bad();
            const uint32_t v = static_cast<uint32_t>(x);
            uint32_t res;
            if (!u) res = (v >> 31) ? (recip_estimate(v >> 23) & 0x1FF) << 23 : 0xFFFFFFFFu;
            else res = (v >> 30) ? (recip_sqrt_estimate(v >> 23) & 0x1FF) << 23 : 0xFFFFFFFFu;
            out = res;
            nan_fix = false;
            break;
          }
          Rounding mode;
          if (op == 0x1A) mode = a_bit ? Rounding::Up : Rounding::NearestEven;
          else if (op == 0x1B) mode = a_bit ? Rounding::TowardZero : Rounding::Down;
          else mode = Rounding::TieAway;
          out = to_integer_saturating(f, x, mode, w, u, 0, env);
          nan_fix = false;
          break;
        }
        case 0x1D: {
          if (!a_bit) {      // SCVTF, UCVTF
            bool negative = false;
            uint64_t magnitude = x & ones(w);
            if (!u) {
              const int64_t sv = sext(x, w);
              negative = sv < 0;
              magnitude = negative ? ~static_cast<uint64_t>(sv) + 1 : static_cast<uint64_t>(sv);
            }
            out = from_integer(f, magnitude, negative, 0, env);
            nan_fix = false;
          } else if (!u) {      // FRECPE
            out = fp_recip_estimate(f, x, env, (s_.fpcr >> 24) & 1);
          } else {              // FRSQRTE
            out = fp_recip_sqrt_estimate(f, x, env, fp_default_nan(f));
          }
          break;
        }
        case 0x1E: {
          const uint32_t width = (a_bit ? 64 : 32);
          if (f != Format::F32 && f != Format::F64) return bad();
          out = frint_to_width(f, x, width, u ? env.mode : Rounding::TowardZero, u, env);
          nan_fix = false;
          break;
        }
        default:      // 0x1F: FSQRT
          out = sqrt(f, x, env);
          break;
      }
      set_lane(&r, w, i, (nan_fix ? fp_default_nan_fix(f, out) : out) & ones(w));
    }
    return finish(r);
  }

  // ---- integer operations ----
  const uint32_t w = 8u << size;
  const uint32_t datasize = q ? 128 : 64;
  switch (opcode) {
    case 0x00: case 0x01: {      // REV64 / REV32 (U = 1) and REV16
      if (scalar) return bad();
      const uint32_t container = opcode == 0x01 ? 16 : (u ? 32 : 64);
      if (opcode == 0x01 && u) return bad();
      if (w >= container) return bad();
      const uint32_t per = container / w, n = datasize / w;
      for (uint32_t i = 0; i < n; ++i) set_lane(&r, w, (i / per) * per + (per - 1 - i % per), lane(a, w, i));
      return finish(r);
    }
    case 0x02: case 0x06: {      // SADDLP / UADDLP, SADALP / UADALP
      if (scalar || size == 3) return bad();
      const uint32_t n = datasize / (2 * w);
      for (uint32_t i = 0; i < n; ++i) {
        const I128 sum = u ? unsigned_of(lane(a, w, 2 * i), w) + unsigned_of(lane(a, w, 2 * i + 1), w) : signed_of(lane(a, w, 2 * i), w) + signed_of(lane(a, w, 2 * i + 1), w);
        uint64_t out = static_cast<uint64_t>(sum);
        if (opcode == 0x06) out += lane(old, 2 * w, i);
        set_lane(&r, 2 * w, i, out & ones(2 * w));
      }
      return finish(r);
    }
    case 0x03: {      // SUQADD / USQADD
      if (!scalar && size == 3 && !q) return bad();
      const uint32_t n = scalar ? 1 : datasize / w;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, w, i), o = lane(old, w, i);
        set_lane(&r, w, i, !u ? saturate_to_signed(signed_of(o, w) + unsigned_of(x, w), w, &sat) : saturate_to_unsigned(unsigned_of(o, w) + signed_of(x, w), w, &sat));
      }
      return finish(r);
    }
    case 0x04: {      // CLS / CLZ
      if (scalar || size == 3) return bad();
      const uint32_t n = datasize / w;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, w, i);
        uint32_t count = 0;
        if (u) {
          for (int b = static_cast<int>(w) - 1; b >= 0 && !((x >> b) & 1); --b) ++count;
        } else {
          const uint64_t top = (x >> (w - 1)) & 1;
          for (int b = static_cast<int>(w) - 2; b >= 0 && ((x >> b) & 1) == top; --b) ++count;
        }
        set_lane(&r, w, i, count);
      }
      return finish(r);
    }
    case 0x05: {      // CNT, NOT, RBIT
      if (scalar) return bad();
      if (!u && size != 0) return bad();
      if (u && size > 1) return bad();
      const uint32_t n = datasize / 8;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, 8, i);
        uint64_t out;
        if (!u) out = static_cast<uint64_t>(__builtin_popcountll(x));
        else if (size == 0) out = ~x & 0xFF;
        else { out = 0; for (uint32_t b = 0; b < 8; ++b) out |= ((x >> b) & 1) << (7 - b); }
        set_lane(&r, 8, i, out);
      }
      return finish(r);
    }
    case 0x07: {      // SQABS / SQNEG
      if (!scalar && size == 3 && !q) return bad();
      const uint32_t n = scalar ? 1 : datasize / w;
      for (uint32_t i = 0; i < n; ++i) {
        const I128 v = signed_of(lane(a, w, i), w);
        set_lane(&r, w, i, saturate_to_signed(u ? -v : (v < 0 ? -v : v), w, &sat));
      }
      return finish(r);
    }
    case 0x08: case 0x09: case 0x0A: {      // CMGT / CMGE, CMEQ / CMLE, CMLT with zero
      if (scalar && size != 3) return bad();
      if (!scalar && size == 3 && !q) return bad();
      if (opcode == 0x0A && u) return bad();
      const uint32_t n = scalar ? 1 : datasize / w;
      for (uint32_t i = 0; i < n; ++i) {
        const int64_t v = sext(lane(a, w, i), w);
        bool t;
        if (opcode == 0x08) t = u ? v >= 0 : v > 0;
        else if (opcode == 0x09) t = u ? v <= 0 : v == 0;
        else t = v < 0;
        set_lane(&r, w, i, t ? ones(w) : 0);
      }
      return finish(r);
    }
    case 0x0B: {      // ABS / NEG
      if (scalar && size != 3) return bad();
      if (!scalar && size == 3 && !q) return bad();
      const uint32_t n = scalar ? 1 : datasize / w;
      for (uint32_t i = 0; i < n; ++i) {
        const int64_t v = sext(lane(a, w, i), w);
        const uint64_t out = u ? static_cast<uint64_t>(0) - static_cast<uint64_t>(v) : static_cast<uint64_t>(v < 0 ? -v : v);
        set_lane(&r, w, i, out & ones(w));
      }
      return finish(r);
    }
    case 0x12: case 0x14: {      // XTN / SQXTUN and SQXTN / UQXTN
      if (size == 3) return bad();
      if (scalar && opcode == 0x12 && !u) return bad();      // there is no scalar XTN
      Vec narrowed;
      const uint32_t n = scalar ? 1 : 64 / w;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, 2 * w, i);
        uint64_t out;
        if (opcode == 0x12 && !u) out = x & ones(w);
        else if (opcode == 0x12) out = saturate_to_unsigned(signed_of(x, 2 * w), w, &sat);
        else if (!u) out = saturate_to_signed(signed_of(x, 2 * w), w, &sat);
        else out = saturate_to_unsigned(unsigned_of(x, 2 * w), w, &sat);
        set_lane(&narrowed, w, i, out);
      }
      if (scalar || !q) { r.lo = narrowed.lo; r.hi = 0; }
      else { r.lo = old.lo; r.hi = narrowed.lo; }
      return finish(r);
    }
    case 0x13: {      // SHLL, SHLL2: shift left by the element size, widening
      if (scalar || !u || size == 3) return bad();
      const uint32_t n = 64 / w;
      const Vec source = q ? Vec{a.hi, 0} : Vec{a.lo, 0};
      for (uint32_t i = 0; i < n; ++i) set_lane(&r, 2 * w, i, (lane(source, w, i) << w) & ones(2 * w));
      return finish(r);
    }
    default:
      return bad();
  }
}

}  // namespace emulation
