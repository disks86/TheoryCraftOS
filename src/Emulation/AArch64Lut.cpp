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

// FEAT_LUT: LUTI2 and LUTI4 in Advanced SIMD and SVE (at a 128-bit vector length they are the same). The table is one or two registers; the indices are the 2- or 4-bit elements of the second
// source that the immediate selects (a segment of as many indices as there are result elements).
namespace emulation {

using namespace sve;

bool AArch64Cpu::lut(uint32_t insn) {
  uint32_t esz, bits, tables, index;
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  auto bit = [&](uint32_t n) { return (insn >> n) & 1u; };
  if ((insn & 0xFFE09C00u) == 0x4E801000u) { esz = 0; bits = 2; tables = 1; index = bit(13) | bit(14) << 1; }
  else if ((insn & 0xFFE08C00u) == 0x4EC00000u) { esz = 1; bits = 2; tables = 1; index = bit(12) | bit(13) << 1 | bit(14) << 2; }
  else if ((insn & 0xFFE0BC00u) == 0x4E402000u) { esz = 0; bits = 4; tables = 1; index = bit(14); }
  else if ((insn & 0xFFE09C00u) == 0x4E401000u) { esz = 1; bits = 4; tables = 2; index = bit(13) | bit(14) << 1; }
  else if ((insn & 0xFF20FC00u) == 0x4520B000u) { esz = 0; bits = 2; tables = 1; index = bit(22) | bit(23) << 1; }
  else if ((insn & 0xFF20EC00u) == 0x4520A800u) { esz = 1; bits = 2; tables = 1; index = bit(12) | bit(22) << 1 | bit(23) << 2; }
  else if ((insn & 0xFF60FC00u) == 0x4560A400u) { esz = 0; bits = 4; tables = 1; index = bit(23); }
  else if ((insn & 0xFF20FC00u) == 0x4520B400u) { esz = 1; bits = 4; tables = 2; index = bit(22) | bit(23) << 1; }
  else return false;
  const uint32_t elements = 16u >> esz;
  const Vec indices = s_.v[rm];
  Vec out;
  for (uint32_t e = 0; e < elements; ++e) {
    const uint32_t position = (index * elements + e) * bits;
    const uint32_t entry = static_cast<uint32_t>(get(indices, 3, position / 64) >> (position % 64)) & ((1u << bits) - 1);
    const Vec& table = s_.v[(rn + (entry >= elements ? 1 : 0)) & 31];
    put(&out, esz, e, get(table, esz, entry % elements));
  }
  s_.v[rd] = out;
  (void)tables;
  return true;
}

}  // namespace emulation
