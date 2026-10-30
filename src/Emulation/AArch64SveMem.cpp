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

// SVE loads and stores (top bytes 0x84 .. 0xE5): LDR / STR of Z and P registers, contiguous loads and stores (one to four registers, first-fault and non-fault, non-temporal, replicating), gather
// loads and scatter stores with vector bases or vector offsets, and the prefetches (which have no effect here).
namespace emulation {

using namespace sve;

namespace {

// (memory element size, element size, sign-extend) of the 4-bit "dtype" field of the load instructions.
struct LoadType {
  uint8_t msz, esz;
  bool is_signed;
};
const LoadType kLoadTypes[16] = {{0, 0, false}, {0, 1, false}, {0, 2, false}, {0, 3, false}, {2, 3, true},  {1, 1, false}, {1, 2, false}, {1, 3, false},
                                 {1, 3, true},  {1, 2, true},  {2, 2, false}, {2, 3, false}, {0, 3, true},  {0, 2, true},  {0, 1, true},  {3, 3, false}};

uint64_t extend(uint64_t v, uint32_t msz, uint32_t esz, bool is_signed) {
  if (!is_signed) return v;
  return static_cast<uint64_t>(sext(v, element_bits(msz))) & element_ones(esz);
}

}  // namespace

AArch64Cpu::Step AArch64Cpu::sve_memory(GuestMemory& m, uint32_t insn) {
  const uint32_t top = insn >> 24;
  const uint32_t rn = bits_of(insn, 9, 5), rt = insn & 31, kind = bits_of(insn, 15, 13), pg = bits_of(insn, 12, 10);

  // LDR / STR of a predicate or vector register: base plus a signed 9-bit multiple of the register size
  {
    const uint32_t t = insn >> 22;
    if ((t == 0x216 || t == 0x396) && (kind == 0 || kind == 2)) {      // 1000 0101 10 (LDR) and 1110 0101 10 (STR)
      const bool store = t == 0x396;
      const int64_t imm = sext((bits_of(insn, 21, 16) << 3) | bits_of(insn, 12, 10), 9);
      if (kind == 0 && (insn & 0x10)) return undefined();
      const uint64_t base = reg_sp(rn, true);
      if (kind == 2) {
        const uint64_t address = base + static_cast<uint64_t>(imm * static_cast<int64_t>(kVectorBytes));
        if (store) return store_vec(m, address, kVectorBytes, s_.v[rt]) ? Step::Ok : Step::Fault;
        Vec v;
        if (!load_vec(m, address, kVectorBytes, &v)) return Step::Fault;
        s_.v[rt] = v;
        return Step::Ok;
      }
      const uint64_t address = base + static_cast<uint64_t>(imm * static_cast<int64_t>(kVectorBytes / 8));
      if (store) return this->store(m, address, 2, s_.p[rt & 15]) ? Step::Ok : Step::Fault;
      uint64_t v;
      if (!this->load(m, address, 2, &v)) return Step::Fault;
      s_.p[rt & 15] = static_cast<uint16_t>(v);
      return Step::Ok;
    }
  }

  // Load driver: reads the active elements (`address_of(i)` gives each address) into `result`, with the fault policy of the instruction.
  // ff: 0 normal (a fault is taken), 1 first-fault (only the first active element may fault), 2 non-fault. A suppressed fault clears the first-fault register from that element on.
  auto load_elements = [&](uint16_t gp, uint32_t esz, uint32_t msz, bool is_signed, int ff, auto&& address_of, Vec* result) -> Step {
    bool first = true;
    for (uint32_t i = 0; i < element_count(esz); ++i) {
      if (!active(gp, i, esz)) continue;
      uint64_t v;
      if (!load(m, address_of(i), 1u << msz, &v)) {
        if (ff == 0 || (ff == 1 && first)) return Step::Fault;
        s_.ffr = static_cast<uint16_t>(s_.ffr & ~(0xFFFFu << (i << esz)));
        break;
      }
      first = false;
      put(result, esz, i, extend(v, msz, esz, is_signed));
    }
    return Step::Ok;
  };
  auto store_elements = [&](uint16_t gp, uint32_t esz, uint32_t msz, const Vec& data, auto&& address_of) -> Step {
    for (uint32_t i = 0; i < element_count(esz); ++i) {
      if (!active(gp, i, esz)) continue;
      if (!store(m, address_of(i), 1u << msz, get(data, esz, i) & ones(8u << msz))) return Step::Fault;
    }
    return Step::Ok;
  };
  const uint64_t base = reg_sp(rn, true);
  const uint16_t gp = s_.p[pg];

  if (top == 0xA4 || top == 0xA5) {      // contiguous loads
    const uint32_t dtype = bits_of(insn, 24, 21), msz_field = bits_of(insn, 24, 23), opc = bits_of(insn, 22, 21);
    const uint32_t rm = bits_of(insn, 20, 16);
    const int64_t imm4 = sext(bits_of(insn, 19, 16), 4);
    switch (kind) {
      case 5: case 2: case 3: {      // LD1 / LDNF1 (scalar plus immediate), LD1 (scalar plus scalar), LDFF1
        const LoadType t = kLoadTypes[dtype];
        const uint32_t elements = element_count(t.esz);
        uint64_t address;
        int ff = 0;
        if (kind == 5) {
          address = base + static_cast<uint64_t>(imm4 * static_cast<int64_t>(elements) * (1 << t.msz));
          ff = ((insn >> 20) & 1) ? 2 : 0;
        } else {
          if (kind == 2 && rm == 31) return undefined();
          address = base + (reg(rm, true) << t.msz);
          ff = kind == 3 ? 1 : 0;
        }
        Vec r;
        const Step s = load_elements(gp, t.esz, t.msz, t.is_signed, ff, [&](uint32_t i) { return address + (static_cast<uint64_t>(i) << t.msz); }, &r);
        if (s != Step::Ok) return s;
        s_.v[rt] = r;
        return Step::Ok;
      }
      case 6: case 7: {      // LDNT1, LD2, LD3, LD4
        const uint32_t nreg = opc + 1, esz = msz_field;
        if (kind == 7 && ((insn >> 20) & 1)) return undefined();
        const uint32_t elements = element_count(esz);
        const uint64_t address = kind == 7 ? base + static_cast<uint64_t>(imm4 * static_cast<int64_t>(elements) * nreg * (1 << esz)) : base + (reg(rm, true) << esz);
        if (kind == 6 && rm == 31) return undefined();
        Vec r[4];
        const uint16_t g = gp;
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(g, i, esz)) continue;
          for (uint32_t k = 0; k < nreg; ++k) {
            uint64_t v;
            if (!load(m, address + ((static_cast<uint64_t>(i) * nreg + k) << esz), 1u << esz, &v)) return Step::Fault;
            put(&r[k], esz, i, v);
          }
        }
        for (uint32_t k = 0; k < nreg; ++k) s_.v[(rt + k) & 31] = r[k];
        return Step::Ok;
      }
      case 0: case 1: {      // LD1RQ, LD1RO
        if (opc > 1) return undefined();
        const uint32_t esz = msz_field;
        const bool octword = opc == 1;
        if (octword) return undefined();      // (LD1RO needs a vector length of at least 256 bits)
        uint64_t address;
        if (kind == 1) {
          if ((insn >> 20) & 1) return undefined();
          address = base + static_cast<uint64_t>(imm4 * (octword ? 32 : 16));
        } else {
          if (rm == 31) return undefined();
          address = base + (reg(rm, true) << esz);
        }
        Vec r;
        const Step s = load_elements(gp, esz, esz, false, 0, [&](uint32_t i) { return address + (static_cast<uint64_t>(i) << esz); }, &r);
        if (s != Step::Ok) return s;
        s_.v[rt] = r;
        return Step::Ok;
      }
      default: return undefined();
    }
  }

  if (top == 0xE4 || top == 0xE5) {      // stores
    const uint32_t msz_field = bits_of(insn, 24, 23), opc = bits_of(insn, 22, 21), rm = bits_of(insn, 20, 16);
    const int64_t imm4 = sext(bits_of(insn, 19, 16), 4);
    const Vec data = s_.v[rt];
    switch (kind) {
      case 2: {      // ST1 (scalar plus scalar)
        const uint32_t esz = opc, msz = msz_field;
        if (msz > esz || rm == 31) return undefined();
        const uint64_t address = base + (reg(rm, true) << msz);
        return store_elements(gp, esz, msz, data, [&](uint32_t i) { return address + (static_cast<uint64_t>(i) << msz); });
      }
      case 7: {      // ST1 (scalar plus immediate), STNT1 / ST2 / ST3 / ST4 (immediate)
        if (!((insn >> 20) & 1)) {
          const uint32_t esz = opc, msz = msz_field;
          if (msz > esz) return undefined();
          const uint64_t address = base + static_cast<uint64_t>(imm4 * static_cast<int64_t>(element_count(esz)) * (1 << msz));
          return store_elements(gp, esz, msz, data, [&](uint32_t i) { return address + (static_cast<uint64_t>(i) << msz); });
        }
        [[fallthrough]];
      }
      case 3: {      // STNT1 / ST2 / ST3 / ST4
        const uint32_t nreg = opc + 1, esz = msz_field, elements = element_count(esz);
        uint64_t address;
        if (kind == 7) address = base + static_cast<uint64_t>(imm4 * static_cast<int64_t>(elements) * nreg * (1 << esz));
        else {
          if (rm == 31) return undefined();
          address = base + (reg(rm, true) << esz);
        }
        for (uint32_t i = 0; i < elements; ++i) {
          if (!active(gp, i, esz)) continue;
          for (uint32_t k = 0; k < nreg; ++k)
            if (!store(m, address + ((static_cast<uint64_t>(i) * nreg + k) << esz), 1u << esz, get(s_.v[(rt + k) & 31], esz, i))) return Step::Fault;
        }
        return Step::Ok;
      }
      case 4: case 6: {      // scatter: scalar plus 32-bit unpacked or packed offsets (UXTW / SXTW)
        const uint32_t msz = msz_field;
        const bool wide32 = (insn >> 22) & 1, scaled = (insn >> 21) & 1, sxtw = kind == 6;
        const uint32_t esz = wide32 ? 2 : 3;
        if (msz > esz || (scaled && msz == 0)) return undefined();
        const Vec offsets = s_.v[rm];
        return store_elements(gp, esz, msz, data, [&](uint32_t i) {
          const uint64_t o = get(offsets, esz, i) & 0xFFFFFFFFu;
          const uint64_t off = sxtw ? static_cast<uint64_t>(sext(o, 32)) : o;
          return base + (scaled ? off << msz : off);
        });
      }
      case 5: {      // scatter: scalar plus 64-bit offsets, or vector plus immediate
        const uint32_t msz = msz_field;
        if (!((insn >> 22) & 1)) {      // scalar plus 64-bit vector (bit 21: scaled)
          const bool scaled = (insn >> 21) & 1;
          if (scaled && msz == 0) return undefined();
          const Vec offsets = s_.v[rm];
          return store_elements(gp, 3, msz, data, [&](uint32_t i) { return base + (scaled ? get(offsets, 3, i) << msz : get(offsets, 3, i)); });
        }
        const uint32_t esz = ((insn >> 21) & 1) ? 2 : 3;
        if (msz > esz) return undefined();
        const Vec bases = s_.v[rn];
        const uint64_t imm = static_cast<uint64_t>(rm) << msz;
        return store_elements(gp, esz, msz, data, [&](uint32_t i) { return get(bases, esz, i) + imm; });
      }
      case 1: {      // STNT1 (vector plus scalar)
        const uint32_t msz = msz_field;
        if (opc != 0 && opc != 2) return undefined();
        const uint32_t esz = opc == 2 ? 2 : 3;
        if (msz > esz) return undefined();
        const Vec bases = s_.v[rn];
        const uint64_t offset = reg(rm, true);
        return store_elements(gp, esz, msz, data, [&](uint32_t i) { return get(bases, esz, i) + offset; });
      }
      default: return undefined();
    }
  }

  // 0x84 / 0x85 (32-bit gathers, LD1R, prefetch) and 0xC4 / 0xC5 (64-bit gathers)
  const bool d64 = (top & 0x40) != 0;
  const uint32_t msz_field = bits_of(insn, 24, 23), opc = bits_of(insn, 22, 21), rm = bits_of(insn, 20, 16);
  const bool b15 = (insn >> 15) & 1, b14 = (insn >> 14) & 1, b13 = (insn >> 13) & 1;
  // prefetch instructions: accepted and ignored
  if (!(insn & 0x10)) {
    if (!d64) {
      if (top == 0x85 && bits_of(insn, 23, 22) == 3 && !b15) return Step::Ok;                                       // scalar plus immediate
      if (!b15 && msz_field == 0 && ((insn >> 21) & 1)) return Step::Ok;                                              // scalar plus 32-bit vector offsets
      if (opc == 0 && kind == 7) return Step::Ok;                                                                    // vector plus immediate
      if (opc == 0 && kind == 6) return Step::Ok;                                                                    // scalar plus scalar
    } else {
      if (!b15 && msz_field == 0 && ((insn >> 21) & 1)) return Step::Ok;                                              // scalar plus unpacked 32-bit offsets
      if (b15 && ((insn >> 22) & 1) == 1 && msz_field == 0 && ((insn >> 21) & 1)) return Step::Ok;                    // scalar plus 64-bit offsets
      if (opc == 0 && kind == 7) return Step::Ok;                                                                    // vector plus immediate
    }
  }

  const uint32_t esz = d64 ? 3 : 2;
  if (!b15) {      // scalar plus vector offsets with 32-bit extension (UXTW / SXTW)
    const bool sxtw = (insn >> 22) & 1, scaled = (insn >> 21) & 1, unsigned_load = b14, ff = b13;
    const uint32_t msz = msz_field;
    if (msz > esz || (scaled && msz == 0) || (!unsigned_load && msz == esz)) return undefined();
    const Vec offsets = s_.v[rm];
    Vec r;
    const Step s = load_elements(gp, esz, msz, !unsigned_load, ff ? 1 : 0, [&](uint32_t i) {
      const uint64_t o = get(offsets, esz, i) & 0xFFFFFFFFu;
      const uint64_t off = sxtw ? static_cast<uint64_t>(sext(o, 32)) : o;
      return base + (scaled ? off << msz : off);
    }, &r);
    if (s != Step::Ok) return s;
    s_.v[rt] = r;
    return Step::Ok;
  }
  // bit 15 set
  if (opc >= 2) {
    if (!d64) {      // LD1R (and its sign-extending forms): the "dtype" is bits 24:23 and 14:13
      const LoadType t = kLoadTypes[(msz_field << 2) | (insn >> 13 & 3)];
      const uint64_t address = base + (static_cast<uint64_t>(bits_of(insn, 21, 16)) << t.msz);
      uint64_t v;
      bool fetched = false;
      Vec r;
      for (uint32_t i = 0; i < element_count(t.esz); ++i) {
        if (!active(gp, i, t.esz)) continue;
        if (!fetched) {
          if (!load(m, address, 1u << t.msz, &v)) return Step::Fault;
          fetched = true;
        }
        put(&r, t.esz, i, extend(v, t.msz, t.esz, t.is_signed));
      }
      s_.v[rt] = r;
      return Step::Ok;
    }
    // 64-bit space, bit 22 set: scalar plus 64-bit vector offsets
    const bool scaled = (insn >> 21) & 1, unsigned_load = b14, ff = b13;
    if (!((insn >> 22) & 1)) return undefined();
    const uint32_t msz = msz_field;
    if (msz > 3 || (scaled && msz == 0) || (!unsigned_load && msz == 3)) return undefined();
    const Vec offsets = s_.v[rm];
    Vec r;
    const Step s = load_elements(gp, 3, msz, !unsigned_load, ff ? 1 : 0, [&](uint32_t i) { return base + (scaled ? get(offsets, 3, i) << msz : get(offsets, 3, i)); }, &r);
    if (s != Step::Ok) return s;
    s_.v[rt] = r;
    return Step::Ok;
  }
  if (d64 && opc >= 2) return undefined();
  if (opc == 1) {      // vector base plus immediate
    const uint32_t msz = msz_field;
    const bool unsigned_load = b14, ff = b13;
    if (msz > esz || (!unsigned_load && msz == esz)) return undefined();
    const Vec bases = s_.v[rn];
    const uint64_t imm = static_cast<uint64_t>(rm) << msz;
    Vec r;
    const Step s = load_elements(gp, esz, msz, !unsigned_load, ff ? 1 : 0, [&](uint32_t i) { return get(bases, esz, i) + imm; }, &r);
    if (s != Step::Ok) return s;
    s_.v[rt] = r;
    return Step::Ok;
  }
  // opc == 0: LDNT1 (vector plus scalar)
  {
    const uint32_t msz = msz_field;
    const bool unsigned_load = d64 ? b14 : b13;
    if (d64 ? b13 : b14) return undefined();
    if (msz > esz || (!unsigned_load && msz == esz)) return undefined();
    const Vec bases = s_.v[rn];
    const uint64_t offset = reg(rm, true);
    Vec r;
    const Step s = load_elements(gp, esz, msz, !unsigned_load, 0, [&](uint32_t i) { return get(bases, esz, i) + offset; }, &r);
    if (s != Step::Ok) return s;
    s_.v[rt] = r;
    return Step::Ok;
  }
}

}  // namespace emulation
