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
#include "Hardware/GraphicsProcessingUnit/Multi/PcieLink.hpp"

namespace hardware {

namespace {
constexpr uint8_t kPciCapabilityPowerManagement = 0x01;
constexpr uint16_t kPmcsrOffset = 4;  // from the capability: power management control/status
constexpr uint32_t kPmcsrStateMask = 0x3;
constexpr uint32_t kSettleMilliseconds = 10;  // D3hot -> D0 recovery time in the PCI PM spec
constexpr uint32_t kMaxPowerOnDelayMs = 1000;  // a handler that asks for more is not believed
// The register is the low half of the dword at cap+4; the high half (PMCSR_BSE, data) is written back as read.

bool same_function(PciAddress a, PciAddress b) { return a.bus == b.bus && a.device == b.device && a.function == b.function; }
}  // namespace

const char* gpu_power_mechanism_name(GpuPowerMechanism mechanism) {
  switch (mechanism) {
    case GpuPowerMechanism::None: return "none";
    case GpuPowerMechanism::AcpiPowerResources: return "ACPI power resources";
    case GpuPowerMechanism::NvidiaOptimus: return "NVIDIA Optimus";
    case GpuPowerMechanism::NvidiaJt: return "NVIDIA JT";
    case GpuPowerMechanism::NvidiaHybridDsm: return "NVIDIA hybrid _DSM";
    case GpuPowerMechanism::AmdAtpx: return "AMD ATPX";
    case GpuPowerMechanism::AppleGmux: return "Apple gmux";
  }
  return "unknown";
}

const char* gpu_power_level_name(GpuPowerLevel level) {
  switch (level) {
    case GpuPowerLevel::On: return "on";
    case GpuPowerLevel::D3Hot: return "D3hot";
    case GpuPowerLevel::D3Cold: return "D3cold";
  }
  return "unknown";
}

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
  kernel::ScopedMutex g(lock_);
  topology_.enumerate(pci_);
  for (uint32_t i = 0; i < GpuTopology::kMaxAdapters; ++i) {
    leases_[i] = 0;
    level_[i] = GpuPowerLevel::On;
    wakes_[i] = 0;
    idle_since_[i] = 0;
    idle_pending_[i] = false;
    companions_[i] = Companions{};
    has_bridge_[i] = false;
    aspm_[i] = false;
    link_off_[i] = false;
    link_is_off_[i] = false;
  }
  for (uint32_t i = 0; i < topology_.count(); ++i) {
    find_companions(i);
    has_bridge_[i] = pci_find_upstream_bridge(pci_, topology_.adapter(i).device.address, &bridge_[i]);
  }
  // Only a machine with a choice has anything to save; a lone adapter is the display.
  if (!topology_.is_hybrid()) return;
  for (uint32_t i = 0; i < topology_.count(); ++i)
    if (sleeps_when_idle(i)) sleep(i);
}

void GpuManager::find_companions(uint32_t index) {
  // The other functions of the GPU's device: on a discrete card these are its own (audio, USB-C), never another GPU's. Display controllers are
  // adapters of their own and are left out.
  const PciAddress gpu = topology_.adapter(index).device.address;
  Companions& c = companions_[index];
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d) && c.count < kMaxCompanions; ++i) {
    if (d.address.bus != gpu.bus || d.address.device != gpu.device || d.address.function == gpu.function) continue;
    if (d.class_code == kPciClassDisplay) continue;
    c.function[c.count] = d.address;
    c.busy[c.count] = false;
    ++c.count;
  }
}

PciAddress GpuManager::companion(uint32_t index, uint32_t which) const {
  kernel::ScopedMutex g(lock_);
  if (index >= topology_.count() || which >= companions_[index].count) return PciAddress{};
  return companions_[index].function[which];
}

bool GpuManager::sleeps_when_idle(uint32_t index) const {
  return static_cast<int32_t>(index) != topology_.boot_index() && topology_.adapter(index).kind == GpuKind::Discrete;
}

int32_t GpuManager::select(const GpuSelectionInput& input) const {
  GpuSelectionInput filled = input;
  {
    kernel::ScopedMutex g(lock_);
    if (filled.power == PowerSource::Unknown) {
      filled.power = power_source_;
      filled.battery_percent = battery_percent_;
    }
    filled.battery_saver = filled.battery_saver || battery_saver_;
  }
  return gpu_select(topology_, filled);
}

void GpuManager::set_power_status(PowerSource source, uint8_t battery_percent, bool battery_saver) {
  kernel::ScopedMutex g(lock_);
  power_source_ = source;
  battery_percent_ = battery_percent > 100 ? 100 : battery_percent;
  battery_saver_ = battery_saver;
}

bool GpuManager::acquire(uint32_t index) {
  kernel::ScopedMutex g(lock_);   // the lease count and the asleep/wake decision must not race across cores
  if (index >= topology_.count()) return false;
  if (leases_[index] == UINT32_MAX) return false;
  if (level_[index] != GpuPowerLevel::On && !wake(index)) return false;
  ++leases_[index];
  idle_pending_[index] = false;
  return true;
}

