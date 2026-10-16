// Everything about PCI that does not depend on how config space is reached:
// scanning, BARs and capability lists. Concrete buses supply read32/write32.
#pragma once
#include "hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

// Interface is IPciBus or an interface derived from it (e.g. IPcieBus).
template <class Interface>
class PciBusBase : public Interface {
 public:
  static constexpr uint32_t kMaxDevices = 128;

  uint32_t device_count() const override { return count_; }

  bool pci_device(uint32_t index, PciDeviceInfo* out) const override {
    if (index >= count_ || !out) return false;
    *out = devices_[index];
    return true;
  }

  uint64_t memory_bar(PciAddress address, uint8_t index) const override {
    if (index > 5) return 0;
    uint32_t low = this->read32(address, 0x10 + 4 * index);
    if (low & 1) return 0;  // I/O space
    uint64_t base = low & ~0xFull;
    if (((low >> 1) & 3) == 2 && index < 5)  // 64-bit BAR spans two registers
      base |= uint64_t{this->read32(address, 0x10 + 4 * (index + 1))} << 32;
    return base;
  }

  uint16_t find_capability(PciAddress address, uint8_t id) const override {
    if (!(this->read32(address, 0x04) & (1u << 20))) return 0;  // status: no capability list
    uint16_t offset = this->read32(address, 0x34) & 0xFC;
    for (int guard = 0; offset && guard < 48; ++guard) {  // list is at most 48 entries
      uint32_t header = this->read32(address, offset);
      if ((header & 0xFF) == id) return offset;
      offset = (header >> 8) & 0xFC;
    }
    return 0;
  }

 protected:
  constexpr PciBusBase() = default;
  ~PciBusBase() = default;

  // Probes every slot on buses first..last and records the functions present.
  void scan(uint32_t first_bus, uint32_t last_bus) {
    count_ = 0;
    for (uint32_t bus = first_bus; bus <= last_bus; ++bus) {
      for (uint32_t device = 0; device < 32; ++device) {
        PciAddress first{static_cast<uint8_t>(bus), static_cast<uint8_t>(device), 0};
        if ((this->read32(first, 0) & 0xFFFF) == 0xFFFF) continue;
        const bool multifunction = (this->read32(first, 0x0C) >> 16) & 0x80;
        for (uint32_t function = 0; function < (multifunction ? 8u : 1u); ++function) {
          PciAddress a{static_cast<uint8_t>(bus), static_cast<uint8_t>(device),
                       static_cast<uint8_t>(function)};
          uint32_t id = this->read32(a, 0);
          if ((id & 0xFFFF) == 0xFFFF || count_ == kMaxDevices) continue;
          uint32_t cls = this->read32(a, 0x08);
          devices_[count_++] = {a,
                                static_cast<uint16_t>(id & 0xFFFF),
                                static_cast<uint16_t>(id >> 16),
                                static_cast<uint8_t>(cls >> 24),
                                static_cast<uint8_t>(cls >> 16),
                                static_cast<uint8_t>(cls >> 8)};
        }
      }
    }
  }

 private:
  PciDeviceInfo devices_[kMaxDevices] = {};
  uint32_t count_ = 0;
};

}  // namespace hardware
