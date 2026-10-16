/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Everything about PCI that does not depend on how config space is reached:
// scanning, BARs and capability lists. Concrete buses supply read32/write32.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

// Interface is IPciBus or an interface derived from it (e.g. IPcieBus).
template <class Interface>
class PciBusBase : public Interface {
 public:
  uint32_t device_count() const override { return static_cast<uint32_t>(devices_.count()); }

  bool pci_device(uint32_t index, PciDeviceInfo* out) const override {
    const PciDeviceInfo* d = devices_.get(index);
    if (!d || !out) return false;
    *out = *d;
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
  // The device list grows with the machine: servers can have many hundreds of
  // functions. (If memory runs out mid-scan the list simply stops there.)
  explicit PciBusBase(allocators::IAllocator& allocator) : devices_(allocator) {}
  ~PciBusBase() = default;

  // Probes every slot on buses first..last and records the functions present.
  void scan(uint32_t first_bus, uint32_t last_bus) {
    devices_.clear();
    for (uint32_t bus = first_bus; bus <= last_bus; ++bus) {
      for (uint32_t device = 0; device < 32; ++device) {
        PciAddress first{static_cast<uint8_t>(bus), static_cast<uint8_t>(device), 0};
        if ((this->read32(first, 0) & 0xFFFF) == 0xFFFF) continue;
        const bool multifunction = (this->read32(first, 0x0C) >> 16) & 0x80;
        for (uint32_t function = 0; function < (multifunction ? 8u : 1u); ++function) {
          PciAddress a{static_cast<uint8_t>(bus), static_cast<uint8_t>(device),
                       static_cast<uint8_t>(function)};
          uint32_t id = this->read32(a, 0);
          if ((id & 0xFFFF) == 0xFFFF) continue;
          uint32_t cls = this->read32(a, 0x08);
          if (!devices_.push_back(PciDeviceInfo{a,
                                                static_cast<uint16_t>(id & 0xFFFF),
                                                static_cast<uint16_t>(id >> 16),
                                                static_cast<uint8_t>(cls >> 24),
                                                static_cast<uint8_t>(cls >> 16),
                                                static_cast<uint8_t>(cls >> 8)}))
            return;  // out of memory
        }
      }
    }
  }

 private:
  List<PciDeviceInfo> devices_;
};

}  // namespace hardware
