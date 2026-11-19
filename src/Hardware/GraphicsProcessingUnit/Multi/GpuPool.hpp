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
// anything until a native driver for it exists. Adding a driver means adding a row to the table in GpuNativeDrivers.cpp.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuProbe.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuTopology.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

class VirtioGpu;
class VmwareSvgaGpu;
class SystemGpu;
class IPanelOutput;
struct GpuNode;

// Gives back what a driver built by the pool took. Set by the pool only.
using GpuDestroyFn = void (*)(GpuNode& node, allocators::IAllocator& allocator);

struct GpuNode {
  GpuAdapter adapter;
  IGraphicsProcessingUnit* gpu = nullptr;  // the display driver; nullptr when this GPU has none
  VirtioGpu* virtio = nullptr;             // the same driver, when it is a virtio-gpu (the 3D command channel)
  VmwareSvgaGpu* vmware = nullptr;         // ... or a VMware SVGA adapter
  IPanelOutput* panel = nullptr;           // the driver's built-in panel output (Multi/GpuMuxPanel.hpp), for the display mux switch; or none
  GpuDestroyFn destroy = nullptr;          // non-null for a driver the pool built (and so frees)
  bool leased_for_driver = false;          // the pool holds a power lease for the driver
  IGpuProbe* probe = nullptr;              // for a GPU with no driver: what can be read off it (owned by the pool)
};

// What GpuPool::refresh_outputs() found.
enum class GpuOutputCheck : uint8_t {
  Unchanged,    // the probe looked: nothing came or went
  Changed,      // the probe looked: a monitor came or went (or its EDID changed)
  CannotTell,   // nothing here can look (a display driver, which its own monitor source polls; no probe; a probe that cannot see outputs)
  NoWake,       // the GPU did not wake, or its probe could not set itself up again
  NoGpu,        // a bad index
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
  // have a driver afterwards. `registers` maps a GPU's register BARs for the drivers that program the hardware themselves (nullptr: those
  // drivers fail and the GPU is left to its probe).
  uint32_t bind(efi::SystemTable* st, const IPciBus& pci, SystemGpu& boot, IRegisterMapper* registers = nullptr);
  // Builds drivers for the adapters that have none yet, and have a native one.
  uint32_t bind_native_drivers(efi::SystemTable* st, const IPciBus& pci, IRegisterMapper* registers = nullptr);

  // Gives every node that has no driver a probe from `factory` (the GPU is woken for it and put back to sleep), and lets each driverless GPU
  // lose its power while it sleeps (a probe sets itself up again after a wake). `registers` maps the GPU's register BAR (nullptr: probes that
  // need it are skipped). Returns how many nodes have a probe.
  uint32_t bind_probes(efi::SystemTable* st, const IPciBus& pci, IRegisterMapper* registers, GpuProbeFactory factory);
  // The probe's temperature, if the GPU is awake (a sensor is not worth waking a GPU for): -1 when it sleeps, has no probe or cannot tell.
  int32_t probe_temperature(uint32_t index);
  // Takes a lease on the GPU (waking it, through D3cold if it lost its power), has its probe look at its outputs again (IGpuProbe::refresh_outputs,
  // after resume() when the GPU was woken since the probe last saw it), and gives the lease back (the GPU sleeps again after the autosuspend
  // time). A GPU with a display driver is not touched (CannotTell): the driver keeps it awake and SystemMonitors polls its monitor source.
  // Thread-safe: the probe is used by one caller at a time.
  [[nodiscard]] GpuOutputCheck refresh_outputs(uint32_t index);
  // Whether refresh_outputs() can tell anything for this GPU (a probe that reads outputs, and no driver).
  bool outputs_readable(uint32_t index) const;
  // The monitors the probe saw at its last look (no wake), 0 without a probe.
  uint32_t probe_connected_outputs(uint32_t index) const;

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
  UniquePointer<IGpuProbe> probes_[GpuTopology::kMaxAdapters];
  uint32_t probe_wakes_[GpuTopology::kMaxAdapters] = {};   // the manager's wake count the probe last saw (a new wake: resume() it)
  bool resume_probe(uint32_t index);   // (probe_lock_ held)
  mutable kernel::Mutex probe_lock_;   // one caller at a time uses a probe (temperature, outputs) and its probe_wakes_; taken after a lease, before the manager's lock
};

}  // namespace hardware
