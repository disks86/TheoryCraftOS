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


// An Intel low power subsystem I2C controller: a PCI function (Sunrise Point-LP: 8086:9D60 to 9D65, the I2C0 to I2C5 at 00:15.x and 00:19.x of the Dell XPS 15 9575) whose first BAR
// holds a DesignWare I2C master and the LPSS private registers (intel-lpss.c). Bringing it up: the function to D0 and memory decoding on, the BAR mapped, the function taken out of
// reset, the master set up for the speed and the counts the firmware gave.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/I2c/DesignWareI2c.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/RegisterMapper.hpp"

namespace hardware {

// Where the registers of a controller come from: the MMIO window of its BAR on the machine, a model in a test.
class II2cRegisterSource {
 public:
  virtual II2cRegisters* open(uint64_t bar) = 0;

 protected:
  constexpr II2cRegisterSource() = default;
  ~II2cRegisterSource() = default;
};

// The MMIO windows of the BARs (the first 4 KiB of each; the register objects live as long as the source).
class MmioI2cRegisterSource final : public II2cRegisterSource {
 public:
  static constexpr uint32_t kMax = 8;
  explicit MmioI2cRegisterSource(IRegisterMapper& mapper) : mapper_(mapper) {}
  II2cRegisters* open(uint64_t bar) override {
    if (count_ >= kMax) return nullptr;
    MmioRegion region;
    if (bar == 0 || !mapper_.map_registers(bar, 0x1000, &region)) return nullptr;
    windows_[count_] = MmioI2cRegisters(region);
    return &windows_[count_++];
  }

 private:
  IRegisterMapper& mapper_;
  MmioI2cRegisters windows_[kMax] = {MmioI2cRegisters(MmioRegion()), MmioI2cRegisters(MmioRegion()), MmioI2cRegisters(MmioRegion()), MmioI2cRegisters(MmioRegion()),
                                     MmioI2cRegisters(MmioRegion()), MmioI2cRegisters(MmioRegion()), MmioI2cRegisters(MmioRegion()), MmioI2cRegisters(MmioRegion())};
  uint32_t count_ = 0;
};

class LpssI2cController {
 public:
  LpssI2cController() : forward_(), master_(forward_) {}
  LpssI2cController(const LpssI2cController&) = delete;
  LpssI2cController& operator=(const LpssI2cController&) = delete;

  // True for the PCI IDs of the Sunrise Point-LP I2C controllers.
  static bool is_lpss_i2c(uint16_t vendor, uint16_t device) { return vendor == 0x8086 && device >= 0x9D60 && device <= 0x9D65; }

  // Checks the function, brings it to D0 with memory decoding on, and gives its first BAR's address (the registers are then opened from it).
  static bool prepare(IPciBus& pci, PciAddress address, uint64_t* bar);
  // Takes the function out of reset and sets the master up on the registers.
  bool initialize(II2cRegisters& registers, PciAddress address, const DesignWareI2c::Config& config);
  II2cBus& bus() { return master_; }
  PciAddress address() const { return address_; }

 private:
  class Forward final : public II2cRegisters {
   public:
    uint32_t read32(uint32_t offset) override { return target ? target->read32(offset) : 0xFFFFFFFFu; }
    void write32(uint32_t offset, uint32_t value) override { if (target) target->write32(offset, value); }
    II2cRegisters* target = nullptr;
  };
  Forward forward_;
  DesignWareI2c master_;
  PciAddress address_ = {0, 0, 0};
};

}  // namespace hardware
