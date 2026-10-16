#include "hardware/Pci/PciBus.hpp"

namespace hardware {

namespace {
uint32_t config_address(PciAddress a, uint8_t offset) {
  return 0x80000000u | (uint32_t{a.bus} << 16) | (uint32_t{a.device} << 11) |
         (uint32_t{a.function} << 8) | (offset & 0xFC);
}
}  // namespace

uint32_t PciBus::read32(PciAddress address, uint8_t offset) const {
  address_port_.write32(config_address(address, offset));
  return data_port_.read32();
}

void PciBus::write32(PciAddress address, uint8_t offset, uint32_t value) const {
  address_port_.write32(config_address(address, offset));
  data_port_.write32(value);
}

void PciBus::enumerate(Visitor visit, void* context) const {
  for (uint32_t bus = 0; bus < 256; ++bus) {
    for (uint32_t device = 0; device < 32; ++device) {
      PciAddress first{static_cast<uint8_t>(bus), static_cast<uint8_t>(device), 0};
      if ((read32(first, 0) & 0xFFFF) == 0xFFFF) continue;
      const bool multifunction = (read32(first, 0x0C) >> 16) & 0x80;
      for (uint32_t function = 0; function < (multifunction ? 8u : 1u); ++function) {
        PciAddress a{static_cast<uint8_t>(bus), static_cast<uint8_t>(device),
                     static_cast<uint8_t>(function)};
        uint32_t id = read32(a, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint32_t cls = read32(a, 0x08);
        PciDeviceInfo info{a,
                           static_cast<uint16_t>(id & 0xFFFF),
                           static_cast<uint16_t>(id >> 16),
                           static_cast<uint8_t>(cls >> 24),
                           static_cast<uint8_t>(cls >> 16),
                           static_cast<uint8_t>(cls >> 8)};
        if (!visit(info, context)) return;
      }
    }
  }
}

uint64_t PciBus::memory_bar(PciAddress address, uint8_t index) const {
  if (index > 5) return 0;
  uint32_t low = read32(address, 0x10 + 4 * index);
  if (low & 1) return 0;  // I/O space
  uint64_t base = low & ~0xFull;
  if (((low >> 1) & 3) == 2 && index < 5)  // 64-bit BAR uses the next register too
    base |= uint64_t{read32(address, 0x10 + 4 * (index + 1))} << 32;
  return base;
}

}  // namespace hardware
