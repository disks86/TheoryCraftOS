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

// The parts of Advanced SIMD "three registers, different types" the main SIMD file lacks: the saturating doubling long multiplies (SQDMLAL, SQDMLSL, SQDMULL and their "2" forms, vector and scalar)
// and the narrowing high-half adds and subtracts (ADDHN, RADDHN, SUBHN, RSUBHN and their "2" forms).
#include "Emulation/AArch64NeonCommon.hpp"

namespace emulation {

using namespace neon;

bool AArch64Cpu::neon_three_different(uint32_t insn, Step* result) {
  const bool vector = (insn & 0x9F200C00u) == 0x0E200000u, scalar = (insn & 0xDF200C00u) == 0x5E200000u;
  if (!vector && !scalar) return false;
  const uint32_t opc = bits_of(insn, 15, 12);
  const bool narrowing_high = opc == 0x4 || opc == 0x6, saturating_long = opc == 0x9 || opc == 0xB || opc == 0xD;
  if (!narrowing_high && !saturating_long) return false;
  const bool q = vector && ((insn >> 30) & 1), u = (insn >> 29) & 1;
  const uint32_t size = bits_of(insn, 23, 22), rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const Vec a = s_.v[rn], b = s_.v[rm], old = s_.v[rd];
  bool sat = false;
  Vec r;
  auto bad = [&]() { *result = undefined(); return true; };
  auto finish = [&](const Vec& v) {
    s_.v[rd] = v;
    if (sat) s_.fpsr |= 1u << 27;
    *result = Step::Ok;
    return true;
  };
  if (size == 3 || size == 0 && saturating_long) return bad();
  if (narrowing_high) {      // the source elements are 2w bits wide, the result w
    if (scalar) return bad();
    const uint32_t w = 8u << size, n = 64 / w;
    const bool subtract = opc == 0x6;
    Vec narrowed;
    for (uint32_t i = 0; i < n; ++i) {
      I128 sum = subtract ? static_cast<I128>(lane(a, 2 * w, i)) - lane(b, 2 * w, i) : static_cast<I128>(lane(a, 2 * w, i)) + lane(b, 2 * w, i);
      if (u) sum += static_cast<I128>(1) << (w - 1);
      set_lane(&narrowed, w, i, static_cast<uint64_t>(sum >> w) & ones(w));
    }
    if (!q) { r.lo = narrowed.lo; r.hi = 0; }
    else { r.lo = old.lo; r.hi = narrowed.lo; }
    return finish(r);
  }
  if (u) return bad();
  const uint32_t w = 8u << size, lw = 2 * w, n = scalar ? 1 : 64 / w;
  const Vec sa = scalar ? a : (q ? Vec{a.hi, 0} : Vec{a.lo, 0}), sb = scalar ? b : (q ? Vec{b.hi, 0} : Vec{b.lo, 0});
  for (uint32_t i = 0; i < n; ++i) {
    const I128 product = static_cast<I128>(sext(lane(sa, w, i), w) * sext(lane(sb, w, i), w)) * 2;
    const uint64_t doubled = saturate_to_signed(product, lw, &sat);
    uint64_t out = doubled;
    if (opc != 0xD) {
      const I128 o = signed_of(lane(old, lw, i), lw);
      out = saturate_to_signed(opc == 0x9 ? o + signed_of(doubled, lw) : o - signed_of(doubled, lw), lw, &sat);
    }
    set_lane(&r, lw, i, out);
  }
  return finish(r);
}

}  // namespace emulation