void GpuManager::release(uint32_t index) {
  kernel::ScopedMutex g(lock_);   // exactly one core may take the last lease and put the adapter to sleep
  if (index >= topology_.count() || leases_[index] == 0) return;
  if (--leases_[index] != 0 || !sleeps_when_idle(index)) return;
  if (autosuspend_ms_ == 0) {
    sleep(index);
  } else {
    idle_pending_[index] = true;
    idle_since_[index] = 0;
  }
}

bool GpuManager::attach_platform_power(uint32_t index, IGpuPlatformPower* power) {
  kernel::ScopedMutex g(lock_);
  if (index >= topology_.count()) return false;
  if (!power && level_[index] == GpuPowerLevel::D3Cold) return false;   // the handler that cut the power is the one that brings it back
  power_[index] = power;
  return true;
}

void GpuManager::allow_cold(uint32_t index, bool allowed) {
  kernel::ScopedMutex g(lock_);
  if (index < topology_.count()) cold_allowed_[index] = allowed;
}

bool GpuManager::mark_function_busy(PciAddress function, bool busy) {
  kernel::ScopedMutex g(lock_);
  for (uint32_t i = 0; i < topology_.count(); ++i) {
    Companions& c = companions_[i];
    for (uint32_t k = 0; k < c.count; ++k) {
      if (!same_function(c.function[k], function)) continue;
      c.busy[k] = busy;
      if (busy) {
        // The driver is about to use it: it must have power, and stays awake (sleep() leaves busy functions alone) even while the GPU sleeps.
        if (level_[i] == GpuPowerLevel::D3Cold) {
          if (!wake(i)) return true;
          // Woken for the function alone: the GPU goes back to D3hot from service() like any idle one.
          idle_pending_[i] = leases_[i] == 0 && sleeps_when_idle(i);
          idle_since_[i] = 0;
        }
        (void)pci_set_power_state(pci_, function, PciPowerState::D0);
        if (delay_) delay_(kSettleMilliseconds);
      }
      return true;
    }
  }
  return false;
}

bool GpuManager::enable_aspm(uint32_t index) {
  kernel::ScopedMutex g(lock_);
  if (index >= topology_.count() || level_[index] == GpuPowerLevel::D3Cold) return false;
  PcieAspmPlan plan;
  if (!pcie_aspm_configure(pci_, topology_.adapter(index).device.address, &plan)) return false;
  aspm_[index] = plan.l0s || plan.l1 || plan.clock_pm;
  return aspm_[index];
}

bool GpuManager::set_link_off_when_cold(uint32_t index, bool enabled) {
  kernel::ScopedMutex g(lock_);
  if (index >= topology_.count() || !has_bridge_[index]) return false;
  link_off_[index] = enabled;
  return true;
}

bool GpuManager::upstream_bridge(uint32_t index, PciAddress* out) const {
  kernel::ScopedMutex g(lock_);
  if (index >= topology_.count() || !has_bridge_[index]) return false;
  *out = bridge_[index];
  return true;
}

void GpuManager::save_express(PciAddress function, uint32_t* out) const {
  const uint16_t cap = pci_.find_capability(function, pcie::kCapabilityExpress);
  for (uint32_t d = 0; d < kSavedExpressDwords; ++d) out[d] = 0;
  if (cap == 0) return;
  out[0] = pci_.read32(function, static_cast<uint16_t>(cap + 0x08)) & 0xFFFF;   // Device Control (the Status half is write-1-to-clear)
  out[1] = pci_.read32(function, static_cast<uint16_t>(cap + 0x10)) & 0xFFFF;   // Link Control (ditto)
  out[2] = pci_.read32(function, static_cast<uint16_t>(cap + 0x28)) & 0xFFFF;   // Device Control 2
  out[3] = pci_.read32(function, static_cast<uint16_t>(cap + 0x30)) & 0xFFFF;   // Link Control 2
}

void GpuManager::restore_express(PciAddress function, const uint32_t* saved) const {
  const uint16_t cap = pci_.find_capability(function, pcie::kCapabilityExpress);
  if (cap == 0) return;
  pci_.write32(function, static_cast<uint16_t>(cap + 0x08), saved[0]);
  pci_.write32(function, static_cast<uint16_t>(cap + 0x28), saved[2]);
  pci_.write32(function, static_cast<uint16_t>(cap + 0x30), saved[3]);
  // (ASPM is enabled at the port end first; the port kept its Link Control, so the function's goes back as it was.)
  pci_.write32(function, static_cast<uint16_t>(cap + 0x10), saved[1] & ~uint32_t{pcie::kLinkCtlRetrain | pcie::kLinkCtlDisable});
}

bool GpuManager::cold_possible(uint32_t index) const {
  if (!power_[index] || !cold_allowed_[index] || leases_[index] != 0) return false;
  const Companions& c = companions_[index];
  for (uint32_t k = 0; k < c.count; ++k)
    if (c.busy[k]) return false;
  return true;
}

