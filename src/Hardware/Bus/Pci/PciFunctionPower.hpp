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

// Runtime power of one PCI function, between the driver that owns the function and whoever owns the power of the device the function is
// part of (the GPU manager, for the HDMI / DisplayPort audio function of a discrete GPU: the GPU can only go to D3cold while none of its
// functions is in use). And the PCI power management (PCI PM 1.2) steps a driver needs to put its own function in D3hot and back.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

// Told by a driver when its function stops and starts needing the hardware. function_idle() comes after the driver has stopped touching the
// function (its registers are not read or written again until function_busy() has returned); function_busy() comes before the driver touches
// it again and may take a while (waking a GPU from D3cold is ~100 ms): the driver calls both from thread context, never under a spin lock,
// and calls are always alternating (idle, busy, idle, ...). The function starts out busy.
class IPciFunctionPowerObserver {
 public:
  virtual void function_idle(PciAddress function) = 0;
  virtual void function_busy(PciAddress function) = 0;

 protected:
  IPciFunctionPowerObserver() = default;
  ~IPciFunctionPowerObserver() = default;
};

// --- PCI power management of one function (the PM capability, ID 01h: PMCSR at +4, bits 1:0 the power state).

constexpr uint8_t kPciCapabilityPowerManagement = 0x01;
constexpr uint16_t kPciPmcsrOffset = 4;
constexpr uint32_t kPciPmcsrStateMask = 0x3;
constexpr uint32_t kPciD3hotToD0Us = 10000;   // the recovery time after D3hot -> D0 (PCI PM 1.2, 5.6.1)

enum class PciFunctionPowerState : uint8_t { D0 = 0, D3Hot = 3 };

// Sets the function's power state; false if it has no PM capability (nothing was written). The upper half of the dword (PMCSR_BSE, Data)
// is written back as read; the PME status bit (15, write 1 to clear) is not set by this write.
[[nodiscard]] inline bool pci_set_function_power(const IPciBus& pci, PciAddress function, PciFunctionPowerState state) {
  const uint16_t cap = pci.find_capability(function, kPciCapabilityPowerManagement);
  if (!cap) return false;
  const uint32_t value = pci.read32(function, static_cast<uint16_t>(cap + kPciPmcsrOffset));
  pci.write32(function, static_cast<uint16_t>(cap + kPciPmcsrOffset), (value & ~(kPciPmcsrStateMask | (1u << 15))) | static_cast<uint32_t>(state));
  return true;
}

// The type 0 configuration header (the first 16 dwords), which a function loses on its way through D3hot -> D0 (unless it reports
// No_Soft_Reset) and always through D3cold.
struct PciSavedHeader {
  uint32_t dwords[16] = {};
  bool valid = false;
};

[[nodiscard]] inline PciSavedHeader pci_save_header(const IPciBus& pci, PciAddress function) {
  PciSavedHeader saved;
  for (uint16_t i = 0; i < 16; ++i) saved.dwords[i] = pci.read32(function, static_cast<uint16_t>(i * 4));
  saved.valid = true;
  return saved;
}

// Writes the header back: the BARs, cache line / latency, and the interrupt line first, the command register (memory space, bus master) last,
// so the function decodes its new BARs only once they are all there. The ID, status and read-only dwords are not written.
inline void pci_restore_header(const IPciBus& pci, PciAddress function, const PciSavedHeader& saved) {
  if (!saved.valid) return;
  static constexpr uint16_t kRestored[] = {0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x30, 0x3C};
  for (uint16_t offset : kRestored) pci.write32(function, offset, saved.dwords[offset / 4]);
  pci.write32(function, 0x04, saved.dwords[1] & 0xFFFF);   // (the status half: write-1-to-clear bits, left alone)
}

}  // namespace hardware
