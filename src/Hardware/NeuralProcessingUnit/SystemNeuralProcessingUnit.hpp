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

#pragma once
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Allocators/IAllocator.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaDevice.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuAccelerator.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuDevice.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuExecutor.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Picks the NPU class for this machine: a vendor class when a known hardware
// NPU is on the PCI bus, otherwise the software NPU. There is always one.
class SystemNeuralProcessingUnit {
 public:
  SystemNeuralProcessingUnit(const IPciBus& pci, ICentralProcessingUnit& cpu,
                             scheduling::ICPUScheduler& scheduler);

  INeuralProcessingUnit& npu() { return *active_; }
  // One line for the boot log: which NPU hardware was found (vendor and platform) and that the computation still runs on the CPU, or that there is none.
  const char* description() const { return description_.c_str(); }
  // Looks at an Intel NPU's fuse and status registers, or an AMD NPU's PSP, SMU and firmware-alive registers (read only; nothing is powered up or
  // written), and adds what they say to description(). Needs the device's memory decoding to be on, as the firmware leaves it. Does nothing for other NPUs
  // or without a mapper.
  void probe_registers(const IPciBus& pci, IRegisterMapper* mmio);
  // What probe_registers found; valid is false when it did not run or the registers did not answer.
  const IntelNpuInfo& intel_info() const { return intel_info_; }
  // The Intel NPU on the bus, if there is one: where it is and what it is, for whoever sets up its interrupt before start_intel_device().
  bool intel_present() const { return intel_found_; }
  PciAddress intel_address() const { return intel_device_.address; }
  IntelNpuPlatform intel_platform() const { return intel_npu_platform(intel_device_.device_id); }
  // Brings the Intel NPU up with a firmware image (intel/vpu/vpu_*_v1.bin; the kernel does not carry one): powers the IP block, turns on the MMU, boots the
  // firmware and waits for it. `firmware` must stay alive while the device is used. `system_time_us` is the wall clock. Returns None on success, and then npu()
  // is the IntelNpuAccelerator (call it again after this: a reference taken before is the software NPU) and intel_executor() runs work on the device. Otherwise
  // the device is left off and description() says why. Without the call nothing is powered up and work stays on the CPU.
  IntelNpuDeviceError start_intel_device(const IPciBus& pci, IRegisterMapper* mmio, allocators::IAllocator& allocator, ConstByteView firmware, uint64_t system_time_us,
                                         const IntelNpuExecutor::Config& executor_config);
  // The running device, or null. Once there is an executor, use that: the device is not thread safe.
  IntelNpuDevice* intel_device() { return intel_dev_.get(); }
  IntelNpuExecutor* intel_executor() { return intel_exec_.get(); }
  // The AMD NPU on the bus, if there is one: its PCI address, revision and the model the Linux amdxdna driver's table gives that (device id, revision).
  bool amd_present() const { return amd_found_; }
  PciAddress amd_address() const { return amd_device_.address; }
  uint8_t amd_revision() const { return amd_revision_; }
  AmdXdnaIdentity amd_identity() const { return amd_xdna_identify(amd_device_.device_id, amd_revision_); }
  // What probe_registers found for the AMD NPU; valid is false when it did not run or the registers did not answer.
  const AmdXdnaProbe& amd_probe() const { return amd_probe_; }
  // Brings the AMD NPU up with its firmware (amdnpu/<id>_<rev>/npu.sbin or npu_7.sbin; AIE4 parts also cert.dev.sbin as `cert_firmware`; the kernel carries
  // none): turns on bus mastering, powers it through the SMU, has the PSP validate and start the firmware, opens the management mailbox and runs the
  // driver's bring-up messages. The images are copied; `clock` (may be null: polls are counted instead) times the waits. Returns None on success, and then
  // amd_device() talks to the firmware; work still runs on the CPU (no graph compiler for the AI Engine array exists). Otherwise the device is powered
  // off again and description() says where it stopped. Not called at boot.
  AmdXdnaError start_amd_device(const IPciBus& pci, IRegisterMapper* mmio, allocators::IAllocator& allocator, ConstByteView firmware, ConstByteView cert_firmware,
                                const IClock* clock);
  AmdXdnaDevice* amd_device() { return amd_dev_.get(); }
  // True when a hardware NPU is on the bus (its work still runs on the CPU until a native driver exists).
  bool hardware_present() const { return hardware_present_; }

 private:
  void probe_amd_registers(const IPciBus& pci, IRegisterMapper* mmio);
  // Vendor classes add no data, so the software NPU's size covers them all.
  alignas(16) unsigned char storage_[sizeof(SoftwareNeuralProcessingUnit)];
  INeuralProcessingUnit* active_ = nullptr;
  FixedString<256> description_;
  PciDeviceInfo intel_device_;
  IntelNpuInfo intel_info_;
  bool intel_found_ = false;
  UniquePointer<MmioNpuRegisters> intel_ip_regs_, intel_btrs_regs_;
  UniquePointer<IntelNpuDevice> intel_dev_;
  UniquePointer<IntelNpuExecutor> intel_exec_;      // after the device: they go first
  UniquePointer<IntelNpuAccelerator> intel_accel_;
  PciDeviceInfo amd_device_{};
  uint8_t amd_revision_ = 0;
  bool amd_found_ = false;
  AmdXdnaProbe amd_probe_;
  UniquePointer<MmioNpuRegisters> amd_regs_[6];
  UniquePointer<AmdXdnaDevice> amd_dev_;            // after the registers it reads through: it goes first
  ICentralProcessingUnit& cpu_;
  scheduling::ICPUScheduler& scheduler_;
  bool hardware_present_ = false;
};

}  // namespace hardware
