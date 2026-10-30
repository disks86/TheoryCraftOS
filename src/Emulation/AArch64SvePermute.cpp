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

// SVE permute, select and element-extract instructions (top byte 0x05): DUP, EXT, INSR, REV, SUNPK / UUNPK, TBL / TBX, ZIP / UZP / TRN (vectors and predicates), SEL, SPLICE, COMPACT,
// LASTA / LASTB, CLASTA / CLASTB, CPY, the element reversals, and the logical instructions with an immediate.
namespace emulation {

using namespace sve;

namespace {

uint64_t expand_fp_immediate_value(uint32_t imm8, uint32_t esz) {
  const uint32_t exp_bits = esz == 1 ? 5 : esz == 2 ? 8 : 11, frac_bits = esz == 1 ? 10 : esz == 2 ? 23 : 52;
  const uint64_t sign = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
  uint64_t exp = (b ^ 1) << (exp_bits - 1);
  for (uint32_t i = 0; i < exp_bits - 3; ++i) exp |= b << (exp_bits - 2 - i);
  exp |= (imm8 >> 4) & 3;
  return (sign << (exp_bits + frac_bits)) | (exp << frac_bits) | (static_cast<uint64_t>(imm8 & 15) << (frac_bits - 4));
}

// ZIP1, ZIP2, UZP1, UZP2, TRN1, TRN2 of two arrays of `elements` elements; the sources and the result are given by element accessors.
template <class GetN, class GetM, class Put>
void interleave(uint32_t kind, uint32_t elements, GetN get_n, GetM get_m, Put put_d) {
  const uint32_t half = elements / 2;
  for (uint32_t i = 0; i < half; ++i) {
    switch (kind) {
      case 0: put_d(2 * i, get_n(i)); put_d(2 * i + 1, get_m(i)); break;
      case 1: put_d(2 * i, get_n(half + i)); put_d(2 * i + 1, get_m(half + i)); break;
      case 4: put_d(2 * i, get_n(2 * i)); put_d(2 * i + 1, get_m(2 * i)); break;
      case 5: put_d(2 * i, get_n(2 * i + 1)); put_d(2 * i + 1, get_m(2 * i + 1)); break;
      default: break;
    }
  }
  if (kind == 2 || kind == 3) {
    const uint32_t offset = kind == 3 ? 1 : 0;
    for (uint32_t i = 0; i < elements; ++i) {
      const uint32_t src = 2 * i + offset;
      put_d(i, src < elements ? get_n(src) : get_m(src - elements));
    }
  }
}

uint32_t lowest_set(uint32_t x) {
  uint32_t n = 0;
  while (!((x >> n) & 1)) ++n;
  return n;
}

}  // namespace

bool AArch64Cpu::sve_permute(uint32_t insn) {
  const uint32_t size = bits_of(insn, 23, 22), esz = size, elements = element_count(esz);
  const uint32_t zd = insn & 31, zn = bits_of(insn, 9, 5), zm = bits_of(insn, 20, 16);
  Vec* const dst = &s_.v[zd];

  if (!((insn >> 21) & 1)) {
    if (bits_of(insn, 21, 18) == 0) {      // ORR, EOR, AND (immediate), DUPM
      uint64_t pattern;
      if (!decode_logical_immediate(bits_of(insn, 17, 5), &pattern)) return false;
      switch (size) {
        case 0: *dst = Vec{dst->lo | pattern, dst->hi | pattern}; break;
        case 1: *dst = Vec{dst->lo ^ pattern, dst->hi ^ pattern}; break;
        case 2: *dst = Vec{dst->lo & pattern, dst->hi & pattern}; break;
        default: *dst = Vec{pattern, pattern}; break;
      }
      return true;
    }
    if (!((insn >> 20) & 1)) return false;
    // CPY (immediate, merging or zeroing) and FCPY
    const uint16_t gp = s_.p[bits_of(insn, 19, 16)];
    const uint32_t imm8 = bits_of(insn, 12, 5);
    uint64_t value;
    if (!((insn >> 15) & 1)) {
      const uint32_t sh = (insn >> 13) & 1;
      if (esz == 0 && sh) return false;
      value = static_cast<uint64_t>(sext(imm8, 8)) << (sh ? 8 : 0);
    } else if (bits_of(insn, 15, 13) == 6) {
      if (esz == 0) return false;
      value = expand_fp_immediate_value(imm8, esz);
    } else {
      return false;
    }
    const bool merging = (insn >> 14) & 1 || ((insn >> 15) & 1);
    for (uint32_t i = 0; i < elements; ++i) {
      if (active(gp, i, esz)) put(dst, esz, i, value & element_ones(esz));
      else if (!merging) put(dst, esz, i, 0);
    }
    return true;
  }

  const uint32_t top3 = bits_of(insn, 15, 13), op = bits_of(insn, 15, 10);
  const Vec n = s_.v[zn], m = s_.v[zm];

  if (top3 == 0) {
    if (size == 0 || size == 1) {      // EXT (destructive: bits 23:21 = 001; constructive: 011)
      const uint32_t imm = (bits_of(insn, 20, 16) << 3) | bits_of(insn, 12, 10);
      const bool constructive = size == 1;
      uint8_t bytes[32];
      const Vec first = constructive ? s_.v[zn] : *dst, second = constructive ? s_.v[(zn + 1) & 31] : s_.v[zn];
      for (uint32_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(first.lo >> (8 * i));
        bytes[8 + i] = static_cast<uint8_t>(first.hi >> (8 * i));
        bytes[16 + i] = static_cast<uint8_t>(second.lo >> (8 * i));
        bytes[24 + i] = static_cast<uint8_t>(second.hi >> (8 * i));
      }
      const uint32_t position = imm >= kVectorBytes ? 0 : imm;
      Vec r;
      for (uint32_t i = 0; i < 8; ++i) {
        r.lo |= static_cast<uint64_t>(bytes[position + i]) << (8 * i);
        r.hi |= static_cast<uint64_t>(bytes[position + 8 + i]) << (8 * i);
      }
      *dst = r;
      return true;
    }
    return false;
  }

  if (top3 == 1) {
    switch (op) {
      case 0x08: case 0x09: {      // DUP (indexed), DUPQ; EXTQ shares the DUPQ space
        const uint32_t tsz = bits_of(insn, 20, 16);
        if (op == 0x09 && size == 1 && ((insn >> 21) & 1)) {      // EXTQ: imm4 in bits 19:16, with bit 20 clear
          if ((insn >> 20) & 1) return false;
          const uint32_t imm = bits_of(insn, 19, 16);
          uint8_t bytes[32];
          for (uint32_t i = 0; i < 8; ++i) {
            bytes[i] = static_cast<uint8_t>(dst->lo >> (8 * i));
            bytes[8 + i] = static_cast<uint8_t>(dst->hi >> (8 * i));
            bytes[16 + i] = static_cast<uint8_t>(n.lo >> (8 * i));
            bytes[24 + i] = static_cast<uint8_t>(n.hi >> (8 * i));
          }
          Vec r;
          for (uint32_t i = 0; i < 8; ++i) {
            r.lo |= static_cast<uint64_t>(bytes[imm + i]) << (8 * i);
            r.hi |= static_cast<uint64_t>(bytes[imm + 8 + i]) << (8 * i);
          }
          *dst = r;
          return true;
        }
        if (tsz == 0 || (op == 0x09 && size != 0)) return false;
        const uint32_t e = lowest_set(tsz);
        if (e > 4) return false;
        const uint32_t index = ((size << 5) | tsz) >> (e + 1);
        const Vec src = n;
        if (e == 4) {
          *dst = index == 0 ? src : Vec{};
          return true;
        }
        const uint64_t value = index < element_count(e) ? get(src, e, index) : 0;
        for (uint32_t i = 0; i < element_count(e); ++i) put(dst, e, i, value);
        return true;
      }
      case 0x0A: case 0x0B: case 0x0C: case 0x0D: {      // TBL (two tables), TBX, TBL, TBXQ
        const bool tbx = op == 0x0B || op == 0x0D;
        const bool two = op == 0x0A;
        const Vec t0 = n, t1 = s_.v[(zn + 1) & 31];
        const Vec original = *dst;
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t idx = get(m, esz, i);
          uint64_t v;
          if (idx < elements) v = get(t0, esz, static_cast<uint32_t>(idx));
          else if (two && idx < 2 * elements) v = get(t1, esz, static_cast<uint32_t>(idx - elements));
          else v = tbx ? get(original, esz, i) : 0;
          put(dst, esz, i, v);
        }
        return true;
      }
      case 0x0E: {      // DUP (scalar), INSR, SUNPK / UUNPK, REV
        const uint32_t sel = bits_of(insn, 20, 16);
        if (sel == 0) {
          const uint64_t v = reg_sp(zn, true) & element_ones(esz);
          for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, v);
          return true;
        }
        if (sel == 4 || sel == 20) {      // INSR
          const uint64_t v = sel == 4 ? reg(zn, true) & element_ones(esz) : get(s_.v[zn], esz, 0);
          for (uint32_t i = elements - 1; i > 0; --i) put(dst, esz, i, get(*dst, esz, i - 1));
          put(dst, esz, 0, v);
          return true;
        }
        if ((sel & 0x1C) == 0x10) {      // unpack
          if (esz == 0) return false;
          const bool is_signed = (sel & 2) == 0, high = (sel & 1) != 0;
          const uint32_t w = element_bits(esz - 1);
          const Vec src = n;
          Vec r;
          for (uint32_t i = 0; i < elements; ++i) {
            const uint64_t x = get(src, esz - 1, i + (high ? elements : 0));
            put(&r, esz, i, is_signed ? static_cast<uint64_t>(sext(x, w)) & element_ones(esz) : x);
          }
          *dst = r;
          return true;
        }
        if (sel == 24) {      // REV
          const Vec src = n;
          for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, get(src, esz, elements - 1 - i));
          return true;
        }
        return false;
      }
      default: return false;
    }
  }

  if (top3 == 2) {      // predicate permutes and PUNPK / REV
    if (((insn >> 9) & 1) || ((insn >> 4) & 1)) return false;
    const uint32_t pd = insn & 15, pn = bits_of(insn, 8, 5), pm = bits_of(insn, 19, 16);
    const uint32_t kind = op - 0x10;
    if (op == 0x10 && ((insn >> 20) & 1)) {
      const uint32_t sel = bits_of(insn, 19, 16);
      const uint16_t src = s_.p[pn];
      if (sel <= 1 && size == 0) {      // PUNPKLO / PUNPKHI: the byte-granular predicate halves become halfword-granular
        uint16_t r = 0;
        for (uint32_t i = 0; i < 8; ++i)
          if ((src >> (i + (sel ? 8 : 0))) & 1) r = static_cast<uint16_t>(r | (1u << (2 * i)));
        s_.p[pd] = r;
        return true;
      }
      if (sel == 4) {      // REV (predicate)
        uint16_t r = 0;
        const uint32_t group = (1u << (1u << esz)) - 1;
        for (uint32_t i = 0; i < elements; ++i) r = static_cast<uint16_t>(r | (((src >> ((elements - 1 - i) << esz)) & group) << (i << esz)));
        s_.p[pd] = r;
        return true;
      }
      return false;
    }
    if (kind > 5 || ((insn >> 20) & 1)) return false;
    const uint16_t a = s_.p[pn], b = s_.p[pm];
    uint16_t r = 0;
    // (a predicate element is 2^esz bits wide here: the whole group moves, not just its lowest bit)
    const uint32_t group = (1u << (1u << esz)) - 1;
    interleave(kind, elements, [&](uint32_t i) { return (a >> (i << esz)) & group; }, [&](uint32_t i) { return (b >> (i << esz)) & group; },
               [&](uint32_t i, uint32_t v) { r = static_cast<uint16_t>(r | (v << (i << esz))); });
    s_.p[pd] = r;
    return true;
  }

  if (top3 == 3) {      // ZIP1, ZIP2, UZP1, UZP2, TRN1, TRN2
    const uint32_t kind = op - 0x18;
    if (kind > 5) return false;
    Vec r;
    const Vec a = n, b = m;
    interleave(kind, elements, [&](uint32_t i) { return get(a, esz, i); }, [&](uint32_t i) { return get(b, esz, i); }, [&](uint32_t i, uint64_t v) { put(&r, esz, i, v); });
    *dst = r;
    return true;
  }

  if (top3 == 6 || top3 == 7) {      // SEL
    const uint16_t gp = s_.p[bits_of(insn, 13, 10)];
    for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, active(gp, i, esz) ? get(n, esz, i) : get(m, esz, i));
    return true;
  }

  // top3 == 4 or 5: operations on the active elements
  const uint32_t sel = bits_of(insn, 20, 16);
  const uint16_t gp = s_.p[bits_of(insn, 12, 10)];
  auto last_active = [&]() {
    int last = -1;
    for (uint32_t i = 0; i < elements; ++i)
      if (active(gp, i, esz)) last = static_cast<int>(i);
    return last;
  };
  if (top3 == 4) {
    switch (sel) {
      case 0: {      // CPY (SIMD&FP scalar)
        const uint64_t v = get(s_.v[zn], esz, 0);
        for (uint32_t i = 0; i < elements; ++i)
          if (active(gp, i, esz)) put(dst, esz, i, v);
        return true;
      }
      case 1: {      // COMPACT
        if (esz < 2) return false;
        Vec r;
        uint32_t k = 0;
        for (uint32_t i = 0; i < elements; ++i)
          if (active(gp, i, esz)) put(&r, esz, k++, get(n, esz, i));
        *dst = r;
        return true;
      }
      case 2: case 3: {      // LASTA / LASTB (SIMD&FP scalar destination)
        int last = last_active();
        if (sel == 2) last = last + 1 >= static_cast<int>(elements) ? 0 : last + 1;
        else if (last < 0) last = static_cast<int>(elements) - 1;
        s_.v[zd] = Vec{get(n, esz, static_cast<uint32_t>(last)), 0};
        return true;
      }
      case 4: case 5: case 6: case 7: {      // REVB, REVH, REVW, RBIT
        const uint32_t w = element_bits(esz);
        const uint32_t unit = sel == 4 ? 8 : sel == 5 ? 16 : sel == 6 ? 32 : 1;
        if (sel != 7 && unit >= w) return false;
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          const uint64_t x = get(n, esz, i);
          uint64_t r = 0;
          for (uint32_t k = 0; k < w; k += unit) r |= ((x >> k) & ones(unit)) << (w - unit - k);
          put(dst, esz, i, r);
        }
        return true;
      }
      case 8: case 9: {      // CLASTA / CLASTB (vector)
        const int last = last_active();
        if (last < 0) return true;      // (the destination keeps its value)
        uint32_t index = static_cast<uint32_t>(last);
        if (sel == 8) index = index + 1 >= elements ? 0 : index + 1;
        const uint64_t v = get(n, esz, index);
        for (uint32_t i = 0; i < elements; ++i) put(dst, esz, i, v);
        return true;
      }
      case 10: case 11: {      // CLASTA / CLASTB (SIMD&FP scalar)
        const int last = last_active();
        const uint64_t keep = get(*dst, esz, 0);
        if (last < 0) { s_.v[zd] = Vec{keep, 0}; return true; }
        uint32_t index = static_cast<uint32_t>(last);
        if (sel == 10) index = index + 1 >= elements ? 0 : index + 1;
        s_.v[zd] = Vec{get(n, esz, index), 0};
        return true;
      }
      case 12: case 13: {      // SPLICE (destructive and constructive)
        const Vec first = sel == 12 ? *dst : s_.v[zn];
        const Vec second = sel == 12 ? s_.v[zn] : s_.v[(zn + 1) & 31];
        int lo = -1, hi = -1;
        for (uint32_t i = 0; i < elements; ++i)
          if (active(gp, i, esz)) { if (lo < 0) lo = static_cast<int>(i); hi = static_cast<int>(i); }
        Vec r;
        uint32_t k = 0;
        if (lo >= 0)
          for (int i = lo; i <= hi; ++i) put(&r, esz, k++, get(first, esz, static_cast<uint32_t>(i)));
        for (uint32_t i = 0; k < elements; ++i) put(&r, esz, k++, get(second, esz, i));
        *dst = r;
        return true;
      }
      case 14: {      // REVD (128-bit elements)
        if (gp & 1) *dst = Vec{n.hi, n.lo};
        return true;
      }
      case 17: {      // EXPAND
        const Vec src = *dst;
        Vec r;
        uint32_t k = 0;
        for (uint32_t i = 0; i < elements; ++i) put(&r, esz, i, active(gp, i, esz) ? get(src, esz, k++) : 0);
        *dst = r;
        return true;
      }
      default: return false;
    }
  }

  // top3 == 5: general-purpose register forms
  switch (sel) {
    case 0: case 1: {      // LASTA / LASTB to a general register
      int last = last_active();
      if (sel == 0) last = last + 1 >= static_cast<int>(elements) ? 0 : last + 1;
      else if (last < 0) last = static_cast<int>(elements) - 1;
      set_reg(zd, get(n, esz, static_cast<uint32_t>(last)), true);
      return true;
    }
    case 8: {      // CPY (general register)
      const uint64_t v = reg_sp(zn, true) & element_ones(esz);
      for (uint32_t i = 0; i < elements; ++i)
        if (active(gp, i, esz)) put(dst, esz, i, v);
      return true;
    }
    case 16: case 17: {      // CLASTA / CLASTB (general register)
      const int last = last_active();
      if (last < 0) { set_reg(zd, reg(zd, true) & element_ones(esz), true); return true; }
      uint32_t index = static_cast<uint32_t>(last);
      if (sel == 16) index = index + 1 >= elements ? 0 : index + 1;
      set_reg(zd, get(n, esz, index), true);
      return true;
    }
    default: return false;
  }
}

}  // namespace emulation
