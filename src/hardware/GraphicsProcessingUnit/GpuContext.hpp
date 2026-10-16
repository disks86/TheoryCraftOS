#pragma once
#include "hardware/Pci/PciBus.hpp"
#include "uefi.hpp"

namespace hardware {

// What a GPU class gets at construction: firmware tables plus the PCI device
// it was matched to (zeroed when no PCI display device was found).
struct GpuContext {
  efi::SystemTable* st;
  const PciBus* pci;
  PciDeviceInfo device;
};

}  // namespace hardware
