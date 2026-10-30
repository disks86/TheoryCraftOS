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
#include "Emulation/AArch64Fp8.hpp"
#include "Emulation/AArch64SveCommon.hpp"
#include "Emulation/SoftFloat.hpp"

// SME at a streaming vector length of 128 bits: the ZA array (16 rows of 16 bytes), its tiles, slice moves, tile loads and stores, ZERO, ADDHA / ADDVA and the outer products.
// A tile of 2^esz-byte elements is made of the ZA rows (i * 2^esz + tile); a horizontal slice is one such row, a vertical slice takes one element from each.
namespace emulation {

using namespace sve;
using namespace softfloat;

namespace {

uint32_t tile_count(uint32_t esz) { return 1u << esz; }
uint32_t slice_count(uint32_t esz) { return 16u >> esz; }

}  // namespace

AArch64Cpu::Step AArch64Cpu::sme(GuestMemory& m, uint32_t insn) {
  if (((insn >> 24) & 0xFE) == 0xA0 && !((insn >> 23) & 1)) {      // SVE2.1 / SME2: LD1, LDNT1, ST1, STNT1 of two or four vectors, governed by a predicate-as-counter
    const bool strided = (insn >> 24) & 1, four = (insn >> 15) & 1, store = (insn >> 21) & 1, immediate = (insn >> 22) & 1;
    const uint32_t esz = bits_of(insn, 14, 13), nreg = four ? 4 : 2, rn = bits_of(insn, 9, 5), pn = 8 + bits_of(insn, 12, 10);
    uint32_t zt;
    bool nontemporal;
    if (!strided) {
      if (four && ((insn >> 1) & 1)) return undefined();
      zt = four ? (bits_of(insn, 4, 2) << 2) : (bits_of(insn, 4, 1) << 1);
      nontemporal = insn & 1;
    } else {
      if (four && ((insn >> 2) & 1)) return undefined();
      zt = (((insn >> 4) & 1) << 4) | (four ? bits_of(insn, 1, 0) : bits_of(insn, 2, 0));
      nontemporal = (insn >> 3) & 1;
    }
    (void)nontemporal;
    uint64_t address = reg_sp(rn, true);
    if (immediate) {
      if ((insn >> 20) & 1) return undefined();
      address += static_cast<uint64_t>(sext(bits_of(insn, 19, 16), 4) * static_cast<int64_t>(nreg) * kVectorBytes);
    } else {
      address += reg(bits_of(insn, 20, 16), true) << esz;      // (Rm = 31 is the zero register)
    }
    Vec data[4];
    uint32_t regs[4];
    for (uint32_t k = 0; k < nreg; ++k) regs[k] = strided ? (zt + k * (four ? 4 : 8)) & 31 : (zt + k) & 31;
    for (uint32_t k = 0; k < nreg; ++k) {
      const uint16_t gp = counter_to_predicate(s_.p[pn], esz, k);
      const uint64_t base = address + static_cast<uint64_t>(k) * kVectorBytes;
      for (uint32_t i = 0; i < element_count(esz); ++i) {
        if (!active(gp, i, esz)) continue;
        const uint64_t where = base + (static_cast<uint64_t>(i) << esz);
        if (store) {
          if (!this->store(m, where, 1u << esz, get(s_.v[regs[k]], esz, i))) return Step::Fault;
        } else {
          uint64_t v;
          if (!this->load(m, where, 1u << esz, &v)) return Step::Fault;
          put(&data[k], esz, i, v);
        }
      }
    }
    if (!store)
      for (uint32_t k = 0; k < nreg; ++k) s_.v[regs[k]] = data[k];
    return Step::Ok;
  }
  if ((insn & 0xFFFFFC1Fu) == 0xE11F8000u || (insn & 0xFFFFFC1Fu) == 0xE13F8000u) {      // LDR / STR ZT0, [Xn]: the 64 byte lookup table
    if (!s_.za_enabled) return undefined();
    const uint64_t address = reg_sp(bits_of(insn, 9, 5), true);
    const bool store = (insn >> 21) & 1;
    for (uint32_t i = 0; i < 8; ++i) {
      if (store) { if (!this->store(m, address + 8ull * i, 8, s_.zt0[i])) return Step::Fault; }
      else if (!this->load(m, address + 8ull * i, 8, &s_.zt0[i])) return Step::Fault;
    }
    return Step::Ok;
  }
  if (insn == 0xC0480001u) {      // ZERO {ZT0}
    if (!s_.za_enabled) return undefined();
    for (uint64_t& w : s_.zt0) w = 0;
    return Step::Ok;
  }
  if ((insn & 0xFFFFCFE0u) == 0xC04F03E0u) {      // MOVT ZT0[#off, MUL VL], Zt: a vector into 16 bytes of ZT0
    if (!s_.za_enabled) return undefined();
    const uint32_t index = 2 * bits_of(insn, 13, 12);
    if (index == 0) for (uint64_t& w : s_.zt0) w = 0;      // (without an offset the whole table is written, the rest zero)
    s_.zt0[index] = s_.v[insn & 31].lo;
    s_.zt0[index + 1] = s_.v[insn & 31].hi;
    return Step::Ok;
  }
  if ((insn & 0xFFFF8FE0u) == 0xC04C03E0u || (insn & 0xFFFF8FE0u) == 0xC04E03E0u) {      // MOVT Xt, ZT0[#off] / MOVT ZT0[#off], Xt
    if (!s_.za_enabled) return undefined();
    const uint32_t index = bits_of(insn, 14, 12), rt = insn & 31;
    if ((insn >> 17) & 1) s_.zt0[index] = reg(rt, true);
    else set_reg(rt, s_.zt0[index], true);
    return Step::Ok;
  }
  if (!s_.streaming && (insn >> 24) != 0xE1) return undefined();      // (only LDR / STR of ZA work outside streaming mode)
  const uint32_t top = insn >> 24, size = bits_of(insn, 23, 22), rn = bits_of(insn, 9, 5), zd = insn & 31;
  const uint32_t pg = bits_of(insn, 12, 10), rs = bits_of(insn, 14, 13);
  const bool vertical = (insn >> 15) & 1;
  if (!s_.za_enabled) return undefined();
  if (top == 0xC1) return sme2(m, insn);

  // slice moves and loads / stores of tile slices
  auto slice_get = [&](uint32_t esz, uint32_t tile, bool vert, uint32_t k) {
    Vec out;
    if (esz == 4) return s_.za[tile];
    const uint32_t n = tile_count(esz);
    if (!vert) return s_.za[k * n + tile];
    for (uint32_t i = 0; i < slice_count(esz); ++i) put(&out, esz, i, get(s_.za[i * n + tile], esz, k));
    return out;
  };
  auto slice_set_element = [&](uint32_t esz, uint32_t tile, bool vert, uint32_t k, uint32_t i, uint64_t value) {
    const uint32_t n = tile_count(esz);
    if (!vert) put(&s_.za[k * n + tile], esz, i, value);
    else put(&s_.za[i * n + tile], esz, k, value);
  };

  if (top == 0xC0 && ((insn >> 23) & 1) && !((insn >> 21) & 1) && !((insn >> 10) & 3) && (bits_of(insn, 19, 18) == 3 || bits_of(insn, 19, 17) == 5) && bits_of(insn, 13, 12) != 3) {      // LUTI2 / LUTI4: expand table indices from a vector through ZT0
    const bool luti2 = bits_of(insn, 19, 18) == 3, single = (insn >> 22) & 1, strided = (insn >> 20) & 1, x2 = !single && ((insn >> 14) & 1), x4 = !single && !x2 && ((insn >> 15) & 1);
    if (!single && !x2 && !x4) return undefined();
    if ((single && strided) || (!luti2 && ((insn >> 17) & 1) == 0)) return undefined();
    const uint32_t esz = bits_of(insn, 13, 12), nreg = single ? 1 : x2 ? 2 : 4;
    if (!luti2 && esz == 0 && x4) return undefined();
    const uint32_t first_bit = single ? 14 : x2 ? 15 : 16;
    const uint32_t width = single ? (luti2 ? (esz == 0 ? 2 : 3) : (esz == 0 ? 1 : 2)) : x2 ? (luti2 ? 3 : 2) : (luti2 ? 2 : 1);
    const uint32_t top_bit = luti2 ? 17 : 16;
    if (first_bit + width <= top_bit && bits_of(insn, top_bit, first_bit + width) != 0) return undefined();      // (bits above the index are reserved)
    const uint32_t imm = bits_of(insn, first_bit + width - 1, first_bit);
    const uint32_t ibits = luti2 ? 2 : 4, elements = element_count(esz), rn = bits_of(insn, 9, 5);
    uint32_t zd;
    if (single) zd = insn & 31;
    else if (!strided) zd = x2 ? (bits_of(insn, 4, 1) << 1) : (bits_of(insn, 4, 2) << 2);
    else zd = (((insn >> 4) & 1) << 4) | (x2 ? bits_of(insn, 2, 0) : bits_of(insn, 1, 0));
    if (!single && !strided && ((x2 && (insn & 1)) || (x4 && (insn & 3)))) return undefined();
    if (strided && ((x2 && ((insn >> 3) & 1)) || (x4 && ((insn >> 2) & 1)))) return undefined();
    const uint32_t stride = strided ? (x2 ? 8 : 4) : 1;
    Vec result[4];
    for (uint32_t r = 0; r < nreg; ++r) {
      const uint32_t segment = (imm * nreg + r) % (128 / ibits / elements);      // (an index beyond the vector wraps around)
      for (uint32_t i = 0; i < elements; ++i) {
        const uint32_t position = (segment * elements + i) * ibits;      // (bit position of the index in the vector)
        const uint32_t index = static_cast<uint32_t>(get(s_.v[rn], 3, position / 64) >> (position % 64)) & ((1u << ibits) - 1);
        const uint64_t entry = s_.zt0[index / 2] >> ((index % 2) * 32);      // (the table is sixteen 32 bit entries; the result takes the low bits of one)
        put(&result[r], esz, i, entry & element_ones(esz));
      }
    }
    for (uint32_t r = 0; r < nreg; ++r) s_.v[(zd + r * stride) & 31] = result[r];
    return Step::Ok;
  }
  if (top == 0xC0 && (bits_of(insn, 21, 17) == 2 || bits_of(insn, 21, 17) == 3) && !((insn >> 16) & 1) && !((insn >> 12) & 1)) {      // MOVA / MOVAZ of two or four vectors: tile slices, or ZA array vectors (bit 11)
    const bool to_vector = (insn >> 17) & 1, four = (insn >> 10) & 1, array = (insn >> 11) & 1, zeroing = to_vector && ((insn >> 9) & 1);
    const uint32_t nreg = four ? 4 : 2, rv = static_cast<uint32_t>(reg(8 + (array ? bits_of(insn, 14, 13) : 4 + bits_of(insn, 14, 13)), false));
    uint32_t zt, field;
    if (to_vector) {
      zt = four ? (bits_of(insn, 4, 2) << 2) : (bits_of(insn, 4, 1) << 1);
      field = bits_of(insn, 9 - zeroing, 5);
      if (array) field = bits_of(insn, 7, 5);
      if ((!four && (insn & 1)) || (four && (insn & 3))) return undefined();
    } else {
      zt = four ? (bits_of(insn, 9, 7) << 2) : (bits_of(insn, 9, 6) << 1);
      field = array ? bits_of(insn, 2, 0) : bits_of(insn, 2, 0);
      if ((insn & 0x18) || (!four && ((insn >> 5) & 1)) || (four && ((insn >> 5) & 3))) return undefined();
    }
    if (array) {      // ZA.D[Wv, #off, VGx2 / VGx4]
      const uint32_t stride = 16 / nreg, first = (rv + field) % stride;      // (the vectors of a group are spread over the array, stride apart)
      for (uint32_t r = 0; r < nreg; ++r) {
        if (to_vector) { s_.v[(zt + r) & 31] = s_.za[first + r * stride]; if (zeroing) s_.za[first + r * stride] = Vec{}; }
        else s_.za[first + r * stride] = s_.v[(zt + r) & 31];
      }
      return Step::Ok;
    }
    const uint32_t esz = size;
    const uint32_t offsets = slice_count(esz) / nreg ? slice_count(esz) / nreg : 1;
    const uint32_t tile = field / offsets, off = (field % offsets) * nreg;
    if (tile >= tile_count(esz)) return undefined();
    for (uint32_t r = 0; r < nreg; ++r) {
      const uint32_t k = (rv - rv % nreg + off + r) % slice_count(esz);
      if (to_vector) {
        s_.v[(zt + r) & 31] = slice_get(esz, tile, vertical, k);
        if (zeroing) for (uint32_t i = 0; i < slice_count(esz); ++i) slice_set_element(esz, tile, vertical, k, i, 0);
      } else {
        for (uint32_t i = 0; i < slice_count(esz); ++i) slice_set_element(esz, tile, vertical, k, i, get(s_.v[(zt + r) & 31], esz, i));
      }
    }
    return Step::Ok;
  }
  if (top == 0xC0) {
    if ((insn & 0xFFFFFF00u) == 0xC0080000u) {      // ZERO {tiles}: a mask of the 64-bit tiles
      for (uint32_t t = 0; t < 8; ++t)
        if ((insn >> t) & 1) { s_.za[t] = Vec{}; s_.za[t + 8] = Vec{}; }
      return Step::Ok;
    }
    const uint32_t b21_17 = bits_of(insn, 21, 17);
    const bool movaz = b21_17 == 1 && ((insn >> 9) & 1);      // MOVAZ: the slice to a vector, without a predicate, and the slice is cleared
    if ((b21_17 == 0 || b21_17 == 1) && !(size != 3 && ((insn >> 16) & 1)) && !(b21_17 == 0 && (insn & 0x10))) {      // MOVA (tile slice to vector: bit 17 set; vector to tile slice: clear)
      const bool to_vector = b21_17 == 1;
      const uint32_t esz = (size == 3 && ((insn >> 16) & 1)) ? 4 : size;
      const uint32_t index_bits = esz == 4 ? 0 : 4 - esz;
      const uint32_t fields = to_vector ? bits_of(insn, 8, 5) : bits_of(insn, 3, 0);
      const uint32_t tile = fields >> index_bits, imm = fields & ((1u << index_bits) - 1);
      if (esz == 4 && (insn & 0x10) && !to_vector) {}      // (bit 4 is reserved for the vector-to-tile form too)
      const uint32_t k = (static_cast<uint32_t>(reg(12 + rs, false)) + imm) % slice_count(esz);
      if (movaz) {
        if (bits_of(insn, 12, 10) != 0) return undefined();
        s_.v[zd] = slice_get(esz, tile, vertical, k);
        if (esz == 4) s_.za[tile] = Vec{};
        else for (uint32_t i = 0; i < slice_count(esz); ++i) slice_set_element(esz, tile, vertical, k, i, 0);
        return Step::Ok;
      }
      if (to_vector) {
        if ((insn >> 9) & 1) return undefined();
        const Vec slice = slice_get(esz, tile, vertical, k);
        const uint16_t gp = s_.p[pg];
        if (esz == 4) { if (active(gp, 0, 4)) s_.v[zd] = slice; return Step::Ok; }
        for (uint32_t i = 0; i < slice_count(esz); ++i)
          if (active(gp, i, esz)) put(&s_.v[zd], esz, i, get(slice, esz, i));
      } else {
        const Vec src = s_.v[rn];
        const uint16_t gp = s_.p[pg];
        if (esz == 4) { if (active(gp, 0, 4)) s_.za[tile] = src; return Step::Ok; }
        for (uint32_t i = 0; i < slice_count(esz); ++i)
          if (active(gp, i, esz)) slice_set_element(esz, tile, vertical, k, i, get(src, esz, i));
      }
      return Step::Ok;
    }
    if ((bits_of(insn, 23, 17) == 0x48 || bits_of(insn, 23, 17) == 0x68) && !((insn >> 3) & (size == 3 ? 0 : 0))) {      // ADDHA / ADDVA
      const bool wide = size == 3, vert = (insn >> 16) & 1;
      const uint32_t esz = wide ? 3 : 2, tile = insn & (wide ? 7 : 3), n = slice_count(esz);
      if (!wide && (insn & 4)) return undefined();
      const uint16_t pn = s_.p[bits_of(insn, 12, 10)], pm = s_.p[bits_of(insn, 15, 13)];
      const Vec src = s_.v[rn];
      for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < n; ++j) {
          if (!active(pn, i, esz) || !active(pm, j, esz)) continue;
          Vec& row = s_.za[i * tile_count(esz) + tile];
          put(&row, esz, j, (get(row, esz, j) + get(src, esz, vert ? i : j)) & element_ones(esz));
        }
      return Step::Ok;
    }
    return undefined();
  }

