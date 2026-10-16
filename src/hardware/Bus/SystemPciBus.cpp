#include "hardware/Bus/SystemPciBus.hpp"
#include <new>

namespace hardware {

SystemPciBus::SystemPciBus(efi::SystemTable* st, allocators::IAllocator& allocator) : acpi_(st) {
  if (acpi_.initialize()) {
    auto* ecam = new (storage_) EcamPcieBus(acpi_, allocator);
    if (ecam->initialize()) {
      active_ = ecam;
      pcie_ = ecam;
      return;
    }
    ecam->~EcamPcieBus();  // no MCFG table: fall back below
  }
  auto* legacy = new (storage_) LegacyPciBus(allocator);
  legacy->initialize();
  active_ = legacy;
}

SystemPciBus::~SystemPciBus() {
  if (pcie_) static_cast<EcamPcieBus*>(pcie_)->~EcamPcieBus();
  else static_cast<LegacyPciBus*>(active_)->~LegacyPciBus();
}

}  // namespace hardware
