#pragma once
#include "hardware/Bus/Pci/PciBusBase.hpp"
#include "hardware/IoPort.hpp"

namespace hardware {

// Conventional PCI through the legacy 0xCF8/0xCFC I/O ports. Works everywhere
// but only sees the first 256 bytes of each device's config space.
class LegacyPciBus final : public PciBusBase<IPciBus> {
 public:
  explicit LegacyPciBus(allocators::IAllocator& allocator) : PciBusBase(allocator) {}

  const char* name() const override { return "PCI bus (I/O ports)"; }
  bool initialize() override;
  void shutdown() override {}

  uint32_t read32(PciAddress address, uint16_t offset) const override;
  void write32(PciAddress address, uint16_t offset, uint32_t value) const override;

 private:
  static constexpr uint16_t kConfigAddress = 0xCF8;
  static constexpr uint16_t kConfigData = 0xCFC;

  IoPort address_port_{kConfigAddress};
  IoPort data_port_{kConfigData};
};

}  // namespace hardware
