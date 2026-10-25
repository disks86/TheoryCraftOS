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

#include "Hardware/VideoCodec/Intel/IntelAv1Cdf.hpp"

namespace hardware {
namespace intel_av1 {

namespace {
#include "Hardware/VideoCodec/Intel/IntelAv1CdfTables.inc"
}  // namespace

bool build_default_cdf_context(uint32_t index, ByteView out) {
  if (index >= kDefaultContexts || out.size() != kCdfContextBytes) return false;
  out.fill(0);
  for (const CdfElement& e : kCdfElements) {
    // One line of the buffer holds 32 entries; the table's entries go `entries_per_line` to a line.
    if (e.total > e.available || e.entries_per_line == 0 || e.entries_per_line > 32) return false;
    const uint16_t* source = e.source[index];
    uint32_t left = e.total, line = e.start_line;
    while (left > 0) {
      const uint32_t n = left < e.entries_per_line ? left : e.entries_per_line;
      if (uint64_t{line} * 64 + uint64_t{n} * 2 > out.size()) return false;
      for (uint32_t i = 0; i < n; ++i) {
        const uint16_t v = source[i];
        out[uint64_t{line} * 64 + uint64_t{i} * 2] = static_cast<uint8_t>(v);
        out[uint64_t{line} * 64 + uint64_t{i} * 2 + 1] = static_cast<uint8_t>(v >> 8);
      }
      source += n;
      left -= n;
      ++line;
    }
  }
  return true;
}

}  // namespace intel_av1
}  // namespace hardware
