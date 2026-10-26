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
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware::v3d_qpu {

// An assembler for the part of the V3D 4.2 QPU instruction set the OS's own shaders use (the layout follows Mesa's broadcom/qpu/qpu_pack.c, which the host test
// uses as the reference). A QPU instruction is 64 bits: an add ALU operation and a multiply ALU operation, signal bits (thread switch, load varying, load
// uniform into a register), up to two register file reads (ports A and B, where B can instead carry a small immediate) and the destinations of each part.
// Nothing here knows the pipeline hazards of the machine; the program builder (V3dShaders) places the nops those need.

enum class Add : uint8_t { Nop, Fadd, Or, Ftoiz, Ffloor, Vfpack, LdVpmvIn, StVpmv, VpmWt };
enum class Mul : uint8_t { Nop, Fmul };

// A source: nothing, a register file entry, an accumulator (r0..r5; r4 is where an SFU result shows up, r5 where the varying's C coefficient is) or a small
// immediate (0..15).
struct Operand {
  enum class Kind : uint8_t { None, Rf, Acc, Imm } kind = Kind::None;
  uint8_t n = 0;
};
constexpr Operand none() { return {}; }
constexpr Operand rf(uint8_t n) { return {Operand::Kind::Rf, n}; }
constexpr Operand acc(uint8_t n) { return {Operand::Kind::Acc, n}; }
constexpr Operand imm(uint8_t n) { return {Operand::Kind::Imm, n}; }

// A destination: a register file entry or a "magic" address (the TLB, the SFU, nowhere).
struct Dest {
  bool magic = true;
  uint8_t addr = 6;  // nowhere
};
constexpr Dest rf_dest(uint8_t n) { return {false, n}; }
constexpr Dest nowhere() { return {true, 6}; }
constexpr Dest tlb() { return {true, 7}; }    // colour data for the tile buffer
constexpr Dest tlbu() { return {true, 8}; }   // the first colour write: takes the next uniform as its configuration
constexpr Dest recip() { return {true, 19}; } // starts a reciprocal; the result shows up in r4 three instructions later

struct Instruction {
  Add add_op = Add::Nop;
  Dest add_dest = nowhere();
  Operand add_a, add_b;
  Mul mul_op = Mul::Nop;
  Dest mul_dest = nowhere();
  Operand mul_a, mul_b;
  bool thrsw = false;      // thread switch (three of these in a row, the first signalled and two delay slots, end a program)
  bool ldvary = false;     // load the next varying into `sig_dest`; the C coefficient follows in r5
  bool ldunifrf = false;   // load the next uniform into `sig_dest`
  Dest sig_dest = rf_dest(0);
};

// Encodes one instruction. False if it cannot be encoded: more than two register file entries or an immediate besides one, an immediate out of range, a register
// beyond the file, or a combination of signals the hardware has no code for.
bool encode(const Instruction& instruction, uint64_t* word);

// A list of instructions in memory the GPU will read (8 bytes each).
class Program {
 public:
  explicit Program(ArrayView<uint64_t> storage) : storage_(storage) {}
  bool emit(const Instruction& instruction);
  bool nop(uint32_t count = 1);
  uint32_t count() const { return count_; }
  bool ok() const { return ok_; }
  ArrayView<const uint64_t> words() const { return ArrayView<const uint64_t>(storage_.data(), count_); }

 private:
  ArrayView<uint64_t> storage_;
  uint32_t count_ = 0;
  bool ok_ = true;
};

}  // namespace hardware::v3d_qpu
