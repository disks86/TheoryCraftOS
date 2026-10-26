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

// Machines with more than one graphics device: a CPU with integrated graphics beside a discrete card (a "hybrid" laptop, or a desktop whose
// monitor sits on the iGPU). The integrated GPU is the power-efficient one (it shares system memory and idles at a fraction of a watt); the
// discrete GPU is the fast one (memory of its own, many more engines, and tens of watts). This file only says what each display device in the
// machine IS; GpuPowerPolicy decides which to use for what, and GpuManager keeps the discrete one asleep while nobody needs it.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

enum class GpuKind : uint8_t {
  Unknown,     // a display controller this OS cannot place (an unlisted vendor, or an ID outside the tables)
  Integrated,  // the graphics of the CPU / APU: shares system memory, low power
  Discrete,    // a card (or soldered chip) with memory of its own: high performance, high power
  Virtual,     // a hypervisor's display adapter (QEMU/VirtualBox VGA, VMware SVGA, virtio-gpu): power is the host's problem
};

const char* gpu_kind_name(GpuKind kind);

struct GpuAdapter {
  PciDeviceInfo device{};
  GpuKind kind = GpuKind::Unknown;
  const char* name = "Display controller";  // vendor and family; static storage
  // The firmware is showing the screen on it (its PCI memory/IO decoding was left on): the one the boot display runs on.
  bool boot_display = false;
};

// Says what a display-class PCI device is, from its vendor and device ID alone.
GpuKind gpu_classify(uint16_t vendor_id, uint16_t device_id, const char** name = nullptr);

// Every display controller on the PCI bus, in discovery order.
class GpuTopology {
 public:
  static constexpr uint32_t kMaxAdapters = 8;

  // Rescans `pci` (previous contents are dropped).
  void enumerate(const IPciBus& pci);

  uint32_t count() const { return count_; }
  const GpuAdapter& adapter(uint32_t index) const { return adapters_[index < count_ ? index : 0]; }
  // The adapter the firmware displays on (it is the one the boot console uses); the first when the firmware left no hint. -1 when there are none.
  int32_t boot_index() const { return boot_; }
  // The first adapter of that kind, or -1.
  int32_t find_first(GpuKind kind) const;
  // Both kinds present: the situation that needs a policy.
  bool is_hybrid() const { return find_first(GpuKind::Integrated) >= 0 && find_first(GpuKind::Discrete) >= 0; }

 private:
  GpuAdapter adapters_[kMaxAdapters];
  uint32_t count_ = 0;
  int32_t boot_ = -1;
};

// The PCI display controller the boot display should be built on (the one the firmware shows the screen on), or false if there is none.
bool gpu_boot_display(const IPciBus& pci, PciDeviceInfo* out);

}  // namespace hardware
