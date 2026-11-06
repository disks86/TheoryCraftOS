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

#include "Hardware/Chipset/SystemChipset.hpp"
#include "Hardware/CentralProcessingUnit/CpuTemperature.hpp"
#include "Hardware/IoPort.hpp"

namespace hardware {

uint8_t AmdPmIndexPorts::read8(uint8_t offset) {
  IoPort(0xCD6).write8(offset);
  return IoPort(0xCD7).read8();
}

uint16_t TcoIoPorts::read16(uint8_t reg) { return IoPort(static_cast<uint16_t>(base_ + reg)).read16(); }

void TcoIoPorts::write16(uint8_t reg, uint16_t value) { IoPort(static_cast<uint16_t>(base_ + reg)).write16(value); }

namespace {
// cpu_temperature_millicelsius's SMN reads, taken under the nodes' lock.
bool read_smn_locked(void* context, uint32_t address, uint32_t* value) {
  return static_cast<SystemChipset*>(context)->with_amd_nodes([&](const AmdNodes& nodes) { return nodes.smn_read(0, address, value); });
}
}  // namespace

SystemChipset::~SystemChipset() { set_amd_smn_reader(nullptr, nullptr); }

void SystemChipset::probe(const ChipsetCpu& cpu, const IPciBus& pci, IRegisterMapper* mapper) {
  set_amd_smn_reader(nullptr, nullptr);
  watchdog_.with([](IntelTcoWatchdog& w) { w.attach(nullptr, IntelTcoKind::None); });
  tco_ports_.reset();
  watchdog_report_ = WatchdogReport();
  pch_thermal_ = IntelPchThermal();
  active_ = nullptr;
  smbus_.with([](SmbusHost& host) { host.attach(nullptr, SmbusHostFlavor::IntelI801); });   // before the ports it points at go
  ports_.reset();

  // Intel first on Intel CPUs and AMD first on AMD ones; with another vendor's CPU (Hygon, Zhaoxin, a virtual CPU) whichever is on the bus.
  const bool amd_first = cpu.vendor == ChipsetVendor::Amd;
  for (int attempt = 0; attempt < 2 && !active_; ++attempt) {
    if ((attempt == 0) == amd_first) {
      if (mapper && !pm_mmio_) {
        MmioRegion region;
        if (mapper->map_registers(AmdPmMmio::kPhysical, AmdPmMmio::kLength, &region)) pm_mmio_.emplace(region);
      }
      amd_.probe(pci, &pm_ports_, pm_mmio_ ? &*pm_mmio_ : nullptr);
      if (amd_.found()) active_ = &amd_;
    } else {
      intel_.probe(pci, cpu);
      if (intel_.found()) active_ = &intel_;
    }
  }
  // AMD Zen: the nodes behind SMN and the Data Fabric.
  if (cpu.vendor == ChipsetVendor::Amd || amd_.found()) {
    const uint32_t nodes = amd_nodes_.with([&](AmdNodes& n) {
      n.discover(pci, cpu.family, cpu.model);
      return n.node_count();
    });
    if (nodes) set_amd_smn_reader(read_smn_locked, this);
  }
  if (!active_) return;

  if (active_ == &intel_) {
    // The PCH thermal sensor (BAR 0 of its function).
    PciAddress thermal;
    if (mapper && intel_.thermal_function(&thermal)) {
      MmioRegion region;
      const uint64_t bar = pci.memory_bar(thermal, 0);
      if (bar && mapper->map_registers(bar, IntelPchThermal::kRegisterBytes, &region)) pch_thermal_.attach(region);
    }
    // The TCO watchdog. Nothing in the OS pings it yet, so one the firmware left counting is halted rather than left to reset the machine.
    const IntelTcoLocation& tco = intel_.tco();
    if (tco.usable) {
      tco_ports_.emplace(tco.io_base);
      TcoIoPorts* ports = &*tco_ports_;
      watchdog_report_ = watchdog_.with([&](IntelTcoWatchdog& w) {
        WatchdogReport report;
        if (!w.attach(ports, tco.kind)) return report;
        report.present = true;
        report.kind = tco.kind;
        report.caused_last_reset = w.caused_last_reset();
        report.timeout_seconds = w.timeout_seconds();
        w.clear_status();
        report.was_running = w.running();
        if (report.was_running) report.stopped = w.stop();
        return report;
      });
    }
  }

  const ChipsetSmbus smbus = active_->smbus();
  if (!smbus.usable) return;
  ports_.emplace(smbus.io_base);
  SmbusIoPorts* ports = &*ports_;
  smbus_.with([&](SmbusHost& host) { host.attach(ports, smbus.flavor); });
}

uint32_t SystemChipset::read_memory_modules(ArrayView<MemoryModuleSlot> out, const char** reason) {
  *reason = nullptr;
  if (!active_) {
    *reason = "no chipset";
    return 0;
  }
  if (!ports_) {
    *reason = active_->smbus().reason;
    return 0;
  }
  const AmdFchChipset* amd_chipset = amd();
  if (amd_chipset && amd_chipset->smbus_port() != 0 && amd_chipset->smbus_port() != 0xFF) {
    *reason = "the FCH routes the SMBus to another port than the memory modules'";
    return 0;
  }
  uint32_t found = 0;
  for (uint8_t slot = 0; slot < 8 && found < out.size(); ++slot) {
    SpdImage image;
    const SmbusStatus status = with_smbus([&](ISmbusController& smbus) { return spd_read(smbus, slot, &image); });
    if (status == SmbusStatus::Busy) {
      *reason = smbus_status_name(status);
      break;
    }
    if (status != SmbusStatus::Ok) continue;
    MemoryModuleSlot entry;
    entry.slot = slot;
    if (!spd_decode(image.view(), &entry.module)) continue;
    out[found++] = entry;
  }
  return found;
}

}  // namespace hardware
