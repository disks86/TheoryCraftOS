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

// Advanced SIMD "vector x indexed element", vector and scalar: MLA / MLS / MUL, the long multiplies (SMLAL, UMLSL, SMULL, SQDMLAL, SQDMULL, ...), SQDMULH / SQRDMULH / SQRDMLAH / SQRDMLSH, the
// floating-point FMLA / FMLS / FMUL / FMULX (half, single and double), the half-to-single FMLAL family, FCMLA, and the dot products (SDOT, UDOT, USDOT, SUDOT).
#include "Emulation/AArch64NeonCommon.hpp"

namespace emulation {

using namespace neon;
using namespace softfloat;

namespace {
uint64_t two_of(Format f) { return f == Format::F64 ? 0x4000000000000000ull : f == Format::F32 ? 0x40000000u : 0x4000u; }
bool infinity_of(Format f, uint64_t a) {
  const uint64_t m = absolute(f, a);
  return f == Format::F64 ? m == 0x7FF0000000000000ull : f == Format::F32 ? m == 0x7F800000u : m == 0x7C00u;
}
}  // namespace

bool AArch64Cpu::neon_by_element(uint32_t insn, Step* result) {
  const bool scalar = (insn & 0xDF000400u) == 0x5F000000u, vector = (insn & 0x9F000400u) == 0x0F000000u;
  if (!scalar && !vector) return false;
  const bool q = vector && ((insn >> 30) & 1), u = (insn >> 29) & 1;
  const uint32_t size = bits_of(insn, 23, 22), l = (insn >> 21) & 1, m = (insn >> 20) & 1, h = (insn >> 11) & 1;
  const uint32_t opc = bits_of(insn, 15, 12), rd = insn & 31, rn = (insn >> 5) & 31, rm_low = bits_of(insn, 19, 16);
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
  // The indexed register and element for elements of w bits.
  struct Element { Vec reg; uint32_t index; };
  auto element = [&](uint32_t w) -> Element {
    if (w == 16) return {s_.v[rm_low], (h << 2) | (l << 1) | m};
    if (w == 32) return {s_.v[rm_low | (m << 4)], (h << 1) | l};
    return {s_.v[rm_low | (m << 4)], h};
  };

  // --- FCMLA by element: U = 1, opc = 0 rot 1 ---
  if (u && (opc & 0x9) == 0x1) {
    if (scalar || (size != 1 && size != 2)) return bad();
    const uint32_t w = 8u << size;
    const Format f = size == 1 ? Format::F16 : Format::F32;
    if (size == 2 && (l || !q)) return bad();
    const Vec b = s_.v[rm_low | (m << 4)];
    const uint32_t pair = size == 1 ? (h << 1) | l : h, rot = (opc >> 1) & 3;
    if (size == 1 && !q && pair > 1) return bad();
    const uint32_t pairs = (q ? 128 : 64) / (2 * w);
    const uint64_t re_b = lane(b, w, 2 * pair), im_b = lane(b, w, 2 * pair + 1);
    for (uint32_t p = 0; p < pairs; ++p) {
      const uint64_t re_a = lane(a, w, 2 * p), im_a = lane(a, w, 2 * p + 1);
      uint64_t e1, e2, e3, e4;
      switch (rot) {
        case 0: e1 = re_a; e2 = re_b; e3 = re_a; e4 = im_b; break;
        case 1: e1 = negate(f, im_a); e2 = im_b; e3 = im_a; e4 = re_b; break;
        case 2: e1 = negate(f, re_a); e2 = re_b; e3 = negate(f, re_a); e4 = im_b; break;
        default: e1 = im_a; e2 = im_b; e3 = negate(f, im_a); e4 = re_b; break;
      }
      set_lane(&r, w, 2 * p, fp_default_nan_fix(f, fused_multiply_add(f, e2, e1, lane(old, w, 2 * p), env)));
      set_lane(&r, w, 2 * p + 1, fp_default_nan_fix(f, fused_multiply_add(f, e4, e3, lane(old, w, 2 * p + 1), env)));
    }
    return finish(r);
  }

  // --- FMLA, FMLS, FMUL, FMULX: half (size 00), single (10), double (11) ---
  if (opc == 0x1 || opc == 0x5 || opc == 0x9) {
    if (size == 1 || (u && opc != 0x9)) return bad();
    const uint32_t w = size == 0 ? 16 : size == 2 ? 32 : 64;
    const Format f = size == 0 ? Format::F16 : size == 2 ? Format::F32 : Format::F64;
    if (w == 64 && (l || (!q && !scalar))) return bad();
    const Element e = element(w);
    const uint64_t y = lane(e.reg, w, e.index);
    const uint32_t n = scalar ? 1 : (q ? 128 : 64) / w;
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t x = lane(a, w, i), o = lane(old, w, i);
      uint64_t out;
      if (opc == 0x1) out = fused_multiply_add(f, x, y, o, env);
      else if (opc == 0x5) out = fused_multiply_add(f, negate(f, x), y, o, env);
      else if (!u) out = mul(f, x, y, env);
      else if (is_nan(f, x) || is_nan(f, y)) out = mul(f, x, y, env);
      else if ((infinity_of(f, x) && absolute(f, y) == 0) || (absolute(f, x) == 0 && infinity_of(f, y))) {
        const uint64_t sign_mask = w == 64 ? 0x8000000000000000ull : w == 32 ? 0x80000000u : 0x8000u;
        out = ((x ^ y) & sign_mask) ? negate(f, two_of(f)) : two_of(f);
      } else {
        out = mul(f, x, y, env);
      }
      set_lane(&r, w, i, fp_default_nan_fix(f, out) & ones(w));
    }
    return finish(r);
  }

