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

// An AArch64 general register by number (0..30), or A64Sp / A64Zr (both are number 31; which one an instruction means depends on the instruction).
using A64Reg = uint8_t;
constexpr A64Reg A64Fp = 29;
constexpr A64Reg A64Lr = 30;
constexpr A64Reg A64Sp = 31;
constexpr A64Reg A64Zr = 31;

// Condition codes of B.cond / CSET, in the order of their encodings.
enum class A64Cond : uint8_t { Eq, Ne, Hs, Lo, Mi, Pl, Vs, Vc, Hi, Ls, Ge, Lt, Gt, Le };

// An AArch64 instruction encoder for the 64-bit integer core: constants, ALU, shifts, loads and stores, branches. All instructions are 4 bytes.
// Destination first, as in assembly.
class AArch64Assembler {
 public:
  explicit AArch64Assembler(CodeBuffer& code) : code_(code) {}
  CodeBuffer& code() { return code_; }

  // A 64-bit constant in as few MOVZ/MOVN/MOVK instructions as it takes.
  void mov_imm(A64Reg rd, uint64_t imm);
  void mov_rr(A64Reg rd, A64Reg rm) { logic(0xAA000000u, rd, A64Zr, rm); }   // ORR rd, xzr, rm   (use add_ri(rd, sp, 0) when SP is involved)

  void add_rr(A64Reg rd, A64Reg rn, A64Reg rm) { emit(0x8B000000u | rm << 16 | rn << 5 | rd); }
  void sub_rr(A64Reg rd, A64Reg rn, A64Reg rm) { emit(0xCB000000u | rm << 16 | rn << 5 | rd); }
  void add_ri(A64Reg rd, A64Reg rn, uint32_t imm12) { emit(0x91000000u | (imm12 & 0xFFFu) << 10 | rn << 5 | rd); }
  void sub_ri(A64Reg rd, A64Reg rn, uint32_t imm12) { emit(0xD1000000u | (imm12 & 0xFFFu) << 10 | rn << 5 | rd); }
  void and_rr(A64Reg rd, A64Reg rn, A64Reg rm) { logic(0x8A000000u, rd, rn, rm); }
  void orr_rr(A64Reg rd, A64Reg rn, A64Reg rm) { logic(0xAA000000u, rd, rn, rm); }
  void eor_rr(A64Reg rd, A64Reg rn, A64Reg rm) { logic(0xCA000000u, rd, rn, rm); }
  void mvn(A64Reg rd, A64Reg rm) { logic(0xAA200000u, rd, A64Zr, rm); }       // ORN rd, xzr, rm
  void neg(A64Reg rd, A64Reg rm) { sub_rr(rd, A64Zr, rm); }
  void mul(A64Reg rd, A64Reg rn, A64Reg rm) { emit(0x9B007C00u | rm << 16 | rn << 5 | rd); }
  void lsl_rr(A64Reg rd, A64Reg rn, A64Reg rm) { emit(0x9AC02000u | rm << 16 | rn << 5 | rd); }
  void lsr_rr(A64Reg rd, A64Reg rn, A64Reg rm) { emit(0x9AC02400u | rm << 16 | rn << 5 | rd); }
  void asr_rr(A64Reg rd, A64Reg rn, A64Reg rm) { emit(0x9AC02800u | rm << 16 | rn << 5 | rd); }
  void cmp_rr(A64Reg rn, A64Reg rm) { emit(0xEB000000u | rm << 16 | rn << 5 | A64Zr); }   // SUBS xzr, rn, rm
  void cset(A64Reg rd, A64Cond cond) { emit(0x9A9F07E0u | (static_cast<uint32_t>(cond) ^ 1u) << 12 | rd); }

  // Loads and stores of 64 bits at [base + offset]; the offset is a multiple of 8 in 0..32760.
  bool ldr(A64Reg rt, A64Reg base, uint32_t offset) { return memory(0xF9400000u, rt, base, offset); }
  bool str(A64Reg rt, A64Reg base, uint32_t offset) { return memory(0xF9000000u, rt, base, offset); }
  // Narrow loads (zero or sign extended to 64 bits) and stores of the low 8 / 16 / 32 bits at [base] (no offset: the back end puts the address in a register).
  void ldrb(A64Reg rt, A64Reg base) { emit(0x39400000u | base << 5 | rt); }
  void ldrh(A64Reg rt, A64Reg base) { emit(0x79400000u | base << 5 | rt); }
  void ldr_w(A64Reg rt, A64Reg base) { emit(0xB9400000u | base << 5 | rt); }
  void ldrsb(A64Reg rt, A64Reg base) { emit(0x39800000u | base << 5 | rt); }
  void ldrsh(A64Reg rt, A64Reg base) { emit(0x79800000u | base << 5 | rt); }
  void ldrsw(A64Reg rt, A64Reg base) { emit(0xB9800000u | base << 5 | rt); }
  void strb(A64Reg rt, A64Reg base) { emit(0x39000000u | base << 5 | rt); }
  void strh(A64Reg rt, A64Reg base) { emit(0x79000000u | base << 5 | rt); }
  void str_w(A64Reg rt, A64Reg base) { emit(0xB9000000u | base << 5 | rt); }

