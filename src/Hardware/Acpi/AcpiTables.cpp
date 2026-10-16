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

#include "Hardware/Acpi/AcpiTables.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
bool same_guid(const efi::Guid& a, const efi::Guid& b) { return __builtin_memcmp(&a, &b, sizeof(a)) == 0; }

// The table at a physical address as a view of exactly the length its header
// claims. Firmware tables are trusted for their contents but not for a wild
// length: a length below the header or above 16 MiB means the pointer is bad.
ConstByteView table_at(uint64_t address) {
  if (!address) return ConstByteView();
  const auto* header = reinterpret_cast<const AcpiTableHeader*>(address);
  if (header->length < sizeof(AcpiTableHeader) || header->length > (16u << 20)) return ConstByteView();
  return ConstByteView(reinterpret_cast<const uint8_t*>(address), header->length);
}

struct Rsdp {
  char signature[8];
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
} __attribute__((packed));
}  // namespace

bool AcpiTables::initialize() {
  if (!st_ || !st_->configuration_table) return false;

  // Prefer the ACPI 2.0 entry, which carries the XSDT.
  const Rsdp* rsdp = nullptr;
  for (uint64_t i = 0; i < st_->number_of_table_entries; ++i) {
    const efi::ConfigurationTable& t = st_->configuration_table[i];
    if (same_guid(t.vendor_guid, efi::kAcpi20TableGuid)) {
      rsdp = static_cast<const Rsdp*>(t.vendor_table);
      break;
    }
    if (!rsdp && same_guid(t.vendor_guid, efi::kAcpi10TableGuid))
      rsdp = static_cast<const Rsdp*>(t.vendor_table);
  }
  if (!rsdp || __builtin_memcmp(rsdp->signature, "RSD PTR ", 8) != 0) return false;

  wide_entries_ = rsdp->revision >= 2 && rsdp->xsdt_address;
  const uint64_t address = wide_entries_ ? rsdp->xsdt_address : uint64_t{rsdp->rsdt_address};
  root_ = table_at(address);
  return !root_.empty();
}

ConstByteView AcpiTables::find(const char (&signature)[5]) const {
  if (root_.size() < sizeof(AcpiTableHeader)) return ConstByteView();
  const uint32_t entry_size = wide_entries_ ? 8 : 4;
  ByteReader entries(root_.dropped(sizeof(AcpiTableHeader)));  // the root table's list of table addresses
  while (entries.remaining() >= entry_size) {
    const uint64_t address = entry_size == 8 ? entries.le64() : entries.le32();
    ConstByteView table = table_at(address);
    if (table.size() >= 4 && __builtin_memcmp(table.data(), signature, 4) == 0) return table;
  }
  return ConstByteView();
}

}  // namespace hardware
