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
#include "Hardware/IoPort.hpp"

namespace hardware {

uint8_t AmdPmIndexPorts::read8(uint8_t offset) {
  IoPort(0xCD6).write8(offset);
  return IoPort(0xCD7).read8();
}

void SystemChipset::probe(const ChipsetCpu& cpu, const IPciBus& pci, IRegisterMapper* mapper) {
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
  if (!active_) return;

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
