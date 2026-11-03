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
#include "Jit/Emitter/RiscV64Assembler.hpp"

namespace jit {

bool RiscV64Assembler::i_type(uint32_t opcode, uint32_t funct3, RvReg rd, RvReg rs1, int32_t imm) {
  if (imm < -2048 || imm > 2047) return false;
  emit(opcode | static_cast<uint32_t>(rd) << 7 | funct3 << 12 | static_cast<uint32_t>(rs1) << 15 | (static_cast<uint32_t>(imm) & 0xFFFu) << 20);
  return true;
}

bool RiscV64Assembler::sd(RvReg base, int32_t offset, RvReg src) {
  if (offset < -2048 || offset > 2047) return false;
  const uint32_t imm = static_cast<uint32_t>(offset) & 0xFFFu;
  emit(0x23u | (imm & 0x1Fu) << 7 | 3u << 12 | static_cast<uint32_t>(base) << 15 | static_cast<uint32_t>(src) << 20 | (imm >> 5) << 25);
  return true;
}

void RiscV64Assembler::branch_to(uint32_t word, FixupKind kind, Label target) {
  const uint64_t at = code_.size();
  emit(word);
  code_.add_fixup(at, kind, target, at);
}

void RiscV64Assembler::li(RvReg rd, int64_t value) {
  // The low 12 bits as a signed number; what is left over is a multiple of 4096.
  const int32_t lo12 = static_cast<int32_t>((value & 0xFFF) ^ 0x800) - 0x800;
  if (value >= INT32_MIN && value <= INT32_MAX) {
    const uint32_t hi20 = static_cast<uint32_t>((value + 0x800) >> 12) & 0xFFFFFu;
    if (hi20 == 0) {
      addi(rd, RvZero, lo12);
      return;
    }
    lui(rd, hi20);
    if (lo12 != 0) addiw(rd, rd, lo12);
    return;
  }
  // (unsigned arithmetic: value - lo12 may leave the signed range, and the shift below then restores it)
  const int64_t rest = static_cast<int64_t>(static_cast<uint64_t>(value) - static_cast<uint64_t>(static_cast<int64_t>(lo12))) >> 12;
  li(rd, rest);
  slli(rd, rd, 12);
  if (lo12 != 0) addi(rd, rd, lo12);
}

}  // namespace jit
