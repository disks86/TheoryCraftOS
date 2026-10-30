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
  if (!s_.streaming && (insn >> 24) != 0xE1) return undefined();      // (only LDR / STR of ZA work outside streaming mode)
  const uint32_t top = insn >> 24, size = bits_of(insn, 23, 22), rn = bits_of(insn, 9, 5), zd = insn & 31;
  const uint32_t pg = bits_of(insn, 12, 10), rs = bits_of(insn, 14, 13);
  const bool vertical = (insn >> 15) & 1;
  if (!s_.za_enabled) return undefined();

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
        for (uint32_t i = 0; i < slice_count(esz); ++i)
          if (active(gp, i, esz)) put(&s_.v[zd], esz, i, get(slice, esz, i));
        if (esz == 4 && active(gp, 0, 4)) s_.v[zd] = slice;
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
    const uint32_t zm = bits_of(insn, 20, 16);
    const uint16_t pm = s_.p[bits_of(insn, 15, 13)], pn = s_.p[bits_of(insn, 12, 10)];
    const bool subtract = (insn >> 4) & 1, b3 = (insn >> 3) & 1;
    const Vec a = s_.v[rn], b = s_.v[zm];
    const uint32_t key = insn >> 21;
    Env env = fp_env();
    auto finish = [&]() { fp_finish(env); return Step::Ok; };
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
      const bool wide = size == 3, n_unsigned = top == 0xA1, m_unsigned = (insn >> 21) & 1;
      if (size < 2) return undefined();
      const uint32_t group = wide ? 4 : (b3 ? 2 : 4), src_esz = (wide || b3) ? 1 : 0;
      if (b3 && !wide && (n_unsigned != m_unsigned)) return undefined();
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
