#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"
#include "hardware/IoPort.hpp"

namespace hardware {

struct PciAddress {
  uint8_t bus, device, function;
};

struct PciDeviceInfo {
  PciAddress address;
  uint16_t vendor_id, device_id;
  uint8_t class_code, subclass, prog_if;
};

constexpr uint8_t kPciClassDisplay = 0x03;

// PCI configuration space through the legacy 0xCF8/0xCFC ports.
class PciBus final : public IHardware {
 public:
  using Visitor = bool (*)(const PciDeviceInfo& device, void* context);  // false stops the scan

  constexpr PciBus() = default;

  const char* name() const override { return "PCI bus"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  uint32_t read32(PciAddress address, uint8_t offset) const;
  void write32(PciAddress address, uint8_t offset, uint32_t value) const;

  // Calls `visit` for every PCI function that is present.
  void enumerate(Visitor visit, void* context) const;

  // Base address of memory BAR `index` (0-5); 0 for I/O or unimplemented BARs.
  uint64_t memory_bar(PciAddress address, uint8_t index) const;

 private:
  static constexpr uint16_t kConfigAddress = 0xCF8;
  static constexpr uint16_t kConfigData = 0xCFC;

  IoPort address_port_{kConfigAddress};
  IoPort data_port_{kConfigData};
};

}  // namespace hardware
