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

#include "Hardware/Bus/Pci/PciMsix.hpp"

namespace hardware {

namespace {
constexpr uint32_t kEnable = 1u << 15, kFunctionMask = 1u << 14, kSizeMask = 0x7FF;
constexpr uint32_t kEntryBytes = 16;
}  // namespace

uint32_t PciMsix::table_size(PciAddress function) {
  const uint16_t cap = bus_.find_capability(function, kCapabilityId);
  if (cap == 0) return 0;
  return ((bus_.read32(function, cap) >> 16) & kSizeMask) + 1;
}

bool PciMsix::enable(PciAddress function, const uint8_t* vectors, uint32_t count, uint32_t apic_id) {
  if (capability_ != 0 || count == 0 || count > kMaxEntries || apic_id > 0xFF) return false;
  const uint16_t cap = bus_.find_capability(function, kCapabilityId);
  if (cap == 0) return false;
  const uint32_t first = bus_.read32(function, cap);                   // id, next, message control in the high half
  const uint32_t size = ((first >> 16) & kSizeMask) + 1;
  if (count > size) return false;
  const uint32_t table = bus_.read32(function, cap + 4);               // BAR index in the low 3 bits, offset in the rest
  const uint64_t bar = bus_.memory_bar(function, static_cast<uint8_t>(table & 7));
  if (bar == 0) return false;
  MmioRegion region;
  if (!mapper_.map_registers(bar + (table & ~7u), static_cast<uint64_t>(size) * kEntryBytes, &region)) return false;
  // MSI-X on with the function masked while the table is written
  const uint32_t control = first >> 16;
  bus_.write32(function, cap, (first & 0xFFFF) | ((control | kEnable | kFunctionMask) << 16));
  for (uint32_t i = 0; i < size; ++i) region.write32(i * kEntryBytes + 12, 1);                  // every entry masked
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t at = static_cast<uint64_t>(i) * kEntryBytes;
    region.write32(at + 0, 0xFEE00000u | (apic_id << 12));            // fixed delivery to that core
    region.write32(at + 4, 0);
    region.write32(at + 8, vectors[i]);                                // edge triggered
    region.write32(at + 12, 0);
  }
  bus_.write32(function, cap, (first & 0xFFFF) | (((control | kEnable) & ~kFunctionMask) << 16));
  function_ = function;
  capability_ = cap;
  table_ = region;
  entries_ = size;
  return true;
}

void PciMsix::disable() {
  if (capability_ == 0) return;
  const uint32_t first = bus_.read32(function_, capability_);
  bus_.write32(function_, capability_, first | (kFunctionMask << 16));                        // nothing is delivered while the entries change
  for (uint32_t i = 0; i < entries_; ++i) table_.write32(i * kEntryBytes + 12, 1);
  bus_.write32(function_, capability_, first & ~((kEnable | kFunctionMask) << 16));          // MSI-X off, function unmasked
  capability_ = 0;
  entries_ = 0;
}

}  // namespace hardware
