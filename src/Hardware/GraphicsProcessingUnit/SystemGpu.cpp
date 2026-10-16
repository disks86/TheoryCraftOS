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

#include "Hardware/GraphicsProcessingUnit/SystemGpu.hpp"
#include <new>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpu.hpp"

namespace hardware {

namespace {
static_assert(sizeof(IntelGpu) <= sizeof(SystemGpu) && sizeof(AmdGpu) <= sizeof(SystemGpu) &&
                  sizeof(NvidiaGpu) <= sizeof(SystemGpu),
              "vendor GPU classes must fit SystemGpu's storage");

// Builds T in `storage`, returning it if it initializes; otherwise tears it down.
template <class T>
IGraphicsProcessingUnit* try_gpu(void* storage, const GpuContext& context) {
  T* gpu = new (storage) T(context);
  if (gpu->initialize()) return gpu;
  gpu->shutdown();
  return nullptr;
}
}  // namespace

SystemGpu::SystemGpu(efi::SystemTable* st, const IPciBus& pci, allocators::IAllocator& allocator) {
  // The first display-class device is assumed to be the primary one.
  PciDeviceInfo d{};
  bool found = false;
  for (uint32_t i = 0; !found && pci.pci_device(i, &d); ++i) found = d.class_code == kPciClassDisplay;
  if (!found) d = {};
  const GpuContext context{st, &pci, d, &allocator};

  if (found) {
    if (d.vendor_id == QemuStdVgaGpu::kPciVendor && d.device_id == QemuStdVgaGpu::kPciDevice)
      active_ = try_gpu<QemuStdVgaGpu>(storage_, context);
    else if (d.vendor_id == VirtioGpu::kPciVendor && d.device_id == VirtioGpu::kPciDeviceId) {
      active_ = try_gpu<VirtioGpu>(storage_, context);
      virtio_ = active_ != nullptr;
    } else if (d.vendor_id == IntelGpu::kPciVendor)
      active_ = try_gpu<IntelGpu>(storage_, context);
    else if (d.vendor_id == AmdGpu::kPciVendor)
      active_ = try_gpu<AmdGpu>(storage_, context);
    else if (d.vendor_id == NvidiaGpu::kPciVendor)
      active_ = try_gpu<NvidiaGpu>(storage_, context);
  }
  if (!active_) active_ = try_gpu<UefiGopGpu>(storage_, context);  // always-available fallback
}

}  // namespace hardware
