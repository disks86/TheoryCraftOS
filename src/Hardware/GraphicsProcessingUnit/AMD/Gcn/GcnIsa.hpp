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


// GCN3 (GFX8, Polaris) machine code: operands and an assembler for the subset of the instruction set that the shader compiler produces and the emulator runs. The opcode numbers are
// generated from LLVM's assembler (scripts/gen_gcn_opcodes.py, GcnOpcodes.inc) and the encoder is tested against it (tests/GcnIsaTest.cpp, scripts/gen_gcn_fixtures.py).
#pragma once

#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace gcn {

#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnOpcodes.inc"

// An instruction source: the 9 bit field (scalar register, special register, inline constant, literal marker or vector register) and the 32 bit literal that follows the instruction when
// the field says so.
struct Src {
  uint16_t code = 0;
  bool literal = false;
  uint32_t value = 0;
};

constexpr uint16_t kCodeVccLo = 106, kCodeVccHi = 107, kCodeM0 = 124, kCodeExecLo = 126, kCodeExecHi = 127, kCodeVccz = 251, kCodeExecz = 252, kCodeScc = 253, kCodeLiteral = 255;

inline constexpr Src sgpr(uint32_t n) { return Src{static_cast<uint16_t>(n), false, 0}; }            // s0 .. s101 (a pair or quad: the number of the first)
inline constexpr Src vgpr(uint32_t n) { return Src{static_cast<uint16_t>(256 + n), false, 0}; }     // v0 .. v255
inline constexpr Src vcc() { return Src{kCodeVccLo, false, 0}; }
inline constexpr Src exec() { return Src{kCodeExecLo, false, 0}; }
inline constexpr Src m0() { return Src{kCodeM0, false, 0}; }
inline constexpr Src scc() { return Src{kCodeScc, false, 0}; }
inline constexpr Src vccz() { return Src{kCodeVccz, false, 0}; }
inline constexpr Src execz() { return Src{kCodeExecz, false, 0}; }
// An integer: an inline constant for -16 .. 64, else a literal.
Src imm_int(int32_t value);
// A 32 bit pattern: the inline constant of the same value if there is one (the integers -16 .. 64 and the floats 0.5, 1, 2, 4 and their negatives; 0 is both), else a literal.
Src imm_bits(uint32_t bits);
// A float: imm_bits of its pattern.
Src imm_float(float value);

// The modifiers of a VOP3 instruction: negate (bit n for source n), absolute value (bit n), clamp and the output multiplier (0 none, 1 x2, 2 x4, 3 /2).
struct Mods {
  uint8_t neg = 0, abs = 0;
  bool clamp = false;
  uint8_t omod = 0;
};

// The `export` targets
constexpr uint32_t kExpMrt0 = 0, kExpMrtz = 8, kExpNull = 9, kExpPos0 = 12, kExpParam0 = 32;

class Code {
 public:
  explicit Code(allocators::IAllocator& allocator) : words_(allocator) {}
  uint32_t size() const { return static_cast<uint32_t>(words_.count()); }        // in dwords
  bool ok() const { return ok_; }                                                  // false once an instruction did not fit in memory or an operand was out of range
  ArrayView<const uint32_t> words() const { return words_.view(); }
  uint32_t word(uint32_t index) const { return index < words_.count() ? words_[index] : 0; }

