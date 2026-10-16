#include "hardware/Bus/Pci/LegacyPciBus.hpp"

namespace hardware {

namespace {
uint32_t config_address(PciAddress a, uint16_t offset) {
  return 0x80000000u | (uint32_t{a.bus} << 16) | (uint32_t{a.device} << 11) |
         (uint32_t{a.function} << 8) | (offset & 0xFC);
}
}  // namespace

bool LegacyPciBus::initialize() {
  scan(0, 255);
  return true;
}

uint32_t LegacyPciBus::read32(PciAddress address, uint16_t offset) const {
  if (offset >= 256) return 0xFFFFFFFF;  // extended config space needs PCIe
  address_port_.write32(config_address(address, offset));
  return data_port_.read32();
}

void LegacyPciBus::write32(PciAddress address, uint16_t offset, uint32_t value) const {
  if (offset >= 256) return;
  address_port_.write32(config_address(address, offset));
  data_port_.write32(value);
}

}  // namespace hardware
