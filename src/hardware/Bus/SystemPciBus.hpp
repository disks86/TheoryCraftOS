#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/Acpi/AcpiTables.hpp"
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/Bus/Pci/LegacyPciBus.hpp"
#include "hardware/Bus/Pcie/EcamPcieBus.hpp"

namespace hardware {

constexpr unsigned long BusMaxOf(unsigned long a, unsigned long b) { return a > b ? a : b; }

// Picks the most capable PCI access method the platform offers: PCI Express
// through ECAM when ACPI provides an MCFG table, else conventional PCI through
// I/O ports. The bus is initialized (scanned) on return.
class SystemPciBus {
 public:
  // The device list is allocated from `allocator`, which must outlive this object.
  SystemPciBus(efi::SystemTable* st, allocators::IAllocator& allocator);
  ~SystemPciBus();
  SystemPciBus(const SystemPciBus&) = delete;
  SystemPciBus& operator=(const SystemPciBus&) = delete;

  IPciBus& pci() { return *active_; }
  // Non-null only when the PCI Express (ECAM) bus is in use.
  IPcieBus* pcie() { return pcie_; }

 private:
  AcpiTables acpi_;
  alignas(16) unsigned char storage_[BusMaxOf(sizeof(LegacyPciBus), sizeof(EcamPcieBus))];
  IPciBus* active_ = nullptr;
  IPcieBus* pcie_ = nullptr;
};

}  // namespace hardware
