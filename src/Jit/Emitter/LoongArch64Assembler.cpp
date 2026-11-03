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
#include "Jit/Emitter/LoongArch64Assembler.hpp"

namespace jit {

bool LoongArch64Assembler::i12(uint32_t opcode, LaReg rd, LaReg a, int32_t imm, bool is_signed) {
  if (is_signed ? (imm < -2048 || imm > 2047) : (imm < 0 || imm > 4095)) return false;
  emit(opcode | (static_cast<uint32_t>(imm) & 0xFFFu) << 10 | static_cast<uint32_t>(a) << 5 | rd);
  return true;
}

void LoongArch64Assembler::branch_to(uint32_t word, FixupKind kind, Label target) {
  const uint64_t at = code_.size();
  emit(word);
  code_.add_fixup(at, kind, target, at);
}

void LoongArch64Assembler::li(LaReg rd, int64_t value) {
  if (value >= -2048 && value <= 2047) {
    addi(rd, LaZero, static_cast<int32_t>(value));
    return;
  }
  const uint32_t low = static_cast<uint32_t>(value);
  const uint32_t hi20 = low >> 12, lo12 = low & 0xFFFu;
  if (hi20 == 0) {
    ori(rd, LaZero, static_cast<int32_t>(lo12));   // 2048..4095 (the rest of the 12-bit range went above)
  } else {
    lu12i_w(rd, hi20);                             // bits 31:12, sign-extended into 63:32
    if (lo12) ori(rd, rd, static_cast<int32_t>(lo12));
  }
  if (value >= INT32_MIN && value <= INT32_MAX) return;   // the sign extension is the value
  lu32i_d(rd, static_cast<uint32_t>(value >> 32) & 0xFFFFFu);   // bits 51:32, sign-extended into 63:52
  const uint32_t top = static_cast<uint32_t>(value >> 52) & 0xFFFu;
  const bool bit51 = (value >> 51) & 1;
  if (top != (bit51 ? 0xFFFu : 0u)) lu52i_d(rd, rd, top);
}

}  // namespace jit
