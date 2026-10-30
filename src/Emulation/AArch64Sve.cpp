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
#include "Emulation/Wide.hpp"

// SVE: the dispatcher over the top byte of the instruction, and what the files share. The vector length is 128 bits.
namespace emulation {

namespace sve {

uint64_t multiply_high(uint64_t a, uint64_t b, bool is_signed) {
  uint64_t hi, lo;
  multiply_unsigned(a, b, &hi, &lo);
  if (is_signed) {
    if (static_cast<int64_t>(a) < 0) hi -= b;
    if (static_cast<int64_t>(b) < 0) hi -= a;
  }
  return hi;
}

}  // namespace sve

using namespace sve;

// PredTest: N is the first active element of `mask` being set in `result`, Z no active element set, C the last active element NOT set.
void AArch64Cpu::sve_set_flags(uint16_t mask, uint16_t result, uint32_t esz) {
  const uint16_t m = static_cast<uint16_t>(mask & element_mask(esz));
  const uint16_t r = static_cast<uint16_t>(result & m);
  uint32_t flags = 0;
  if (m != 0) {
    uint32_t first = 0, last = 15;
    while (!((m >> first) & 1)) ++first;
    while (!((m >> last) & 1)) --last;
    if ((r >> first) & 1) flags |= kN;
    if (!((r >> last) & 1)) flags |= kC;
  } else {
    flags |= kC;
  }
  if (r == 0) flags |= kZ;
  s_.nzcv = flags;
}

AArch64Cpu::Step AArch64Cpu::sve(GuestMemory& m, uint32_t insn) {
  switch (insn >> 24) {
    case 0x04: return sve_integer(insn) ? Step::Ok : undefined();
    case 0x05: return sve_permute(insn) ? Step::Ok : undefined();
    case 0x24: case 0x25: return sve_predicate(insn) ? Step::Ok : undefined();
    case 0x44: case 0x45: {
      Step fp8_result;
      if (((insn >> 24) & 0xFE) == 0x44 && lut(insn)) return Step::Ok;
      return sve2(insn) ? Step::Ok : undefined();
    }
    case 0x64: case 0x65: {
      Step fp8_result;
      if (fp8_sve(insn, &fp8_result)) return fp8_result;
      return sve_float(insn) ? Step::Ok : undefined();
    }
    case 0x84: case 0x85: case 0xA4: case 0xA5: case 0xC4: case 0xC5: case 0xE4: case 0xE5: return sve_memory(m, insn);
    default: return undefined();
  }
}

}  // namespace emulation