  // Scalar double precision floating point on d0..d31 (given by number); fmov moves the bits between a general and a floating point register.
  void fmov_to_d(uint8_t d, A64Reg x) { emit(0x9E670000u | x << 5 | d); }
  void fmov_from_d(A64Reg x, uint8_t d) { emit(0x9E660000u | d << 5 | x); }
  void fadd(uint8_t d, uint8_t n, uint8_t m) { emit(0x1E602800u | m << 16 | n << 5 | d); }
  void fsub(uint8_t d, uint8_t n, uint8_t m) { emit(0x1E603800u | m << 16 | n << 5 | d); }
  void fmul(uint8_t d, uint8_t n, uint8_t m) { emit(0x1E600800u | m << 16 | n << 5 | d); }
  void fdiv(uint8_t d, uint8_t n, uint8_t m) { emit(0x1E601800u | m << 16 | n << 5 | d); }
  void fcmp(uint8_t n, uint8_t m) { emit(0x1E602000u | m << 16 | n << 5); }          // unordered: N=0 Z=0 C=1 V=1
  void scvtf(uint8_t d, A64Reg x) { emit(0x9E620000u | x << 5 | d); }                 // d = (double) signed 64-bit x
  void fcvtzs(A64Reg x, uint8_t d) { emit(0x9E780000u | d << 5 | x); }                // truncating, saturating; a NaN gives 0
  void fmov_to_s(uint8_t s, A64Reg w) { emit(0x1E270000u | w << 5 | s); }             // the low 32 bits
  void fmov_from_s(A64Reg w, uint8_t s) { emit(0x1E260000u | s << 5 | w); }           // zero extended (a write to w clears the upper half)
  void fcvt_s_to_d(uint8_t d, uint8_t s) { emit(0x1E22C000u | s << 5 | d); }
  void fcvt_d_to_s(uint8_t s, uint8_t d) { emit(0x1E624000u | d << 5 | s); }
  // STP rt, rt2, [sp, #-bytes]! and LDP rt, rt2, [sp], #bytes: push and pop a pair (bytes a multiple of 8 in 8..504).
  void stp_push(A64Reg rt, A64Reg rt2, uint32_t bytes) { pair(0xA9800000u, rt, rt2, -static_cast<int32_t>(bytes)); }
  void ldp_pop(A64Reg rt, A64Reg rt2, uint32_t bytes) { pair(0xA8C00000u, rt, rt2, static_cast<int32_t>(bytes)); }

  void b(Label target) { branch(0x14000000u, FixupKind::A64Branch26, target); }
  void bl(Label target) { branch(0x94000000u, FixupKind::A64Branch26, target); }
  void b_cond(A64Cond cond, Label target) { branch(0x54000000u | static_cast<uint32_t>(cond), FixupKind::A64Branch19, target); }
  void cbz(A64Reg rt, Label target) { branch(0xB4000000u | rt, FixupKind::A64Branch19, target); }
  void cbnz(A64Reg rt, Label target) { branch(0xB5000000u | rt, FixupKind::A64Branch19, target); }
  void br(A64Reg rn) { emit(0xD61F0000u | rn << 5); }
  void blr(A64Reg rn) { emit(0xD63F0000u | rn << 5); }
  void ret() { emit(0xD65F03C0u); }
  void brk(uint16_t imm = 0) { emit(0xD4200000u | static_cast<uint32_t>(imm) << 5); }

 private:
  void emit(uint32_t word) { code_.emit32(word); }
  void logic(uint32_t base, A64Reg rd, A64Reg rn, A64Reg rm) { emit(base | rm << 16 | rn << 5 | rd); }
  bool memory(uint32_t base, A64Reg rt, A64Reg rn, uint32_t offset);
  void pair(uint32_t base, A64Reg rt, A64Reg rt2, int32_t bytes);
  void branch(uint32_t word, FixupKind kind, Label target);

  CodeBuffer& code_;
};

}  // namespace jit
