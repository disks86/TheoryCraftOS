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
#include "Emulation/AArch64SveCommon.hpp"

// SVE loads and stores (top bytes 0x84 .. 0xE5).
namespace emulation {

using namespace sve;

AArch64Cpu::Step AArch64Cpu::sve_memory(GuestMemory& m, uint32_t insn) {
  // LDR / STR of a predicate or vector register: base plus a signed 9-bit multiple of the register size
  const uint32_t top = insn >> 22;
  if (top == 0x216 || top == 0x396) {      // 1000 0101 10 (LDR) and 1110 0101 10 (STR)
    const bool store = top == 0x396;
    const uint32_t kind = bits_of(insn, 15, 13);
    if (kind != 0 && kind != 2) return undefined();
    const int64_t imm = sext((bits_of(insn, 21, 16) << 3) | bits_of(insn, 12, 10), 9);
    const uint32_t rt = insn & 31, rn = bits_of(insn, 9, 5);
    if (kind == 0 && (insn & 0x10)) return undefined();
    const uint64_t base = reg_sp(rn, true);
    if (kind == 2) {
      const uint64_t address = base + static_cast<uint64_t>(imm * static_cast<int64_t>(kVectorBytes));
      if (store) return store_vec(m, address, kVectorBytes, s_.v[rt]) ? Step::Ok : Step::Fault;
      Vec v;
      if (!load_vec(m, address, kVectorBytes, &v)) return Step::Fault;
      s_.v[rt] = v;
      return Step::Ok;
    }
    const uint64_t address = base + static_cast<uint64_t>(imm * static_cast<int64_t>(kVectorBytes / 8));
    if (store) return this->store(m, address, 2, s_.p[rt & 15]) ? Step::Ok : Step::Fault;
    uint64_t v;
    if (!this->load(m, address, 2, &v)) return Step::Fault;
    s_.p[rt & 15] = static_cast<uint16_t>(v);
    return Step::Ok;
  }
  return undefined();
}

}  // namespace emulation