  if (top == 0xE0 || top == 0xE1) {      // LD1 / ST1 of a tile slice, LDR / STR of a ZA array vector
    const bool store = (insn >> 21) & 1;
    const uint32_t rm = bits_of(insn, 20, 16);
    if (top == 0xE1 && size == 0) {      // LDR / STR ZA[Wv, #imm], [Xn, #imm, MUL VL]
      if (bits_of(insn, 15, 15) != 0 || bits_of(insn, 12, 10) != 0 || bits_of(insn, 20, 16) != 0 || (insn & 0x10)) return undefined();
      const uint32_t imm = insn & 15, row = (static_cast<uint32_t>(reg(12 + rs, false)) + imm) % 16;
      const uint64_t address = reg_sp(rn, true) + static_cast<uint64_t>(imm) * kVectorBytes;
      if (store) return store_vec(m, address, kVectorBytes, s_.za[row]) ? Step::Ok : Step::Fault;
      Vec v;
      if (!load_vec(m, address, kVectorBytes, &v)) return Step::Fault;
      s_.za[row] = v;
      return Step::Ok;
    }
    const uint32_t esz = top == 0xE1 ? (size == 3 ? 4 : 99) : size;
    if (esz == 99) return undefined();
    const uint32_t index_bits = esz == 4 ? 0 : 4 - esz;
    const uint32_t fields = bits_of(insn, 3, 0), tile = fields >> index_bits, imm = fields & ((1u << index_bits) - 1);
    if (insn & 0x10) return undefined();
    const uint32_t k = (static_cast<uint32_t>(reg(12 + rs, false)) + imm) % slice_count(esz);
    const uint64_t address = reg_sp(rn, true) + (reg(rm, true) << esz);
    const uint16_t gp = s_.p[pg];
    const uint32_t bytes = 1u << esz;
    if (esz == 4) {
      if (store) return active(gp, 0, 4) ? (store_vec(m, address, 16, s_.za[tile]) ? Step::Ok : Step::Fault) : Step::Ok;
      Vec v;
      if (active(gp, 0, 4) && !load_vec(m, address, 16, &v)) return Step::Fault;
      s_.za[tile] = v;
      return Step::Ok;
    }
    Vec slice = store ? slice_get(esz, tile, vertical, k) : Vec{};
    for (uint32_t i = 0; i < slice_count(esz); ++i) {
      const bool on = active(gp, i, esz);
      if (store) {
        if (on && !this->store(m, address + static_cast<uint64_t>(i) * bytes, bytes, get(slice, esz, i))) return Step::Fault;
      } else {
        uint64_t v = 0;
        if (on && !this->load(m, address + static_cast<uint64_t>(i) * bytes, bytes, &v)) return Step::Fault;
        put(&slice, esz, i, v);
      }
    }
    if (!store) {
      for (uint32_t i = 0; i < slice_count(esz); ++i) slice_set_element(esz, tile, vertical, k, i, get(slice, esz, i));
    }
    return Step::Ok;
  }

