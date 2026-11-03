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
  // Narrow loads (zero or sign extended to 64 bits) and stores of the low 8 / 16 / 32 bits.
  void mov_load8_zx(X86Reg dst, X86Reg base, int32_t disp) { memory2(0x0F, 0xB6, true, dst, base, disp); }   // movzx r64, byte [base+disp]
  void mov_load16_zx(X86Reg dst, X86Reg base, int32_t disp) { memory2(0x0F, 0xB7, true, dst, base, disp); }
  void mov_load8_sx(X86Reg dst, X86Reg base, int32_t disp) { memory2(0x0F, 0xBE, true, dst, base, disp); }   // movsx r64, byte [base+disp]
  void mov_load16_sx(X86Reg dst, X86Reg base, int32_t disp) { memory2(0x0F, 0xBF, true, dst, base, disp); }
  void mov_load32_sx(X86Reg dst, X86Reg base, int32_t disp) { memory(0x63, true, dst, base, disp); }         // movsxd r64, dword [base+disp]
  void mov_store8(X86Reg base, int32_t disp, X86Reg src);
  void mov_store16(X86Reg base, int32_t disp, X86Reg src);

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
  // Scalar double precision SSE2 on xmm0..xmm15 (given by number). movq moves the 64 bits between a general and an xmm register.
  void movq_to_xmm(uint8_t xmm, X86Reg src) { sse(0x66, 0x6E, true, xmm, static_cast<uint8_t>(src)); }
  void movq_from_xmm(X86Reg dst, uint8_t xmm) { sse(0x66, 0x7E, true, xmm, static_cast<uint8_t>(dst)); }
  void addsd(uint8_t dst, uint8_t src) { sse(0xF2, 0x58, false, dst, src); }
  void mulsd(uint8_t dst, uint8_t src) { sse(0xF2, 0x59, false, dst, src); }
  void subsd(uint8_t dst, uint8_t src) { sse(0xF2, 0x5C, false, dst, src); }
  void divsd(uint8_t dst, uint8_t src) { sse(0xF2, 0x5E, false, dst, src); }
  void ucomisd(uint8_t a, uint8_t b) { sse(0x66, 0x2E, false, a, b); }                                // flags of a compared with b (unordered: ZF=PF=CF=1)
  void movd_to_xmm(uint8_t xmm, X86Reg src) { sse(0x66, 0x6E, false, xmm, static_cast<uint8_t>(src)); }      // the low 32 bits
  void movd_from_xmm(X86Reg dst, uint8_t xmm) { sse(0x66, 0x7E, false, xmm, static_cast<uint8_t>(dst)); }   // zero extended
  void cvtss2sd(uint8_t dst, uint8_t src) { sse(0xF3, 0x5A, false, dst, src); }
  void cvtsd2ss(uint8_t dst, uint8_t src) { sse(0xF2, 0x5A, false, dst, src); }
  void cvtsi2sd(uint8_t dst, X86Reg src) { sse(0xF2, 0x2A, true, dst, static_cast<uint8_t>(src)); }   // xmm = (double) signed 64-bit register
  void cvttsd2si(X86Reg dst, uint8_t src) { sse(0xF2, 0x2C, true, static_cast<uint8_t>(dst), src); }  // truncating; 0x8000000000000000 when it does not fit
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
  void memory_operand(uint8_t opcode, X86Reg reg, X86Reg base, int32_t disp, bool opcode_done = false);
  void memory2(uint8_t first, uint8_t second, bool wide, X86Reg reg, X86Reg base, int32_t disp);   // the same with a two byte opcode
  void sse(uint8_t prefix, uint8_t opcode, bool wide, uint8_t reg, uint8_t rm);                      // prefix [REX] 0F opcode, register to register
  void branch(uint8_t first, uint8_t second, bool two_bytes, Label target);

  CodeBuffer& code_;
};

}  // namespace jit
