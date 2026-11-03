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
#include "Jit/Emitter/CodeBuffer.hpp"

namespace jit {

// A RISC-V integer register by number (x0..x31) and the ones the calling convention names.
using RvReg = uint8_t;
constexpr RvReg RvZero = 0;
constexpr RvReg RvRa = 1;
constexpr RvReg RvSp = 2;
constexpr RvReg RvT0 = 5;
constexpr RvReg RvT1 = 6;
constexpr RvReg RvT2 = 7;
constexpr RvReg RvS0 = 8;
constexpr RvReg RvS1 = 9;
constexpr RvReg RvA0 = 10;
constexpr RvReg RvA1 = 11;
constexpr RvReg RvA2 = 12;
constexpr RvReg RvA3 = 13;

// Branch conditions (the funct3 of the B-type instructions).
enum class RvBranch : uint8_t { Eq = 0, Ne = 1, Lt = 4, Ge = 5, Ltu = 6, Geu = 7 };

// A RISC-V RV64IM instruction encoder (32-bit instructions only, no C extension): constants, ALU, multiply, shifts, loads and stores,
// branches and jumps. Destination first, as in assembly.
class RiscV64Assembler {
 public:
  explicit RiscV64Assembler(CodeBuffer& code) : code_(code) {}
  CodeBuffer& code() { return code_; }

  // Loads any 64-bit constant (the standard LUI/ADDIW/SLLI/ADDI sequence).
  void li(RvReg rd, int64_t value);
  void mv(RvReg rd, RvReg rs) { addi(rd, rs, 0); }

  void add(RvReg rd, RvReg a, RvReg b) { r_type(0x00, 0, a, b, rd); }
  void sub(RvReg rd, RvReg a, RvReg b) { r_type(0x20, 0, a, b, rd); }
  void sll(RvReg rd, RvReg a, RvReg b) { r_type(0x00, 1, a, b, rd); }
  void slt(RvReg rd, RvReg a, RvReg b) { r_type(0x00, 2, a, b, rd); }
  void sltu(RvReg rd, RvReg a, RvReg b) { r_type(0x00, 3, a, b, rd); }
  void xor_(RvReg rd, RvReg a, RvReg b) { r_type(0x00, 4, a, b, rd); }
  void srl(RvReg rd, RvReg a, RvReg b) { r_type(0x00, 5, a, b, rd); }
  void sra(RvReg rd, RvReg a, RvReg b) { r_type(0x20, 5, a, b, rd); }
  void or_(RvReg rd, RvReg a, RvReg b) { r_type(0x00, 6, a, b, rd); }
  void and_(RvReg rd, RvReg a, RvReg b) { r_type(0x00, 7, a, b, rd); }
  void mul(RvReg rd, RvReg a, RvReg b) { r_type(0x01, 0, a, b, rd); }
  void neg(RvReg rd, RvReg a) { sub(rd, RvZero, a); }
  void not_(RvReg rd, RvReg a) { i_type(0x13, 4, rd, a, -1); }               // XORI rd, a, -1
  void seqz(RvReg rd, RvReg a) { i_type(0x13, 3, rd, a, 1); }                // SLTIU rd, a, 1

  // Immediates are 12 bits, signed; false (nothing emitted) if out of range.
  bool addi(RvReg rd, RvReg a, int32_t imm) { return i_type(0x13, 0, rd, a, imm); }
  bool addiw(RvReg rd, RvReg a, int32_t imm) { return i_type(0x1B, 0, rd, a, imm); }
  bool andi(RvReg rd, RvReg a, int32_t imm) { return i_type(0x13, 7, rd, a, imm); }
  bool ori(RvReg rd, RvReg a, int32_t imm) { return i_type(0x13, 6, rd, a, imm); }
  bool xori(RvReg rd, RvReg a, int32_t imm) { return i_type(0x13, 4, rd, a, imm); }
  void slli(RvReg rd, RvReg a, uint32_t n) { shift_imm(1, 0x00, rd, a, n); }
  void srli(RvReg rd, RvReg a, uint32_t n) { shift_imm(5, 0x00, rd, a, n); }
  void srai(RvReg rd, RvReg a, uint32_t n) { shift_imm(5, 0x10, rd, a, n); }
  void lui(RvReg rd, uint32_t imm20) { emit(0x37u | static_cast<uint32_t>(rd) << 7 | (imm20 & 0xFFFFFu) << 12); }

  // 64-bit load / store at [base + offset] (offset 12 bits, signed).
  bool ld(RvReg rd, RvReg base, int32_t offset) { return i_type(0x03, 3, rd, base, offset); }
  bool sd(RvReg base, int32_t offset, RvReg src);

  void jal(RvReg rd, Label target) { branch_to(0x6Fu | static_cast<uint32_t>(rd) << 7, FixupKind::RvJal20, target); }
  void j(Label target) { jal(RvZero, target); }
  void branch(RvBranch cond, RvReg a, RvReg b, Label target) {
    branch_to(0x63u | static_cast<uint32_t>(cond) << 12 | static_cast<uint32_t>(a) << 15 | static_cast<uint32_t>(b) << 20, FixupKind::RvBranch12, target);
  }
  void jalr(RvReg rd, RvReg base, int32_t offset) { i_type(0x67, 0, rd, base, offset); }
  void jr(RvReg base) { jalr(RvZero, base, 0); }
  void ret() { jalr(RvZero, RvRa, 0); }
  void ebreak() { emit(0x00100073u); }

 private:
  void emit(uint32_t word) { code_.emit32(word); }
  void r_type(uint32_t funct7, uint32_t funct3, RvReg a, RvReg b, RvReg rd) {
    emit(0x33u | static_cast<uint32_t>(rd) << 7 | funct3 << 12 | static_cast<uint32_t>(a) << 15 | static_cast<uint32_t>(b) << 20 | funct7 << 25);
  }
  bool i_type(uint32_t opcode, uint32_t funct3, RvReg rd, RvReg rs1, int32_t imm);
  void shift_imm(uint32_t funct3, uint32_t funct6, RvReg rd, RvReg a, uint32_t n) {
    emit(0x13u | static_cast<uint32_t>(rd) << 7 | funct3 << 12 | static_cast<uint32_t>(a) << 15 | (n & 63u) << 20 | funct6 << 26);
  }
  void branch_to(uint32_t word, FixupKind kind, Label target);

  CodeBuffer& code_;
};

}  // namespace jit
