/*
 * TheoryCraftOS - a minimal UEFI operating system.
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

// A LoongArch integer register by number (r0..r31) and the ones the calling convention names.
using LaReg = uint8_t;
constexpr LaReg LaZero = 0;
constexpr LaReg LaRa = 1;
constexpr LaReg LaSp = 3;
constexpr LaReg LaA0 = 4;
constexpr LaReg LaA1 = 5;
constexpr LaReg LaA2 = 6;
constexpr LaReg LaT0 = 12;
constexpr LaReg LaT1 = 13;
constexpr LaReg LaT2 = 14;
constexpr LaReg LaS0 = 23;

// Conditional branches (the opcodes of BEQ, BNE, BLT, BGE, BLTU, BGEU).
enum class LaBranch : uint32_t { Eq = 0x58000000u, Ne = 0x5C000000u, Lt = 0x60000000u, Ge = 0x64000000u, Ltu = 0x68000000u, Geu = 0x6C000000u };

// A LoongArch64 (LA64 base ISA) instruction encoder: constants, ALU, multiply, shifts, loads and stores, branches and jumps. Every instruction is
// 32 bits. Destination first, as in assembly.
class LoongArch64Assembler {
 public:
  explicit LoongArch64Assembler(CodeBuffer& code) : code_(code) {}
  CodeBuffer& code() { return code_; }

  // Loads any 64-bit constant (ADDI.D, or LU12I.W / ORI, then LU32I.D and LU52I.D as far as the value needs them).
  void li(LaReg rd, int64_t value);
  void mv(LaReg rd, LaReg rs) { or_(rd, rs, LaZero); }

  void add(LaReg rd, LaReg a, LaReg b) { r3(0x00108000u, rd, a, b); }
  void sub(LaReg rd, LaReg a, LaReg b) { r3(0x00118000u, rd, a, b); }
  void slt(LaReg rd, LaReg a, LaReg b) { r3(0x00120000u, rd, a, b); }
  void sltu(LaReg rd, LaReg a, LaReg b) { r3(0x00128000u, rd, a, b); }
  void nor(LaReg rd, LaReg a, LaReg b) { r3(0x00140000u, rd, a, b); }
  void and_(LaReg rd, LaReg a, LaReg b) { r3(0x00148000u, rd, a, b); }
  void or_(LaReg rd, LaReg a, LaReg b) { r3(0x00150000u, rd, a, b); }
  void xor_(LaReg rd, LaReg a, LaReg b) { r3(0x00158000u, rd, a, b); }
  void sll(LaReg rd, LaReg a, LaReg b) { r3(0x00188000u, rd, a, b); }   // (the variable shifts use the low 6 bits of the count, as the IR defines)
  void srl(LaReg rd, LaReg a, LaReg b) { r3(0x00190000u, rd, a, b); }
  void sra(LaReg rd, LaReg a, LaReg b) { r3(0x00198000u, rd, a, b); }
  void mul(LaReg rd, LaReg a, LaReg b) { r3(0x001D8000u, rd, a, b); }
  void neg(LaReg rd, LaReg a) { sub(rd, LaZero, a); }
  void not_(LaReg rd, LaReg a) { nor(rd, a, LaZero); }
  void seqz(LaReg rd, LaReg a) { sltui(rd, a, 1); }

  // Immediates are 12 bits; false (nothing emitted) if out of range. ADDI.D / SLTUI are signed, ORI / XORI / ANDI unsigned.
  bool addi(LaReg rd, LaReg a, int32_t imm) { return i12(0x02C00000u, rd, a, imm, true); }
  bool sltui(LaReg rd, LaReg a, int32_t imm) { return i12(0x02400000u, rd, a, imm, true); }
  bool andi(LaReg rd, LaReg a, int32_t imm) { return i12(0x03400000u, rd, a, imm, false); }
  bool ori(LaReg rd, LaReg a, int32_t imm) { return i12(0x03800000u, rd, a, imm, false); }
  bool xori(LaReg rd, LaReg a, int32_t imm) { return i12(0x03C00000u, rd, a, imm, false); }
  void lu12i_w(LaReg rd, uint32_t imm20) { emit(0x14000000u | (imm20 & 0xFFFFFu) << 5 | rd); }
  void lu32i_d(LaReg rd, uint32_t imm20) { emit(0x16000000u | (imm20 & 0xFFFFFu) << 5 | rd); }
  void lu52i_d(LaReg rd, LaReg a, uint32_t imm12) { emit(0x03000000u | (imm12 & 0xFFFu) << 10 | static_cast<uint32_t>(a) << 5 | rd); }

  // 64-bit load / store at [base + offset] (offset 12 bits, signed).
  bool ld(LaReg rd, LaReg base, int32_t offset) { return i12(0x28C00000u, rd, base, offset, true); }
  bool sd(LaReg base, int32_t offset, LaReg src) { return i12(0x29C00000u, src, base, offset, true); }

  void b(Label target) { branch_to(0x50000000u, FixupKind::LaB26, target); }
  void j(Label target) { b(target); }
  // Branches when a <cond> b (the first operand is the instruction's rj, the second its rd).
  void branch(LaBranch cond, LaReg a, LaReg b, Label target) {
    branch_to(static_cast<uint32_t>(cond) | static_cast<uint32_t>(a) << 5 | b, FixupKind::LaBranch16, target);
  }
  void jirl(LaReg rd, LaReg base, int32_t offset_words) { emit(0x4C000000u | (static_cast<uint32_t>(offset_words) & 0xFFFFu) << 10 | static_cast<uint32_t>(base) << 5 | rd); }
  void ret() { jirl(LaZero, LaRa, 0); }
  void brk(uint32_t code = 0) { emit(0x002A0000u | (code & 0x7FFFu)); }

 private:
  void emit(uint32_t word) { code_.emit32(word); }
  void r3(uint32_t opcode, LaReg rd, LaReg a, LaReg b) { emit(opcode | static_cast<uint32_t>(b) << 10 | static_cast<uint32_t>(a) << 5 | rd); }
  bool i12(uint32_t opcode, LaReg rd, LaReg a, int32_t imm, bool is_signed);
  void branch_to(uint32_t word, FixupKind kind, Label target);

  CodeBuffer& code_;
};

}  // namespace jit
