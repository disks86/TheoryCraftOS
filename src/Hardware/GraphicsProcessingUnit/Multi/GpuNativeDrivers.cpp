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

// The native display drivers a GpuPool can build for a GPU that is not the boot display. Apart from GpuPool.cpp so that what needs no driver code
// (the pool's bookkeeping) can be built and tested without every driver.
#include <new>
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPool.hpp"
#include "Hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/SystemGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/VMware/VmwareSvgaGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.hpp"

namespace hardware {

namespace {

template <class T>
void destroy_driver(GpuNode& node, allocators::IAllocator& allocator) {
  T* gpu = static_cast<T*>(node.gpu);
  gpu->shutdown();
  gpu->~T();
  allocator.deallocate(gpu);
  node.gpu = nullptr;
  node.virtio = nullptr;
  node.vmware = nullptr;
  node.destroy = nullptr;
}

// Builds a T on the adapter; on success `node` has it. Memory comes from the allocator, aligned for T.
template <class T>
T* build_driver(GpuNode& node, const GpuContext& context, allocators::IAllocator& allocator) {
  void* memory = allocator.allocate_aligned(sizeof(T), alignof(T) < 16 ? 16 : alignof(T));
  if (!memory) return nullptr;
  T* gpu = new (memory) T(context);
  if (!gpu->initialize()) {
    gpu->shutdown();
    gpu->~T();
    allocator.deallocate(memory);
    return nullptr;
  }
  node.gpu = gpu;
  node.destroy = &destroy_driver<T>;
  return gpu;
}

// One row per native driver: does it take this PCI function, and how is it built.
struct DriverRow {
  bool (*matches)(const PciDeviceInfo& device);
  bool (*build)(GpuNode& node, const GpuContext& context, allocators::IAllocator& allocator);
};

const DriverRow kDrivers[] = {
    {[](const PciDeviceInfo& d) { return QemuStdVgaGpu::supports(d.vendor_id, d.device_id); },
     [](GpuNode& n, const GpuContext& c, allocators::IAllocator& a) { return build_driver<QemuStdVgaGpu>(n, c, a) != nullptr; }},
    {[](const PciDeviceInfo& d) { return d.vendor_id == VmwareSvgaGpu::kPciVendor && d.device_id == VmwareSvgaGpu::kPciDevice; },
     [](GpuNode& n, const GpuContext& c, allocators::IAllocator& a) {
       n.vmware = build_driver<VmwareSvgaGpu>(n, c, a);
       return n.vmware != nullptr;
     }},
    {[](const PciDeviceInfo& d) { return d.vendor_id == VirtioGpu::kPciVendor && d.device_id == VirtioGpu::kPciDeviceId; },
     [](GpuNode& n, const GpuContext& c, allocators::IAllocator& a) {
       n.virtio = build_driver<VirtioGpu>(n, c, a);
       return n.virtio != nullptr;
     }},
};
}  // namespace

uint32_t GpuPool::bind(efi::SystemTable* st, const IPciBus& pci, SystemGpu& boot) {
  const int32_t display = manager_.display_index();
  if (display >= 0 && boot.gpu()) adopt(static_cast<uint32_t>(display), boot.gpu(), boot.virtio_gpu(), boot.vmware_gpu());
  return bind_native_drivers(st, pci);
}

uint32_t GpuPool::bind_native_drivers(efi::SystemTable* st, const IPciBus& pci) {
  for (uint32_t i = 0; i < manager_.count(); ++i) {
    GpuNode& n = nodes_[i];
    if (n.gpu) continue;
    const GpuContext context{st, &pci, n.adapter.device, &allocator_};
    for (const DriverRow& row : kDrivers) {
      if (!row.matches(n.adapter.device)) continue;
      // The device has to be awake while the driver sets it up, and stays so as long as the driver exists (a driver has no way yet to take
      // its device through a power cycle).
      if (!manager_.acquire(i)) break;
      if (row.build(n, context, allocator_))
        n.leased_for_driver = true;
      else
        manager_.release(i);
      break;
    }
  }
  return driver_count();
}

}  // namespace hardware
