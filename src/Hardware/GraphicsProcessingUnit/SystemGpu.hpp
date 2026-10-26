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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/VMware/VmwareSvgaGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.hpp"

namespace hardware {

constexpr unsigned long GpuMaxOf(unsigned long a, unsigned long b) { return a > b ? a : b; }

// Finds the display controller on the PCI bus and brings up the best GPU class
// for it: a vendor/model class where one exists, else the UEFI framebuffer.
// Storage is inline because there is no heap yet.
class SystemGpu {
 public:
  // Tries candidates in order and keeps the first whose initialize() succeeds.
  // `registers` (when given) lets a driver map its device's registers and take over mode setting from the firmware (Intel integrated GPUs); without it
  // every GPU uses the framebuffer the firmware set up.
  SystemGpu(efi::SystemTable* st, const IPciBus& pci, allocators::IAllocator& allocator, IRegisterMapper* registers = nullptr);

  // The initialized GPU, or nullptr if no usable display was found.
  IGraphicsProcessingUnit* gpu() { return active_; }
  // The active GPU as a virtio-gpu if it is one (the 3D path needs its command channel), else nullptr.
  VirtioGpu* virtio_gpu() { return virtio_ ? static_cast<VirtioGpu*>(active_) : nullptr; }
  // The active GPU as an AMD one if it is (on a Raven-family APU it can show an OS framebuffer of its own), else nullptr.
  AmdGpu* amd_gpu() { return amd_ ? static_cast<AmdGpu*>(active_) : nullptr; }
  // The active GPU as an Intel one if it is (it can drive the display engine natively), else nullptr.
  IntelGpu* intel_gpu() { return intel_ ? static_cast<IntelGpu*>(active_) : nullptr; }
  // The active GPU as a VMware SVGA II adapter if it is one (its 3D engine takes commands through the FIFO), else nullptr.
  VmwareSvgaGpu* vmware_gpu() { return vmware_ ? static_cast<VmwareSvgaGpu*>(active_) : nullptr; }

 private:
  static constexpr unsigned long kSize =
      GpuMaxOf(GpuMaxOf(GpuMaxOf(GpuMaxOf(GpuMaxOf(sizeof(UefiGopGpu), sizeof(QemuStdVgaGpu)), sizeof(VirtioGpu)), sizeof(VmwareSvgaGpu)), sizeof(IntelGpu)), sizeof(AmdGpu));

  alignas(16) unsigned char storage_[kSize];
  IGraphicsProcessingUnit* active_ = nullptr;
  bool virtio_ = false;
  bool vmware_ = false;
  bool intel_ = false;
  bool amd_ = false;
};

}  // namespace hardware
