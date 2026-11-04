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
                                                       scheduling::ICPUScheduler& scheduler)
    : cpu_(cpu), scheduler_(scheduler) {
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

IntelNpuDeviceError SystemNeuralProcessingUnit::start_intel_device(const IPciBus& pci, IRegisterMapper* mmio, allocators::IAllocator& allocator,
                                                                  ConstByteView firmware, uint64_t system_time_us,
                                                                  const IntelNpuExecutor::Config& executor_config) {
  constexpr uint16_t kPciCommand = 0x04, kPciRevision = 0x08;
  constexpr uint32_t kMemorySpace = 1u << 1;
  constexpr uint8_t kIpBar = 0, kButtressBar = 4;
  if (!intel_found_ || !mmio || intel_dev_ || !(pci.read32(intel_device_.address, kPciCommand) & kMemorySpace)) return IntelNpuDeviceError::BadState;
  const IntelNpuPlatform platform = intel_npu_platform(intel_device_.device_id);
  const uint64_t ip_base = pci.memory_bar(intel_device_.address, kIpBar);
  const uint64_t btrs_base = pci.memory_bar(intel_device_.address, kButtressBar);
  MmioRegion ip, btrs;
  if (ip_base == 0 || btrs_base == 0 || !mmio->map_registers(ip_base, IntelNpuHw::operational_window(platform.ip), &ip) ||
      !mmio->map_registers(btrs_base, IntelNpuHw::kButtressWindow, &btrs))
    return IntelNpuDeviceError::Registers;
  intel_ip_regs_ = UniquePointer<MmioNpuRegisters>::make(allocator, ip);
  intel_btrs_regs_ = UniquePointer<MmioNpuRegisters>::make(allocator, btrs);
  if (!intel_ip_regs_ || !intel_btrs_regs_) return IntelNpuDeviceError::Memory;
  IntelNpuDeviceConfig config;
  config.ip = intel_ip_regs_.get();
  config.buttress = intel_btrs_regs_.get();
  config.allocator = &allocator;
  config.device_id = intel_device_.device_id;
  config.revision = static_cast<uint8_t>(pci.read32(intel_device_.address, kPciRevision) & 0xFF);
  config.pci_bus = intel_device_.address.bus;
  config.snoop = false;   // flush shared memory: the NPU is not assumed to snoop the CPU caches
  UniquePointer<IntelNpuDevice> device = UniquePointer<IntelNpuDevice>::make(allocator, config);
  if (!device) return IntelNpuDeviceError::Memory;
  IntelNpuDeviceError error = device->load_firmware(firmware);
  if (error == IntelNpuDeviceError::None) error = device->start(system_time_us);
  if (error != IntelNpuDeviceError::None) {
    description_.append("; firmware not started: ");
    description_.append(intel_npu_device_error_name(error));
    return error;   // the device (off again) is destroyed here, before the register wrappers it points at
  }
  intel_dev_ = util::move(device);
  intel_exec_ = UniquePointer<IntelNpuExecutor>::make(allocator, *intel_dev_, allocator, executor_config);
  if (intel_exec_) intel_accel_ = UniquePointer<IntelNpuAccelerator>::make(allocator, cpu_, scheduler_, *intel_exec_);
  if (intel_accel_) {
    intel_accel_->set_clock(executor_config.clock);
    active_ = intel_accel_.get();
  }
  description_.append("; firmware running");
  return IntelNpuDeviceError::None;
}

}  // namespace hardware
