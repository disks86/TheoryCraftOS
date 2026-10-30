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
#include "Emulation/CryptoShared.hpp"
#include "Emulation/Wide.hpp"

// SVE2 integer instructions (top bytes 0x44 and 0x45): halving and saturating arithmetic, shifts by element, pairwise operations, dot products and matrix multiplies, widening and narrowing
// arithmetic, multiply-add and multiply by element, complex arithmetic, bit permutation, histograms and matching, and the SVE2 cryptographic instructions.
namespace emulation {

using namespace sve;

namespace {

// The signed shift amount of an element, clamped to what the shifters handle (the whole element counts, not just its low byte).
int32_t shift_amount(uint64_t element, uint32_t w) {
  const int64_t v = sext(element, w);
  return v > 4096 ? 4096 : v < -4096 ? -4096 : static_cast<int32_t>(v);
}

uint64_t shift_by_element(I128 v, int32_t sh, uint32_t w, bool round, bool saturate, bool is_unsigned) {
  bool sat = false;
  if (sh >= 0) {
    if (saturate) {
      if (v == 0) return 0;
      if (sh >= static_cast<int32_t>(w)) return is_unsigned ? ones(w) : (v < 0 ? uint64_t{1} << (w - 1) : ones(w - 1));      // (any non-zero value overflows)
      const I128 shifted = v << sh;
      return is_unsigned ? saturate_to_unsigned(shifted, w, &sat) : saturate_to_signed(shifted, w, &sat);
    }
    return sh >= static_cast<int32_t>(w) ? 0 : (static_cast<uint64_t>(v) << sh) & ones(w);
  }
  return static_cast<uint64_t>(shift_right(v, static_cast<uint32_t>(-sh), round)) & ones(w);
}

// BEXT / BDEP / BGRP on one element.
uint64_t bit_extract(uint64_t x, uint64_t mask, uint32_t w) {
  uint64_t r = 0;
  uint32_t k = 0;
  for (uint32_t i = 0; i < w; ++i)
    if ((mask >> i) & 1) r |= ((x >> i) & 1) << k++;
  return r;
}
uint64_t bit_deposit(uint64_t x, uint64_t mask, uint32_t w) {
  uint64_t r = 0;
  uint32_t k = 0;
  for (uint32_t i = 0; i < w; ++i)
    if ((mask >> i) & 1) r |= ((x >> k++) & 1) << i;
  return r;
}
uint64_t bit_group(uint64_t x, uint64_t mask, uint32_t w) {
  uint32_t selected = 0;
  for (uint32_t i = 0; i < w; ++i) selected += (mask >> i) & 1;
  const uint64_t low = bit_extract(x, mask, w), high = bit_extract(x, ~mask & ones(w), w);
  return (low | (selected >= 64 ? 0 : high << selected)) & ones(w);
}

bool shll_fields(uint32_t insn, uint32_t* src_esz, uint32_t* amount, bool narrowing) {
  const uint32_t tsz = ((insn >> 22) & 1) << 2 | bits_of(insn, 20, 19), imm3 = bits_of(insn, 18, 16);
  if (tsz == 0) return false;
  uint32_t z = 2;
  while (!((tsz >> z) & 1)) --z;
  *src_esz = z;
  const uint32_t combined = (tsz << 3) | imm3, esize = 8u << z;
  *amount = narrowing ? 2 * esize - combined : combined - esize;
  return true;
}

// Four-way (or, for the two-way forms, two-way) dot product of the elements of `n` and `m` that sit under element `index` of the wider destination.
int64_t dot_sum(const Vec& n, const Vec& m, uint32_t narrow, uint32_t group, uint32_t index, bool n_signed, bool m_signed, uint32_t m_group_base) {
  int64_t sum = 0;
  const uint32_t w = element_bits(narrow);
  for (uint32_t k = 0; k < group; ++k) {
    const uint64_t x = get(n, narrow, index * group + k), y = get(m, narrow, m_group_base + k);
    const int64_t sx = n_signed ? sext(x, w) : static_cast<int64_t>(x), sy = m_signed ? sext(y, w) : static_cast<int64_t>(y);
    sum += sx * sy;
  }
  return sum;
}

}  // namespace

bool AArch64Cpu::sve2(uint32_t insn) {
  const bool is45 = (insn >> 24) & 1;
  const uint32_t size = bits_of(insn, 23, 22), esz = size, elements = element_count(esz);
  const uint32_t zd = insn & 31, zn = bits_of(insn, 9, 5), zm = bits_of(insn, 20, 16), op = bits_of(insn, 15, 10), group = bits_of(insn, 15, 13);
  const bool b21 = (insn >> 21) & 1;
  Vec* const dst = &s_.v[zd];
  const uint32_t w = element_bits(esz);

  if (!is45) {
    if (!b21) {
      if (group == 4) {      // predicated binary: halving, saturating and rounding arithmetic and shifts by element
        const uint16_t gp = s_.p[bits_of(insn, 12, 10)];
        const uint32_t opc = bits_of(insn, 20, 16);
        const Vec b = s_.v[zn];
        if (opc < 2 || (opc > 3 && opc < 6)) return false;
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          uint64_t x = get(*dst, esz, i), y = get(b, esz, i);
          if ((opc & 4) && opc < 0x10 && ((opc & 3) <= 3)) {}      // (shift-reversed forms handled below)
          uint64_t r;
          bool sat = false;
          bool reversed = false;
          switch (opc) {
            case 0x02: case 0x03: case 0x06: case 0x07: {      // SRSHL URSHL SRSHLR URSHLR
              reversed = opc >= 6;
              if (reversed) { const uint64_t t = x; x = y; y = t; }
              const bool u = (opc & 1) != 0;
              r = shift_by_element(u ? unsigned_of(x, w) : signed_of(x, w), shift_amount(y, w), w, true, false, u);
              break;
            }
            case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {      // SQSHL UQSHL SQRSHL UQRSHL and the reversed forms
              reversed = opc >= 0x0C;
              if (reversed) { const uint64_t t = x; x = y; y = t; }
              const bool u = (opc & 1) != 0, round = (opc & 2) != 0;
              r = shift_by_element(u ? unsigned_of(x, w) : signed_of(x, w), shift_amount(y, w), w, round, true, u);
              break;
            }
            case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: {      // SHADD UHADD SHSUB UHSUB SRHADD URHADD SHSUBR UHSUBR
              const bool u = (opc & 1) != 0;
              const uint32_t kind = opc >> 1;      // 8 add, 9 sub, 10 round-add, 11 reversed sub
              if (kind == 0xB) { const uint64_t t = x; x = y; y = t; }
              const I128 a = u ? unsigned_of(x, w) : signed_of(x, w), c = u ? unsigned_of(y, w) : signed_of(y, w);
              I128 v;
              if (kind == 8) v = (a + c) >> 1;
              else if (kind == 0xA) v = (a + c + 1) >> 1;
              else v = (a - c) >> 1;
              r = static_cast<uint64_t>(v) & ones(w);
              break;
            }
            case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1E: case 0x1F: {      // SQADD UQADD SQSUB UQSUB SQSUBR UQSUBR
              const bool u = (opc & 1) != 0;
              if (opc >= 0x1E) { const uint64_t t = x; x = y; y = t; }
              const bool sub = opc >= 0x1A;
              const I128 a = u ? unsigned_of(x, w) : signed_of(x, w), c = u ? unsigned_of(y, w) : signed_of(y, w);
              const I128 v = sub ? a - c : a + c;
              r = u ? saturate_to_unsigned(v, w, &sat) : saturate_to_signed(v, w, &sat);
              break;
            }
            case 0x1C: {      // SUQADD: signed accumulator plus an unsigned operand, saturating to signed
              r = saturate_to_signed(signed_of(x, w) + unsigned_of(y, w), w, &sat);
              break;
            }
            case 0x1D: {      // USQADD
              r = saturate_to_unsigned(unsigned_of(x, w) + signed_of(y, w), w, &sat);
              break;
            }
            default: return false;
          }
          put(dst, esz, i, r);
        }
        return true;
      }
      if (group == 5) {      // predicated unary and pairwise
        const uint16_t gp = s_.p[bits_of(insn, 12, 10)];
        const uint32_t opc = bits_of(insn, 20, 16);
        const Vec src = s_.v[zn];
        if (opc == 0 || opc == 1) {      // URECPE, URSQRTE (32-bit only)
          if (esz != 2) return false;
          for (uint32_t i = 0; i < elements; ++i) {
            if (!active(gp, i, 2)) continue;
            const uint32_t v = static_cast<uint32_t>(get(src, 2, i));
            uint32_t res;
            if (opc == 0) res = (v >> 31) ? (recip_estimate(v >> 23) & 0x1FF) << 23 : 0xFFFFFFFFu;
            else res = (v >> 30) ? (recip_sqrt_estimate(v >> 23) & 0x1FF) << 23 : 0xFFFFFFFFu;
            put(dst, 2, i, res);
          }
          return true;
        }
        if (opc == 4 || opc == 5) {      // SADALP, UADALP
          if (esz == 0) return false;
          for (uint32_t i = 0; i < elements; ++i) {
            if (!active(gp, i, esz)) continue;
            const uint32_t nw = element_bits(esz - 1);
            const uint64_t a = get(src, esz - 1, 2 * i), c = get(src, esz - 1, 2 * i + 1);
            const uint64_t sum = opc == 4 ? static_cast<uint64_t>(sext(a, nw) + sext(c, nw)) : a + c;
            put(dst, esz, i, (get(*dst, esz, i) + sum) & ones(w));
          }
          return true;
        }
        if (opc == 8 || opc == 9) {      // SQABS, SQNEG
          for (uint32_t i = 0; i < elements; ++i) {
            if (!active(gp, i, esz)) continue;
            bool sat = false;
            const I128 x = signed_of(get(src, esz, i), w);
            put(dst, esz, i, saturate_to_signed(opc == 8 ? (x < 0 ? -x : x) : -x, w, &sat));
          }
          return true;
        }
        if (opc == 0x11 || (opc >= 0x14 && opc <= 0x17)) {      // ADDP SMAXP UMAXP SMINP UMINP: the destination is also the first operand, Zm in bits 9:5
          const Vec first = *dst, second = src;
          for (uint32_t i = 0; i < elements; ++i) {
            if (!active(gp, i, esz)) continue;
            const Vec& v = (i & 1) ? second : first;
            const uint64_t x = get(v, esz, i & ~1u), y = get(v, esz, i | 1u);
            uint64_t r;
            switch (opc) {
              case 0x11: r = (x + y) & ones(w); break;
              case 0x14: r = sext(x, w) > sext(y, w) ? x : y; break;
              case 0x15: r = x > y ? x : y; break;
              case 0x16: r = sext(x, w) < sext(y, w) ? x : y; break;
              default: r = x < y ? x : y; break;
            }
            put(dst, esz, i, r);
          }
          return true;
        }
        return false;
      }
      // unpredicated three-register forms
      const Vec n = s_.v[zn], m = s_.v[zm];
      if (op == 0x00 || op == 0x01) {      // SDOT, UDOT (vectors): size 10 .s from bytes, 11 .d from halfwords
        if (esz < 2) return false;
        for (uint32_t i = 0; i < elements; ++i) {
          const int64_t sum = dot_sum(n, m, esz - 2, 4, i, op == 0, op == 0, i * 4);
          put(dst, esz, i, (get(*dst, esz, i) + static_cast<uint64_t>(sum)) & ones(w));
        }
        return true;
      }
      if (op == 0x1E) {      // USDOT (vectors)
        if (esz != 2) return false;
        for (uint32_t i = 0; i < elements; ++i) put(dst, 2, i, (get(*dst, 2, i) + static_cast<uint64_t>(dot_sum(n, m, 0, 4, i, false, true, i * 4))) & 0xFFFFFFFFu);
        return true;
      }
      if (op == 0x32 || op == 0x33) {      // SDOT, UDOT (two-way, halfwords into words)
        if (esz != 0) return false;
        for (uint32_t i = 0; i < 4; ++i) put(dst, 2, i, (get(*dst, 2, i) + static_cast<uint64_t>(dot_sum(n, m, 1, 2, i, op == 0x32, op == 0x32, i * 2))) & 0xFFFFFFFFu);
        return true;
      }
      if (op == 0x02 || op == 0x03) {      // SQDMLALBT, SQDMLSLBT
        if (esz == 0) return false;
        for (uint32_t i = 0; i < elements; ++i) {
          const uint32_t nw = element_bits(esz - 1);
          bool sat = false;
          const int64_t prod = sext(get(n, esz - 1, 2 * i), nw) * sext(get(m, esz - 1, 2 * i + 1), nw);
          const I128 doubled = static_cast<I128>(prod) + static_cast<I128>(prod);
          const I128 p = static_cast<I128>(static_cast<int64_t>(saturate_to_signed(doubled, w, &sat) << (64 - w)) >> (64 - w));
          put(dst, esz, i, saturate_to_signed(signed_of(get(*dst, esz, i), w) + (op == 3 ? -p : p), w, &sat));
        }
        return true;
      }
      if ((op & 0x3C) == 0x04 || (op & 0x3C) == 0x08 || (op & 0x3C) == 0x0C || (op & 0x3C) == 0x0C) {      // CDOT (0x04..0x07), CMLA (0x08..0x0B), SQRDCMLAH (0x0C..0x0F)
        const uint32_t rot = op & 3;
        if ((op & 0x3C) == 0x04) {      // CDOT
          if (esz < 2) return false;
          const uint32_t narrow = esz - 2, nw = element_bits(narrow);
          for (uint32_t i = 0; i < elements; ++i) {
            int64_t sum = 0;
            for (uint32_t k = 0; k < 2; ++k) {
              const int64_t nr = sext(get(n, narrow, 4 * i + 2 * k), nw), ni = sext(get(n, narrow, 4 * i + 2 * k + 1), nw);
              const int64_t mr = sext(get(m, narrow, 4 * i + 2 * k), nw), mi = sext(get(m, narrow, 4 * i + 2 * k + 1), nw);
              switch (rot) {
                case 0: sum += nr * mr - ni * mi; break;
                case 1: sum += nr * mi + ni * mr; break;
                case 2: sum += nr * mr + ni * mi; break;
                default: sum += nr * mi - ni * mr; break;
              }
            }
            put(dst, esz, i, (get(*dst, esz, i) + static_cast<uint64_t>(sum)) & ones(w));
          }
          return true;
        }
        if (esz == 0 && false) return false;
        const bool saturating = (op & 0x3C) == 0x0C;
        for (uint32_t p = 0; p < elements / 2; ++p) {
          const int64_t ar = sext(get(n, esz, 2 * p), w), ai = sext(get(n, esz, 2 * p + 1), w), br = sext(get(m, esz, 2 * p), w), bi = sext(get(m, esz, 2 * p + 1), w);
          int64_t t1a, t1b, t2a, t2b;
          int s1, s2;
          switch (rot) {
            case 0: t1a = ar; t1b = br; s1 = 1; t2a = ar; t2b = bi; s2 = 1; break;
            case 1: t1a = ai; t1b = bi; s1 = -1; t2a = ai; t2b = br; s2 = 1; break;
            case 2: t1a = ar; t1b = br; s1 = -1; t2a = ar; t2b = bi; s2 = -1; break;
            default: t1a = ai; t1b = bi; s1 = 1; t2a = ai; t2b = br; s2 = -1; break;
          }
          const uint64_t accr = get(*dst, esz, 2 * p), acci = get(*dst, esz, 2 * p + 1);
          if (!saturating) {
            put(dst, esz, 2 * p, (accr + (s1 > 0 ? product_bits(t1a, t1b) : uint64_t{0} - product_bits(t1a, t1b))) & ones(w));
            put(dst, esz, 2 * p + 1, (acci + (s2 > 0 ? product_bits(t2a, t2b) : uint64_t{0} - product_bits(t2a, t2b))) & ones(w));
          } else {
            bool sat = false;
            const I128 r1 = doubling_high(signed_of(accr, w), (s1 > 0 ? multiply_signed(t1a, t1b) : -multiply_signed(t1a, t1b)), w, true);
            const I128 r2 = doubling_high(signed_of(acci, w), (s2 > 0 ? multiply_signed(t2a, t2b) : -multiply_signed(t2a, t2b)), w, true);
            put(dst, esz, 2 * p, saturate_to_signed(r1, w, &sat));
            put(dst, esz, 2 * p + 1, saturate_to_signed(r2, w, &sat));
          }
        }
        return true;
      }
      if (op >= 0x10 && op <= 0x1D) {      // widening multiply-add: SMLALB.. UMLSLT (0x10..0x17), SQDMLALB.. (0x18..0x1B), SQRDMLAH / SQRDMLSH (0x1C, 0x1D)
        if (op >= 0x1C) {
          const bool subtract = op == 0x1D;
          for (uint32_t i = 0; i < elements; ++i) {
            bool sat = false;
            const I128 acc = signed_of(get(*dst, esz, i), w);
            const I128 prod = multiply_signed(sext(get(n, esz, i), w), sext(get(m, esz, i), w));
            put(dst, esz, i, saturate_to_signed(doubling_high(acc, subtract ? -prod : prod, w, true), w, &sat));
          }
          return true;
        }
        if (esz == 0) return false;
        const uint32_t nw = element_bits(esz - 1);
        const bool top = (op & 1) != 0;
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t xa = get(n, esz - 1, 2 * i + top), xb = get(m, esz - 1, 2 * i + top);
          const uint64_t acc = get(*dst, esz, i);
          if (op <= 0x17) {
            const bool is_unsigned = (op & 2) != 0, subtract = op >= 0x14;
            const uint64_t prod = is_unsigned ? xa * xb : static_cast<uint64_t>(sext(xa, nw) * sext(xb, nw));
            put(dst, esz, i, (subtract ? acc - prod : acc + prod) & ones(w));
          } else {
            bool sat = false;
            const int64_t prod = sext(xa, nw) * sext(xb, nw);
            const I128 doubled = static_cast<I128>(prod) + static_cast<I128>(prod);
            const I128 p = static_cast<I128>(static_cast<int64_t>(saturate_to_signed(doubled, w, &sat) << (64 - w)) >> (64 - w));
            const bool subtract = op >= 0x1A;
            put(dst, esz, i, saturate_to_signed(signed_of(acc, w) + (subtract ? -p : p), w, &sat));
          }
        }
        return true;
      }
      if (op == 0x30 || op == 0x31) {      // SCLAMP, UCLAMP
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t x = get(*dst, esz, i), lo = get(n, esz, i), hi = get(m, esz, i);
          uint64_t r;
          if (op == 0x30) {
            int64_t v = sext(x, w);
            if (v < sext(lo, w)) v = sext(lo, w);
            if (v > sext(hi, w)) v = sext(hi, w);
            r = static_cast<uint64_t>(v) & ones(w);
          } else {
            r = x < lo ? lo : x;
            if (r > hi) r = hi;
          }
          put(dst, esz, i, r);
        }
        return true;
      }
      if (op == 0x38 || op == 0x39 || op == 0x3A || op == 0x3B) {      // ZIPQ1, ZIPQ2, UZPQ1, UZPQ2 (one 128-bit segment: the same as ZIP / UZP)
        Vec r;
        const uint32_t half = elements / 2;
        for (uint32_t i = 0; i < half; ++i) {
          if (op == 0x38) { put(&r, esz, 2 * i, get(n, esz, i)); put(&r, esz, 2 * i + 1, get(m, esz, i)); }
          else if (op == 0x39) { put(&r, esz, 2 * i, get(n, esz, half + i)); put(&r, esz, 2 * i + 1, get(m, esz, half + i)); }
        }
        if (op >= 0x3A) {
          for (uint32_t i = 0; i < elements; ++i) {
            const uint32_t src = 2 * i + (op == 0x3B ? 1 : 0);
            put(&r, esz, i, src < elements ? get(n, esz, src) : get(m, esz, src - elements));
          }
        }
        *dst = r;
        return true;
      }
      if (op == 0x3E) {      // TBLQ
        const Vec t = n;
        Vec r;
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t idx = get(m, esz, i);
          put(&r, esz, i, idx < elements ? get(t, esz, static_cast<uint32_t>(idx)) : 0);
        }
        *dst = r;
        return true;
      }
      return false;
    }

    // indexed forms (bit 21 set)
    uint32_t zmr, index;
    // the encodings put the index and the Zm number in different places for each element size
    auto narrow_index = [&](uint32_t sz) {
      if (sz == 3) { zmr = bits_of(insn, 19, 16); index = (insn >> 20) & 1; }      // .d destination: 1-bit index
      else { zmr = bits_of(insn, 18, 16); index = bits_of(insn, 20, 19); }
    };
    if ((op == 0x32 || op == 0x33) && size == 2 && !((insn >> 21) & 1)) {      // SDOT, UDOT (two-way, halfwords into words, by element)
      const Vec n = s_.v[zn], m = s_.v[bits_of(insn, 18, 16)];
      const uint32_t index = bits_of(insn, 20, 19);
      for (uint32_t i = 0; i < 4; ++i) put(dst, 2, i, (get(*dst, 2, i) + static_cast<uint64_t>(dot_sum(n, m, 1, 2, i, op == 0x32, op == 0x32, index * 2))) & 0xFFFFFFFFu);
      return true;
    }
    if (op == 0x00 || op == 0x01 || op == 0x06 || op == 0x07) {      // SDOT UDOT USDOT SUDOT (by element)
      if (esz < 2) return false;
      narrow_index(esz);
      const Vec n = s_.v[zn], m = s_.v[zmr];
      const uint32_t narrow = esz - 2, per = 4;
      if ((op == 0x06 || op == 0x07) && esz != 2) return false;
      for (uint32_t i = 0; i < elements; ++i) {
        int64_t sum;
        if (op == 0x00) sum = dot_sum(n, m, narrow, per, i, true, true, index * per);
        else if (op == 0x01) sum = dot_sum(n, m, narrow, per, i, false, false, index * per);
        else if (op == 0x06) sum = dot_sum(n, m, narrow, per, i, false, true, index * per);
        else sum = dot_sum(n, m, narrow, per, i, true, false, index * per);
        put(dst, esz, i, (get(*dst, esz, i) + static_cast<uint64_t>(sum)) & ones(w));
      }
      return true;
    }
    if (op == 0x02 || op == 0x03 || op == 0x04 || op == 0x05 || op == 0x3C || op == 0x3D || op == 0x3E) {      // MLA MLS SQRDMLAH SQRDMLSH SQDMULH SQRDMULH MUL by element
      if (esz == 0 || esz == 1 + 0) {}
      if (esz == 3) { zmr = bits_of(insn, 19, 16); index = (insn >> 20) & 1; }
      else if (esz == 2) { zmr = bits_of(insn, 18, 16); index = bits_of(insn, 20, 19); }
      else { zmr = bits_of(insn, 18, 16); index = ((size & 1) << 2) | bits_of(insn, 20, 19); }
      if (size == 0 && false) return false;
      const uint32_t e = size < 2 ? 1 : size;
      const uint32_t ew = element_bits(e);
      const Vec n = s_.v[zn], m = s_.v[zmr];
      const uint64_t y = get(m, e, index);
      for (uint32_t i = 0; i < element_count(e); ++i) {
        const uint64_t x = get(n, e, i), acc = get(*dst, e, i);
        bool sat = false;
        uint64_t r;
        switch (op) {
          case 0x02: r = (acc + x * y) & ones(ew); break;
          case 0x03: r = (acc - x * y) & ones(ew); break;
          case 0x3E: r = (x * y) & ones(ew); break;
          case 0x04: case 0x05: {
            const I128 prod = multiply_signed(sext(x, ew), sext(y, ew));
            r = saturate_to_signed(doubling_high(signed_of(acc, ew), op == 0x05 ? -prod : prod, ew, true), ew, &sat);
            break;
          }
          case 0x3C: r = saturate_to_signed(doubling_high(0, multiply_signed(sext(x, ew), sext(y, ew)), ew, false), ew, &sat); break;
          default: r = saturate_to_signed(doubling_high(0, multiply_signed(sext(x, ew), sext(y, ew)), ew, true), ew, &sat); break;
        }
        put(dst, e, i, r);
      }
      return true;
    }
    // widening multiplies by element: bits 15:12 select the operation, bit 11 is the low index bit and bit 10 selects top
    {
      const uint32_t kind = bits_of(insn, 15, 12), top = (insn >> 10) & 1;
      if (kind == 2 || kind == 3 || kind == 8 || kind == 9 || kind == 10 || kind == 11 || kind == 12 || kind == 13 || kind == 14) {
        if (esz < 2) return false;
        uint32_t zmr2, idx;
        if (esz == 3) { zmr2 = bits_of(insn, 19, 16); idx = ((insn >> 20) & 1) << 1 | ((insn >> 11) & 1); }
        else { zmr2 = bits_of(insn, 18, 16); idx = bits_of(insn, 20, 19) << 1 | ((insn >> 11) & 1); }
        const uint32_t nw = element_bits(esz - 1);
        const Vec n = s_.v[zn], m = s_.v[zmr2];
        const uint64_t yb = get(m, esz - 1, idx);
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t xa = get(n, esz - 1, 2 * i + top);
          const uint64_t acc = get(*dst, esz, i);
          bool sat = false;
          uint64_t r;
          switch (kind) {
            case 8: case 9: case 10: case 11: {      // SMLAL UMLAL SMLSL UMLSL
              const bool is_unsigned = kind == 9 || kind == 11, subtract = kind >= 10;
              const uint64_t prod = is_unsigned ? xa * yb : static_cast<uint64_t>(sext(xa, nw) * sext(yb, nw));
              r = (subtract ? acc - prod : acc + prod) & ones(w);
              break;
            }
            case 12: r = static_cast<uint64_t>(sext(xa, nw) * sext(yb, nw)) & ones(w); break;      // SMULL
            case 13: r = (xa * yb) & ones(w); break;                                              // UMULL
            case 14: {      // SQDMULL
              const int64_t prod = sext(xa, nw) * sext(yb, nw);
              const I128 doubled = static_cast<I128>(prod) + static_cast<I128>(prod);
              r = saturate_to_signed(doubled, w, &sat);
              break;
            }
            default: {      // SQDMLAL, SQDMLSL
              const int64_t prod = sext(xa, nw) * sext(yb, nw);
              const I128 doubled = static_cast<I128>(prod) + static_cast<I128>(prod);
              const I128 p = static_cast<I128>(static_cast<int64_t>(saturate_to_signed(doubled, w, &sat) << (64 - w)) >> (64 - w));
              r = saturate_to_signed(signed_of(acc, w) + (kind == 3 ? -p : p), w, &sat);
              break;
            }
          }
          put(dst, esz, i, r);
        }
        return true;
      }
      if (kind == 4 || kind == 6 || kind == 7) {      // CDOT, CMLA, SQRDCMLAH by element
        const uint32_t rot = bits_of(insn, 11, 10);
        uint32_t zmr2, idx;
        if (esz == 3) { zmr2 = bits_of(insn, 19, 16); idx = (insn >> 20) & 1; }
        else { zmr2 = bits_of(insn, 18, 16); idx = bits_of(insn, 20, 19); }
        if (esz < 2) return false;
        const Vec n = s_.v[zn], m = s_.v[zmr2];
        if (kind == 4) {
          const uint32_t narrow = esz - 2, nw = element_bits(narrow);
          for (uint32_t i = 0; i < elements; ++i) {
            int64_t sum = 0;
            for (uint32_t k = 0; k < 2; ++k) {
              const int64_t nr = sext(get(n, narrow, 4 * i + 2 * k), nw), ni = sext(get(n, narrow, 4 * i + 2 * k + 1), nw);
              const int64_t mr = sext(get(m, narrow, 4 * idx + 2 * k), nw), mi = sext(get(m, narrow, 4 * idx + 2 * k + 1), nw);
              switch (rot) {
                case 0: sum += nr * mr - ni * mi; break;
                case 1: sum += nr * mi + ni * mr; break;
                case 2: sum += nr * mr + ni * mi; break;
                default: sum += nr * mi - ni * mr; break;
              }
            }
            put(dst, esz, i, (get(*dst, esz, i) + static_cast<uint64_t>(sum)) & ones(w));
          }
          return true;
        }
        // CMLA / SQRDCMLAH: the element size is one step narrower than the field says (z.h for size 10, z.s for size 11)
        const uint32_t e = esz - 1, ew = element_bits(e);
        const bool saturating = kind == 7;
        for (uint32_t p = 0; p < element_count(e) / 2; ++p) {
          const int64_t ar = sext(get(n, e, 2 * p), ew), ai = sext(get(n, e, 2 * p + 1), ew), br = sext(get(m, e, 2 * idx), ew), bi = sext(get(m, e, 2 * idx + 1), ew);
          int64_t t1a, t1b, t2a, t2b;
          int s1, s2;
          switch (rot) {
            case 0: t1a = ar; t1b = br; s1 = 1; t2a = ar; t2b = bi; s2 = 1; break;
            case 1: t1a = ai; t1b = bi; s1 = -1; t2a = ai; t2b = br; s2 = 1; break;
            case 2: t1a = ar; t1b = br; s1 = -1; t2a = ar; t2b = bi; s2 = -1; break;
            default: t1a = ai; t1b = bi; s1 = 1; t2a = ai; t2b = br; s2 = -1; break;
          }
          const uint64_t accr = get(*dst, e, 2 * p), acci = get(*dst, e, 2 * p + 1);
          if (!saturating) {
            put(dst, e, 2 * p, (accr + (s1 > 0 ? product_bits(t1a, t1b) : uint64_t{0} - product_bits(t1a, t1b))) & ones(ew));
            put(dst, e, 2 * p + 1, (acci + (s2 > 0 ? product_bits(t2a, t2b) : uint64_t{0} - product_bits(t2a, t2b))) & ones(ew));
          } else {
            bool sat = false;
            const I128 r1 = doubling_high(signed_of(accr, ew), (s1 > 0 ? multiply_signed(t1a, t1b) : -multiply_signed(t1a, t1b)), ew, true);
            const I128 r2 = doubling_high(signed_of(acci, ew), (s2 > 0 ? multiply_signed(t2a, t2b) : -multiply_signed(t2a, t2b)), ew, true);
            put(dst, e, 2 * p, saturate_to_signed(r1, ew, &sat));
            put(dst, e, 2 * p + 1, saturate_to_signed(r2, ew, &sat));
          }
        }
        return true;
      }
    }
    return false;
  }

  // 0x45
  if (!b21) {
    if (op <= 0x1F && (op < 0x08 || op > 0x0B)) {      // widening add / subtract / absolute difference / multiply, bottom and top
      const uint32_t src_esz = esz - 1;
      if (op == 0x1A || op == 0x1B) {      // PMULLB / PMULLT
        const Vec n = s_.v[zn], m = s_.v[zm];
        const bool top = op == 0x1B;
        Vec r;
        if (esz == 0) {      // 128-bit result from 64-bit sources
          uint64_t lo, hi;
          crypto_clmul(top ? n.hi : n.lo, top ? m.hi : m.lo, &lo, &hi);
          *dst = Vec{lo, hi};
          return true;
        }
        if (esz == 2) return false;
        const uint32_t sw = element_bits(src_esz);
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t a = get(n, src_esz, 2 * i + top), b = get(m, src_esz, 2 * i + top);
          uint64_t lo, hi;
          crypto_clmul(a, b, &lo, &hi);
          (void)sw;
          put(&r, esz, i, esz == 3 ? lo & 0xFFFFFFFFFFFFFFFFull : lo & ones(w));
        }
        *dst = r;
        return true;
      }
      if (esz == 0) return false;
      const uint32_t nw = element_bits(src_esz);
      const Vec n = s_.v[zn], m = s_.v[zm];
      const bool top = (op & 1) != 0;
      Vec r;
      for (uint32_t i = 0; i < elements; ++i) {
        const uint64_t base = get(n, esz, i);      // (the wide operand of the "w" forms)
        const uint64_t a = get(n, src_esz, 2 * i + top), b = get(m, src_esz, 2 * i + top);
        uint64_t v;
        bool sat = false;
        switch (op >> 1) {
          case 0: v = static_cast<uint64_t>(sext(a, nw) + sext(b, nw)); break;      // SADDL
          case 1: v = a + b; break;                                                // UADDL
          case 2: v = static_cast<uint64_t>(sext(a, nw) - sext(b, nw)); break;      // SSUBL
          case 3: v = a - b; break;                                                // USUBL
          case 6: { const int64_t x = sext(a, nw), y = sext(b, nw); v = static_cast<uint64_t>(x > y ? x - y : y - x); break; }      // SABDL
          case 7: v = a > b ? a - b : b - a; break;                                // UABDL
          case 8: v = base + static_cast<uint64_t>(sext(m.lo * 0 + get(m, src_esz, 2 * i + top), nw)); break;      // SADDW
          case 9: v = base + get(m, src_esz, 2 * i + top); break;                  // UADDW
          case 10: v = base - static_cast<uint64_t>(sext(get(m, src_esz, 2 * i + top), nw)); break;      // SSUBW
          case 11: v = base - get(m, src_esz, 2 * i + top); break;                 // USUBW
          case 12: {      // SQDMULL
            const int64_t prod = sext(a, nw) * sext(b, nw);
            v = saturate_to_signed(static_cast<I128>(prod) + static_cast<I128>(prod), w, &sat);
            break;
          }
          case 14: v = static_cast<uint64_t>(sext(a, nw) * sext(b, nw)); break;      // SMULL
          default: v = a * b; break;                                                 // UMULL
        }
        if (op >= 0x10 && op < 0x18) {      // the wide-operand forms read the second operand from the narrow elements
          const uint64_t s = get(m, src_esz, 2 * i + top);
          switch (op >> 1) {
            case 8: v = base + static_cast<uint64_t>(sext(s, nw)); break;
            case 9: v = base + s; break;
            case 10: v = base - static_cast<uint64_t>(sext(s, nw)); break;
            default: v = base - s; break;
          }
        }
        put(&r, esz, i, v & ones(w));
      }
      *dst = r;
      return true;
    }
    switch (op) {
      case 0x20: case 0x22: case 0x23: {      // SADDLBT, SSUBLBT, SSUBLTB
        if (esz == 0) return false;
        const uint32_t src_esz = esz - 1, nw = element_bits(src_esz);
        const Vec n = s_.v[zn], m = s_.v[zm];
        Vec r;
        for (uint32_t i = 0; i < elements; ++i) {
          const int64_t a = sext(get(n, src_esz, 2 * i + (op == 0x23 ? 1 : 0)), nw), b = sext(get(m, src_esz, 2 * i + (op == 0x23 ? 0 : 1)), nw);
          put(&r, esz, i, static_cast<uint64_t>(op == 0x20 ? a + b : a - b) & ones(w));
        }
        *dst = r;
        return true;
      }
      case 0x24: case 0x25: {      // EORBT, EORTB
        const Vec n = s_.v[zn], m = s_.v[zm];
        Vec r = *dst;
        for (uint32_t i = 0; i < elements / 2; ++i) {
          if (op == 0x24) put(&r, esz, 2 * i, get(n, esz, 2 * i) ^ get(m, esz, 2 * i + 1));
          else put(&r, esz, 2 * i + 1, get(n, esz, 2 * i + 1) ^ get(m, esz, 2 * i));
        }
        *dst = r;
        return true;
      }
      case 0x26: {      // SMMLA, USMMLA, UMMLA
        if (size == 1) return false;
        const Vec n = s_.v[zn], m = s_.v[zm], old = *dst;
        Vec r = old;
        for (uint32_t i = 0; i < 2; ++i)
          for (uint32_t j = 0; j < 2; ++j) {
            int64_t sum = static_cast<int64_t>(static_cast<int32_t>(get(old, 2, 2 * i + j)));
            for (uint32_t k = 0; k < 8; ++k) {
              const uint64_t x = get(n, 0, 8 * i + k), y = get(m, 0, 8 * j + k);
              const int64_t sx = (size == 3) ? static_cast<int64_t>(x) : (size == 2 ? static_cast<int64_t>(x) : sext(x, 8));
              const int64_t sy = size == 3 ? static_cast<int64_t>(y) : sext(y, 8);
              sum += sx * sy;
            }
            put(&r, 2, 2 * i + j, static_cast<uint64_t>(sum) & 0xFFFFFFFFu);
          }
        *dst = r;
        return true;
      }
      case 0x28: case 0x29: case 0x2A: case 0x2B: {      // SSHLLB, SSHLLT, USHLLB, USHLLT
        uint32_t src_esz, amount;
        if (((insn >> 23) & 1) || !shll_fields(insn, &src_esz, &amount, false) || src_esz > 2) return false;
        const uint32_t d = src_esz + 1, nw = element_bits(src_esz);
        const bool top = (op & 1) != 0, is_unsigned = op >= 0x2A;
        const Vec n = s_.v[zn];
        Vec r;
        for (uint32_t i = 0; i < element_count(d); ++i) {
          const uint64_t x = get(n, src_esz, 2 * i + top);
          put(&r, d, i, ((is_unsigned ? x : static_cast<uint64_t>(sext(x, nw))) << amount) & element_ones(d));
        }
        *dst = r;
        return true;
      }
      case 0x2C: case 0x2D: case 0x2E: {      // BEXT, BDEP, BGRP
        const Vec n = s_.v[zn], m = s_.v[zm];
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t x = get(n, esz, i), mask = get(m, esz, i);
          put(dst, esz, i, op == 0x2C ? bit_extract(x, mask, w) : op == 0x2D ? bit_deposit(x, mask, w) : bit_group(x, mask, w));
        }
        return true;
      }
      case 0x30: case 0x31: case 0x32: case 0x33: {      // SABALB, SABALT, UABALB, UABALT
        if (esz == 0) return false;
        const uint32_t src_esz = esz - 1, nw = element_bits(src_esz);
        const Vec n = s_.v[zn], m = s_.v[zm];
        const bool top = (op & 1) != 0, is_unsigned = op >= 0x32;
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t a = get(n, src_esz, 2 * i + top), b = get(m, src_esz, 2 * i + top);
          uint64_t d;
          if (is_unsigned) d = a > b ? a - b : b - a;
          else { const int64_t x = sext(a, nw), y = sext(b, nw); d = static_cast<uint64_t>(x > y ? x - y : y - x); }
          put(dst, esz, i, (get(*dst, esz, i) + d) & ones(w));
        }
        return true;
      }
      case 0x34: case 0x35: {      // ADCLB, ADCLT, SBCLB, SBCLT (the element size is 32 or 64 bits, selected by bit 22)
        const uint32_t e = (size & 1) ? 3 : 2, ew = element_bits(e);
        const bool subtract = (size >> 1) & 1, top = (op & 1) != 0;
        const Vec n = s_.v[zn], m = s_.v[zm], old = *dst;
        Vec r = old;
        for (uint32_t i = 0; i < element_count(e) / 2; ++i) {
          const uint64_t a = get(old, e, 2 * i);
          uint64_t b = get(n, e, 2 * i + top);
          if (subtract) b = ~b & ones(ew);
          const uint64_t carry = get(m, e, 2 * i + 1) & 1;
          const uint64_t sum = (a + b + carry) & ones(ew);
          const bool carry_out = ew == 64 ? (sum < a || (carry && sum == a) ) : ((a + b + carry) >> ew) != 0;
          put(&r, e, 2 * i, sum);
          put(&r, e, 2 * i + 1, carry_out ? 1 : 0);
        }
        *dst = r;
        return true;
      }
      case 0x36: case 0x37: {      // CADD, SQCADD
        if (bits_of(insn, 20, 17) != 0) return false;
        const bool saturating = (insn >> 16) & 1, rot270 = (insn >> 10) & 1;
        const Vec first = *dst, second = s_.v[zn];
        for (uint32_t p = 0; p < elements / 2; ++p) {
          bool sat = false;
          const I128 ar = signed_of(get(first, esz, 2 * p), w), ai = signed_of(get(first, esz, 2 * p + 1), w), br = signed_of(get(second, esz, 2 * p), w), bi = signed_of(get(second, esz, 2 * p + 1), w);
          const I128 re = rot270 ? ar + bi : ar - bi, im = rot270 ? ai - br : ai + br;
          put(dst, esz, 2 * p, saturating ? saturate_to_signed(re, w, &sat) : static_cast<uint64_t>(re) & ones(w));
          put(dst, esz, 2 * p + 1, saturating ? saturate_to_signed(im, w, &sat) : static_cast<uint64_t>(im) & ones(w));
        }
        return true;
      }
      case 0x38: case 0x39: case 0x3A: case 0x3B: {      // SSRA USRA SRSRA URSRA
        uint32_t e, amount;
        const uint32_t tsz = (size << 2) | bits_of(insn, 20, 19), imm3 = bits_of(insn, 18, 16);
        if (tsz == 0) return false;
        uint32_t z = 3;
        while (!((tsz >> z) & 1)) --z;
        e = z;
        amount = 2 * (8u << z) - ((tsz << 3) | imm3);
        const uint32_t ew = element_bits(e);
        const Vec n = s_.v[zn];
        const bool is_unsigned = (op & 1) != 0, round = (op & 2) != 0;
        for (uint32_t i = 0; i < element_count(e); ++i) {
          const uint64_t x = get(n, e, i);
          const I128 v = is_unsigned ? unsigned_of(x, ew) : signed_of(x, ew);
          put(dst, e, i, (get(*dst, e, i) + static_cast<uint64_t>(shift_right(v, amount, round))) & ones(ew));
        }
        return true;
      }
      case 0x3C: case 0x3D: {      // SRI, SLI
        const uint32_t tsz = (size << 2) | bits_of(insn, 20, 19), imm3 = bits_of(insn, 18, 16);
        if (tsz == 0) return false;
        uint32_t z = 3;
        while (!((tsz >> z) & 1)) --z;
        const uint32_t e = z, ew = 8u << z, combined = (tsz << 3) | imm3;
        const Vec n = s_.v[zn];
        const bool left = op == 0x3D;
        const uint32_t amount = left ? combined - ew : 2 * ew - combined;
        for (uint32_t i = 0; i < element_count(e); ++i) {
          const uint64_t x = get(n, e, i), old = get(*dst, e, i);
          uint64_t r;
          if (left) {
            const uint64_t keep = ones(amount);
            r = ((x << amount) & ones(ew)) | (old & keep);
          } else {
            const uint64_t mask = amount >= ew ? 0 : ones(ew) >> amount;      // the bits that come from the shifted source
            r = (amount >= ew ? 0 : x >> amount) | (old & ~mask & ones(ew));
          }
          put(dst, e, i, r & ones(ew));
        }
        return true;
      }
      case 0x3E: case 0x3F: {      // SABA, UABA
        const Vec n = s_.v[zn], m = s_.v[zm];
        for (uint32_t i = 0; i < elements; ++i) {
          const uint64_t a = get(n, esz, i), b = get(m, esz, i);
          uint64_t d;
          if (op == 0x3F) d = a > b ? a - b : b - a;
          else { const int64_t x = sext(a, w), y = sext(b, w); d = static_cast<uint64_t>(x > y ? x - y : y - x); }
          put(dst, esz, i, (get(*dst, esz, i) + d) & ones(w));
        }
        return true;
      }
      default: break;
    }
    // MATCH / NMATCH / HISTCNT do not belong here (bit 21 is set for them)
    return false;
  }

  // 0x45 with bit 21 set
  if (((insn >> 23) & 1) && ((insn >> 21) & 1) && ((insn >> 20) & 1) && !((insn >> 22) & 1) && (op == 0x00 || op == 0x02 || op == 0x04 || op == 0x08 || op == 0x0A || op == 0x0E) && !((insn >> 5) & 1)) {      // SQSHRN .. UQRSHRN from a pair of vectors
    const uint32_t shift = 16 - bits_of(insn, 19, 16);
    const Vec n0 = s_.v[(zn & ~1u)], n1 = s_.v[(zn | 1u) & 31];
    Vec r;
    for (uint32_t i = 0; i < 8; ++i) {      // (the results interleave: even halfwords from the first vector, odd from the second)
      const Vec& src = (i & 1) ? n1 : n0;
      const uint64_t x = get(src, 2, i >> 1);
      bool sat = false;
      const I128 sx = signed_of(x, 32), ux = unsigned_of(x, 32);
      uint64_t v;
      switch (op) {
        case 0x00: v = saturate_to_signed(shift_right(sx, shift, false), 16, &sat); break;      // SQSHRN
        case 0x02: v = saturate_to_unsigned(shift_right(sx, shift, true), 16, &sat); break;     // SQRSHRUN
        case 0x04: v = saturate_to_unsigned(shift_right(ux, shift, false), 16, &sat); break;   // UQSHRN
        case 0x08: v = saturate_to_unsigned(shift_right(sx, shift, false), 16, &sat); break;   // SQSHRUN
        case 0x0A: v = saturate_to_signed(shift_right(sx, shift, true), 16, &sat); break;      // SQRSHRN
        default: v = saturate_to_unsigned(shift_right(ux, shift, true), 16, &sat); break;      // UQRSHRN
      }
      put(&r, 1, i, v);
    }
    *dst = r;
    return true;
  }
  if (bits_of(insn, 23, 16) == 0x31 && (op == 0x10 || op == 0x12 || op == 0x14) && !((insn >> 5) & 1)) {      // SQCVTN, UQCVTN, SQCVTUN from a pair of vectors
    const Vec n0 = s_.v[(zn & ~1u)], n1 = s_.v[(zn | 1u) & 31];
    Vec r;
    for (uint32_t i = 0; i < 8; ++i) {
      const uint64_t x = get((i & 1) ? n1 : n0, 2, i >> 1);
      bool sat = false;
      put(&r, 1, i, op == 0x10 ? saturate_to_signed(signed_of(x, 32), 16, &sat) : op == 0x12 ? saturate_to_unsigned(unsigned_of(x, 32), 16, &sat) : saturate_to_unsigned(signed_of(x, 32), 16, &sat));
    }
    *dst = r;
    return true;
  }
  if (group == 4 && op != 0x28) {      // MATCH, NMATCH (bits 15:13 = 100): the predicate result is in bits 3:0
    if (esz > 1) return false;
    const uint16_t gp = s_.p[bits_of(insn, 12, 10)];
    const Vec n = s_.v[zn], m = s_.v[zm];
    const bool negate = (insn >> 4) & 1;
    uint16_t r = 0;
    for (uint32_t i = 0; i < elements; ++i) {
      if (!active(gp, i, esz)) continue;
      bool found = false;
      for (uint32_t j = 0; j < elements; ++j)
        if (get(m, esz, j) == get(n, esz, i)) found = true;
      if (found != negate) r = static_cast<uint16_t>(r | (1u << (i << esz)));
    }
    s_.p[insn & 15] = r;
    sve_set_flags(gp, r, esz);
    return true;
  }
  if (group == 6 && ((insn >> 23) & 1)) {      // HISTCNT
    const uint16_t gp = s_.p[bits_of(insn, 12, 10)];
    const Vec n = s_.v[zn], m = s_.v[zm];
    const uint32_t e = (size & 1) ? 3 : 2;
    Vec r;
    for (uint32_t i = 0; i < element_count(e); ++i) {
      if (!active(gp, i, e)) continue;
      uint64_t count = 0;
      for (uint32_t j = 0; j <= i; ++j)
        if (active(gp, j, e) && get(m, e, j) == get(n, e, i)) ++count;
      put(&r, e, i, count);
    }
    *dst = r;
    return true;
  }
  if (op == 0x28 && size == 0) {      // HISTSEG
    const Vec n = s_.v[zn], m = s_.v[zm];
    Vec r;
    for (uint32_t i = 0; i < 16; ++i) {
      uint64_t count = 0;
      for (uint32_t j = 0; j < 16; ++j)
        if (get(m, 0, j) == get(n, 0, i)) ++count;
      put(&r, 0, i, count);
    }
    *dst = r;
    return true;
  }
  if ((op <= 0x0F || (op >= 0x10 && op <= 0x15)) && !((insn >> 23) & 1)) {      // narrowing shifts and saturating extracts
    uint32_t src_esz, amount;
    if (op <= 0x0F) {
      if (!shll_fields(insn, &src_esz, &amount, true) || src_esz > 2) return false;
    } else {
      if (bits_of(insn, 18, 16) != 0) return false;
      amount = 0;
      const uint32_t tsz = ((insn >> 22) & 1) << 2 | bits_of(insn, 20, 19);
      if (tsz == 0) return false;
      uint32_t z = 2;
      while (!((tsz >> z) & 1)) --z;
      src_esz = z;
      if (src_esz > 2) return false;
    }
    const uint32_t s = src_esz + 1, sw = element_bits(s), dw = element_bits(src_esz);
    const Vec n = s_.v[zn];
    const bool top = (op & 1) != 0;
    Vec r = top ? *dst : Vec{};
    for (uint32_t i = 0; i < element_count(s); ++i) {
      const uint64_t x = get(n, s, i);
      bool sat = false;
      uint64_t v;
      const I128 sx = signed_of(x, sw), ux = unsigned_of(x, sw);
      switch (op >> 1) {
        case 0: v = saturate_to_unsigned(shift_right(sx, amount, false), dw, &sat); break;      // SQSHRUN
        case 1: v = saturate_to_unsigned(shift_right(sx, amount, true), dw, &sat); break;       // SQRSHRUN
        case 2: v = static_cast<uint64_t>(shift_right(ux, amount, false)) & ones(dw); break;    // SHRN
        case 3: v = static_cast<uint64_t>(shift_right(ux, amount, true)) & ones(dw); break;     // RSHRN
        case 4: v = saturate_to_signed(shift_right(sx, amount, false), dw, &sat); break;        // SQSHRN
        case 5: v = saturate_to_signed(shift_right(sx, amount, true), dw, &sat); break;         // SQRSHRN
        case 6: v = saturate_to_unsigned(shift_right(ux, amount, false), dw, &sat); break;      // UQSHRN
        case 7: v = saturate_to_unsigned(shift_right(ux, amount, true), dw, &sat); break;       // UQRSHRN
        default: v = 0; break;
      }
      if (op >= 0x10) {      // SQXTN, UQXTN, SQXTUN
        switch ((op - 0x10) >> 1) {
          case 0: v = saturate_to_signed(sx, dw, &sat); break;
          case 1: v = saturate_to_unsigned(ux, dw, &sat); break;
          default: v = saturate_to_unsigned(sx, dw, &sat); break;
        }
      }
      put(&r, src_esz, 2 * i + top, v);
    }
    *dst = r;
    return true;
  }
  if (op >= 0x18 && op <= 0x1F) {      // ADDHNB .. RSUBHNT
    if (esz == 0) return false;
    const uint32_t dn = esz - 1, dw = element_bits(dn);
    const Vec n = s_.v[zn], m = s_.v[zm];
    const bool top = (op & 1) != 0, round = (op & 2) != 0, subtract = (op & 4) != 0;
    Vec r = top ? *dst : Vec{};
    for (uint32_t i = 0; i < elements; ++i) {
      const I128 a = unsigned_of(get(n, esz, i), w), b = unsigned_of(get(m, esz, i), w);
      I128 v = subtract ? a - b : a + b;
      if (round) v += static_cast<I128>(1) << (dw - 1);
      put(&r, dn, 2 * i + top, static_cast<uint64_t>(v >> dw) & ones(dw));
    }
    *dst = r;
    return true;
  }
  // cryptography: AESE, AESD, AESMC, AESIMC, SM4E, SM4EKEY, RAX1
  if (size == 0 && (op == 0x38 || op == 0x39 || op == 0x3C || op == 0x3D)) {
    const Vec n = s_.v[zn], m = s_.v[zm], d = *dst;
    const uint32_t sel = bits_of(insn, 20, 16);
    uint64_t lo, hi;
    if (op == 0x3D) {      // RAX1: bits 20:16 = Zm
      Vec r;
      r.lo = n.lo ^ ((m.lo << 1) | (m.lo >> 63));
      r.hi = n.hi ^ ((m.hi << 1) | (m.hi >> 63));
      *dst = r;
      return true;
    }
    if (op == 0x3C) {      // SM4EKEY
      const uint64_t state[2] = {n.lo, n.hi}, keys[2] = {m.lo, m.hi};
      uint64_t out[2];
      crypto_sm4_rounds(state, keys, true, out);
      *dst = Vec{out[0], out[1]};
      return true;
    }
    if (sel == 2) {      // AESE / AESD: the second operand is in bits 9:5
      lo = d.lo ^ n.lo;
      hi = d.hi ^ n.hi;
      crypto_aes_sub_shift(&lo, &hi, op == 0x39);
      *dst = Vec{lo, hi};
      return true;
    }
    if (sel == 0) {      // AESMC / AESIMC: Zdn, with the source in bits 9:5 being the same register
      lo = d.lo;
      hi = d.hi;
      crypto_aes_mix(&lo, &hi, op == 0x39);
      *dst = Vec{lo, hi};
      return true;
    }
    if (sel == 3 && op == 0x38) {      // SM4E
      const uint64_t state[2] = {d.lo, d.hi}, keys[2] = {n.lo, n.hi};
      uint64_t out[2];
      crypto_sm4_rounds(state, keys, false, out);
      *dst = Vec{out[0], out[1]};
      return true;
    }
    return false;
  }
  return false;
}

}  // namespace emulation
