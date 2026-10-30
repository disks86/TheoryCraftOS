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

// SVE predicate and compare instructions (top bytes 0x24 and 0x25): integer compares, predicate logic, break and propagate, PTRUE / PFIRST / PNEXT / PTEST, the first-fault register,
// WHILE, element counts by predicate, and the integer instructions with a wide immediate.
namespace emulation {

using namespace sve;

namespace {

// The 8-bit floating-point immediate (VFPExpandImm) in half, single or double precision.
uint64_t expand_fp_immediate(uint32_t imm8, uint32_t esz) {
  const uint32_t exp_bits = esz == 1 ? 5 : esz == 2 ? 8 : 11, frac_bits = esz == 1 ? 10 : esz == 2 ? 23 : 52;
  const uint64_t sign = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
  uint64_t exp = (b ^ 1) << (exp_bits - 1);
  for (uint32_t i = 0; i < exp_bits - 3; ++i) exp |= b << (exp_bits - 2 - i);
  exp |= (imm8 >> 4) & 3;
  return (sign << (exp_bits + frac_bits)) | (exp << frac_bits) | (static_cast<uint64_t>(imm8 & 15) << (frac_bits - 4));
}

// Compare kinds, in the order of the instruction mnemonics.
enum Cmp : uint32_t { kEq, kNe, kGe, kGt, kLt, kLe, kHs, kHi, kLo, kLs };

bool compare(uint32_t kind, uint64_t a, uint64_t b, uint32_t w) {
  const int64_t sa = sext(a, w), sb = sext(b, w);
  a &= ones(w);
  b &= ones(w);
  switch (kind) {
    case kEq: return a == b;
    case kNe: return a != b;
    case kGe: return sa >= sb;
    case kGt: return sa > sb;
    case kLt: return sa < sb;
    case kLe: return sa <= sb;
    case kHs: return a >= b;
    case kHi: return a > b;
    case kLo: return a < b;
    default: return a <= b;
  }
}

// The number of active elements (of size esz) in `a` that are also active in `b`.
uint32_t count_both(uint16_t a, uint16_t b, uint32_t esz) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < element_count(esz); ++i)
    if (active(a, i, esz) && active(b, i, esz)) ++n;
  return n;
}

}  // namespace

