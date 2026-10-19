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

#pragma once
#include <stdint.h>
#include "Hardware/IHardware.hpp"
#include "Utility/ArrayView.hpp"
#include "uefi.hpp"

namespace hardware {

struct AcpiTableHeader {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed));

// Read-only access to the firmware's ACPI tables, found through the UEFI
// configuration table (RSDP -> XSDT, or RSDT on ACPI 1.0 firmware).
class AcpiTables final : public IHardware {
 public:
  explicit constexpr AcpiTables(efi::SystemTable* st) : st_(st) {}

  const char* name() const override { return "ACPI tables"; }
  bool initialize() override;
  void shutdown() override { root_ = ConstByteView(); }

  // The first table with this 4-character signature (e.g. "MCFG"), as exactly
  // its bytes (header included, as long as the header says), or an empty view.
  // The length comes from firmware memory, so it is sanity-checked.
  ConstByteView find(const char (&signature)[5]) const;
  // The n-th table (counting from 0) with this signature: a machine has one DSDT but many SSDTs.
  ConstByteView find_nth(const char (&signature)[5], uint32_t n) const;
  // The DSDT, which no root table lists: the FADT (signature "FACP") points to it. Empty if there is none.
  ConstByteView dsdt() const;

 private:
  efi::SystemTable* st_;
  ConstByteView root_;  // the XSDT or RSDT, header included
  bool wide_entries_ = false;              // XSDT: 8-byte pointers
};

}  // namespace hardware