  // --- FMLAL, FMLSL, FMLAL2, FMLSL2: half products accumulated into single lanes ---
  if (size == 2 && ((!u && (opc == 0x0 || opc == 0x4)) || (u && (opc == 0x8 || opc == 0xC)))) {
    if (scalar) return bad();
    const Element e = element(16);
    const uint32_t n = q ? 4 : 2, base = u ? (q ? 4 : 2) : 0;
    const bool negative = opc == 0x4 || opc == 0xC;
    const uint64_t y = convert(Format::F16, Format::F32, lane(e.reg, 16, e.index), env);
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t x = convert(Format::F16, Format::F32, lane(a, 16, base + i), env);
      if (negative) x = negate(Format::F32, x);
      set_lane(&r, 32, i, fp_default_nan_fix(Format::F32, fused_multiply_add(Format::F32, x, y, lane(old, 32, i), env)));
    }
    return finish(r);
  }

  // --- dot products: SDOT / UDOT (opc 1110, size 10), USDOT (U = 0, opc 1111, size 10), SUDOT (U = 0, opc 1111, size 00) ---
  if ((opc == 0xE && size == 2) || (opc == 0xF && !u && (size == 2 || size == 0))) {
    if (scalar) return bad();
    const uint32_t idx = (h << 1) | l;
    const Vec b = s_.v[rm_low | (m << 4)];
    const uint32_t n = q ? 4 : 2;
    for (uint32_t i = 0; i < n; ++i) {
      int64_t sum = static_cast<int64_t>(lane(old, 32, i));
      for (uint32_t k = 0; k < 4; ++k) {
        const uint64_t x = lane(a, 8, 4 * i + k), y = lane(b, 8, 4 * idx + k);
        if (opc == 0xE) sum += u ? static_cast<int64_t>(x * y) : sext(x, 8) * sext(y, 8);
        else if (size == 2) sum += static_cast<int64_t>(x) * sext(y, 8);
        else sum += sext(x, 8) * static_cast<int64_t>(y);
      }
      set_lane(&r, 32, i, static_cast<uint64_t>(sum) & 0xFFFFFFFFu);
    }
    return finish(r);
  }

  // --- integer: half and single elements only ---
  if (size != 1 && size != 2) return bad();
  const uint32_t w = 8u << size;
  const Element e = element(w);
  const uint64_t y = lane(e.reg, w, e.index);
  switch (opc) {
    case 0x0: case 0x4: case 0x8: {      // MLA (U = 1), MLS (U = 1), MUL (U = 0)
      if (scalar || (opc == 0x8 ? u : !u)) return bad();
      const uint32_t n = (q ? 128 : 64) / w;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(a, w, i), o = lane(old, w, i), p = x * y;
        set_lane(&r, w, i, (opc == 0x8 ? p : opc == 0x0 ? o + p : o - p) & ones(w));
      }
      return finish(r);
    }
    case 0x2: case 0x6: case 0xA: {      // SMLAL / UMLAL, SMLSL / UMLSL, SMULL / UMULL
      if (scalar) return bad();
      const uint32_t n = 64 / w, lw = 2 * w;
      const Vec source = q ? Vec{a.hi, 0} : Vec{a.lo, 0};
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = lane(source, w, i);
        const uint64_t p = u ? x * y : static_cast<uint64_t>(sext(x, w) * sext(y, w));
        const uint64_t o = lane(old, lw, i);
        set_lane(&r, lw, i, (opc == 0xA ? p : opc == 0x2 ? o + p : o - p) & ones(lw));
      }
      return finish(r);
    }
    case 0x3: case 0x7: case 0xB: {      // SQDMLAL, SQDMLSL, SQDMULL
      if (u) return bad();
      const uint32_t n = scalar ? 1 : 64 / w, lw = 2 * w;
      const Vec source = scalar ? a : (q ? Vec{a.hi, 0} : Vec{a.lo, 0});
      for (uint32_t i = 0; i < n; ++i) {
        const I128 product = static_cast<I128>(sext(lane(source, w, i), w) * sext(y, w)) * 2;
        const uint64_t doubled = saturate_to_signed(product, lw, &sat);
        uint64_t out = doubled;
        if (opc != 0xB) {
          const I128 o = signed_of(lane(old, lw, i), lw);
          out = saturate_to_signed(opc == 0x3 ? o + signed_of(doubled, lw) : o - signed_of(doubled, lw), lw, &sat);
        }
        set_lane(&r, lw, i, out);
      }
      return finish(r);
    }
    case 0xC: case 0xD: case 0xF: {      // SQDMULH (0xC), SQRDMULH (0xD) with U = 0; SQRDMLAH (0xD), SQRDMLSH (0xF) with U = 1
      if ((opc == 0xC || !u) && opc == 0xF) return bad();
      if (opc == 0xC && u) return bad();
      if (opc == 0xF && !u) return bad();
      const uint32_t n = scalar ? 1 : (q ? 128 : 64) / w;
      for (uint32_t i = 0; i < n; ++i) {
        const I128 product = static_cast<I128>(sext(lane(a, w, i), w) * sext(y, w)) * 2;
        I128 value;
        if (!u) value = (product + (opc == 0xD ? static_cast<I128>(1) << (w - 1) : 0)) >> w;
        else value = ((signed_of(lane(old, w, i), w) << w) + (opc == 0xD ? product : -product) + (static_cast<I128>(1) << (w - 1))) >> w;
        set_lane(&r, w, i, saturate_to_signed(value, w, &sat));
      }
      return finish(r);
    }
    default:
      return bad();
  }
}

}  // namespace emulation