bool AArch64Cpu::sve_predicate(uint32_t insn) {
  const uint32_t size = bits_of(insn, 23, 22), esz = size, pd = insn & 15, bit4 = (insn >> 4) & 1;
  const bool b24 = ((insn >> 24) & 1) != 0, b21 = ((insn >> 21) & 1) != 0;
  const uint32_t elements = element_count(esz);

  if (!b24) {      // 0x24: integer compares of vectors, and of vectors with an unsigned immediate
    const uint32_t pg = bits_of(insn, 12, 10), zn = bits_of(insn, 9, 5);
    const uint16_t gp = s_.p[pg];
    const Vec a = s_.v[zn];
    uint16_t result = 0;
    if (!b21) {
      const uint32_t zm = bits_of(insn, 20, 16), op = bits_of(insn, 15, 13);
      uint32_t kind;
      bool wide = true;
      switch (op) {
        case 0: kind = bit4 ? kHi : kHs; wide = false; break;
        case 1: kind = bit4 ? kNe : kEq; break;
        case 2: kind = bit4 ? kGt : kGe; break;
        case 3: kind = bit4 ? kLe : kLt; break;
        case 4: kind = bit4 ? kGt : kGe; wide = false; break;
        case 5: kind = bit4 ? kNe : kEq; wide = false; break;
        case 6: kind = bit4 ? kHi : kHs; break;
        default: kind = bit4 ? kLs : kLo; break;
      }
      if (wide && esz == 3) return false;
      const Vec b = s_.v[zm];
      const uint32_t w = element_bits(esz);
      for (uint32_t i = 0; i < elements; ++i) {
        if (!active(gp, i, esz)) continue;
        // (a wide compare reads the 64-bit element that holds the narrow one; signed kinds then sign-extend the narrow element to 64 bits)
        const uint64_t x = get(a, esz, i);
        const bool is_signed = kind == kGe || kind == kGt || kind == kLt || kind == kLe || kind == kEq || kind == kNe;      // (the equality compares sign-extend too)
        bool r;
        if (wide) {
          const uint64_t y = get(b, 3, (i << esz) / 8);
          const uint64_t xe = is_signed ? static_cast<uint64_t>(sext(x, w)) : x;
          r = compare(kind, xe, y, 64);
        } else {
          r = compare(kind, x, get(b, esz, i), w);
        }
        if (r) result = static_cast<uint16_t>(result | (1u << (i << esz)));
      }
    } else {
      const uint32_t imm = bits_of(insn, 20, 14), lt = (insn >> 13) & 1;
      const uint32_t kind = lt ? (bit4 ? kLs : kLo) : (bit4 ? kHi : kHs);
      const uint32_t w = element_bits(esz);
      for (uint32_t i = 0; i < elements; ++i)
        if (active(gp, i, esz) && compare(kind, get(a, esz, i), imm, w)) result = static_cast<uint16_t>(result | (1u << (i << esz)));
    }
    s_.p[pd] = result;
    sve_set_flags(gp, result, esz);
    return true;
  }

  // 0x25
  const uint32_t pg3 = bits_of(insn, 12, 10), pg4 = bits_of(insn, 13, 10), pn = bits_of(insn, 8, 5), pm = bits_of(insn, 19, 16);
  const uint32_t top2 = bits_of(insn, 15, 14);
  if (!b21) {
    if (top2 == 0 || top2 == 2) {      // compares with a signed 5-bit immediate
      const uint32_t op = bits_of(insn, 15, 13);
      uint32_t kind;
      switch (op) {
        case 0: kind = bit4 ? kGt : kGe; break;
        case 1: kind = bit4 ? kLe : kLt; break;
        case 4: kind = bit4 ? kNe : kEq; break;
        default: return false;
      }
      const uint16_t gp = s_.p[pg3];
      const Vec a = s_.v[bits_of(insn, 9, 5)];
      const uint32_t w = element_bits(esz);
      const uint64_t imm = static_cast<uint64_t>(sext(bits_of(insn, 20, 16), 5));
      uint16_t result = 0;
      for (uint32_t i = 0; i < elements; ++i)
        if (active(gp, i, esz) && compare(kind, get(a, esz, i), imm & ones(w), w)) result = static_cast<uint16_t>(result | (1u << (i << esz)));
      s_.p[pd] = result;
      sve_set_flags(gp, result, esz);
      return true;
    }
    const bool b20 = (insn >> 20) & 1;
    if (top2 == 1 && !b20) {      // predicate logic: bits 23 = op, 22 = S, 9 = o2, 4 = o3
      const uint32_t op = (insn >> 23) & 1, s = (insn >> 22) & 1, o2 = (insn >> 9) & 1, o3 = bit4;
      const uint16_t gp = s_.p[pg4], n = s_.p[pn], m = s_.p[pm];
      uint16_t r;
      const uint32_t kind = (op << 2) | (o2 << 1) | o3;
      switch (kind) {
        case 0: r = n & m; break;      // AND
        case 1: r = n & ~m; break;     // BIC
        case 2: r = n ^ m; break;      // EOR
        case 3: if (s) return false; r = (n & gp) | (m & ~gp); s_.p[pd] = r; return true;      // SEL
        case 4: r = n | m; break;      // ORR
        case 5: r = n | ~m; break;     // ORN
        case 6: r = ~(n | m); break;   // NOR
        default: r = ~(n & m); break;  // NAND
      }
      if (s && kind == 3) return false;
      r = static_cast<uint16_t>(r & gp);
      s_.p[pd] = r;
      if (s) sve_set_flags(gp, r, 0);
      return true;
    }
    if (top2 == 3 && !b20) {      // BRKPA / BRKPB (and the flag-setting forms)
      if ((insn >> 9) & 1) return false;
      const bool s = (insn >> 22) & 1, before = bit4;
      if ((insn >> 23) & 1) return false;
      const uint16_t gp = s_.p[pg4], n = s_.p[pn], m = s_.p[pm];
      uint16_t r = 0;
      // last active element of Pg set in Pn
      int last = -1;
      for (int i = 0; i < 16; ++i)
        if ((gp >> i) & 1) last = i;
      if (last >= 0 && ((n >> last) & 1)) {
        bool flag = true;
        for (uint32_t i = 0; i < 16; ++i) {
          if (!((gp >> i) & 1)) continue;
          if (before) flag = flag && !((m >> i) & 1);
          if (flag) r = static_cast<uint16_t>(r | (1u << i));
          if (!before) flag = flag && !((m >> i) & 1);
        }
      }
      s_.p[pd] = r;
      if (s) sve_set_flags(gp, r, 0);
      return true;
    }
    if (top2 == 1 && b20) {      // BRKA, BRKB, BRKN
      const uint32_t sel = bits_of(insn, 19, 16);
      const bool s = (insn >> 22) & 1, bb = (insn >> 23) & 1;
      const uint16_t gp = s_.p[pg4], n = s_.p[pn];
      if ((insn >> 21) & 1) return false;
      if ((insn >> 9) & 1) return false;
      uint16_t r = 0;
      if (sel == 8) {      // BRKN: Pdm
        if (bb) return false;
        int last = -1;
        for (int i = 0; i < 16; ++i)
          if ((gp >> i) & 1) last = i;
        r = (last >= 0 && ((n >> last) & 1)) ? s_.p[pd] : 0;
        s_.p[pd] = r;
        if (bit4) return false;
        if (s) sve_set_flags(0xFFFF, r, 0);
        return true;
      }
      if (sel != 0) return false;
      bool flag = true;
      for (uint32_t i = 0; i < 16; ++i) {
        if (!((gp >> i) & 1)) continue;
        if (bb) { if ((n >> i) & 1) flag = false; if (flag) r = static_cast<uint16_t>(r | (1u << i)); }      // BRKB: break before
        else { if (flag) r = static_cast<uint16_t>(r | (1u << i)); if ((n >> i) & 1) flag = false; }       // BRKA: break after
      }
      if (bit4) r = static_cast<uint16_t>(r | (s_.p[pd] & ~gp));      // merging
      s_.p[pd] = r;
      if (s) sve_set_flags(gp, r, 0);
      return true;
    }
    if (top2 == 3 && b20) {      // PTEST, PFIRST, PNEXT, PTRUE, RDFFR
      const uint32_t sel = bits_of(insn, 19, 16);
      if (bits_of(insn, 23, 16) == 0x50 && bits_of(insn, 9, 9) == 0 && (insn & 0x1F) == 0) {      // PTEST
        sve_set_flags(s_.p[pg4], s_.p[pn], 0);
        return true;
      }
      if (bits_of(insn, 23, 16) == 0x58 && bits_of(insn, 15, 9) == 0x60 && !bit4) {      // PFIRST Pdn, Pg, Pdn
        const uint16_t gp = s_.p[pn];
        uint16_t r = s_.p[pd];
        for (uint32_t i = 0; i < 16; ++i)
          if ((gp >> i) & 1) { r = static_cast<uint16_t>(r | (1u << i)); break; }
        s_.p[pd] = r;
        sve_set_flags(gp, r, 0);
        return true;
      }
      if ((sel & 0xE) == 0x8 && bits_of(insn, 15, 10) == 0x31 && !(insn >> 9 & 1) && !bit4 && (insn >> 20 & 1)) {      // PNEXT (bits 20:16 = 11001)
        if (sel != 9) return false;
        const uint16_t gp = s_.p[pn], dn = s_.p[pd];
        int last = -1;
        for (uint32_t i = 0; i < elements; ++i)
          if (active(gp, i, esz) && active(dn, i, esz)) last = static_cast<int>(i);
        uint16_t r = 0;
        for (uint32_t i = static_cast<uint32_t>(last + 1); i < elements; ++i)
          if (active(gp, i, esz)) { r = static_cast<uint16_t>(1u << (i << esz)); break; }
        s_.p[pd] = r;
        sve_set_flags(gp, r, esz);
        return true;
      }
      if (bits_of(insn, 15, 10) == 0x38 && !bit4 && (insn >> 17 & 15) == 12) {      // PTRUE / PTRUES
        const uint32_t count = pattern_count(bits_of(insn, 9, 5), elements);
        uint16_t r = 0;
        for (uint32_t i = 0; i < count; ++i) r = static_cast<uint16_t>(r | (1u << (i << esz)));
        s_.p[pd] = r;
        if (insn & (1u << 16)) sve_set_flags(r, r, esz);
        return true;
      }
      // first-fault register
      const uint32_t hi = bits_of(insn, 23, 16);
      if (bits_of(insn, 15, 11) == 0x1E && !bit4 && bits_of(insn, 9, 9) == 0 && (hi == 0x18 || hi == 0x58)) {      // RDFFR / RDFFRS Pd, Pg/z
        const uint16_t gp = s_.p[pn];
        const uint16_t r = static_cast<uint16_t>(s_.ffr & gp);
        s_.p[pd] = r;
        if (hi == 0x58) sve_set_flags(gp, r, 0);
        return true;
      }
    }
    // RDFFR (unpredicated)
    if ((insn & 0xFFFFFFF0u) == 0x2519F000u) { s_.p[pd] = s_.ffr; return true; }
    return false;
  }

  // SETFFR and WRFFR share the space of the element counts
  if (insn == 0x252C9000u) { s_.ffr = 0xFFFF; return true; }
  if ((insn & 0xFFFFFE1Fu) == 0x25289000u) { s_.ffr = s_.p[pn]; return true; }

  // 0x25 with bit 21 set
  const uint32_t d = insn & 31;
  if (top2 == 3) {      // integer instructions with a wide immediate
    const uint32_t group = bits_of(insn, 20, 19), opc = bits_of(insn, 18, 16), imm8 = bits_of(insn, 12, 5), sh = (insn >> 13) & 1;
    Vec* const dst = &s_.v[d];
    const uint32_t w = element_bits(esz);
    if (group == 0) {
      if (opc == 2) return false;
      if (esz == 0 && sh) return false;
      const uint64_t imm = static_cast<uint64_t>(imm8) << (sh ? 8 : 0);
      for (uint32_t i = 0; i < elements; ++i) {
        const uint64_t x = get(*dst, esz, i);
        bool sat = false;
        uint64_t r;
        switch (opc) {
          case 0: r = (x + imm) & ones(w); break;
          case 1: r = (x - imm) & ones(w); break;
          case 3: r = (imm - x) & ones(w); break;
          case 4: r = saturate_to_signed(signed_of(x, w) + static_cast<I128>(imm & ones(w)), w, &sat); break;      // SQADD: the immediate is unsigned
          case 5: r = saturate_to_unsigned(unsigned_of(x, w) + static_cast<I128>(imm & ones(w)), w, &sat); break;
          case 6: r = saturate_to_signed(signed_of(x, w) - static_cast<I128>(imm & ones(w)), w, &sat); break;
          default: r = saturate_to_unsigned(unsigned_of(x, w) - static_cast<I128>(imm & ones(w)), w, &sat); break;
        }
        put(dst, esz, i, r);
      }
      return true;
    }
    if (group == 1) {      // SMAX UMAX SMIN UMIN
      if (opc > 3 || sh) return false;
      for (uint32_t i = 0; i < elements; ++i) {
        const uint64_t x = get(*dst, esz, i);
        const int64_t sx = sext(x, w), si = sext(imm8, 8);
        uint64_t r;
        switch (opc) {
          case 0: r = static_cast<uint64_t>(sx > si ? sx : si); break;
          case 1: r = x > imm8 ? x : imm8; break;
          case 2: r = static_cast<uint64_t>(sx < si ? sx : si); break;
          default: r = x < imm8 ? x : imm8; break;
        }
        put(dst, esz, i, r & ones(w));
      }
      return true;
    }
    if (group == 2) {      // MUL
      if (opc != 0 || sh) return false;
      for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, (get(*dst, esz, i) * static_cast<uint64_t>(sext(imm8, 8))) & ones(w));
      return true;
    }
    // DUP (immediate) and FDUP
    if (opc == 0) {
      if (esz == 0 && sh) return false;
      const uint64_t imm = static_cast<uint64_t>(sext(imm8, 8)) << (sh ? 8 : 0);
      for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, imm & ones(w));
      return true;
    }
    if (opc == 1) {
      if (esz == 0 || sh) return false;
      const uint64_t v = expand_fp_immediate(imm8, esz);
      for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, v);
      return true;
    }
    return false;
  }

  if (top2 == 0) {
    // WHILE*, CTERMEQ / CTERMNE, WHILERW / WHILEWR
    const uint32_t rn = bits_of(insn, 9, 5), rm = bits_of(insn, 20, 16);
    const uint32_t sel = bits_of(insn, 15, 10);
    if (sel == 0x08) {      // CTERMEQ / CTERMNE (bit 22 = sz: 32 or 64-bit operands)
      if ((insn & 0xF) != 0 || !(insn >> 23 & 1)) return false;
      const bool is64 = (insn >> 22) & 1;
      const uint64_t a = reg(rn, is64), b = reg(rm, is64);
      const bool term = bit4 ? a != b : a == b;
      const bool carry = (s_.nzcv & kC) != 0;
      s_.nzcv = (s_.nzcv & (kZ | kC)) | (term ? kN : 0) | (!term && !carry ? kV : 0);
      return true;
    }
    if (sel == 0x0C) {      // WHILEWR / WHILERW
      const bool rw = bit4;
      const int64_t diff = (static_cast<int64_t>(reg(rm, true)) - static_cast<int64_t>(reg(rn, true))) / static_cast<int64_t>(1u << esz);
      uint16_t r = 0;
      const int64_t absdiff = diff < 0 ? -diff : diff;
      for (uint32_t i = 0; i < elements; ++i) {
        const bool cond = rw ? (absdiff == 0 || static_cast<int64_t>(i) < absdiff) : (diff <= 0 || static_cast<int64_t>(i) < diff);
        if (cond) r = static_cast<uint16_t>(r | (1u << (i << esz)));
        else break;
      }
      s_.p[pd] = r;
      sve_set_flags(0xFFFF, r, esz);
      return true;
    }
    if (sel >= 8) return false;
    const bool sf = (insn >> 12) & 1, is_unsigned = (insn >> 11) & 1, lt = (insn >> 10) & 1, eq = bit4;
    const I128 a = is_unsigned ? static_cast<I128>(reg(rn, sf)) : static_cast<I128>(sf ? static_cast<int64_t>(reg(rn, true)) : sext(reg(rn, false), 32));
    const I128 b = is_unsigned ? static_cast<I128>(reg(rm, sf)) : static_cast<I128>(sf ? static_cast<int64_t>(reg(rm, true)) : sext(reg(rm, false), 32));
    uint16_t r = 0;
    for (uint32_t i = 0; i < elements; ++i) {
      const I128 x = lt ? a + i : a - i;
      const bool cond = lt ? (eq ? x <= b : x < b) : (eq ? x > b : x >= b);
      // (the encodings: lt with eq = LE / LS, lt without = LT / LO, and for the decrementing forms eq = GT / HI, otherwise GE / HS)
      if (!cond) break;
      r = static_cast<uint16_t>(r | (1u << ((lt ? i : elements - 1 - i) << esz)));      // (the decrementing forms fill from the top element down)
    }
    s_.p[pd] = r;
    sve_set_flags(0xFFFF, r, esz);
    return true;
  }

  // element counts by predicate: CNTP, INCP / DECP, and the saturating forms
  if (top2 == 2) {
    const uint32_t sel = bits_of(insn, 21, 16);
    const uint32_t rd = insn & 31;
    if (sel == 0x20) {      // CNTP Xd, Pg, Pn.T
      if ((insn >> 9) & 1) return false;
      set_reg(rd, count_both(s_.p[pg4], s_.p[pn], esz), true);
      return true;
    }
    if (sel < 0x28 || sel > 0x2D || (insn >> 9) & 1) return false;
    const uint32_t pm2 = pn;      // (Pm sits in bits 8:5)
    const int64_t count = count_both(s_.p[pm2], s_.p[pm2], esz);
    const bool vector = ((insn >> 11) & 1) == 0, wide = (insn >> 10) & 1;
    if (bits_of(insn, 15, 12) != 8) return false;
    if (sel >= 0x2C) {      // INCP / DECP
      const bool dec = sel & 1;
      if (vector) {
        if (esz == 0) return false;
        Vec* const dst = &s_.v[rd];
        for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, (get(*dst, esz, i) + static_cast<uint64_t>(dec ? -count : count)) & element_ones(esz));
      } else {
        if (wide) return false;
        set_reg(rd, reg(rd, true) + static_cast<uint64_t>(dec ? -count : count), true);
      }
      return true;
    }
    const bool dec = (sel & 2) != 0, is_unsigned = (sel & 1) != 0;
    const int64_t delta = dec ? -count : count;
    if (vector) {
      if (esz == 0) return false;
      Vec* const dst = &s_.v[rd];
      const uint32_t w = element_bits(esz);
      for (uint32_t i = 0; i < elements; ++i) {
        bool sat = false;
        const uint64_t x = get(*dst, esz, i);
        put(dst, esz, i, is_unsigned ? saturate_to_unsigned(unsigned_of(x, w) + delta, w, &sat) : saturate_to_signed(signed_of(x, w) + delta, w, &sat));
      }
      return true;
    }
    bool sat = false;
    if (wide) {
      const uint64_t x = reg(rd, true);
      set_reg(rd, is_unsigned ? saturate_to_unsigned(static_cast<I128>(x) + delta, 64, &sat) : saturate_to_signed(static_cast<I128>(static_cast<int64_t>(x)) + delta, 64, &sat), true);
    } else {
      const uint64_t x = reg(rd, false);
      const uint64_t r = is_unsigned ? saturate_to_unsigned(static_cast<I128>(x) + delta, 32, &sat) : saturate_to_signed(static_cast<I128>(sext(x, 32)) + delta, 32, &sat);
      set_reg(rd, is_unsigned ? r : static_cast<uint64_t>(sext(r, 32)), true);
    }
    return true;
  }
  return false;
}

}  // namespace emulation
