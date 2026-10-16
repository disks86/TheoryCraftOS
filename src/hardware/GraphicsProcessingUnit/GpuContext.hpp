#pragma once
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "uefi.hpp"

namespace hardware {

// What a GPU class gets at construction: firmware tables plus the PCI device
// it was matched to (zeroed when no PCI display device was found).
struct GpuContext {
  efi::SystemTable* st;
  const IPciBus* pci;
  PciDeviceInfo device;
};

}  // namespace hardware
