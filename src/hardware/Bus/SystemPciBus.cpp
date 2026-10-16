#include "hardware/Bus/SystemPciBus.hpp"
#include <new>

namespace hardware {

SystemPciBus::SystemPciBus(efi::SystemTable* st) : acpi_(st) {
  if (acpi_.initialize()) {
    auto* ecam = new (storage_) EcamPcieBus(acpi_);
    if (ecam->initialize()) {
      active_ = ecam;
      pcie_ = ecam;
      return;
    }
  }
  auto* legacy = new (storage_) LegacyPciBus();
  legacy->initialize();
  active_ = legacy;
}

}  // namespace hardware
