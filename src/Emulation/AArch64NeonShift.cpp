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

// Advanced SIMD "shift by immediate", vector and scalar: the plain, accumulating and rounding right shifts, SRI / SHL / SLI, the saturating left shifts, the narrowing shifts (plain, rounding,
// saturating and signed-to-unsigned), the widening shifts (SSHLL / USHLL) and the fixed-point conversions (SCVTF, UCVTF, FCVTZS, FCVTZU with a fractional-bits operand).
#include "Emulation/AArch64NeonCommon.hpp"

namespace emulation {

using namespace neon;
using namespace softfloat;

bool AArch64Cpu::neon_shift_immediate(uint32_t insn, Step* result) {
  const bool scalar = (insn & 0xDF800400u) == 0x5F000400u, vector = (insn & 0x9F800400u) == 0x0F000400u;
  if (!scalar && !vector) return false;
  const uint32_t immh = bits_of(insn, 22, 19);
  if (immh == 0) return false;
  const bool q = vector && ((insn >> 30) & 1), u = (insn >> 29) & 1;
  const uint32_t immb = bits_of(insn, 18, 16), opc = bits_of(insn, 15, 11), rd = insn & 31, rn = (insn >> 5) & 31;
  const uint32_t immhb = (immh << 3) | immb;
  uint32_t hb = 3;
  while (!((immh >> hb) & 1)) --hb;
  const uint32_t esize = 8u << hb;
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

  switch (opc) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x08: case 0x0A: case 0x0C: case 0x0E: {
      if (scalar && esize != 64) {
        if (opc != 0x0C && opc != 0x0E) return bad();      // only the saturating left shifts have byte, half and single scalar forms
      }
      if (esize == 64 && !q && !scalar) return bad();
      const uint32_t n = scalar ? 1 : (q ? 128 : 64) / esize;
      const uint32_t right = 2 * esize - immhb, left = immhb - esize;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, esize, i), o = lane(old, esize, i);
        const I128 value = (u && opc != 0x0C) ? unsigned_of(x, esize) : signed_of(x, esize);      // (SQSHLU reads a signed value)
        uint64_t out = 0;
        switch (opc) {
          case 0x00: out = static_cast<uint64_t>(shift_right(value, right, false)); break;
          case 0x02: out = o + static_cast<uint64_t>(shift_right(value, right, false)); break;
          case 0x04: out = static_cast<uint64_t>(shift_right(value, right, true)); break;
          case 0x06: out = o + static_cast<uint64_t>(shift_right(value, right, true)); break;
          case 0x08: {      // SRI
            if (!u) return bad();
            const uint64_t keep = right >= esize ? 0 : (ones(esize) >> right);
            out = (o & ~keep) | ((x >> (right >= 64 ? 63 : right)) & keep);
            if (right >= esize) out = o;
            break;
          }
          case 0x0A:      // SHL, SLI
            if (!u) out = x << left;
            else {
              const uint64_t keep = (ones(esize) << left) & ones(esize);
              out = (o & ~keep) | ((x << left) & keep);
            }
            break;
          case 0x0C:      // SQSHLU
            if (!u) return bad();
            out = saturate_to_unsigned(value << left, esize, &sat);
            break;
          default:        // SQSHL, UQSHL
            out = u ? saturate_to_unsigned(value << left, esize, &sat) : saturate_to_signed(value << left, esize, &sat);
            break;
        }
        set_lane(&r, esize, i, out & ones(esize));
      }
      return finish(r);
    }
    case 0x10: case 0x11: case 0x12: case 0x13: {      // the narrowing shifts
      if (hb == 3 || hb == 0 || (immh == 1 && false)) {
        if (hb == 3) return bad();
      }
      const uint32_t dst = esize;      // (immh decodes the destination element size here: 0001 -> 8 bits)
      if (hb == 3) return bad();
      const uint32_t n = scalar ? 1 : 64 / dst;
      const uint32_t shift = 2 * dst - immhb;
      Vec narrowed;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, 2 * dst, i);
        const bool rounding = opc & 1;
        uint64_t out;
        if (opc <= 0x11 && !u) {      // SHRN, RSHRN
          out = static_cast<uint64_t>(shift_right(unsigned_of(x, 2 * dst), shift, rounding));
        } else if (opc <= 0x11) {     // SQSHRUN, SQRSHRUN: signed in, unsigned out
          out = saturate_to_unsigned(shift_right(signed_of(x, 2 * dst), shift, rounding), dst, &sat);
        } else if (!u) {              // SQSHRN, SQRSHRN
          out = saturate_to_signed(shift_right(signed_of(x, 2 * dst), shift, rounding), dst, &sat);
        } else {                      // UQSHRN, UQRSHRN
          out = saturate_to_unsigned(shift_right(unsigned_of(x, 2 * dst), shift, rounding), dst, &sat);
        }
        set_lane(&narrowed, dst, i, out & ones(dst));
      }
      if (scalar || !q) {
        r.lo = narrowed.lo;
        r.hi = 0;
      } else {
        r.lo = old.lo;
        r.hi = narrowed.lo;
      }
      return finish(r);
    }
    case 0x14: {      // SSHLL, USHLL (SXTL and UXTL are the shift-by-zero forms)
      if (scalar || hb == 3) return bad();
      const uint32_t n = 64 / esize, left = immhb - esize;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(q ? Vec{a.hi, 0} : Vec{a.lo, 0}, esize, i);
        const I128 value = u ? unsigned_of(x, esize) : signed_of(x, esize);
        set_lane(&r, 2 * esize, i, static_cast<uint64_t>(value << left) & ones(2 * esize));
      }
      return finish(r);
    }
    case 0x1C: case 0x1F: {      // SCVTF / UCVTF and FCVTZS / FCVTZU with fractional bits
      if (hb == 0) return bad();
      if (esize == 64 && !q && !scalar) return bad();
      const Format f = esize == 64 ? Format::F64 : esize == 32 ? Format::F32 : Format::F16;
      const uint32_t n = scalar ? 1 : (q ? 128 : 64) / esize, fbits = 2 * esize - immhb;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, esize, i);
        uint64_t out;
        if (opc == 0x1C) {
          bool negative = false;
          uint64_t magnitude = x & ones(esize);
          if (!u) {
            const int64_t sv = sext(x, esize);
            negative = sv < 0;
            magnitude = negative ? ~static_cast<uint64_t>(sv) + 1 : static_cast<uint64_t>(sv);
          }
          out = from_integer(f, magnitude, negative, fbits, env);
        } else {
          out = to_integer_saturating(f, x & ones(esize), Rounding::TowardZero, esize, u, fbits, env);
        }
        set_lane(&r, esize, i, out & ones(esize));
      }
      return finish(r);
    }
    default:
      return bad();
  }
}

}  // namespace emulation
