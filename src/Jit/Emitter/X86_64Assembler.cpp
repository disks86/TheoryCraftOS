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
#include "Jit/Emitter/X86_64Assembler.hpp"

namespace jit {

void X86_64Assembler::rex(bool w, X86Reg reg, X86Reg rm, bool force) {
  const uint8_t value = static_cast<uint8_t>(0x40 | (w ? 8 : 0) | (high(reg) ? 4 : 0) | (high(rm) ? 1 : 0));
  if (value != 0x40 || force) code_.emit8(value);
}

void X86_64Assembler::alu_rr(uint8_t opcode, X86Reg dst, X86Reg src) {
  rex(true, src, dst);
  code_.emit8(opcode);
  code_.emit8(static_cast<uint8_t>(0xC0 | low(src) << 3 | low(dst)));
}

void X86_64Assembler::alu_ri(uint8_t ext, X86Reg dst, int32_t imm) {
  rex(true, X86Reg::Rax, dst);
  code_.emit8(0x81);
  code_.emit8(static_cast<uint8_t>(0xC0 | ext << 3 | low(dst)));
  code_.emit32(static_cast<uint32_t>(imm));
}

void X86_64Assembler::opcode_ext(uint8_t opcode, uint8_t ext, X86Reg rm, bool wide) {
  rex(wide, X86Reg::Rax, rm);
  code_.emit8(opcode);
  code_.emit8(static_cast<uint8_t>(0xC0 | ext << 3 | low(rm)));
}

void X86_64Assembler::shift_ri(uint8_t ext, X86Reg r, uint8_t n) {
  rex(true, X86Reg::Rax, r);
  code_.emit8(0xC1);
  code_.emit8(static_cast<uint8_t>(0xC0 | ext << 3 | low(r)));
  code_.emit8(static_cast<uint8_t>(n & 63));
}

void X86_64Assembler::imul_rr(X86Reg dst, X86Reg src) {
  rex(true, dst, src);
  code_.emit8(0x0F);
  code_.emit8(0xAF);
  code_.emit8(static_cast<uint8_t>(0xC0 | low(dst) << 3 | low(src)));
}

void X86_64Assembler::mov_ri(X86Reg dst, uint64_t imm) {
  if (imm <= 0xFFFFFFFFull) {
    // mov r32, imm32 (clears the upper half)
    if (high(dst)) code_.emit8(0x41);
    code_.emit8(static_cast<uint8_t>(0xB8 + low(dst)));
    code_.emit32(static_cast<uint32_t>(imm));
  } else if (static_cast<int64_t>(imm) >= INT32_MIN && static_cast<int64_t>(imm) < 0) {
    rex(true, X86Reg::Rax, dst);   // mov r/m64, imm32 (sign-extended)
    code_.emit8(0xC7);
    code_.emit8(static_cast<uint8_t>(0xC0 | low(dst)));
    code_.emit32(static_cast<uint32_t>(imm));
  } else {
    rex(true, X86Reg::Rax, dst);   // movabs
    code_.emit8(static_cast<uint8_t>(0xB8 + low(dst)));
    code_.emit64(imm);
  }
}

void X86_64Assembler::setcc(X86Cond cond, X86Reg dst) {
  // setcc r8 needs a REX prefix for SPL/BPL/SIL/DIL; then zero-extend with movzx r32, r8.
  rex(false, X86Reg::Rax, dst, true);
  code_.emit8(0x0F);
  code_.emit8(static_cast<uint8_t>(0x90 + static_cast<uint8_t>(cond)));
  code_.emit8(static_cast<uint8_t>(0xC0 | low(dst)));
  rex(false, dst, dst, true);
  code_.emit8(0x0F);
  code_.emit8(0xB6);
  code_.emit8(static_cast<uint8_t>(0xC0 | low(dst) << 3 | low(dst)));
}

void X86_64Assembler::push(X86Reg r) {
  if (high(r)) code_.emit8(0x41);
  code_.emit8(static_cast<uint8_t>(0x50 + low(r)));
}

void X86_64Assembler::pop(X86Reg r) {
  if (high(r)) code_.emit8(0x41);
  code_.emit8(static_cast<uint8_t>(0x58 + low(r)));
}

void X86_64Assembler::memory(uint8_t opcode, bool wide, X86Reg reg, X86Reg base, int32_t disp) {
  rex(wide, reg, base);
  memory_operand(opcode, reg, base, disp);
}

// The ModRM byte (and SIB, displacement) of [base+disp] after the opcode `opcode` has been emitted by the caller or is emitted here.
void X86_64Assembler::memory_operand(uint8_t opcode, X86Reg reg, X86Reg base, int32_t disp, bool opcode_done) {
  if (!opcode_done) code_.emit8(opcode);
  // mod: 00 no displacement (not allowed with rbp/r13 as base), 01 disp8, 10 disp32. rsp/r12 as base need a SIB byte.
  const bool no_disp = disp == 0 && low(base) != 5;
  const bool small = disp >= -128 && disp <= 127;
  const uint8_t mod = no_disp ? 0x00 : small ? 0x40 : 0x80;
  code_.emit8(static_cast<uint8_t>(mod | low(reg) << 3 | low(base)));
  if (low(base) == 4) code_.emit8(0x24);
  if (!no_disp) {
    if (small) code_.emit8(static_cast<uint8_t>(disp));
    else code_.emit32(static_cast<uint32_t>(disp));
  }
}

void X86_64Assembler::memory2(uint8_t first, uint8_t second, bool wide, X86Reg reg, X86Reg base, int32_t disp) {
  rex(wide, reg, base);
  code_.emit8(first);
  memory_operand(second, reg, base, disp);
}

void X86_64Assembler::mov_store8(X86Reg base, int32_t disp, X86Reg src) {
  rex(false, src, base, true);   // (a REX prefix makes the register operand sil / dil / spl / bpl, not dh / bh ...)
  code_.emit8(0x88);
  memory_operand(0x88, src, base, disp, true);
}

void X86_64Assembler::mov_store16(X86Reg base, int32_t disp, X86Reg src) {
  code_.emit8(0x66);
  rex(false, src, base);
  code_.emit8(0x89);
  memory_operand(0x89, src, base, disp, true);
}

void X86_64Assembler::sse(uint8_t prefix, uint8_t opcode, bool wide, uint8_t reg, uint8_t rm) {
  code_.emit8(prefix);
  const uint8_t value = static_cast<uint8_t>(0x40 | (wide ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0));
  if (value != 0x40) code_.emit8(value);
  code_.emit8(0x0F);
  code_.emit8(opcode);
  code_.emit8(static_cast<uint8_t>(0xC0 | (reg & 7) << 3 | (rm & 7)));
}

void X86_64Assembler::branch(uint8_t first, uint8_t second, bool two_bytes, Label target) {
  code_.emit8(first);
  if (two_bytes) code_.emit8(second);
  const uint64_t at = code_.size();
  code_.emit32(0);
  code_.add_fixup(at, FixupKind::X86Rel32, target, at + 4);
}

void X86_64Assembler::jmp(Label target) { branch(0xE9, 0, false, target); }
void X86_64Assembler::call(Label target) { branch(0xE8, 0, false, target); }
void X86_64Assembler::jcc(X86Cond cond, Label target) { branch(0x0F, static_cast<uint8_t>(0x80 + static_cast<uint8_t>(cond)), true, target); }

}  // namespace jit