  if (top == 0x80 || top == 0x81 || top == 0xA0 || top == 0xA1) {      // outer products
    uint32_t mop4_form;
    if (sme2_mop4_match(insn, &mop4_form)) return sme2_mop4(mop4_form, insn);
    const uint32_t zm = bits_of(insn, 20, 16);
    const uint16_t pm = s_.p[bits_of(insn, 15, 13)], pn = s_.p[bits_of(insn, 12, 10)];
    const bool subtract = (insn >> 4) & 1, b3 = (insn >> 3) & 1;
    const Vec a = s_.v[rn], b = s_.v[zm];
    const uint32_t key = insn >> 21;
    Env env = fp_env();
    auto finish = [&]() { fp_finish(env); return Step::Ok; };
    if (top == 0x80 && key == 0x405 && !(insn & 4) && !(b3 && (insn & 2))) {      // FMOPA / FMOPS (FP8 into single, or half with bit 3): groups of four (two) bytes, predicated per byte
      const fp8::Fmt fa = static_cast<fp8::Fmt>(s_.fpmr & 1), fb = static_cast<fp8::Fmt>((s_.fpmr >> 3) & 1);
      const uint32_t group = b3 ? 2 : 4, rows = b3 ? 8 : 4, tiles = b3 ? 2 : 4, tile = insn & (b3 ? 1u : 3u);
      const uint32_t scale = b3 ? static_cast<uint32_t>((s_.fpmr >> 16) & 15) : static_cast<uint32_t>((s_.fpmr >> 16) & 0x7F);
      for (uint32_t i = 0; i < rows; ++i)
        for (uint32_t j = 0; j < rows; ++j) {
          bool any = false;
          uint8_t x[4], y[4];
          for (uint32_t k = 0; k < group; ++k) {
            const bool row_on = active(pn, group * i + k, 0), col_on = active(pm, group * j + k, 0);
            any = any || (row_on && col_on);
            x[k] = row_on ? static_cast<uint8_t>(get(a, 0, group * i + k)) : 0;      // (each operand is zeroed by its own predicate, so a NaN under the other one still counts)
            y[k] = col_on ? static_cast<uint8_t>(get(b, 0, group * j + k)) : 0;
            if (subtract) x[k] ^= 0x80;
          }
          if (!any) continue;
          Vec& row = s_.za[i * tiles + tile];
          put(&row, b3 ? 1 : 2, j, fp8::dot_add(fa, x, fb, y, group, scale, b3 ? Format::F16 : Format::F32, static_cast<uint32_t>(get(row, b3 ? 1 : 2, j)), (s_.fpmr >> 14) & 1));
        }
      return Step::Ok;
    }
    if (top == 0x80 && key == 0x404 && !b3) {      // FMOPA / FMOPS (single precision)
      const uint32_t tile = insn & 3;
      for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t j = 0; j < 4; ++j) {
          if (!active(pn, i, 2) || !active(pm, j, 2)) continue;
          Vec& row = s_.za[i * 4 + tile];
          const uint64_t x = subtract ? negate(Format::F32, get(a, 2, i)) : get(a, 2, i);
          put(&row, 2, j, fp_default_nan_fix(Format::F32, fused_multiply_add(Format::F32, x, get(b, 2, j), get(row, 2, j), env)));
        }
      return finish();
    }
    if (top == 0x80 && key == 0x404 && b3) {      // BMOPA / BMOPS: 32 minus the number of differing bits
      const uint32_t tile = insn & 3;
      for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t j = 0; j < 4; ++j) {
          if (!active(pn, i, 2) || !active(pm, j, 2)) continue;
          uint32_t diff = 0;
          for (uint64_t x = (get(a, 2, i) ^ get(b, 2, j)) & 0xFFFFFFFFu; x != 0; x &= x - 1) ++diff;
          Vec& row = s_.za[i * 4 + tile];
          const uint64_t delta = 32 - diff;
          put(&row, 2, j, (subtract ? get(row, 2, j) - delta : get(row, 2, j) + delta) & 0xFFFFFFFFu);
        }
      return Step::Ok;
    }
    if (top == 0x80 && key == 0x406 && !b3) {      // FMOPA / FMOPS (double precision)
      const uint32_t tile = insn & 7;
      for (uint32_t i = 0; i < 2; ++i)
        for (uint32_t j = 0; j < 2; ++j) {
          if (!active(pn, i, 3) || !active(pm, j, 3)) continue;
          Vec& row = s_.za[i * 8 + tile];
          const uint64_t x = subtract ? negate(Format::F64, get(a, 3, i)) : get(a, 3, i);
          put(&row, 3, j, fp_default_nan_fix(Format::F64, fused_multiply_add(Format::F64, x, get(b, 3, j), get(row, 3, j), env)));
        }
      return finish();
    }
    if (top == 0x81 && (key == 0x40C || key == 0x40D) && b3 && !(insn & 4) && !(insn & 2)) {      // FMOPA / FMOPS (0x40C) and BFMOPA / BFMOPS (0x40D) into half precision tiles: default NaN, no flags
      const bool bfloat = key == 0x40D;
      const Format fmt = bfloat ? Format::BF16 : Format::F16;
      const uint32_t tile = insn & 1;
      Env quiet = fp_env();
      for (uint32_t i = 0; i < 8; ++i)
        for (uint32_t j = 0; j < 8; ++j) {
          if (!active(pn, i, 1) || !active(pm, j, 1)) continue;
          Vec& row = s_.za[i * 2 + tile];
          const uint64_t x = subtract ? negate(fmt, get(a, 1, i)) : get(a, 1, i);
          uint64_t result = fused_multiply_add(fmt, x, get(b, 1, j), get(row, 1, j), quiet);
          if (is_nan(fmt, result)) result = bfloat ? 0x7FC0u : 0x7E00u;
          put(&row, 1, j, result);
        }
      return Step::Ok;
    }
    if (top == 0x81 && (key == 0x40C || key == 0x40D) && !b3) {      // BFMOPA (0x40C) / FMOPA (0x40D) widening half precision into single: no exception flags, the default NaN
      const bool bfloat = key == 0x40C;
      const uint32_t tile = insn & 3;
      Env quiet = fp_env();
      for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t j = 0; j < 4; ++j) {
          if (!(active(pn, 2 * i, 1) && active(pm, 2 * j, 1)) && !(active(pn, 2 * i + 1, 1) && active(pm, 2 * j + 1, 1))) continue;      // (an element changes only when some product term is active)
          Vec& row = s_.za[i * 4 + tile];
          const uint64_t acc = get(row, 2, j);
          uint64_t a0 = get(a, 1, 2 * i), a1 = get(a, 1, 2 * i + 1), b0 = get(b, 1, 2 * j), b1 = get(b, 1, 2 * j + 1);
          if (subtract) { a0 ^= 0x8000; a1 ^= 0x8000; }
          if (!active(pn, 2 * i, 1)) { a0 = 0; }
          if (!active(pn, 2 * i + 1, 1)) a1 = 0;
          if (!active(pm, 2 * j, 1)) b0 = 0;
          if (!active(pm, 2 * j + 1, 1)) b1 = 0;
          uint64_t result;
          if (bfloat) {
            result = bfloat_dot_add(acc, a0, a1, b0, b1, quiet);
          } else {
            Env e = quiet;
            auto widen = [&](uint64_t v) { return convert(Format::F16, Format::F32, v & 0xFFFF, e); };
            const uint64_t t = fused_multiply_add(Format::F32, widen(a1), widen(b1), mul(Format::F32, widen(a0), widen(b0), e), e);
            result = add(Format::F32, acc, is_nan(Format::F32, t) ? 0x7FC00000u : t, e);
            if (is_nan(Format::F32, result)) result = 0x7FC00000u;
          }
          put(&row, 2, j, result);
        }
      return Step::Ok;
    }
    if ((top == 0xA0 || top == 0xA1) && bits_of(insn, 23, 22) >= 2 && !((insn >> 20) & 1) == false) {}
    if (top == 0xA0 || top == 0xA1) {      // SMOPA .. USMOPS: signed / unsigned integer outer products accumulating into 32-bit (bytes, or halfwords when bit 3 is set) or 64-bit (halfwords) tiles
      const bool wide = size == 3, n_unsigned = top == 0xA1, m_unsigned = (b3 && !wide) ? n_unsigned : ((insn >> 21) & 1);      // (the two-way halfword form has one signedness for both)
      if (size < 2) return undefined();
      const uint32_t group = wide ? 4 : (b3 ? 2 : 4), src_esz = (wide || b3) ? 1 : 0;
      if (wide && b3) return undefined();
      const uint32_t esz = wide ? 3 : 2, n = slice_count(esz), tile = insn & (wide ? 7 : 3), sw = element_bits(src_esz);
      for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < n; ++j) {
          bool row_on = false, col_on = false;
          for (uint32_t k = 0; k < group; ++k) {
            row_on = row_on || active(pn, group * i + k, src_esz);
            col_on = col_on || active(pm, group * j + k, src_esz);
          }
          if (!row_on || !col_on) continue;
          int64_t sum = 0;
          for (uint32_t k = 0; k < group; ++k) {
            if (!active(pn, group * i + k, src_esz) || !active(pm, group * j + k, src_esz)) continue;
            const uint64_t x = get(a, src_esz, group * i + k), y = get(b, src_esz, group * j + k);
            const int64_t sx = n_unsigned ? static_cast<int64_t>(x) : sext(x, sw), sy = m_unsigned ? static_cast<int64_t>(y) : sext(y, sw);
            sum += sx * sy;
          }
          Vec& row = s_.za[i * tile_count(esz) + tile];
          put(&row, esz, j, ((subtract ? get(row, esz, j) - static_cast<uint64_t>(sum) : get(row, esz, j) + static_cast<uint64_t>(sum))) & element_ones(esz));
        }
      return Step::Ok;
    }
  }
  return undefined();
}

}  // namespace emulation
