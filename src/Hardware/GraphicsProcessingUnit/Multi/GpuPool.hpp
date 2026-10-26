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

// The display drivers of every GPU in the machine, however many there are (up to GpuTopology::kMaxAdapters). SystemGpu builds the driver of the
// boot display, which is what the console and the desktop run on; the pool adds the others, each on its own PCI function, and owns the
// GpuManager that keeps the GPUs asleep while nothing uses them.
//
// A driver is built for a non-boot GPU only where a native one exists (QEMU/VirtualBox VGA, VMware SVGA, virtio-gpu): the Intel, AMD and NVIDIA
// classes drive the UEFI framebuffer, and that belongs to the boot display alone. Such a GPU has a node with no driver, and cannot show or render
// anything until a native driver for it exists. Adding a driver means adding a row to the table in GpuPool.cpp.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuTopology.hpp"
#include "uefi.hpp"

namespace hardware {

class VirtioGpu;
class VmwareSvgaGpu;
class SystemGpu;
struct GpuNode;

// Gives back what a driver built by the pool took. Set by the pool only.
using GpuDestroyFn = void (*)(GpuNode& node, allocators::IAllocator& allocator);

struct GpuNode {
  GpuAdapter adapter;
  IGraphicsProcessingUnit* gpu = nullptr;  // the display driver; nullptr when this GPU has none
  VirtioGpu* virtio = nullptr;             // the same driver, when it is a virtio-gpu (the 3D command channel)
  VmwareSvgaGpu* vmware = nullptr;         // ... or a VMware SVGA adapter
  GpuDestroyFn destroy = nullptr;          // non-null for a driver the pool built (and so frees)
  bool leased_for_driver = false;          // the pool holds a power lease for the driver
};

class GpuPool {
 public:
  GpuPool(const IPciBus& pci, allocators::IAllocator& allocator, GpuManager::DelayFn delay = nullptr) : allocator_(allocator), manager_(pci, delay) {}
  ~GpuPool();
  GpuPool(const GpuPool&) = delete;
  GpuPool& operator=(const GpuPool&) = delete;

  // Finds the GPUs (see GpuManager::initialize()).
  void initialize();
  // Takes a driver that someone else owns (the boot display's, from SystemGpu) as the driver of the adapter at `index`.
  bool adopt(uint32_t index, IGraphicsProcessingUnit* gpu, VirtioGpu* virtio = nullptr, VmwareSvgaGpu* vmware = nullptr);
  // Adopts the boot display's driver from `boot`, then builds a driver for every other adapter that has a native one. Returns how many nodes
  // have a driver afterwards.
  uint32_t bind(efi::SystemTable* st, const IPciBus& pci, SystemGpu& boot);
  // Builds drivers for the adapters that have none yet, and have a native one.
  uint32_t bind_native_drivers(efi::SystemTable* st, const IPciBus& pci);

  uint32_t count() const { return manager_.count(); }
  GpuNode* node(uint32_t index) { return index < manager_.count() ? &nodes_[index] : nullptr; }
  const GpuNode* node(uint32_t index) const { return index < manager_.count() ? &nodes_[index] : nullptr; }
  // How many nodes have a driver.
  uint32_t driver_count() const;
  GpuManager& manager() { return manager_; }
  const GpuManager& manager() const { return manager_; }

 private:
  allocators::IAllocator& allocator_;
  GpuManager manager_;
  GpuNode nodes_[GpuTopology::kMaxAdapters];
};

}  // namespace hardware
