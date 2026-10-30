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

// The way to the Thunderbolt controller's own registers that does not go through the NHI: the vendor specific extended capability of the controller's PCIe upstream port (the
// "PCIe to CIO" mailbox of icm.c). A command word names a configuration space (switch or port), a port and a register index; the controller reads or writes that register of the
// root router and the result is in the data registers. It works when the firmware is not running, which is how the firmware is restarted and how the links of the ports are bounced.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltControl.hpp"

namespace hardware {

// A PCI function's configuration space (the capability's registers are in it).
class ITbConfigSpace {
 public:
  virtual uint32_t read32(uint16_t offset) = 0;
  virtual void write32(uint16_t offset, uint32_t value) = 0;

 protected:
  constexpr ITbConfigSpace() = default;
  ~ITbConfigSpace() = default;
};

class PciFunctionConfig final : public ITbConfigSpace {
 public:
  PciFunctionConfig(IPciBus& bus, PciAddress address) : bus_(bus), address_(address) {}
  uint32_t read32(uint16_t offset) override { return bus_.read32(address_, offset); }
  void write32(uint16_t offset, uint32_t value) override { bus_.write32(address_, offset, value); }

 private:
  IPciBus& bus_;
  PciAddress address_;
};

namespace tb_cio {
// the registers inside the capability
constexpr uint16_t kCmd = 0x30, kWriteData = 0x34, kReadData = 0x38;
constexpr uint32_t kCmdTimeout = 1u << 31, kCmdStart = 1u << 30, kCmdWrite = 1u << 21;
constexpr uint32_t kCmdSpaceShift = 19, kCmdSpaceMask = 3u << 19, kCmdPortShift = 13, kCmdPortMask = 0x3Fu << 13, kCmdIndexMask = 0x1FFF;
constexpr uint16_t kVendorExtendedCapability = 0x000B;
// the registers it is used for
constexpr uint32_t kSwitchResetIndex = 0x50, kSwitchResetBit = 1u << 9;        // the root router's reset (switch space, port 0)
constexpr uint32_t kTitanSwitchResetIndex = 0x777, kTitanSwitchResetBit = 1u << 1;   // (Titan Ridge keeps it elsewhere)
constexpr uint32_t kPhyPortCs1 = 0x37, kPhyLinkDisable = 1u << 14, kPhyLinkStateShift = 26, kPhyLinkStateMask = 0xFu << 26, kPortStateUp = 2;
}  // namespace tb_cio

class TbPcie2Cio {
 public:
  static constexpr uint32_t kTimeoutUs = 5000000;
  // `capability` is the offset of the vendor specific extended capability.
  TbPcie2Cio(ITbConfigSpace& config, uint16_t capability) : config_(config), capability_(capability) {}

  bool read(TbConfigSpace space, uint32_t port, uint32_t index, uint32_t* value, uint32_t timeout_us = kTimeoutUs);
  bool write(TbConfigSpace space, uint32_t port, uint32_t index, uint32_t value, uint32_t timeout_us = kTimeoutUs);

 private:
  bool wait(uint32_t timeout_us);
  ITbConfigSpace& config_;
  uint16_t capability_;
};

// The offset of the vendor specific extended capability of a PCIe function, or 0.
uint16_t tb_find_vendor_capability(ITbConfigSpace& config);
// The upstream port of the Thunderbolt switch above a host interface (the nearest PCIe upstream port of an Alpine Ridge bridge whose bus range holds the NHI), or false.
bool tb_find_upstream_port(const IPciBus& bus, const PciDeviceInfo& nhi, PciAddress* port);

}  // namespace hardware
