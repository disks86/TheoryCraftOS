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

// The AMD extensions of the VEX encoding: FMA4 (four-operand fused multiply-add, VEX map 3), XOP (the 8F prefix, maps 8, 9 and A: integer multiply-accumulate, vpcmov, vpperm, rotates, arithmetic and
// logical per-element shifts, vpcom, horizontal adds and subtracts, vfrcz) and TBM (the trailing bit manipulation instructions, XOP maps 9 and A).
// Not implemented: vpermil2ps / pd, and LWP (llwpcb, slwpcb, lwpins, lwpval).
#include "Emulation/SoftFloat.hpp"
#include "Emulation/Wide.hpp"
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

using namespace softfloat;

namespace {

typedef X86_64Cpu::Xmm Xmm;

uint64_t el(const Xmm& x, uint32_t w, uint32_t i) { return lane_get(x.lo, x.hi, w, i); }
void put_el(Xmm* x, uint32_t w, uint32_t i, uint64_t v) { lane_set(&x->lo, &x->hi, w, i, v); }
int64_t sx(uint64_t v, uint32_t bits) { return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
uint64_t mask_of_bits(uint32_t w) { return w >= 64 ? ~uint64_t{0} : (uint64_t{1} << w) - 1; }
int64_t clamp(__int128 v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : static_cast<int64_t>(v); }

uint64_t rotate_left(uint64_t v, uint32_t count, uint32_t w) {
  count %= w;
  if (count == 0) return v;
  return ((v << count) | (v >> (w - count))) & mask_of_bits(w);
}

uint8_t bit_reverse(uint8_t b) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i) r = static_cast<uint8_t>((r << 1) | ((b >> i) & 1));
  return r;
}

}  // namespace

// --- FMA4 ----------------------------------------------------------------------------------------------------------------------------------

// dest = a * b +/- c, where a is VEX.vvvv; with VEX.W clear b is the r/m operand and c the register in imm8[7:4], with VEX.W set they are the other way round.
X86_64Cpu::Step X86_64Cpu::fma4_instruction(GuestMemory& m, uint8_t op, bool wide_w, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm()) return bad_decode();
  const uint32_t imm = static_cast<uint32_t>(next_imm(1, false));
  if (!finish_decode()) return bad_decode();
  const bool is_double = op & 1;
  const bool addsub = op >= 0x5C && op <= 0x5F;
  const bool scalar = !addsub && (op & 2);
  const bool wide = vex_.wide && !scalar;
  const bool negate_product = op >= 0x78;
  const bool negate_addend = addsub ? op >= 0x5E : (op & 4);
  const Format f = is_double ? Format::F64 : Format::F32;
  const uint32_t w = is_double ? 64 : 32;
  Xmm rm[2];
  if (!read_y(m, scalar ? w / 8 : wide ? 32 : 16, rm)) return Step::Fault;
  const uint32_t r4 = (imm >> 4) & 15;
  const Xmm reg4[2] = {s_.xmm[r4], s_.ymm_high[r4]}, va[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv]};
  const Xmm* b = wide_w ? reg4 : rm;
  const Xmm* c = wide_w ? rm : reg4;
  Xmm out[2];
  Env env;
  env.mode = static_cast<Rounding>((s_.mxcsr >> 13) & 3);
  env.daz = (s_.mxcsr & 0x40) != 0;
  env.ftz = (s_.mxcsr & 0x8000) != 0;
  for (uint32_t h = 0; h < (wide ? 2u : 1u); ++h)
    for (uint32_t i = 0; i < (scalar ? 1u : 128 / w); ++i) {
      uint64_t p = el(va[h], w, i), q = el(b[h], w, i), r = el(c[h], w, i);
      const uint64_t vals[3] = {p, q, r}, quiet_bit = w == 64 ? uint64_t{1} << 51 : uint64_t{1} << 22;
      int pick = -1;
      bool any_signaling = false;
      for (int k = 0; k < 3; ++k) {
        if (!is_nan(f, vals[k])) continue;
        if (!(vals[k] & quiet_bit)) any_signaling = true;
        if (pick < 0) pick = k;
      }
      if (pick >= 0) {
        if (any_signaling) env.flags |= kInvalid;
        put_el(&out[h], w, i, vals[pick] | quiet_bit);
        continue;
      }
      bool neg_r = negate_addend;
      if (op == 0x5C || op == 0x5D) neg_r = (i & 1) == 0;      // vfmaddsub: subtract in the even elements
      if (op == 0x5E || op == 0x5F) neg_r = (i & 1) == 1;      // vfmsubadd: subtract in the odd ones
      if (negate_product) p = negate(f, p);
      if (neg_r) r = negate(f, r);
      put_el(&out[h], w, i, fused_multiply_add(f, p, q, r, env));
    }
  if (env.flags & kInvalid) env.flags &= ~kDenormal;
  s_.mxcsr |= env.flags;
  s_.xmm[d_.reg] = out[0];      // (a scalar result leaves the rest of the register zero)
  s_.ymm_high[d_.reg] = wide ? out[1] : Xmm();
  s_.rip += d_.position;
  return Step::Ok;
}

