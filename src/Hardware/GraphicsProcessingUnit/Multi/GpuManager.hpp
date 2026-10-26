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

// Keeps track of which GPUs are in use and lets the ones that are not sleep. The display GPU (the one the firmware shows the screen on) is never
// suspended: a driver is running on it. Every other adapter is put into PCI power state D3hot while nothing holds a lease on it and brought
// back to D0 (waiting the 10 ms the PCI power management spec requires, through the delay hook) when something acquires it. On a hybrid laptop
// this is what lets the discrete GPU draw almost nothing while the desktop runs on the integrated one.
//
// Not done: a driver for a second GPU (SystemGpu builds the display driver for the boot display only), so "use" here means the policy decision
// and the power state; a render offload path would take a lease around its work.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPowerPolicy.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuTopology.hpp"

namespace hardware {

enum class PciPowerState : uint8_t { D0 = 0, D1 = 1, D2 = 2, D3Hot = 3 };

// The device's current PCI power state (from the PM capability); false if it has none.
bool pci_get_power_state(const IPciBus& pci, PciAddress address, PciPowerState* out);
// Sets the power state through the PM capability; false if the device has none. Waiting for the device to settle is the caller's job.
bool pci_set_power_state(const IPciBus& pci, PciAddress address, PciPowerState state);

class GpuManager {
 public:
  using DelayFn = void (*)(uint32_t milliseconds);

  // `delay` is called (when set) after a state change that needs the device to settle.
  explicit GpuManager(const IPciBus& pci, DelayFn delay = nullptr) : pci_(pci), delay_(delay) {}

  // Finds the GPUs and puts every adapter but the display one to sleep.
  void initialize();

  const GpuTopology& topology() const { return topology_; }
  uint32_t count() const { return topology_.count(); }
  bool is_hybrid() const { return topology_.is_hybrid(); }

  // The adapter that carries the screen, or -1.
  int32_t display_index() const { return topology_.boot_index(); }
  // Picks the adapter for this work (see gpu_select()); does not wake it.
  int32_t select(const GpuSelectionInput& input) const { return gpu_select(topology_, input); }

  // Takes a lease on the adapter, waking it if it slept. False for a bad index or a device that would not wake.
  bool acquire(uint32_t index);
  // Gives a lease back; the adapter sleeps when the last one goes (unless it carries the display).
  void release(uint32_t index);
  uint32_t leases(uint32_t index) const { return index < topology_.count() ? leases_[index] : 0; }
  // True while the adapter is powered down by this manager.
  bool asleep(uint32_t index) const { return index < topology_.count() && asleep_[index]; }

 private:
  bool wake(uint32_t index);
  void sleep(uint32_t index);

  const IPciBus& pci_;
  DelayFn delay_;
  GpuTopology topology_;
  uint32_t leases_[GpuTopology::kMaxAdapters] = {};
  bool asleep_[GpuTopology::kMaxAdapters] = {};
};

}  // namespace hardware
