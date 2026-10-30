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

// The PCI power state of the host interface (the power management capability, id 0x01): D3hot for sleep and runtime suspend, with PME enabled when the function should wake the machine
// (a device plugged in or a host answering on the cable), and back to D0. The controller's own side of sleep is ThunderboltIcm::suspend / resume.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

class TbPciPower {
 public:
  static constexpr uint8_t kCapabilityId = 0x01;
  // PMC (capabilities, bits 15:11 say from which states a PME can be raised) and PMCSR (state bits 1:0, PME enable bit 8, PME status bit 15, write one to clear)
  static constexpr uint32_t kPmeFromD3hot = 1u << 14, kStateMask = 3, kStateD0 = 0, kStateD3hot = 3, kPmeEnable = 1u << 8, kPmeStatus = 1u << 15;
  static constexpr uint32_t kD3hotRecoveryUs = 10000;          // the specification's time before a function answers after D3hot to D0

  TbPciPower(IPciBus& bus, PciAddress address) : bus_(bus), address_(address) {}

  // The function has the capability.
  bool available();
  // A PME can be raised from D3hot.
  bool can_wake();
  // 0 for D0, 3 for D3hot, or 0xFF if there is no capability.
  uint8_t state();
  // Puts the function in D3hot, with the PME enabled if `wake`. False if there is no capability, or `wake` is asked for and the function cannot raise a PME from D3hot (then nothing changes).
  bool enter_d3hot(bool wake);
  // Back to D0: the PME disabled and its status cleared; waits for the function to recover.
  bool enter_d0();
  // A wake event is pending (PME status).
  bool wake_pending();

 private:
  uint16_t capability() { return bus_.find_capability(address_, kCapabilityId); }
  IPciBus& bus_;
  PciAddress address_;
};

}  // namespace hardware
