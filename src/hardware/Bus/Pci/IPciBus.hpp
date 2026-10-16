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

#pragma once
#include <stdint.h>
#include "hardware/Bus/IBus.hpp"

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
constexpr uint8_t kPciClassSerialBus = 0x0C;
constexpr uint8_t kPciSubclassUsb = 0x03;

// A PCI bus hierarchy (conventional PCI or PCI Express). Devices are numbered
// 0..device_count()-1 in discovery order. Offsets are config-space byte
// offsets; conventional PCI only reaches the first 256 bytes.
class IPciBus : public IBus {
 public:
  virtual bool pci_device(uint32_t index, PciDeviceInfo* out) const = 0;

  virtual uint32_t read32(PciAddress address, uint16_t offset) const = 0;
  virtual void write32(PciAddress address, uint16_t offset, uint32_t value) const = 0;

  // Base address of memory BAR `index` (0-5); 0 for I/O or unimplemented BARs.
  virtual uint64_t memory_bar(PciAddress address, uint8_t index) const = 0;
  // Offset of the capability with this ID in the device's capability list, or 0.
  virtual uint16_t find_capability(PciAddress address, uint8_t id) const = 0;

 protected:
  constexpr IPciBus() = default;
  ~IPciBus() = default;
};

}  // namespace hardware