  // Scalar ALU
  void sop1(Sop1 op, uint32_t sdst, Src ssrc0);
  void sop2(Sop2 op, uint32_t sdst, Src ssrc0, Src ssrc1);
  void sopk(Sopk op, uint32_t sdst, int32_t simm16);
  void sopc(Sopc op, Src ssrc0, Src ssrc1);
  void sopp(Sopp op, int32_t simm16 = 0);
  // s_waitcnt with the counters (vmcnt 0..15, expcnt 0..7, lgkmcnt 0..15; pass the maximum for "do not wait")
  void waitcnt(uint32_t vmcnt, uint32_t expcnt, uint32_t lgkmcnt);
  // A branch (any SOPP branch opcode) to a position found later: returns where to patch. patch_branch sets the offset to the instruction at `target` (in dwords).
  uint32_t branch(Sopp op);
  void patch_branch(uint32_t at, uint32_t target);
  // Scalar memory: sbase is the first of a pair (s_load) or a quad (s_buffer_load); `offset_bytes` is an immediate (21 bits)
  void smem(Smem op, uint32_t sdata, uint32_t sbase, uint32_t offset_bytes);
  // Vector ALU. VOP2 with a literal as the first source puts it after the instruction; VOP2 madmk / madak: `literal` follows.
  void vop1(Vop1 op, uint32_t vdst, Src src0);
  void vop2(Vop2 op, uint32_t vdst, Src src0, uint32_t vsrc1);
  void vop2_literal(Vop2 op, uint32_t vdst, Src src0, uint32_t vsrc1, uint32_t literal);       // v_madmk_f32 (vdst = src0 * literal + vsrc1) and v_madak_f32 (src0 * vsrc1 + literal)
  void vopc(Vopc op, Src src0, uint32_t vsrc1);                                                  // writes VCC
  void vop3(Vop3 op, uint32_t vdst, Src src0, Src src1, Src src2, Mods mods = Mods());
  // VOP3 forms of the VOP1 / VOP2 / VOPC instructions (for modifiers, a scalar destination, or sources a 32 bit encoding cannot name)
  void vop3_of(Vop1 op, uint32_t vdst, Src src0, Mods mods = Mods());
  void vop3_of(Vop2 op, uint32_t vdst, Src src0, Src src1, Mods mods = Mods());
  void vop3_of(Vopc op, uint32_t sdst, Src src0, Src src1, Mods mods = Mods());                 // the result mask goes to the SGPR pair `sdst`
  void vop3_of(Vop2 op, uint32_t vdst, Src src0, Src src1, Src carry_or_select, Mods mods = Mods());       // v_cndmask_b32 with an SGPR pair as the select
  void vintrp(Vintrp op, uint32_t vdst, uint32_t vsrc, uint32_t attr, uint32_t chan);             // vsrc: the VGPR with i or j; for v_interp_mov_f32 the parameter (0 = P10, 1 = P20, 2 = P0)
  // Export: `enable` is a 4 bit mask of the sources; with `compressed` the sources are two packed 16 bit pairs in vsrc0 and vsrc1
  void exp(uint32_t target, uint32_t enable, uint32_t vsrc0, uint32_t vsrc1, uint32_t vsrc2, uint32_t vsrc3, bool done, bool valid_mask, bool compressed = false);
  // Buffer access: vdata (first of the registers), vaddr, srsrc (first SGPR of the quad), soffset (a scalar source), the 12 bit immediate offset and the address modes
  struct BufferMode {
    bool offen = false, idxen = false, glc = false, slc = false;
    uint32_t offset = 0;
  };
  void mubuf(Mubuf op, uint32_t vdata, uint32_t vaddr, uint32_t srsrc, Src soffset, BufferMode mode);
  // Image access: vdata, vaddr (the first of the coordinates), srsrc (first SGPR of the 8), ssamp (first SGPR of the 4), the component mask
  void mimg(Mimg op, uint32_t vdata, uint32_t vaddr, uint32_t srsrc, uint32_t ssamp, uint32_t dmask, bool unorm = false);

 private:
  void put(uint32_t word);
  void bad() { ok_ = false; }
  void src_literal(const Src& s);
  List<uint32_t> words_;
  bool ok_ = true;
};

// Reading the code back (for the emulator and for tests): the length in dwords of the instruction starting at `words[0]`, 0 if it is not one this assembler makes.
uint32_t instruction_length(ArrayView<const uint32_t> words);

}  // namespace gcn
