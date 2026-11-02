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
#include "Emulation/RiscV64VectorUnit.hpp"
#include "Emulation/Wide.hpp"

// Zvbb, Zvkb and Zvbc: the vector bit-manipulation and carry-less multiplication instructions (vandn, vrol, vror, vwsll, vbrev, vbrev8, vrev8, vclz, vctz, vcpop, vclmul, vclmulh).
namespace emulation {

namespace {

constexpr int64_t sext(uint64_t v, uint32_t bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }

uint64_t rotate_in(uint64_t x, uint32_t amount, uint32_t bits) {
  amount %= bits;
  const uint64_t mask = bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
  return amount == 0 ? x : ((x << amount) | (x >> (bits - amount))) & mask;
}

uint64_t reverse_bits(uint64_t x, uint32_t bits) {
  uint64_t r = 0;
  for (uint32_t i = 0; i < bits; ++i) r |= ((x >> i) & 1) << (bits - 1 - i);
  return r;
}

}  // namespace

// Returns true (and the result in *step) when the instruction is one of them.
bool VectorUnit::bit_instruction(Step* step) {
  const bool opi = funct3 == 0 || funct3 == 3 || funct3 == 4, opm = funct3 == 2 || funct3 == 6;
  const bool vv = funct3 == 0 || funct3 == 2, vi = funct3 == 3;
  const uint32_t bits = sew;
  const uint64_t mask = sew_mask();
  const uint64_t scalar = vv ? 0 : vi ? ((funct6 & 1 ? 32u : 0u) | vs1) : s.x[vs1];
  if (opi) {
    switch (funct6) {
      case 1:      // vandn.vv / vandn.vx
        if (vi) return false;
        *step = binary_loop(*this, vv, s.x[vs1] & mask, [](uint64_t a, uint64_t b, uint64_t) { return a & ~b; });
        return true;
      case 20: case 21:      // vror (vv, vx, vi; the immediate has six bits, the top one in the opcode) and vrol (vv, vx)
        if (funct6 == 21 && vi) {      // (vror.vi uses both funct6 values)
          *step = binary_loop(*this, false, (uint64_t{1} << 5 | vs1) % bits, [bits](uint64_t a, uint64_t b, uint64_t) { return rotate_in(a, static_cast<uint32_t>(bits - b), bits); });
          return true;
        }
        if (vi) {
          *step = binary_loop(*this, false, vs1 % bits, [bits](uint64_t a, uint64_t b, uint64_t) { return rotate_in(a, static_cast<uint32_t>(bits - b), bits); });
          return true;
        }
        if (funct6 == 21) *step = binary_loop(*this, vv, s.x[vs1], [bits](uint64_t a, uint64_t b, uint64_t) { return rotate_in(a, static_cast<uint32_t>(b % bits), bits); });
        else *step = binary_loop(*this, vv, s.x[vs1], [bits](uint64_t a, uint64_t b, uint64_t) { return rotate_in(a, static_cast<uint32_t>(bits - b % bits), bits); });
        return true;
      case 53: {      // vwsll: the destination is twice as wide
        if (sew > 32 || lmul_log > 2) { *step = undefined(); return true; }
        const uint32_t wide_bytes = sew_bytes * 2, wide_bits = sew * 2;
        const uint32_t regs = regs_of(lmul_log), wide_regs = regs_of(lmul_log + 1);
        if (!group_ok(vd, wide_regs) || !group_ok(vs2, regs) || (vv && !group_ok(vs1, regs)) || (!vm && vd == 0)) { *step = undefined(); return true; }
        if (vd + wide_regs > vs2 && vd < vs2 + regs && !(vd + wide_regs - 1 == vs2 + regs - 1 && wide_regs > 1)) { *step = undefined(); return true; }      // (the source group may overlap only the top of the destination)
        if (s.vstart >= vl) { *step = ok(); return true; }
        const uint64_t wide_mask = wide_bits == 64 ? ~uint64_t{0} : (uint64_t{1} << wide_bits) - 1;
        for (uint64_t i = s.vstart; i < vl; ++i) {
          if (!active(i)) { if (ma) ones(vd, wide_bytes, i, i + 1); continue; }
          const uint64_t amount = (vv ? get(vs1, sew_bytes, i) : vi ? vs1 : s.x[vs1]) & (wide_bits - 1);
          put(vd, wide_bytes, i, (get(vs2, sew_bytes, i) << amount) & wide_mask);
        }
        tail(vd, wide_regs, wide_bytes, vl);
        *step = ok();
        return true;
      }
      default: return false;
    }
  }
  if (opm) {
    if (funct6 == 12 || funct6 == 13) {      // vclmul, vclmulh: 64-bit elements only
      if (sew != 64) { *step = undefined(); return true; }
      const bool high = funct6 == 13;
      *step = binary_loop(*this, vv, s.x[vs1], [high](uint64_t a, uint64_t b, uint64_t) {
        uint64_t lo = 0, hi = 0;
        for (uint32_t i = 0; i < 64; ++i)
          if ((b >> i) & 1) {
            lo ^= a << i;
            if (i != 0) hi ^= a >> (64 - i);
          }
        return high ? hi : lo;
      });
      return true;
    }
    if (funct6 == 18 && funct3 == 2) {      // the unary instructions, selected by vs1
      uint32_t kind;
      switch (vs1) {
        case 8: case 9: case 10: case 12: case 13: case 14: kind = vs1; break;
        default: return false;
      }
      *step = binary_loop(*this, false, 0, [kind, bits](uint64_t a, uint64_t, uint64_t) -> uint64_t {
        switch (kind) {
          case 8: {      // vbrev8
            uint64_t r = 0;
            for (uint32_t i = 0; i < bits; i += 8) r |= reverse_bits((a >> i) & 0xFF, 8) << i;
            return r;
          }
          case 9: {      // vrev8
            uint64_t r = 0;
            for (uint32_t i = 0; i < bits; i += 8) r |= ((a >> i) & 0xFF) << (bits - 8 - i);
            return r;
          }
          case 10: return reverse_bits(a, bits);      // vbrev
          case 12: { uint32_t n = 0; while (n < bits && !((a >> (bits - 1 - n)) & 1)) ++n; return n; }      // vclz
          case 13: { uint32_t n = 0; while (n < bits && !((a >> n) & 1)) ++n; return n; }      // vctz
          default: { uint32_t n = 0; for (uint32_t i = 0; i < bits; ++i) n += (a >> i) & 1; return n; }      // vcpop
        }
      });
      return true;
    }
  }
  return false;
}

}  // namespace emulation
