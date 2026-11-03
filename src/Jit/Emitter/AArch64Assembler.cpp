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
#include "Jit/Emitter/AArch64Assembler.hpp"

namespace jit {

void AArch64Assembler::mov_imm(A64Reg rd, uint64_t imm) {
  // Count the 16-bit chunks that are all zeros and all ones; start from whichever kind there are more of (MOVZ or MOVN), then MOVK the rest.
  uint32_t zeros = 0, ones = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint16_t chunk = static_cast<uint16_t>(imm >> (16 * i));
    if (chunk == 0) ++zeros;
    if (chunk == 0xFFFF) ++ones;
  }
  const bool invert = ones > zeros;
  const uint16_t skip = invert ? 0xFFFF : 0;
  bool first = true;
  bool any = false;
  for (uint32_t i = 0; i < 4; ++i) any = any || static_cast<uint16_t>(imm >> (16 * i)) != skip;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint16_t chunk = static_cast<uint16_t>(imm >> (16 * i));
    if (chunk == skip && (any || i != 0)) continue;   // (all chunks equal to `skip`: one instruction for chunk 0)
    if (first) {
      const uint16_t field = invert ? static_cast<uint16_t>(~chunk) : chunk;
      emit((invert ? 0x92800000u : 0xD2800000u) | i << 21 | static_cast<uint32_t>(field) << 5 | rd);
      first = false;
    } else {
      emit(0xF2800000u | i << 21 | static_cast<uint32_t>(chunk) << 5 | rd);
    }
  }
}

bool AArch64Assembler::memory(uint32_t base, A64Reg rt, A64Reg rn, uint32_t offset) {
  if (offset % 8 != 0 || offset / 8 > 0xFFFu) return false;
  emit(base | (offset / 8) << 10 | rn << 5 | rt);
  return true;
}

void AArch64Assembler::pair(uint32_t base, A64Reg rt, A64Reg rt2, int32_t bytes) {
  emit(base | (static_cast<uint32_t>(bytes / 8) & 0x7Fu) << 15 | static_cast<uint32_t>(rt2) << 10 | static_cast<uint32_t>(A64Sp) << 5 | rt);
}

void AArch64Assembler::branch(uint32_t word, FixupKind kind, Label target) {
  const uint64_t at = code_.size();
  emit(word);
  code_.add_fixup(at, kind, target, at);
}

}  // namespace jit
