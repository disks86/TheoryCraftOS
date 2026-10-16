#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"
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
  void shutdown() override { root_ = nullptr; }

  // The first table with this 4-character signature (e.g. "MCFG"), or nullptr.
  const AcpiTableHeader* find(const char (&signature)[5]) const;

 private:
  efi::SystemTable* st_;
  const AcpiTableHeader* root_ = nullptr;  // XSDT or RSDT
  bool wide_entries_ = false;              // XSDT: 8-byte pointers
};

}  // namespace hardware
