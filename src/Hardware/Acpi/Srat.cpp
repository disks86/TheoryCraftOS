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

#include "Hardware/Acpi/Srat.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {

constexpr uint64_t kHeaderBytes = 36;
constexpr uint64_t kSratEntries = 48;   // the header, then 4 bytes "table revision" (1) and 8 reserved
constexpr uint8_t kTypeLocalApic = 0, kTypeMemory = 1, kTypeX2Apic = 2, kTypeGicc = 3;
constexpr uint32_t kFlagEnabled = 1u << 0, kFlagHotPluggable = 1u << 1, kFlagNonVolatile = 1u << 2;

bool has_signature(ConstByteView table, char a, char b, char c, char d) {
  return table.size() >= kHeaderBytes && table[0] == static_cast<uint8_t>(a) && table[1] == static_cast<uint8_t>(b) &&
         table[2] == static_cast<uint8_t>(c) && table[3] == static_cast<uint8_t>(d);
}

// The table as far as its own length says, or empty when that length is shorter than the header or longer than the view.
ConstByteView body(ConstByteView table) {
  ByteReader r(table.subview(4, 4));
  const uint32_t length = r.le32();
  return length >= kHeaderBytes && length <= table.size() ? table.first(length) : ConstByteView();
}

// Calls `visit(type, entry)` for every whole entry, `entry` being all of it (type and length bytes included).
template <typename Visit>
void for_each_entry(ConstByteView srat, Visit&& visit) {
  if (!has_signature(srat, 'S', 'R', 'A', 'T')) return;
  const ConstByteView table = body(srat);
  for (uint64_t at = kSratEntries; at + 2 <= table.size();) {
    const uint8_t type = table[at], length = table[at + 1];
    if (length < 2 || length > table.size() - at) break;   // (an entry that does not fit ends the list)
    visit(type, table.subview(at, length));
    at += length;
  }
}

}  // namespace

uint32_t srat_cpus(ConstByteView srat, ArrayView<SratCpu> out) {
  uint32_t count = 0;
  for_each_entry(srat, [&](uint8_t type, ConstByteView entry) {
    SratCpu cpu;
    uint32_t flags = 0;
    ByteReader r(entry.subview(2));
    if (type == kTypeLocalApic && entry.size() >= 16) {
      const uint8_t node_low = r.u8();
      cpu.id = r.u8();
      flags = r.le32();
      r.skip(1);   // local SAPIC EID
      const uint32_t node_high = r.u8() | (uint32_t{r.u8()} << 8) | (uint32_t{r.u8()} << 16);
      cpu.node = node_low | (node_high << 8);
      cpu.kind = SratCpuId::LocalApic;
    } else if (type == kTypeX2Apic && entry.size() >= 24) {
      r.skip(2);
      cpu.node = r.le32();
      cpu.id = r.le32();
      flags = r.le32();
      cpu.kind = SratCpuId::X2Apic;
    } else if (type == kTypeGicc && entry.size() >= 18) {
      cpu.node = r.le32();
      cpu.id = r.le32();
      flags = r.le32();
      cpu.kind = SratCpuId::Gicc;
    } else {
      return;
    }
    if (!r.ok() || !(flags & kFlagEnabled)) return;
    if (count < out.size()) out[count] = cpu;
    ++count;
  });
  return count;
}

uint32_t srat_memory(ConstByteView srat, ArrayView<SratMemory> out) {
  uint32_t count = 0;
  for_each_entry(srat, [&](uint8_t type, ConstByteView entry) {
    if (type != kTypeMemory || entry.size() < 40) return;
    ByteReader r(entry.subview(2));
    SratMemory memory;
    memory.node = r.le32();
    r.skip(2);
    memory.base = r.le64();
    memory.length = r.le64();
    r.skip(4);
    const uint32_t flags = r.le32();
    if (!r.ok() || !(flags & kFlagEnabled) || memory.length == 0) return;
    memory.hot_pluggable = (flags & kFlagHotPluggable) != 0;
    memory.non_volatile = (flags & kFlagNonVolatile) != 0;
    if (count < out.size()) out[count] = memory;
    ++count;
  });
  return count;
}

uint8_t slit_distance(ConstByteView slit, uint32_t from, uint32_t to) {
  if (!has_signature(slit, 'S', 'L', 'I', 'T')) return 0;
  const ConstByteView table = body(slit);
  if (table.size() < kHeaderBytes + 8) return 0;
  ByteReader r(table.subview(kHeaderBytes));
  const uint64_t localities = r.le64();
  if (!r.ok() || from >= localities || to >= localities) return 0;
  // (both below `localities`, so the product only overflows for a count no table of this size can hold; checked against the size first)
  const uint64_t matrix = table.size() - kHeaderBytes - 8;
  if (localities > matrix || localities * localities > matrix) return 0;
  return table[kHeaderBytes + 8 + uint64_t{from} * localities + to];
}

}  // namespace hardware
