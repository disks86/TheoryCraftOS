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

#include "Hardware/GraphicsProcessingUnit/Multi/GpuTopology.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.hpp"

namespace hardware {

namespace {
constexpr uint16_t kVendorIntel = 0x8086, kVendorAmd = 0x1002, kVendorNvidia = 0x10DE;
constexpr uint16_t kVendorVmware = 0x15AD, kVendorVirtio = 0x1AF4;
constexpr uint16_t kPciCommand = 0x04;
constexpr uint32_t kCommandIoMemory = 0x3;  // I/O space and memory space decoding enabled
}  // namespace

const char* gpu_kind_name(GpuKind kind) {
  switch (kind) {
    case GpuKind::Integrated: return "integrated";
    case GpuKind::Discrete: return "discrete";
    case GpuKind::Virtual: return "virtual";
    default: return "unknown";
  }
}

GpuKind gpu_classify(uint16_t vendor_id, uint16_t device_id, const char** name) {
  const char* unused = nullptr;
  if (!name) name = &unused;
  if (QemuStdVgaGpu::supports(vendor_id, device_id) || vendor_id == kVendorVmware || (vendor_id == kVendorVirtio && device_id == 0x1050)) {
    *name = "Virtual display adapter";
    return GpuKind::Virtual;
  }
  if (vendor_id == kVendorIntel) {
    const IntelGpuInfo info = intel_gpu_identify(device_id);
    *name = info.name;
    // Before the Arc cards every Intel display controller was the graphics of a CPU, so an ID the table does not know is integrated too.
    return info.discrete ? GpuKind::Discrete : GpuKind::Integrated;
  }
  if (vendor_id == kVendorAmd) {
    const AmdGpuInfo info = amd_gpu_identify(device_id);
    *name = info.name;
    if (info.architecture == AmdGpuArchitecture::Unknown) return GpuKind::Unknown;
    return info.discrete ? GpuKind::Discrete : GpuKind::Integrated;
  }
  if (vendor_id == kVendorNvidia) {
    *name = nvidia_gpu_identify(device_id).name;
    return GpuKind::Discrete;  // NVIDIA has no PCI display controller that is part of a CPU
  }
  return GpuKind::Unknown;
}

void GpuTopology::enumerate(const IPciBus& pci) {
  count_ = 0;
  boot_ = -1;
  PciDeviceInfo d{};
  for (uint32_t i = 0; count_ < kMaxAdapters && pci.pci_device(i, &d); ++i) {
    if (d.class_code != kPciClassDisplay) continue;
    GpuAdapter& a = adapters_[count_];
    a = GpuAdapter{};
    a.device = d;
    a.kind = gpu_classify(d.vendor_id, d.device_id, &a.name);
    a.boot_display = (pci.read32(d.address, kPciCommand) & kCommandIoMemory) != 0;
    if (boot_ < 0 && a.boot_display) boot_ = static_cast<int32_t>(count_);
    ++count_;
  }
  if (boot_ < 0 && count_ != 0) boot_ = 0;
}

int32_t GpuTopology::find_first(GpuKind kind) const {
  for (uint32_t i = 0; i < count_; ++i)
    if (adapters_[i].kind == kind) return static_cast<int32_t>(i);
  return -1;
}

bool gpu_boot_display(const IPciBus& pci, PciDeviceInfo* out) {
  GpuTopology topology;
  topology.enumerate(pci);
  if (topology.boot_index() < 0) return false;
  *out = topology.adapter(static_cast<uint32_t>(topology.boot_index())).device;
  return true;
}

}  // namespace hardware
