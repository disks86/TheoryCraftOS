#pragma once
#include "hardware/Acpi/AcpiTables.hpp"
#include "hardware/Bus/Pci/PciBusBase.hpp"
#include "hardware/Bus/Pcie/IPcieBus.hpp"

namespace hardware {

// PCI Express through ECAM: config space is memory-mapped, at an address the
// ACPI MCFG table provides. Only PCI segment 0 is used.
class EcamPcieBus final : public PciBusBase<IPcieBus> {
 public:
  explicit constexpr EcamPcieBus(const AcpiTables& acpi) : acpi_(acpi) {}

  const char* name() const override { return "PCI Express bus (ECAM)"; }
  bool initialize() override;  // false if firmware has no MCFG table
  void shutdown() override { base_ = 0; }

  uint32_t read32(PciAddress address, uint16_t offset) const override;
  void write32(PciAddress address, uint16_t offset, uint32_t value) const override;
  uint16_t find_extended_capability(PciAddress address, uint16_t id) const override;

  uint64_t ecam_base() const { return base_; }

 private:
  volatile uint32_t* locate(PciAddress address, uint16_t offset) const;

  const AcpiTables& acpi_;
  uint64_t base_ = 0;  // ECAM address of bus first_bus_
  uint32_t first_bus_ = 0, last_bus_ = 0;
};

}  // namespace hardware
