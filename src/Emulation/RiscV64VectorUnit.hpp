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
#pragma once
#include "Emulation/RiscV64Cpu.hpp"

namespace emulation {

// The decoding and execution state of one vector instruction (RiscV64Vector.cpp: configuration, loads and stores, integer and fixed-point, mask and permutation
// instructions; RiscV64VectorFp.cpp: floating point). A friend of RiscV64Cpu, so it reads and writes the architectural state directly.
struct VectorUnit {
  using Step = RiscV64Cpu::Step;
  static constexpr uint32_t kVlenBytes = 16;      // VLEN = 128

  VectorUnit(RiscV64Cpu& cpu, GuestMemory& memory, uint32_t instruction);

  RiscV64Cpu& c;
  RiscV64Cpu::State& s;
  GuestMemory& mem;
  uint32_t insn;
  uint32_t vd, vs1, vs2, funct3, funct6;
  bool vm;      // true: unmasked

  // the vector configuration in effect
  bool valid = false;      // false: vill
  uint32_t sew = 8;        // bits
  uint32_t sew_bytes = 1;
  int32_t lmul_log = 0;    // log2 of LMUL, -3..3
  bool ta = false, ma = false;
  uint64_t vl = 0, vlmax = 0;

  // --- registers
  static bool decode_type(uint64_t vtype, uint32_t* sew_bits, int32_t* lmul_log, bool* ta, bool* ma);
  uint32_t regs_of(int32_t emul_log) const { return emul_log > 0 ? 1u << emul_log : 1u; }      // registers in a group (a fractional group still takes one)
  bool emul_for(uint32_t eew_bytes, int32_t* emul_log) const;      // false: EMUL outside 1/8..8
  bool group_ok(uint32_t reg, uint32_t regs) const { return regs != 0 && reg % regs == 0 && reg + regs <= 32; }
  uint64_t get(uint32_t reg, uint32_t eew_bytes, uint64_t i) const;
  void put(uint32_t reg, uint32_t eew_bytes, uint64_t i, uint64_t value);
  void ones(uint32_t reg, uint32_t eew_bytes, uint64_t from, uint64_t to);      // fills the elements [from, to) with 1 bits (agnostic policy)
  bool mask_bit(uint32_t reg, uint64_t i) const { return (s.v[reg * kVlenBytes + i / 8] >> (i % 8)) & 1; }
  void set_mask_bit(uint32_t reg, uint64_t i, bool value);
  bool active(uint64_t i) const { return vm || mask_bit(0, i); }
  uint64_t sew_mask() const { return sew == 64 ? ~uint64_t{0} : (uint64_t{1} << sew) - 1; }
  Step undefined() { return c.undefined(); }
  Step ok() {
    s.vstart = 0;
    return Step::Ok;
  }
  // Writes the tail of a destination group of `regs` registers with element width `eew_bytes`, from element `from` on, per the tail policy.
  void tail(uint32_t reg, uint32_t regs, uint32_t eew_bytes, uint64_t from);
  // After the elements of a mask-producing instruction: the bits past vl.
  void mask_tail(uint32_t reg);

  // --- the pieces
  bool prepare();      // reads vtype and vl; false: the instruction is illegal (vill)
  Step configure();
  Step memory_access();
  Step integer();
  Step opm();
  Step integer_wide();
  Step mask_and_permute();
  Step reductions();
  Step floating();
  Step floating_convert();
  Step float_reduction();
  Step float_wide();
  uint64_t estimate7(softfloat::Format f, uint64_t a, bool reciprocal, softfloat::Env& env);      // vfrec7.v, vfrsqrt7.v

  // Every element-wise instruction with one destination group the size of LMUL: checks the register groups and the mask, then runs `f(a, b, old)` over the active elements and
  // applies the mask and tail policies. `a` is the vs2 element, `b` the vs1 element or the scalar, `old` the destination's current element.
  template <class F>
  static Step binary_loop(VectorUnit& u, bool vv, uint64_t scalar, F&& f) {
    const uint32_t regs = u.regs_of(u.lmul_log);
    if (!u.group_ok(u.vd, regs) || !u.group_ok(u.vs2, regs) || (vv && !u.group_ok(u.vs1, regs))) return u.undefined();
    if (!u.vm && u.vd == 0) return u.undefined();
    if (u.s.vstart >= u.vl) return u.ok();
    for (uint64_t i = u.s.vstart; i < u.vl; ++i) {
      if (!u.active(i)) {
        if (u.ma) u.ones(u.vd, u.sew_bytes, i, i + 1);
        continue;
      }
      const uint64_t b = vv ? u.get(u.vs1, u.sew_bytes, i) : scalar;
      u.put(u.vd, u.sew_bytes, i, f(u.get(u.vs2, u.sew_bytes, i), b, u.get(u.vd, u.sew_bytes, i)) & u.sew_mask());
    }
    u.tail(u.vd, regs, u.sew_bytes, u.vl);
    return u.ok();
  }

  uint64_t round_shift(__int128 value, uint32_t shift) const;      // value >> shift, rounded per vxrm
};

}  // namespace emulation