// --- XOP -----------------------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::xop_instruction(GuestMemory& m, uint32_t map, uint8_t op, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const bool l = vex_.wide, w1 = d_.rex_w;
  const uint32_t opsize = d_.rex_w ? 8 : 4;
  // TBM: general registers
  if ((map == 9 && (op == 0x01 || op == 0x02)) || (map == 10 && op == 0x10)) {
    if (!decode_modrm()) return bad_decode();
    uint64_t imm = 0;
    if (map == 10) imm = next_imm(4, false);
    if (!finish_decode()) return bad_decode();
    if (l) return fail(6);
    const uint64_t mask = opsize == 8 ? ~uint64_t{0} : 0xFFFFFFFFull;
    uint64_t x = 0;
    if (!read_rm(m, opsize, &x)) return Step::Fault;
    uint64_t result;
    bool carry;
    if (map == 10) {      // bextr r, r/m, imm32: start in bits 7:0, length in bits 15:8
      const uint32_t start = imm & 0xFF, length = (imm >> 8) & 0xFF;
      result = (start >= opsize * 8 ? 0 : x >> start) & (length >= opsize * 8 ? mask : (uint64_t{1} << length) - 1);
      set_reg(d_.reg, opsize, result);
      s_.rflags &= ~(kCarry | kOverflow | kZero | kSign | kParity | kAdjust);
      if (result == 0) s_.rflags |= kZero;
      s_.rip += d_.position;
      return Step::Ok;
    }
    const uint32_t kind = d_.reg & 7;
    const uint64_t inc = (x + 1) & mask, dec = (x - 1) & mask;
    const bool all_ones = x == mask, zero = x == 0;
    if (op == 0x01) {
      switch (kind) {
        case 1: result = x & inc; carry = all_ones; break;       // blcfill
        case 2: result = x | dec; carry = zero; break;           // blsfill
        case 3: result = x | inc; carry = all_ones; break;       // blcs
        case 4: result = ~x & dec; carry = zero; break;          // tzmsk
        case 5: result = ~x & inc; carry = all_ones; break;      // blcic
        case 6: result = ~x | dec; carry = zero; break;          // blsic
        case 7: result = ~x | inc; carry = all_ones; break;      // t1mskc
        default: return fail(6);
      }
    } else {
      switch (kind) {
        case 1: result = x ^ inc; carry = all_ones; break;       // blcmsk
        case 6: result = x | ~inc; carry = all_ones; break;      // blci
        default: return fail(6);
      }
    }
    result &= mask;
    set_reg(vvvv, opsize, result);
    s_.rflags &= ~(kCarry | kOverflow | kZero | kSign | kParity | kAdjust);
    if (carry) s_.rflags |= kCarry;
    if (result == 0) s_.rflags |= kZero;
    if (result >> (opsize * 8 - 1)) s_.rflags |= kSign;
    s_.rip += d_.position;
    return Step::Ok;
  }

  if (!decode_modrm()) return bad_decode();
  uint32_t imm = 0;
  if (map == 8) imm = static_cast<uint32_t>(next_imm(1, false));
  if (!finish_decode()) return bad_decode();
  const bool packed_ok = (map == 8 && op == 0xA2) || (map == 9 && (op == 0x80 || op == 0x81));      // (the instructions that have a 256-bit form)
  if (l && !packed_ok) return fail(6);
  const bool wide = l;
  const uint32_t reg = d_.reg;
  Xmm rm[2];
  const Xmm va[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv]};
  Xmm out[2];
  auto finish = [&](bool zero_upper = true) {
    s_.xmm[reg] = out[0];
    s_.ymm_high[reg] = wide ? out[1] : Xmm();
    (void)zero_upper;
    s_.rip += d_.position;
    return Step::Ok;
  };
  Env env;
  env.mode = static_cast<Rounding>((s_.mxcsr >> 13) & 3);
  env.daz = (s_.mxcsr & 0x40) != 0;
  env.ftz = (s_.mxcsr & 0x8000) != 0;

  if (map == 8) {
    const uint32_t r4 = (imm >> 4) & 15;
    const Xmm c4[2] = {s_.xmm[r4], s_.ymm_high[r4]};
    if (op >= 0x85 && op <= 0xB6 && op != 0xA2 && op != 0xA3) {      // integer multiply-accumulate: dest = vvvv * rm + imm8 register
      static const uint8_t kOps[] = {0x85, 0x86, 0x87, 0x8E, 0x8F, 0x95, 0x96, 0x97, 0x9E, 0x9F, 0xA6, 0xB6};
      bool known = false;
      for (uint8_t k : kOps) known |= k == op;
      if (!known || w1) return fail(6);
      if (!read_y(m, 16, rm)) return Step::Fault;
      const Xmm &a = va[0], &b = rm[0], &c = c4[0];
      const bool saturate = op == 0x85 || op == 0x86 || op == 0x87 || op == 0x8E || op == 0x8F || op == 0xA6;
      switch (op) {
        case 0x85: case 0x95:      // vpmacssww, vpmacsww
          for (uint32_t i = 0; i < 8; ++i) {
            const __int128 v = static_cast<__int128>(sx(el(a, 16, i), 16)) * sx(el(b, 16, i), 16) + sx(el(c, 16, i), 16);
            put_el(&out[0], 16, i, saturate ? static_cast<uint64_t>(clamp(v, -32768, 32767)) : static_cast<uint64_t>(v));
          }
          break;
        case 0x86: case 0x96:      // vpmacsswd, vpmacswd: the odd words, to doublewords
          for (uint32_t i = 0; i < 4; ++i) {
            const __int128 v = static_cast<__int128>(sx(el(a, 16, 2 * i + 1), 16)) * sx(el(b, 16, 2 * i + 1), 16) + sx(el(c, 32, i), 32);
            put_el(&out[0], 32, i, saturate ? static_cast<uint64_t>(clamp(v, INT32_MIN, INT32_MAX)) : static_cast<uint64_t>(v));
          }
          break;
        case 0x8E: case 0x9E:      // vpmacssdd, vpmacsdd
          for (uint32_t i = 0; i < 4; ++i) {
            const __int128 v = static_cast<__int128>(sx(el(a, 32, i), 32)) * sx(el(b, 32, i), 32) + sx(el(c, 32, i), 32);
            put_el(&out[0], 32, i, saturate ? static_cast<uint64_t>(clamp(v, INT32_MIN, INT32_MAX)) : static_cast<uint64_t>(v));
          }
          break;
        case 0x87: case 0x97: case 0x8F: case 0x9F: {      // vpmacssdql / vpmacsdql (even doublewords), vpmacssdqh / vpmacsdqh (odd ones), to quadwords
          const uint32_t base = (op & 0xF) == 0xF ? 1 : 0;
          for (uint32_t i = 0; i < 2; ++i) {
            const __int128 v = static_cast<__int128>(sx(el(a, 32, 2 * i + base), 32)) * sx(el(b, 32, 2 * i + base), 32) + sx(el(c, 64, i), 64);
            put_el(&out[0], 64, i, saturate ? static_cast<uint64_t>(clamp(v, INT64_MIN, INT64_MAX)) : static_cast<uint64_t>(v));
          }
          break;
        }
        default:      // vpmadcsswd, vpmadcswd: both words of each doubleword, summed
          for (uint32_t i = 0; i < 4; ++i) {
            const __int128 v = static_cast<__int128>(sx(el(a, 16, 2 * i), 16)) * sx(el(b, 16, 2 * i), 16) + static_cast<__int128>(sx(el(a, 16, 2 * i + 1), 16)) * sx(el(b, 16, 2 * i + 1), 16) + sx(el(c, 32, i), 32);
            put_el(&out[0], 32, i, saturate ? static_cast<uint64_t>(clamp(v, INT32_MIN, INT32_MAX)) : static_cast<uint64_t>(v));
          }
          break;
      }
      return finish();
    }
    if (op == 0xA2 || op == 0xA3) {      // vpcmov: (op2 & sel) | (op3 & ~sel); vpperm: bytes picked from op2:op3 by sel. VEX.W swaps the roles of the r/m operand and the register in imm8
      if (!read_y(m, wide ? 32 : 16, rm)) return Step::Fault;
      const Xmm* op3 = w1 ? c4 : rm;
      const Xmm* sel = w1 ? rm : c4;
      if (op == 0xA2) {
        for (uint32_t h = 0; h < (wide ? 2u : 1u); ++h) {
          out[h].lo = (va[h].lo & sel[h].lo) | (op3[h].lo & ~sel[h].lo);
          out[h].hi = (va[h].hi & sel[h].hi) | (op3[h].hi & ~sel[h].hi);
        }
        return finish();
      }
      if (wide) return fail(6);
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t s = static_cast<uint32_t>(el(sel[0], 8, i)), index = s & 15;
        const uint8_t byte = static_cast<uint8_t>(el((s & 16) ? op3[0] : va[0], 8, index));
        uint8_t r;
        switch (s >> 5) {
          case 0: r = byte; break;
          case 1: r = static_cast<uint8_t>(~byte); break;
          case 2: r = bit_reverse(byte); break;
          case 3: r = static_cast<uint8_t>(~bit_reverse(byte)); break;
          case 4: r = 0; break;
          case 5: r = 0xFF; break;
          case 6: r = (byte & 0x80) ? 0xFF : 0; break;
          default: r = (byte & 0x80) ? 0 : 0xFF; break;
        }
        put_el(&out[0], 8, i, r);
      }
      return finish();
    }
    if (op >= 0xC0 && op <= 0xC3) {      // vprot by an immediate
      if (!read_y(m, 16, rm)) return Step::Fault;
      const uint32_t w = 8u << (op - 0xC0);
      const int32_t c = static_cast<int8_t>(imm & 0xFF);      // (positive: rotate left; negative: rotate right; modulo the width)
      const uint32_t n = static_cast<uint32_t>(c < 0 ? -c : c) % w;
      for (uint32_t i = 0; i < 128 / w; ++i) put_el(&out[0], w, i, rotate_left(el(rm[0], w, i), c >= 0 ? n : (w - n) % w, w));
      return finish();
    }
    if ((op >= 0xCC && op <= 0xCF) || (op >= 0xEC && op <= 0xEF)) {      // vpcom
      if (!read_y(m, 16, rm)) return Step::Fault;
      if ((imm & 0xFF) > 7) return fail(6);
      const bool is_unsigned = op >= 0xEC;
      const uint32_t w = 8u << (op & 3);
      for (uint32_t i = 0; i < 128 / w; ++i) {
        const uint64_t x = el(va[0], w, i), y = el(rm[0], w, i);
        const bool lt = is_unsigned ? x < y : sx(x, w) < sx(y, w), eq = x == y;
        bool r;
        switch (imm & 7) {
          case 0: r = lt; break;
          case 1: r = lt || eq; break;
          case 2: r = !lt && !eq; break;
          case 3: r = !lt; break;
          case 4: r = eq; break;
          case 5: r = !eq; break;
          case 6: r = false; break;
          default: r = true; break;
        }
        put_el(&out[0], w, i, r ? mask_of_bits(w) : 0);
      }
      return finish();
    }
    return fail(6);
  }

  if (map == 9) {
    if (!read_y(m, wide ? 32 : 16, rm)) return Step::Fault;
    if (op >= 0x80 && op <= 0x83) {      // vfrczps / pd / ss / sd: the fractional part, x - trunc(x), keeping the sign of x
      const bool is_double = op & 1, scalar = op >= 0x82;
      const Format f = is_double ? Format::F64 : Format::F32;
      const uint32_t w = is_double ? 64 : 32;
      for (uint32_t h = 0; h < (wide ? 2u : 1u); ++h)
        for (uint32_t i = 0; i < (scalar ? 1u : 128 / w); ++i) {
          const uint64_t x = el(rm[h], w, i);
          uint64_t r;
          if (is_nan(f, x)) {
            Env e = env;
            r = add(f, x, x, e);      // (quiets a signaling NaN and raises invalid)
            env.flags |= e.flags;
          } else {
            const uint64_t sign = x & (uint64_t{1} << (w - 1));
            const uint64_t magnitude = x & ~(uint64_t{1} << (w - 1));
            if ((is_double ? magnitude == 0x7FF0000000000000ull : magnitude == 0x7F800000u)) {
              env.flags |= kInvalid;
              r = is_double ? 0xFFF8000000000000ull : 0xFFC00000u;
            } else {
              Env e = env;
              const uint64_t whole = round_to_integral(f, x, Rounding::TowardZero, false, e);
              r = sub(f, x, whole, e);
              env.flags |= e.flags & ~kInexact;
              if ((r & ~(uint64_t{1} << (w - 1))) == 0) r = sign;      // (an integral input gives a zero of its own sign)
            }
          }
          put_el(&out[h], w, i, r);
        }
      s_.mxcsr |= env.flags;
      return finish();
    }
    if (op >= 0x90 && op <= 0x9B) {      // variable rotate / logical shift / arithmetic shift: the count is the signed low byte of each element
      const uint32_t w = 8u << (op & 3), kind = (op - 0x90) >> 2;      // 0 rotate, 1 shl, 2 sha
      const Xmm& data = w1 ? va[0] : rm[0];
      const Xmm& count = w1 ? rm[0] : va[0];
      for (uint32_t i = 0; i < 128 / w; ++i) {
        const uint64_t x = el(data, w, i);
        const int32_t c = static_cast<int8_t>(el(count, w, i) & 0xFF);
        const uint32_t n = static_cast<uint32_t>(c < 0 ? -c : c) & (w - 1);
        uint64_t r;
        if (kind == 0) r = c >= 0 ? rotate_left(x, n, w) : rotate_left(x, (w - n) % w, w);
        else if (c >= 0) r = (x << n) & mask_of_bits(w);
        else if (kind == 1) r = x >> n;
        else r = static_cast<uint64_t>(sx(x, w) >> n) & mask_of_bits(w);
        put_el(&out[0], w, i, r);
      }
      return finish();
    }
    // horizontal add / subtract of adjacent elements into wider ones
    struct Hop { uint8_t op; uint32_t from, count; bool is_unsigned, subtract; };
    static const Hop kHops[] = {
        {0xC1, 8, 2, false, false}, {0xC2, 8, 4, false, false}, {0xC3, 8, 8, false, false}, {0xC6, 16, 2, false, false}, {0xC7, 16, 4, false, false}, {0xCB, 32, 2, false, false},
        {0xD1, 8, 2, true, false}, {0xD2, 8, 4, true, false}, {0xD3, 8, 8, true, false}, {0xD6, 16, 2, true, false}, {0xD7, 16, 4, true, false}, {0xDB, 32, 2, true, false},
        {0xE1, 8, 2, false, true}, {0xE2, 16, 2, false, true}, {0xE3, 32, 2, false, true}};
    for (const Hop& hop : kHops) {
      if (hop.op != op) continue;
      const uint32_t to = hop.from * hop.count;
      for (uint32_t i = 0; i < 128 / to; ++i) {
        int64_t sum = 0;
        for (uint32_t k = 0; k < hop.count; ++k) {
          const uint64_t raw = el(rm[0], hop.from, i * hop.count + k);
          const int64_t v = hop.is_unsigned ? static_cast<int64_t>(raw) : sx(raw, hop.from);
          sum += (hop.subtract && k == 1) ? -v : v;
        }
        put_el(&out[0], to, i, static_cast<uint64_t>(sum) & mask_of_bits(to));
      }
      return finish();
    }
  }
  return fail(6);
}

}  // namespace emulation
