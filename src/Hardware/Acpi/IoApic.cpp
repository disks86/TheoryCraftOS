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

#include "Hardware/Acpi/IoApic.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint32_t kMadtEntries = 44;          // the header (36 bytes), the local APIC address and the flags
constexpr uint8_t kTypeIoApic = 1, kTypeOverride = 2;
}  // namespace

uint32_t madt_ioapics(ConstByteView madt, ArrayView<IoApicInfo> out) {
  if (madt.size() < kMadtEntries || madt[0] != 'A' || madt[1] != 'P' || madt[2] != 'I' || madt[3] != 'C') return 0;
  uint32_t count = 0;
  for (uint64_t at = kMadtEntries; at + 2 <= madt.size();) {
    const uint8_t type = madt[at], length = madt[at + 1];
    if (length < 2 || length > madt.size() - at) break;               // (an entry that does not fit ends the list)
    if (type == kTypeIoApic && length >= 12) {
      ByteReader r(madt.subview(at + 2, length - 2));
      IoApicInfo info;
      info.id = r.u8();
      r.skip(1);
      info.address = r.le32();
      info.gsi_base = r.le32();
      if (r.ok()) {
        if (count < out.size()) out[count] = info;
        ++count;
      }
    }
    at += length;
  }
  return count;
}

SciRoute madt_sci_route(ConstByteView madt, uint32_t sci_irq) {
  SciRoute route;
  route.gsi = sci_irq;
  if (madt.size() < kMadtEntries || madt[0] != 'A' || madt[1] != 'P' || madt[2] != 'I' || madt[3] != 'C') return route;
  for (uint64_t at = kMadtEntries; at + 2 <= madt.size();) {
    const uint8_t type = madt[at], length = madt[at + 1];
    if (length < 2 || length > madt.size() - at) break;
    if (type == kTypeOverride && length >= 10) {
      ByteReader r(madt.subview(at + 2, length - 2));
      const uint8_t bus = r.u8(), source = r.u8();
      const uint32_t gsi = r.le32();
      const uint16_t flags = r.le16();
      if (r.ok() && bus == 0 && source == sci_irq) {
        route.gsi = gsi;
        // polarity (bits 1:0) and trigger (bits 3:2): 0 conforms to the bus (ISA: active high, edge), 1 high or edge, 3 low or level
        const uint32_t polarity = flags & 3, trigger = (flags >> 2) & 3;
        route.active_low = polarity == 3;
        route.level = trigger == 3;
        return route;
      }
    }
    at += length;
  }
  return route;
}

uint32_t IoApic::inputs() { return ((window_.read(kVersion) >> 16) & 0xFF) + 1; }

bool IoApic::route(uint32_t gsi, uint8_t vector, uint32_t apic_id, bool level, bool active_low, bool masked_now) {
  if (!contains(gsi) || apic_id > 0xFF) return false;
  const uint8_t entry = static_cast<uint8_t>(kRedirectionBase + (gsi - gsi_base_) * 2);
  window_.write(entry, kMasked | vector);                                     // masked first: nothing can arrive half programmed
  window_.write(static_cast<uint8_t>(entry + 1), apic_id << 24);
  window_.write(entry, vector | (level ? kLevel : 0) | (active_low ? kActiveLow : 0) | (masked_now ? kMasked : 0));
  return true;
}

bool IoApic::mask(uint32_t gsi, bool masked_now) {
  if (!contains(gsi)) return false;
  const uint8_t entry = static_cast<uint8_t>(kRedirectionBase + (gsi - gsi_base_) * 2);
  const uint32_t value = window_.read(entry);
  window_.write(entry, masked_now ? (value | kMasked) : (value & ~kMasked));
  return true;
}

bool IoApic::masked(uint32_t gsi) {
  if (!contains(gsi)) return true;
  return (window_.read(static_cast<uint8_t>(kRedirectionBase + (gsi - gsi_base_) * 2)) & kMasked) != 0;
}

}  // namespace hardware