uint32_t GpuManager::service(uint64_t now_ms) {
  kernel::ScopedMutex g(lock_);
  uint32_t changed = 0;
  const uint64_t stamp = now_ms + 1;   // 0 means "not timed yet"
  for (uint32_t i = 0; i < topology_.count(); ++i) {
    if (idle_pending_[i] && leases_[i] == 0 && level_[i] == GpuPowerLevel::On) {
      if (idle_since_[i] == 0) {
        idle_since_[i] = stamp;
      } else if (stamp >= idle_since_[i] && stamp - idle_since_[i] >= autosuspend_ms_) {
        sleep(i);
        idle_pending_[i] = false;
        if (level_[i] != GpuPowerLevel::On) ++changed;
      }
    }
    if (level_[i] == GpuPowerLevel::D3Hot && cold_possible(i) && go_cold(i)) ++changed;
  }
  return changed;
}

void GpuManager::save_config(PciAddress function, uint32_t* out) const {
  for (uint32_t d = 0; d < kSavedConfigDwords; ++d) out[d] = pci_.read32(function, static_cast<uint16_t>(d * 4));
}

void GpuManager::restore_config(PciAddress function, const uint32_t* saved) const {
  // What a reset device has lost and the OS had set (pci_restore_state, the type 0 header part): cache line size and latency timer, the BARs,
  // the expansion ROM, the interrupt line; the command register last, so the decoding comes on with the BARs already right.
  pci_.write32(function, 0x0C, saved[3]);
  for (uint16_t offset = 0x10; offset <= 0x24; offset = static_cast<uint16_t>(offset + 4)) pci_.write32(function, offset, saved[offset / 4]);
  pci_.write32(function, 0x30, saved[0x30 / 4]);
  pci_.write32(function, 0x3C, saved[0x3C / 4]);
  pci_.write32(function, 0x04, saved[1] & 0xFFFF);
}

bool GpuManager::go_cold(uint32_t index) {
  const Companions& c = companions_[index];
  const PciAddress gpu = topology_.adapter(index).device.address;
  save_config(gpu, saved_[index][0]);
  save_express(gpu, saved_express_[index][0]);
  for (uint32_t k = 0; k < c.count; ++k) {
    save_config(c.function[k], saved_[index][1 + k]);
    save_express(c.function[k], saved_express_[index][1 + k]);
  }
  // The link goes down before the power, so the port does not see a surprise link loss.
  link_is_off_[index] = link_off_[index] && has_bridge_[index] && pcie_link_disable(pci_, bridge_[index]);
  if (!power_[index]->power_off()) {
    if (link_is_off_[index]) (void)pcie_link_enable(pci_, bridge_[index], delay_);
    link_is_off_[index] = false;
    return false;   // still in D3hot, which is where it was
  }
  level_[index] = GpuPowerLevel::D3Cold;
  return true;
}

bool GpuManager::wake(uint32_t index) {
  const PciAddress gpu = topology_.adapter(index).device.address;
  const Companions& c = companions_[index];
  if (level_[index] == GpuPowerLevel::D3Cold) {
    if (!power_[index] || !power_[index]->power_on()) return false;
    const uint32_t wait = power_[index]->power_on_delay_ms();
    if (delay_) delay_(wait > kMaxPowerOnDelayMs ? kMaxPowerOnDelayMs : wait);
    if (link_is_off_[index]) {
      // Retrain and wait for the data link (pcie_link_enable bounds the wait); a link that never comes up leaves the GPU unreachable.
      if (!pcie_link_enable(pci_, bridge_[index], delay_)) return false;
      link_is_off_[index] = false;
    }
    if ((pci_.read32(gpu, 0) & 0xFFFF) == 0xFFFF) return false;   // nothing answers: the power did not come back
    restore_config(gpu, saved_[index][0]);
    restore_express(gpu, saved_express_[index][0]);
    for (uint32_t k = 0; k < c.count; ++k) {
      if ((pci_.read32(c.function[k], 0) & 0xFFFF) == 0xFFFF) continue;
      restore_config(c.function[k], saved_[index][1 + k]);
      restore_express(c.function[k], saved_express_[index][1 + k]);
    }
    level_[index] = GpuPowerLevel::D3Hot;   // a reset device starts in D0; the PM writes below are then harmless
  }
  if (!pci_set_power_state(pci_, gpu, PciPowerState::D0)) return false;
  for (uint32_t k = 0; k < c.count; ++k) (void)pci_set_power_state(pci_, c.function[k], PciPowerState::D0);
  if (delay_) delay_(kSettleMilliseconds);
  PciPowerState now = PciPowerState::D3Hot;
  if (!pci_get_power_state(pci_, gpu, &now) || now != PciPowerState::D0) return false;
  level_[index] = GpuPowerLevel::On;
  ++wakes_[index];
  return true;
}

void GpuManager::sleep(uint32_t index) {
  // Its other functions first (the GPU is the upstream end of whatever they do), and only those no driver uses.
  const Companions& c = companions_[index];
  for (uint32_t k = 0; k < c.count; ++k)
    if (!c.busy[k]) (void)pci_set_power_state(pci_, c.function[k], PciPowerState::D3Hot);
  if (pci_set_power_state(pci_, topology_.adapter(index).device.address, PciPowerState::D3Hot)) level_[index] = GpuPowerLevel::D3Hot;
}

}  // namespace hardware
