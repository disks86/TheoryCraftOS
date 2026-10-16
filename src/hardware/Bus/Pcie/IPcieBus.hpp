#pragma once
#include "hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

// PCI Express: PCI plus 4 KiB of config space per function and the extended
// capability list that lives in the part above 256 bytes.
class IPcieBus : public IPciBus {
 public:
  // Offset (>= 0x100) of the extended capability with this ID, or 0.
  virtual uint16_t find_extended_capability(PciAddress address, uint16_t id) const = 0;

 protected:
  constexpr IPcieBus() = default;
  ~IPcieBus() = default;
};

}  // namespace hardware
