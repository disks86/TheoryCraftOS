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

// A PCI bus with a few display controllers on it, for the multi-GPU host tests: each device has a command register and (optionally) a power
// management capability whose control register keeps what is written to it.
#pragma once
#include "Hardware/Bus/Pci/IPciBus.hpp"

namespace fakegpu {
using namespace hardware;

constexpr uint16_t kPmCap = 0x50;

struct FakeDevice {
  PciDeviceInfo info;
  uint32_t command;
  bool has_pm;
  uint32_t pmcsr;
  bool stuck;  // ignores writes to PMCSR (a device that will not wake)
};

class FakePci : public IPciBus {
 public:
  mutable FakeDevice devices[4] = {};
  uint32_t count = 0;
  void add(uint8_t bus, uint16_t vendor, uint16_t device, uint8_t cls, uint32_t command, bool pm) {
    devices[count++] = {{{bus, 0, 0}, vendor, device, cls, 0, 0}, command, pm, 0, false};
  }
  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return count; }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override { if (i >= count) return false; *out = devices[i].info; return true; }
  FakeDevice* find(PciAddress a) const {
    for (uint32_t i = 0; i < count; ++i) if (devices[i].info.address.bus == a.bus) return &devices[i];
    return nullptr;
  }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    FakeDevice* d = find(a);
    if (!d) return 0;
    if (offset == 0x04) return d->command;
    if (offset == kPmCap + 4) return d->pmcsr;
    return 0;
  }
  void write32(PciAddress a, uint16_t offset, uint32_t value) const override {
    FakeDevice* d = find(a);
    if (d && offset == kPmCap + 4 && !d->stuck) d->pmcsr = value;
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress a, uint8_t id) const override {
    FakeDevice* d = find(a);
    return d && d->has_pm && id == 0x01 ? kPmCap : 0;
  }
};

constexpr uint16_t kIntel = 0x8086, kNvidia = 0x10DE, kAmd = 0x1002;
constexpr uint16_t kIntelTigerLake = 0x9A49;  // Xe-LP integrated
constexpr uint16_t kRtx4060Laptop = 0x28E0;   // AD107M
}  // namespace fakegpu
