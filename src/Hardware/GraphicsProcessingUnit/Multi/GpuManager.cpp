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

#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"

namespace hardware {

namespace {
constexpr uint8_t kPciCapabilityPowerManagement = 0x01;
constexpr uint16_t kPmcsrOffset = 4;  // from the capability: power management control/status
constexpr uint32_t kPmcsrStateMask = 0x3;
constexpr uint32_t kSettleMilliseconds = 10;  // D3hot -> D0 recovery time in the PCI PM spec
// The register is the low half of the dword at cap+4; the high half (PMCSR_BSE, data) is written back as read.
}  // namespace

bool pci_get_power_state(const IPciBus& pci, PciAddress address, PciPowerState* out) {
  const uint16_t cap = pci.find_capability(address, kPciCapabilityPowerManagement);
  if (cap == 0) return false;
  *out = static_cast<PciPowerState>(pci.read32(address, cap + kPmcsrOffset) & kPmcsrStateMask);
  return true;
}

bool pci_set_power_state(const IPciBus& pci, PciAddress address, PciPowerState state) {
  const uint16_t cap = pci.find_capability(address, kPciCapabilityPowerManagement);
  if (cap == 0) return false;
  const uint32_t value = pci.read32(address, cap + kPmcsrOffset);
  pci.write32(address, cap + kPmcsrOffset, (value & ~kPmcsrStateMask) | static_cast<uint32_t>(state));
  return true;
}

void GpuManager::initialize() {
  topology_.enumerate(pci_);
  for (uint32_t i = 0; i < GpuTopology::kMaxAdapters; ++i) {
    leases_[i] = 0;
    asleep_[i] = false;
  }
  // Only a machine with a choice has anything to save; a lone adapter is the display.
  if (!topology_.is_hybrid()) return;
  for (uint32_t i = 0; i < topology_.count(); ++i)
    if (static_cast<int32_t>(i) != topology_.boot_index() && topology_.adapter(i).kind == GpuKind::Discrete) sleep(i);
}

bool GpuManager::acquire(uint32_t index) {
  if (index >= topology_.count()) return false;
  if (leases_[index] == UINT32_MAX) return false;
  if (asleep_[index] && !wake(index)) return false;
  ++leases_[index];
  return true;
}

void GpuManager::release(uint32_t index) {
  if (index >= topology_.count() || leases_[index] == 0) return;
  if (--leases_[index] == 0 && static_cast<int32_t>(index) != topology_.boot_index() && topology_.adapter(index).kind == GpuKind::Discrete) sleep(index);
}

bool GpuManager::wake(uint32_t index) {
  if (!pci_set_power_state(pci_, topology_.adapter(index).device.address, PciPowerState::D0)) return false;
  if (delay_) delay_(kSettleMilliseconds);
  PciPowerState now = PciPowerState::D3Hot;
  if (!pci_get_power_state(pci_, topology_.adapter(index).device.address, &now) || now != PciPowerState::D0) return false;
  asleep_[index] = false;
  return true;
}

void GpuManager::sleep(uint32_t index) {
  if (pci_set_power_state(pci_, topology_.adapter(index).device.address, PciPowerState::D3Hot)) asleep_[index] = true;
}

}  // namespace hardware
