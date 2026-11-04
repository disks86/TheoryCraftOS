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

#include "Hardware/NeuralProcessingUnit/SystemNeuralProcessingUnit.hpp"
#include <new>
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaNpu.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpu.hpp"

namespace hardware {

namespace {
static_assert(sizeof(IntelNpu) == sizeof(SoftwareNeuralProcessingUnit) &&
                  sizeof(AmdXdnaNpu) == sizeof(SoftwareNeuralProcessingUnit),
              "NPU classes must fit SystemNeuralProcessingUnit's storage");
}

SystemNeuralProcessingUnit::SystemNeuralProcessingUnit(const IPciBus& pci, ICentralProcessingUnit& cpu,
                                                       scheduling::ICPUScheduler& scheduler) {
  PciDeviceInfo d;
  for (uint32_t i = 0; !active_ && pci.pci_device(i, &d); ++i) {
    if (d.vendor_id == IntelNpu::kPciVendor && IntelNpu::matches(d.device_id)) {
      active_ = new (storage_) IntelNpu(cpu, scheduler);
      intel_device_ = d;
      intel_found_ = true;
      description_.assign("Intel NPU (");
      description_.append(IntelNpu::platform(d.device_id));
      description_.append(")");
    } else if (d.vendor_id == AmdXdnaNpu::kPciVendor && AmdXdnaNpu::matches(d.device_id)) {
      active_ = new (storage_) AmdXdnaNpu(cpu, scheduler);
      description_.assign("AMD NPU (");
      description_.append(AmdXdnaNpu::platform(d.device_id));
      description_.append(")");
    }
  }
  hardware_present_ = active_ != nullptr;
  if (hardware_present_) description_.append(", computation on the CPU until a native driver exists");
  else description_.assign("no NPU hardware found, computation on the CPU");
  if (!active_) active_ = new (storage_) SoftwareNeuralProcessingUnit(cpu, scheduler);
  active_->initialize();
}

void SystemNeuralProcessingUnit::probe_registers(const IPciBus& pci, IRegisterMapper* mmio) {
  constexpr uint16_t kPciCommand = 0x04;
  constexpr uint32_t kMemorySpace = 1u << 1;
  constexpr uint8_t kIpBar = 0, kButtressBar = 4;  // ivpu's regv and regb
  if (!intel_found_ || !mmio || !(pci.read32(intel_device_.address, kPciCommand) & kMemorySpace)) return;
  const IntelNpuPlatform platform = intel_npu_platform(intel_device_.device_id);
  const uint64_t ip_base = pci.memory_bar(intel_device_.address, kIpBar);
  const uint64_t btrs_base = pci.memory_bar(intel_device_.address, kButtressBar);
  MmioRegion ip, btrs;
  if (ip_base == 0 || btrs_base == 0 || !mmio->map_registers(ip_base, IntelNpuHw::ip_window(platform.ip), &ip) ||
      !mmio->map_registers(btrs_base, IntelNpuHw::kButtressWindow, &btrs))
    return;
  MmioNpuRegisters ip_regs(ip), btrs_regs(btrs);
  IntelNpuHw hw(ip_regs, btrs_regs, platform);
  intel_info_ = hw.read_info();
  if (!intel_info_.valid) {
    description_.append("; registers did not answer");
    return;
  }
  description_.append("; up to ");
  description_.append_uint(intel_info_.max_mhz);
  description_.append(" MHz");
  if (intel_info_.tile_disable_mask) description_.append(", tiles fused off");
}

}  // namespace hardware
