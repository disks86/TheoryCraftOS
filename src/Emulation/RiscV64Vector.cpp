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

namespace emulation {

namespace {

constexpr int64_t sext(uint64_t v, uint32_t bits) { return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
constexpr uint32_t field(uint32_t v, uint32_t hi, uint32_t lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
constexpr uint32_t log2_of(uint32_t v) { return v == 1 ? 0 : v == 2 ? 1 : v == 4 ? 2 : 3; }

}  // namespace

VectorUnit::VectorUnit(RiscV64Cpu& cpu, GuestMemory& memory, uint32_t instruction)
    : c(cpu), s(cpu.s_), mem(memory), insn(instruction), vd(field(instruction, 11, 7)), vs1(field(instruction, 19, 15)), vs2(field(instruction, 24, 20)),
      funct3(field(instruction, 14, 12)), funct6(instruction >> 26), vm((instruction >> 25) & 1) {}

bool VectorUnit::decode_type(uint64_t vtype, uint32_t* sew_bits, int32_t* lmul_log, bool* ta, bool* ma) {
  if ((vtype >> 8) != 0) return false;      // vill, or a reserved bit
  const uint32_t vlmul = vtype & 7, vsew = (vtype >> 3) & 7;
  if (vlmul == 4 || vsew > 3) return false;
  const int32_t lmul = vlmul < 4 ? static_cast<int32_t>(vlmul) : static_cast<int32_t>(vlmul) - 8;
  const uint32_t sew = 8u << vsew;
  const uint32_t numerator = lmul > 0 ? 1u << lmul : 1u, denominator = lmul < 0 ? 1u << -lmul : 1u;
  if (sew * denominator > 64 * numerator) return false;      // SEW above ELEN * LMUL
  *sew_bits = sew;
  *lmul_log = lmul;
  *ta = (vtype >> 6) & 1;
  *ma = (vtype >> 7) & 1;
  return true;
}

bool VectorUnit::prepare() {
  valid = decode_type(s.vtype, &sew, &lmul_log, &ta, &ma);
  if (!valid) return false;
  sew_bytes = sew / 8;
  const uint32_t per_register = kVlenBytes * 8 / sew;
  vlmax = lmul_log >= 0 ? static_cast<uint64_t>(per_register) << lmul_log : per_register >> -lmul_log;
  vl = s.vl;
  return true;
}

bool VectorUnit::emul_for(uint32_t eew_bytes, int32_t* emul_log) const {
  const int32_t e = static_cast<int32_t>(log2_of(eew_bytes)) - static_cast<int32_t>(log2_of(sew_bytes)) + lmul_log;
  *emul_log = e;
  return e >= -3 && e <= 3;
}

uint64_t VectorUnit::get(uint32_t reg, uint32_t eew_bytes, uint64_t i) const {
  const uint64_t offset = uint64_t{reg} * kVlenBytes + i * eew_bytes;
  if (offset + eew_bytes > sizeof(s.v)) return 0;
  uint64_t v = 0;
  for (uint32_t k = 0; k < eew_bytes; ++k) v |= uint64_t{s.v[offset + k]} << (8 * k);
  return v;
}

void VectorUnit::put(uint32_t reg, uint32_t eew_bytes, uint64_t i, uint64_t value) {
  const uint64_t offset = uint64_t{reg} * kVlenBytes + i * eew_bytes;
  if (offset + eew_bytes > sizeof(s.v)) return;
  for (uint32_t k = 0; k < eew_bytes; ++k) s.v[offset + k] = static_cast<uint8_t>(value >> (8 * k));
}

void VectorUnit::ones(uint32_t reg, uint32_t eew_bytes, uint64_t from, uint64_t to) {
  for (uint64_t i = from; i < to; ++i) put(reg, eew_bytes, i, ~uint64_t{0});
}

void VectorUnit::set_mask_bit(uint32_t reg, uint64_t i, bool value) {
  const uint64_t offset = uint64_t{reg} * kVlenBytes + i / 8;
  if (offset >= sizeof(s.v)) return;
  const uint8_t bit = static_cast<uint8_t>(1u << (i % 8));
  s.v[offset] = value ? (s.v[offset] | bit) : (s.v[offset] & static_cast<uint8_t>(~bit));
}

void VectorUnit::tail(uint32_t reg, uint32_t regs, uint32_t eew_bytes, uint64_t from) {
  if (ta) ones(reg, eew_bytes, from, uint64_t{regs} * kVlenBytes / eew_bytes);
}

void VectorUnit::mask_tail(uint32_t reg) {
  for (uint64_t i = vl; i < kVlenBytes * 8; ++i) set_mask_bit(reg, i, true);      // (a mask destination's tail is always agnostic)
}

// --- vsetvli, vsetivli, vsetvl -----------------------------------------------------------------------------------------------------------------

RiscV64Cpu::Step VectorUnit::configure() {
  uint64_t requested;
  uint64_t avl;
  const uint32_t rs1 = vs1, rd = vd;
  bool keep_vl = false;
  if ((insn >> 30) == 3) {      // vsetivli
    requested = field(insn, 29, 20);
    avl = rs1;
  } else if ((insn >> 31) == 0) {      // vsetvli
    requested = field(insn, 30, 20);
    avl = rs1 != 0 ? s.x[rs1] : (rd != 0 ? ~uint64_t{0} : 0);
    keep_vl = rs1 == 0 && rd == 0;
  } else {      // vsetvl
    if ((insn >> 25) != 0x40) return undefined();
    requested = s.x[vs2];
    avl = rs1 != 0 ? s.x[rs1] : (rd != 0 ? ~uint64_t{0} : 0);
    keep_vl = rs1 == 0 && rd == 0;
  }
  uint32_t new_sew;
  int32_t new_lmul;
  bool new_ta, new_ma;
  if (!decode_type(requested, &new_sew, &new_lmul, &new_ta, &new_ma)) {
    s.vtype = uint64_t{1} << 63;
    s.vl = 0;
  } else {
    const uint32_t per_register = kVlenBytes * 8 / new_sew;
    const uint64_t new_max = new_lmul >= 0 ? static_cast<uint64_t>(per_register) << new_lmul : per_register >> -new_lmul;
    bool legal = true;
    if (keep_vl) {      // the new configuration has to keep the current VLMAX
      uint32_t old_sew;
      int32_t old_lmul;
      bool old_ta, old_ma;
      if (!decode_type(s.vtype, &old_sew, &old_lmul, &old_ta, &old_ma)) {
        legal = false;
      } else {
        const uint32_t old_per = kVlenBytes * 8 / old_sew;
        const uint64_t old_max = old_lmul >= 0 ? static_cast<uint64_t>(old_per) << old_lmul : old_per >> -old_lmul;
        legal = old_max == new_max;
      }
    }
    if (!legal) {
      s.vtype = uint64_t{1} << 63;
      s.vl = 0;
    } else {
      s.vtype = requested;
      if (!keep_vl) s.vl = avl < new_max ? avl : new_max;
    }
  }
  s.vstart = 0;
  c.set_x(rd, s.vl);
  return Step::Ok;
}

// --- loads and stores --------------------------------------------------------------------------------------------------------------------------

RiscV64Cpu::Step VectorUnit::memory_access() {
  const bool is_store = (insn & 0x7F) == 0x27;
  const uint32_t width = funct3, nf = (insn >> 29) + 1, mop = field(insn, 27, 26), lumop = vs2;
  if ((insn >> 28) & 1) return undefined();      // mew: a 128-bit element
  if (width != 0 && width < 5) return undefined();      // (the half-precision and quad-precision loads and stores)
  const uint32_t mem_eew = width == 0 ? 1 : 2u << (width - 5);      // 5: 16 bits, 6: 32, 7: 64
  const uint64_t base = s.x[vs1];
  const uint32_t data = vd;      // vd for loads, vs3 for stores

  if (mop == 0 && lumop == 8) {      // vl<nr>re<eew>.v, vs<nr>r.v: whole registers
    if (!vm) return undefined();
    if (nf != 1 && nf != 2 && nf != 4 && nf != 8) return undefined();
    if (is_store && width != 0) return undefined();
    if (!group_ok(data, nf)) return undefined();
    for (uint64_t k = s.vstart; k < uint64_t{nf} * kVlenBytes; ++k) {
      if (is_store) {
        if (!c.store(mem, base + k, 1, s.v[data * kVlenBytes + k])) {
          s.vstart = k;
          return Step::Fault;
        }
      } else {
        uint64_t b;
        if (!c.load(mem, base + k, 1, &b)) {
          s.vstart = k;
          return Step::Fault;
        }
        s.v[data * kVlenBytes + k] = static_cast<uint8_t>(b);
      }
    }
    return ok();
  }

  if (!prepare()) return undefined();
  if (mop == 0 && lumop == 0x0B) {      // vlm.v, vsm.v: ceil(vl / 8) bytes of a mask
    if (width != 0 || nf != 1 || !vm) return undefined();
    if (s.vstart >= vl) return ok();
    const uint64_t bytes = (vl + 7) / 8;
    for (uint64_t k = s.vstart; k < bytes; ++k) {
      if (is_store) {
        if (!c.store(mem, base + k, 1, s.v[data * kVlenBytes + k])) {
          s.vstart = k;
          return Step::Fault;
        }
      } else {
        uint64_t b;
        if (!c.load(mem, base + k, 1, &b)) {
          s.vstart = k;
          return Step::Fault;
        }
        s.v[data * kVlenBytes + k] = static_cast<uint8_t>(b);
      }
    }
    if (!is_store) {
      for (uint64_t k = bytes; k < kVlenBytes; ++k) s.v[data * kVlenBytes + k] = 0xFF;      // (the tail of a mask is agnostic)
    }
    return ok();
  }

  const bool indexed = mop == 1 || mop == 3, strided = mop == 2;
  const bool fault_only_first = mop == 0 && lumop == 0x10;
  if (mop == 0 && lumop != 0 && !fault_only_first) return undefined();
  if (fault_only_first && is_store) return undefined();

  // the data elements: EEW = the width for unit-stride and strided accesses, SEW for indexed ones (whose index EEW is the width)
  const uint32_t data_eew = indexed ? sew_bytes : mem_eew;
  int32_t data_emul, index_emul = 0;
  if (!emul_for(data_eew, &data_emul)) return undefined();
  const uint32_t data_regs = regs_of(data_emul);
  if (!group_ok(data, data_regs) || uint64_t{nf} * data_regs > 8 || data + nf * data_regs > 32) return undefined();
  if (indexed) {
    if (!emul_for(mem_eew, &index_emul) || !group_ok(vs2, regs_of(index_emul))) return undefined();
  }
  if (!vm && !is_store && data == 0) return undefined();      // a masked load cannot overwrite v0
  const int64_t stride = strided ? static_cast<int64_t>(s.x[vs2]) : static_cast<int64_t>(uint64_t{nf} * data_eew);

  if (s.vstart >= vl) return ok();
  uint64_t effective_vl = vl;
  for (uint64_t i = s.vstart; i < effective_vl; ++i) {
    if (!active(i)) {
      if (!is_store && ma) {
        for (uint32_t f = 0; f < nf; ++f) ones(data + f * data_regs, data_eew, i, i + 1);
      }
      continue;
    }
    uint64_t address;
    if (indexed) address = base + get(vs2, mem_eew, i);
    else address = base + static_cast<uint64_t>(stride) * i;
    for (uint32_t f = 0; f < nf; ++f) {
      const uint64_t where = address + uint64_t{f} * data_eew;
      const uint32_t reg = data + f * data_regs;
      if (is_store) {
        if (!c.store(mem, where, data_eew, get(reg, data_eew, i))) {
          s.vstart = i;
          return Step::Fault;
        }
      } else {
        uint64_t b;
        if (!c.load(mem, where, data_eew, &b)) {
          if (fault_only_first && i > 0) {      // the load stops short instead of trapping
            c.fault_ = GuestFault();
            c.s_.pc = c.instruction_start_;
            s.vl = i;
            effective_vl = i;
            vl = i;
            break;
          }
          s.vstart = i;
          return Step::Fault;
        }
        put(reg, data_eew, i, b);
      }
    }
  }
  if (!is_store) {
    for (uint32_t f = 0; f < nf; ++f) tail(data + f * data_regs, data_regs, data_eew, vl);
  }
  return ok();
}

// --- fixed-point helpers -----------------------------------------------------------------------------------------------------------------------

uint64_t VectorUnit::round_shift(__int128 value, uint32_t shift) const {
  if (shift == 0) return static_cast<uint64_t>(value);
  const __int128 shifted = value >> shift;
  const bool half = (value >> (shift - 1)) & 1;
  const bool below = shift >= 2 && (value & ((static_cast<__int128>(1) << (shift - 1)) - 1)) != 0;
  const bool lsb = shifted & 1;
  bool increment;
  switch (s.vxrm & 3) {
    case 0: increment = half; break;      // round to nearest, ties up
    case 1: increment = half && (below || lsb); break;      // ties to even
    case 2: increment = false; break;      // down (truncate)
    default: increment = !lsb && (half || below); break;      // round to odd
  }
  return static_cast<uint64_t>(shifted + (increment ? 1 : 0));
}

}  // namespace emulation
