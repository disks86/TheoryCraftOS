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

enum class X86Reg : uint8_t { Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15 };

// The condition codes of Jcc/SETcc, in the order of their encodings.
enum class X86Cond : uint8_t { O, No, B, Ae, E, Ne, Be, A, S, Ns, P, Np, L, Ge, Le, G };

// An x86-64 instruction encoder: the 64-bit integer core a code generator needs (moves, ALU, shifts, memory operands of the form
// [base + disp], jumps, calls). Every instruction is emitted into a CodeBuffer; the operand order is destination first. Wider instruction
// sets (SSE, AVX) are added here as the emulator and the VM need them.
class X86_64Assembler {
 public:
  explicit X86_64Assembler(CodeBuffer& code) : code_(code) {}
  CodeBuffer& code() { return code_; }

  // Moves
  void mov_rr(X86Reg dst, X86Reg src) { alu_rr(0x89, dst, src); }
  // Picks the shortest encoding for the constant: a 32-bit move (zero-extends), a sign-extended 32-bit immediate, or the full 64-bit one.
  void mov_ri(X86Reg dst, uint64_t imm);
  void mov_load(X86Reg dst, X86Reg base, int32_t disp) { memory(0x8B, true, dst, base, disp); }       // mov dst, [base+disp]
  void mov_store(X86Reg base, int32_t disp, X86Reg src) { memory(0x89, true, src, base, disp); }      // mov [base+disp], src
  void mov_load32(X86Reg dst, X86Reg base, int32_t disp) { memory(0x8B, false, dst, base, disp); }    // zero-extending 32-bit load
  void mov_store32(X86Reg base, int32_t disp, X86Reg src) { memory(0x89, false, src, base, disp); }
  void lea(X86Reg dst, X86Reg base, int32_t disp) { memory(0x8D, true, dst, base, disp); }

  // Arithmetic and logic, two-operand: dst = dst op src.
  void add_rr(X86Reg dst, X86Reg src) { alu_rr(0x01, dst, src); }
  void sub_rr(X86Reg dst, X86Reg src) { alu_rr(0x29, dst, src); }
  void and_rr(X86Reg dst, X86Reg src) { alu_rr(0x21, dst, src); }
  void or_rr(X86Reg dst, X86Reg src) { alu_rr(0x09, dst, src); }
  void xor_rr(X86Reg dst, X86Reg src) { alu_rr(0x31, dst, src); }
  void cmp_rr(X86Reg a, X86Reg b) { alu_rr(0x39, a, b); }   // flags of a - b
  void test_rr(X86Reg a, X86Reg b) { alu_rr(0x85, a, b); }
  void add_ri(X86Reg dst, int32_t imm) { alu_ri(0, dst, imm); }
  void or_ri(X86Reg dst, int32_t imm) { alu_ri(1, dst, imm); }
  void and_ri(X86Reg dst, int32_t imm) { alu_ri(4, dst, imm); }
  void sub_ri(X86Reg dst, int32_t imm) { alu_ri(5, dst, imm); }
  void xor_ri(X86Reg dst, int32_t imm) { alu_ri(6, dst, imm); }
  void cmp_ri(X86Reg a, int32_t imm) { alu_ri(7, a, imm); }
  void imul_rr(X86Reg dst, X86Reg src);
  void neg(X86Reg r) { group3(3, r); }
  void not_(X86Reg r) { group3(2, r); }
  // Shifts by CL and by an immediate (0..63).
  void shl_cl(X86Reg r) { shift_cl(4, r); }
  void shr_cl(X86Reg r) { shift_cl(5, r); }
  void sar_cl(X86Reg r) { shift_cl(7, r); }
  void shl_ri(X86Reg r, uint8_t n) { shift_ri(4, r, n); }
  void shr_ri(X86Reg r, uint8_t n) { shift_ri(5, r, n); }
  void sar_ri(X86Reg r, uint8_t n) { shift_ri(7, r, n); }
  // dst = (flags satisfy cond) ? 1 : 0, as a full 64-bit value.
  void setcc(X86Cond cond, X86Reg dst);

  // Stack and control flow
  void push(X86Reg r);
  void pop(X86Reg r);
  void ret() { code_.emit8(0xC3); }
  void int3() { code_.emit8(0xCC); }
  void call_r(X86Reg r) { opcode_ext(0xFF, 2, r, false); }
  void jmp_r(X86Reg r) { opcode_ext(0xFF, 4, r, false); }
  void jmp(Label target);
  void jcc(X86Cond cond, Label target);
  void call(Label target);

 private:
  static uint8_t low(X86Reg r) { return static_cast<uint8_t>(r) & 7; }
  static bool high(X86Reg r) { return static_cast<uint8_t>(r) >= 8; }
  void rex(bool w, X86Reg reg, X86Reg rm, bool force = false);
  void alu_rr(uint8_t opcode, X86Reg dst, X86Reg src);
  void alu_ri(uint8_t ext, X86Reg dst, int32_t imm);
  void group3(uint8_t ext, X86Reg r) { opcode_ext(0xF7, ext, r); }
  void opcode_ext(uint8_t opcode, uint8_t ext, X86Reg rm, bool wide = true);   // [REX] opcode /ext with a register operand
  void shift_cl(uint8_t ext, X86Reg r) { opcode_ext(0xD3, ext, r); }
  void shift_ri(uint8_t ext, X86Reg r, uint8_t n);
  void memory(uint8_t opcode, bool wide, X86Reg reg, X86Reg base, int32_t disp);
  void branch(uint8_t first, uint8_t second, bool two_bytes, Label target);

  CodeBuffer& code_;
};

}  // namespace jit
