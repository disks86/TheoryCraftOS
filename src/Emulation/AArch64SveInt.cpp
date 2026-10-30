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
#include "Emulation/AArch64SveCommon.hpp"

// SVE integer instructions (top byte 0x04): predicated and unpredicated arithmetic, logic, shifts, reductions, multiply-add, the counting instructions and INDEX.
namespace emulation {

using namespace sve;

namespace {

enum IntOp : uint32_t {
  kAdd, kSub, kSubr, kSmax, kUmax, kSmin, kUmin, kSabd, kUabd, kMul, kSmulh, kUmulh, kSdiv, kUdiv, kSdivr, kUdivr,
  kOrr, kEor, kAnd, kBic, kAsr, kLsr, kLsl, kAsrr, kLsrr, kLslr
};

// One element of a binary integer operation; the result is truncated to the element width.
uint64_t int_op(uint32_t op, uint32_t esz, uint64_t a, uint64_t b) {
  const uint32_t w = element_bits(esz);
  const uint64_t m = ones(w);
  a &= m;
  if (op < kAsr) b &= m;      // (a shift amount is not truncated to the element: the wide forms take a 64-bit amount)
  switch (op) {
    case kSubr: case kSdivr: case kUdivr: case kAsrr: case kLsrr: case kLslr: {
      const uint64_t t = a;
      a = b & m;
      b = t;
      op = op == kSubr ? kSub : op == kSdivr ? kSdiv : op == kUdivr ? kUdiv : op == kAsrr ? kAsr : op == kLsrr ? kLsr : kLsl;
      break;
    }
    default: break;
  }
  const int64_t sa = sext(a, w), sb = sext(b, w);
  switch (op) {
    case kAdd: return (a + b) & m;
    case kSub: return (a - b) & m;
    case kSmax: return static_cast<uint64_t>(sa > sb ? sa : sb) & m;
    case kUmax: return a > b ? a : b;
    case kSmin: return static_cast<uint64_t>(sa < sb ? sa : sb) & m;
    case kUmin: return a < b ? a : b;
    case kSabd: return static_cast<uint64_t>(sa > sb ? static_cast<uint64_t>(sa) - static_cast<uint64_t>(sb) : static_cast<uint64_t>(sb) - static_cast<uint64_t>(sa)) & m;
    case kUabd: return (a > b ? a - b : b - a) & m;
    case kMul: return (a * b) & m;
    case kSmulh: return esz == 3 ? multiply_high(a, b, true) : static_cast<uint64_t>((sa * sb) >> w) & m;
    case kUmulh: return esz == 3 ? multiply_high(a, b, false) : ((a * b) >> w) & m;
    case kSdiv: {
      if (sb == 0) return 0;
      if (sb == -1) return (uint64_t{0} - a) & m;
      return static_cast<uint64_t>(sa / sb) & m;
    }
    case kUdiv: return b == 0 ? 0 : a / b;
    case kOrr: return a | b;
    case kEor: return a ^ b;
    case kAnd: return a & b;
    case kBic: return a & ~b & m;
    case kAsr: return static_cast<uint64_t>(sa >> (b >= w ? w - 1 : b)) & m;
    case kLsr: return b >= w ? 0 : a >> b;
    case kLsl: return b >= w ? 0 : (a << b) & m;
    default: return 0;
  }
}

constexpr bool is_division(uint32_t op) { return op == kSdiv || op == kUdiv || op == kSdivr || op == kUdivr; }

// The element size and shift amount of an SVE shift-by-immediate: tsz is the 4-bit (or, for the predicated form, tszh:tszl) size field.
bool decode_shift(uint32_t tsz, uint32_t imm3, bool left, uint32_t* esz, uint32_t* amount) {
  if (tsz == 0) return false;
  uint32_t z = 3;
  while (!((tsz >> z) & 1)) --z;
  *esz = z;
  const uint32_t value = (tsz << 3) | imm3, esize = 8u << z;
  *amount = left ? value - esize : 2 * esize - value;
  return true;
}

uint32_t count_leading(uint64_t x, uint32_t w) {
  uint32_t n = 0;
  for (int32_t b = static_cast<int32_t>(w) - 1; b >= 0 && !((x >> b) & 1); --b) ++n;
  return n;
}

uint32_t popcount(uint64_t x) {
  uint32_t n = 0;
  for (; x != 0; x &= x - 1) ++n;
  return n;
}

// Sum of `delta` and a scalar for INC / DEC and the saturating variants. `bits` is the width the result saturates at.
uint64_t saturating_add(uint64_t value, int64_t delta, bool is_unsigned, uint32_t bits, bool saturate) {
  const uint64_t m = ones(bits);
  if (!saturate) return (value + static_cast<uint64_t>(delta)) & m;
  bool sat = false;
  if (is_unsigned) {
    const I128 r = static_cast<I128>(value & m) + delta;
    return saturate_to_unsigned(r, bits, &sat);
  }
  const I128 r = static_cast<I128>(sext(value, bits)) + delta;
  return saturate_to_signed(r, bits, &sat);
}

}  // namespace

bool AArch64Cpu::sve_integer(uint32_t insn) {
  const uint32_t size = bits_of(insn, 23, 22), zd = insn & 31, zn = bits_of(insn, 9, 5), zm = bits_of(insn, 20, 16), pg = bits_of(insn, 12, 10);
  const uint32_t esz = size, elements = element_count(esz);
  Vec* const dst = &s_.v[zd];

  if (!((insn >> 21) & 1)) {
    const uint32_t group = bits_of(insn, 15, 13), opc = bits_of(insn, 20, 16);
    const uint16_t gp = s_.p[pg];
    switch (group) {
      case 0: {      // predicated binary arithmetic and logic
        static const int8_t kMap[32] = {kAdd, kSub, -1, kSubr, -1, -1, -1, -1, kSmax, kUmax, kSmin, kUmin, kSabd, kUabd, -1, -1,
                                        kMul, -1, kSmulh, kUmulh, kSdiv, kUdiv, kSdivr, kUdivr, kOrr, kEor, kAnd, kBic, -1, -1, -1, -1};
        if (kMap[opc] < 0) return false;
        const uint32_t op = static_cast<uint32_t>(kMap[opc]);
        if (is_division(op) && esz < 2) return false;
        const Vec b = s_.v[zn];      // (Zm is bits 9:5 in this class, Zdn is bits 4:0)
        for (uint32_t i = 0; i < elements; ++i)
          if (active(gp, i, esz)) put(dst, esz, i, int_op(op, esz, get(*dst, esz, i), get(b, esz, i)));
        return true;
      }
      case 1: {      // reductions and MOVPRFX (predicated)
        if ((opc & 0x1E) == 0x10) {      // movprfx
          const bool merging = (opc & 1) != 0;
          const Vec src = s_.v[zn];
          for (uint32_t i = 0; i < elements; ++i) {
            if (active(gp, i, esz)) put(dst, esz, i, get(src, esz, i));
            else if (!merging) put(dst, esz, i, 0);
          }
          return true;
        }
        const Vec src = s_.v[zn];
        const uint32_t w = element_bits(esz);
        uint64_t result;
        if (opc == 0 || opc == 1) {      // SADDV / UADDV (64-bit sum)
          if (opc == 0 && esz == 3) return false;
          uint64_t sum = 0;
          for (uint32_t i = 0; i < elements; ++i)
            if (active(gp, i, esz)) sum += opc == 0 ? static_cast<uint64_t>(sext(get(src, esz, i), w)) : get(src, esz, i);
          s_.v[zd] = Vec{sum, 0};
          return true;
        }
        switch (opc) {
          case 8: result = uint64_t{1} << (w - 1); break;      // SMAXV: the most negative value
          case 9: result = 0; break;
          case 10: result = (uint64_t{1} << (w - 1)) - 1; break;
          case 11: result = ones(w); break;
          case 24: result = 0; break;
          case 25: result = 0; break;
          case 26: result = ones(w); break;
          default: return false;
        }
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          const uint64_t x = get(src, esz, i);
          switch (opc) {
            case 8: if (sext(x, w) > sext(result, w)) result = x; break;
            case 9: if (x > result) result = x; break;
            case 10: if (sext(x, w) < sext(result, w)) result = x; break;
            case 11: if (x < result) result = x; break;
            case 24: result |= x; break;
            case 25: result ^= x; break;
            default: result &= x; break;
          }
        }
        s_.v[zd] = Vec{result, 0};
        return true;
      }
      case 2: case 3: case 6: case 7: {      // MLA, MLS (accumulator Zda, operands Zn and Zm), MAD, MSB (Zdn, multiplier Zm, addend Za)
        const bool madd = group >= 6;
        const bool subtract = (group & 1) != 0;
        const Vec a = s_.v[zn], b = s_.v[zm];
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          const uint64_t d = get(*dst, esz, i);
          uint64_t product, addend;
          if (madd) {
            product = d * get(b, esz, i);      // Zdn * Zm
            addend = get(a, esz, i);           // Za
          } else {
            product = get(a, esz, i) * get(b, esz, i);
            addend = d;
          }
          put(dst, esz, i, (subtract ? addend - product : addend + product) & element_ones(esz));
        }
        return true;
      }
      case 4: {      // shifts: by immediate, by vector, by wide elements
        if (((insn >> 20) & 1) == 0) {      // by immediate (predicated): tszh:tszl:imm3
          const uint32_t tsz = (size << 2) | bits_of(insn, 9, 8), imm3 = bits_of(insn, 7, 5), op = bits_of(insn, 19, 16);
          const bool left = op == 3 || op == 6 || op == 7 || op == 15;
          uint32_t e, amount;
          if (!decode_shift(tsz, imm3, left, &e, &amount)) return false;
          if (op != 0 && op != 1 && op != 3 && op != 4 && op != 6 && op != 7 && op != 12 && op != 13 && op != 15) return false;
          const uint32_t w = element_bits(e);
          Vec* const d = &s_.v[insn & 31];
          for (uint32_t i = 0; i < element_count(e); ++i) {
            if (!active(gp, i, e)) continue;
            const uint64_t x = get(*d, e, i);
            const int64_t sx = sext(x, w);
            uint64_t r = 0;
            switch (op) {
              case 0: r = static_cast<uint64_t>(sx >> (amount >= w ? w - 1 : amount)); break;
              case 1: r = amount >= w ? 0 : x >> amount; break;
              case 3: r = x << amount; break;
              case 4: r = static_cast<uint64_t>((static_cast<I128>(sx) + (sx < 0 ? (static_cast<I128>(1) << amount) - 1 : 0)) >> amount); break;      // ASRD
              case 12: r = static_cast<uint64_t>(shift_right(static_cast<I128>(sx), amount, true)); break;      // SRSHR
              case 13: r = static_cast<uint64_t>(shift_right(static_cast<I128>(x), amount, true)); break;      // URSHR
              case 6: { bool s; r = saturate_to_signed(static_cast<I128>(sx) << amount, w, &s); break; }      // SQSHL
              case 7: { bool s; r = saturate_to_unsigned(static_cast<I128>(x) << amount, w, &s); break; }      // UQSHL
              default: { bool s; r = saturate_to_unsigned(static_cast<I128>(sx) << amount, w, &s); break; }      // SQSHLU
            }
            put(d, e, i, r & ones(w));
          }
          return true;
        }
        const uint32_t sel = bits_of(insn, 19, 16);      // 0 asr, 1 lsr, 3 lsl, 4 asrr, 5 lsrr, 7 lslr; with bit 3 the wide forms
        if (sel & 8) {
          if ((sel & 4) != 0 || sel == 10 || esz == 3) return false;
          const uint32_t op = sel == 8 ? kAsr : sel == 9 ? kLsr : kLsl;
          const Vec wide = s_.v[zn];
          for (uint32_t i = 0; i < elements; ++i)
            if (active(gp, i, esz)) put(dst, esz, i, int_op(op, esz, get(*dst, esz, i), get(wide, 3, (i << esz) / 8)));
          return true;
        }
        uint32_t op;
        switch (sel) {
          case 0: op = kAsr; break;
          case 1: op = kLsr; break;
          case 3: op = kLsl; break;
          case 4: op = kAsrr; break;
          case 5: op = kLsrr; break;
          case 7: op = kLslr; break;
          default: return false;
        }
        const Vec b = s_.v[zn];
        for (uint32_t i = 0; i < elements; ++i)
          if (active(gp, i, esz)) put(dst, esz, i, int_op(op, esz, get(*dst, esz, i), get(b, esz, i)));
        return true;
      }
      case 5: {      // unary: extend, abs, neg, cls, clz, cnt, cnot, fabs, fneg, not
        const uint32_t op = bits_of(insn, 19, 16);
        const bool zeroing = ((insn >> 20) & 1) == 0;
        const Vec src = s_.v[zn];
        const uint32_t w = element_bits(esz);
        if (op <= 5 && esz <= op / 2) return false;      // sxtb/uxtb need size >= 1, sxth/uxth >= 2, sxtw/uxtw 3
        if ((op == 12 || op == 13) && esz == 0) return false;
        if (op > 14) return false;
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) {
            if (zeroing) put(dst, esz, i, 0);
            continue;
          }
          const uint64_t x = get(src, esz, i);
          uint64_t r = 0;
          switch (op) {
            case 0: r = static_cast<uint64_t>(sext(x, 8)); break;
            case 1: r = x & 0xFF; break;
            case 2: r = static_cast<uint64_t>(sext(x, 16)); break;
            case 3: r = x & 0xFFFF; break;
            case 4: r = static_cast<uint64_t>(sext(x, 32)); break;
            case 5: r = x & 0xFFFFFFFFu; break;
            case 6: r = sext(x, w) < 0 ? uint64_t{0} - x : x; break;
            case 7: r = uint64_t{0} - x; break;
            case 8: r = count_leading((x ^ (x >> 1)) & ones(w - 1), w - 1); break;      // CLS: leading bits equal to the sign bit, not counting it
            case 9: r = count_leading(x, w); break;
            case 10: r = popcount(x); break;
            case 11: r = x == 0 ? 1 : 0; break;
            case 12: r = x & ~(uint64_t{1} << (w - 1)); break;
            case 13: r = x ^ (uint64_t{1} << (w - 1)); break;
            default: r = ~x; break;
          }
          put(dst, esz, i, r & ones(w));
        }
        return true;
      }
    }
    return false;
  }

  // bit 21 set: unpredicated forms
  const uint32_t op = bits_of(insn, 15, 10);
  switch (op) {
    case 0x00: case 0x01: case 0x04: case 0x05: case 0x06: case 0x07: case 0x02: case 0x03: {      // ADD SUB (ADDPT SUBPT) SQADD UQADD SQSUB UQSUB
      const Vec a = s_.v[zn], b = s_.v[zm];
      if ((op == 2 || op == 3) && esz != 3) return false;
      const uint32_t w = element_bits(esz);
      for (uint32_t i = 0; i < elements; ++i) {
        const uint64_t x = get(a, esz, i), y = get(b, esz, i);
        bool sat = false;
        uint64_t r;
        switch (op) {
          case 0: case 2: r = (x + y) & ones(w); break;
          case 1: case 3: r = (x - y) & ones(w); break;
          case 4: r = saturate_to_signed(signed_of(x, w) + signed_of(y, w), w, &sat); break;
          case 5: r = saturate_to_unsigned(unsigned_of(x, w) + unsigned_of(y, w), w, &sat); break;
          case 6: r = saturate_to_signed(signed_of(x, w) - signed_of(y, w), w, &sat); break;
          default: r = saturate_to_unsigned(unsigned_of(x, w) - unsigned_of(y, w), w, &sat); break;
        }
        put(dst, esz, i, r);
      }
      return true;
    }
    case 0x0C: {      // AND ORR EOR BIC (unpredicated, the size field is the operation)
      const Vec a = s_.v[zn], b = s_.v[zm];
      uint64_t lo, hi;
      switch (size) {
        case 0: lo = a.lo & b.lo; hi = a.hi & b.hi; break;
        case 1: lo = a.lo | b.lo; hi = a.hi | b.hi; break;
        case 2: lo = a.lo ^ b.lo; hi = a.hi ^ b.hi; break;
        default: lo = a.lo & ~b.lo; hi = a.hi & ~b.hi; break;
      }
      *dst = Vec{lo, hi};
      return true;
    }
    case 0x0E: {      // EOR3 (size 0), BCAX (size 1): Zdn, Zm = bits 20:16, Zk = bits 9:5
      if (size > 1) return false;
      const Vec d = *dst, m = s_.v[zm], k = s_.v[zn];
      if (size == 0) *dst = Vec{d.lo ^ m.lo ^ k.lo, d.hi ^ m.hi ^ k.hi};
      else *dst = Vec{d.lo ^ (m.lo & ~k.lo), d.hi ^ (m.hi & ~k.hi)};
      return true;
    }
    case 0x0F: {      // BSL, BSL1N, BSL2N, NBSL
      const Vec d = *dst, m = s_.v[zm], k = s_.v[zn];
      auto bsl = [&](uint64_t a, uint64_t b, uint64_t sel) {
        switch (size) {
          case 0: return (a & sel) | (b & ~sel);
          case 1: return (~a & sel) | (b & ~sel);
          case 2: return (a & sel) | (~b & ~sel);
          default: return ~((a & sel) | (b & ~sel));
        }
      };
      *dst = Vec{bsl(d.lo, m.lo, k.lo), bsl(d.hi, m.hi, k.hi)};
      return true;
    }
    case 0x0D: {      // XAR
      uint32_t e, amount;
      if (!decode_shift((size << 2) | bits_of(insn, 20, 19), bits_of(insn, 18, 16), false, &e, &amount)) return false;
      const Vec d = *dst, m = s_.v[zn];
      const uint32_t w = element_bits(e);
      for (uint32_t i = 0; i < element_count(e); ++i) put(dst, e, i, rotate_right(get(d, e, i) ^ get(m, e, i), amount, w));
      return true;
    }
    case 0x10: case 0x11: case 0x12: case 0x13: {      // INDEX
      const uint32_t w = element_bits(esz);
      const bool start_reg = (op & 1) != 0, step_reg = (op & 2) != 0;
      const int64_t start = start_reg ? static_cast<int64_t>(reg(zn, true)) : sext(zn, 5);
      const int64_t step = step_reg ? static_cast<int64_t>(reg(zm, true)) : sext(zm, 5);
      for (uint32_t i = 0; i < element_count(esz); ++i) put(dst, esz, i, (static_cast<uint64_t>(start) + static_cast<uint64_t>(step) * i) & ones(w));
      return true;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {      // ADDVL / ADDPL / RDVL, and the streaming-length forms ADDSVL / ADDSPL / RDSVL (the same length here)
      const int64_t imm = sext(bits_of(insn, 10, 5), 6);
      const uint32_t rd = insn & 31;
      if (size == 2) {
        if (zm != 31) return false;
        set_reg(rd, static_cast<uint64_t>(imm) * kVectorBytes, true);
        return true;
      }
      if (size > 1) return false;
      const int64_t scale = size == 0 ? kVectorBytes : kVectorBytes / 8;
      set_reg_sp(rd, reg_sp(zm, true) + static_cast<uint64_t>(imm * scale), true);
      return true;
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B: {      // MUL, PMUL, SMULH, UMULH (op 0x18..0x1B), SQDMULH SQRDMULH ADDQP ADDSUBP follow
      const Vec a = s_.v[zn], b = s_.v[zm];
      for (uint32_t i = 0; i < elements; ++i) {
        const uint64_t x = get(a, esz, i), y = get(b, esz, i);
        uint64_t r;
        if (op == 0x19) {      // PMUL: carry-less multiply of bytes
          if (esz != 0) return false;
          r = 0;
          for (uint32_t k = 0; k < 8; ++k)
            if ((y >> k) & 1) r ^= x << k;
          r &= 0xFF;
        } else {
          r = int_op(op == 0x18 ? kMul : op == 0x1A ? kSmulh : kUmulh, esz, x, y);
        }
        put(dst, esz, i, r);
      }
      return true;
    }
    case 0x20: case 0x21: case 0x23: {      // ASR LSR LSL by wide elements (unpredicated)
      if (esz == 3) return false;
      const Vec a = s_.v[zn], wide = s_.v[zm];
      const uint32_t kind = op == 0x20 ? kAsr : op == 0x21 ? kLsr : kLsl;
      for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, int_op(kind, esz, get(a, esz, i), get(wide, 3, (i << esz) / 8)));
      return true;
    }
    case 0x24: case 0x25: case 0x27: {      // ASR LSR LSL by immediate (unpredicated)
      uint32_t e, amount;
      if (!decode_shift((size << 2) | bits_of(insn, 20, 19), bits_of(insn, 18, 16), op == 0x27, &e, &amount)) return false;
      const Vec a = s_.v[zn];
      const uint32_t w = element_bits(e);
      for (uint32_t i = 0; i < element_count(e); ++i) {
        const uint64_t x = get(a, e, i);
        const uint64_t r = op == 0x24 ? static_cast<uint64_t>(sext(x, w) >> (amount >= w ? w - 1 : amount)) : op == 0x25 ? (amount >= w ? 0 : x >> amount) : (amount >= w ? 0 : x << amount);
        put(dst, e, i, r & ones(w));
      }
      return true;
    }
    case 0x28: case 0x29: case 0x2A: case 0x2B: {      // ADR: vector base plus a (packed, extended or shifted) vector offset
      const Vec base = s_.v[zn], off = s_.v[zm];
      const uint32_t shift = bits_of(insn, 11, 10), e = size >= 2 ? size : 3;
      for (uint32_t i = 0; i < element_count(e); ++i) {
        uint64_t o = get(off, e, i);
        if (size == 0) o = static_cast<uint64_t>(sext(o, 32));
        else if (size == 1) o &= 0xFFFFFFFFu;
        put(dst, e, i, (get(base, e, i) + (o << shift)) & element_ones(e));
      }
      return true;
    }
    case 0x2C: {      // FTSSEL
      if (esz == 0) return false;
      const Vec a = s_.v[zn], b = s_.v[zm];
      const uint32_t w = element_bits(esz);
      const uint64_t one = esz == 1 ? 0x3C00 : esz == 2 ? 0x3F800000u : 0x3FF0000000000000ull;
      for (uint32_t i = 0; i < elements; ++i) {
        uint64_t x = (b.lo & 0) | get(b, esz, i);
        uint64_t v = (x & 1) ? one : get(a, esz, i);
        if ((x >> 1) & 1) v ^= uint64_t{1} << (w - 1);
        put(dst, esz, i, v);
      }
      return true;
    }
    case 0x2E: {      // FEXPA
      if (zm != 0 || esz == 0) return false;
      const Vec a = s_.v[zn];
      for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, sve_expa(esz, get(a, esz, i)));
      return true;
    }
    case 0x2F: {
      if (zm == 0 && size == 0) { *dst = s_.v[zn]; return true; }
      return false;
    }
    default: break;
  }
  if (op < 0x30) return false;

  // counting instructions: CNTx, INCx / DECx (scalar and vector) and the saturating forms
  const uint32_t kind = bits_of(insn, 15, 12), sub = bits_of(insn, 11, 10), pattern = bits_of(insn, 9, 5), wide = (insn >> 20) & 1;
  const int64_t count = static_cast<int64_t>(pattern_count(pattern, elements)) * (bits_of(insn, 19, 16) + 1);
  if (kind == 14) {
    if (!wide) {
      if (sub != 0) return false;
      set_reg(zd, static_cast<uint64_t>(count), true);      // CNTx
      return true;
    }
    if (sub > 1) return false;
    set_reg(zd, reg(zd, true) + static_cast<uint64_t>(sub ? -count : count), true);
    return true;
  }
  if (kind == 15) {      // SQINCx / UQINCx / SQDECx / UQDECx on a scalar
    const bool decrement = (sub & 2) != 0, is_unsigned = (sub & 1) != 0;
    const int64_t delta = decrement ? -count : count;
    if (wide) {
      set_reg(zd, saturating_add(reg(zd, true), delta, is_unsigned, 64, true), true);
    } else {
      const uint64_t r = saturating_add(reg(zd, false), delta, is_unsigned, 32, true);
      set_reg(zd, is_unsigned ? r : static_cast<uint64_t>(sext(r, 32)), true);
    }
    return true;
  }
  if (kind == 12) {      // vector forms
    if (esz == 0) return false;
    const uint32_t w = element_bits(esz);
    if (!wide) {
      const bool decrement = (sub & 2) != 0, is_unsigned = (sub & 1) != 0;
      const int64_t delta = decrement ? -count : count;
      for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, saturating_add(get(*dst, esz, i), delta, is_unsigned, w, true));
      return true;
    }
    if (sub > 1) return false;
    for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, (get(*dst, esz, i) + static_cast<uint64_t>(sub ? -count : count)) & ones(w));
    return true;
  }
  return false;
}

}  // namespace emulation
