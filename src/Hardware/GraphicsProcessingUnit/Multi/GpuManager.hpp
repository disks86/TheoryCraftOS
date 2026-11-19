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
// suspended: a driver is running on it. Every other discrete adapter is put into PCI power state D3hot while nothing holds a lease on it and
// brought back to D0 (waiting the 10 ms the PCI power management spec requires, through the delay hook) when something acquires it. On a hybrid
// laptop this is what lets the discrete GPU draw almost nothing while the desktop runs on the integrated one.
//
// Runtime power management of a laptop's discrete GPU, the way Linux's runtime PM and vga_switcheroo do it:
//  - The GPU's other functions (an NVIDIA or AMD card's HD-audio function, Turing's USB-C controller and UCSI function) sleep and wake with it:
//    they are found at initialize() and put in D3hot before the GPU, woken after it.
//  - With autosuspend set, the last release() does not put the GPU to sleep at once; service() does, once it has been idle that long (Linux waits
//    5 s), so work that comes in bursts does not power-cycle it each time.
//  - With a platform power handler (IGpuPlatformPower: ACPI power resources, NVIDIA Optimus / JT, AMD ATPX, Apple gmux) and cold sleep allowed,
//    the GPU goes on from D3hot to D3cold: its power is cut. Its configuration header (and its functions') is saved before and written back after
//    power returns, since the device comes back reset. Allowed only while no function of the GPU is used by a driver that cannot set the device up
//    again (allow_cold() says so per adapter; mark_function_busy() vetoes it).
//  - The power source (mains or battery, and the charge) is kept here and fills in what a caller of select() leaves Unknown.
//  - The PCI Express link (Multi/PcieLink): enable_aspm() turns on the link power states both ends support (L0s/L1, clock PM) and they are put back
//    after a power cut, together with the PCI Express capability's Device and Link Control registers; with set_link_off_when_cold() the upstream
//    port's link is disabled before the power goes and retrained (waiting for the data link to come up) when it returns, for platforms whose
//    power methods leave the link alone.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPlatformPower.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPowerPolicy.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuTopology.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

enum class PciPowerState : uint8_t { D0 = 0, D1 = 1, D2 = 2, D3Hot = 3 };

// The device's current PCI power state (from the PM capability); false if it has none.
bool pci_get_power_state(const IPciBus& pci, PciAddress address, PciPowerState* out);
// Sets the power state through the PM capability; false if the device has none. Waiting for the device to settle is the caller's job.
bool pci_set_power_state(const IPciBus& pci, PciAddress address, PciPowerState state);

// How far down an adapter is.
enum class GpuPowerLevel : uint8_t { On, D3Hot, D3Cold };
const char* gpu_power_level_name(GpuPowerLevel level);

class GpuManager {
 public:
  using DelayFn = void (*)(uint32_t milliseconds);
  static constexpr uint32_t kMaxCompanions = 7;          // functions 1-7 of the GPU's device
  static constexpr uint32_t kSavedConfigDwords = 16;     // the type 0 header, 0x00-0x3F
  static constexpr uint32_t kDefaultAutosuspendMs = 5000;

  // `delay` is called (when set) after a state change that needs the device to settle.
  explicit GpuManager(const IPciBus& pci, DelayFn delay = nullptr) : pci_(pci), delay_(delay) {}

  // Finds the GPUs and their other functions, and puts every discrete adapter but the display one to sleep (D3hot: cold sleep waits for
  // service(), after the drivers of the other functions have had their say).
  void initialize();

  const GpuTopology& topology() const { return topology_; }
  uint32_t count() const { return topology_.count(); }
  bool is_hybrid() const { return topology_.is_hybrid(); }

  // The render lease: wake/sleep is serialized so two cores may take and drop it at the same time
  // (the wake on the first lease and the sleep on the last never race). Guarded by `lock_`
  // (a blocking Mutex: wake() may settle the GPU for a while while holding it, and it works before
  // the scheduler comes up); wake()/sleep() assume the caller already holds it.
  //
  // The adapter that carries the screen, or -1.
  int32_t display_index() const { return topology_.boot_index(); }
  // Picks the adapter for this work (see gpu_select()); does not wake it. A power source the input leaves Unknown is taken from
  // set_power_status().
  int32_t select(const GpuSelectionInput& input) const;

  // Takes a lease on the adapter, waking it if it slept. False for a bad index or a device that would not wake.
  // Thread-safe: two cores may call it in parallel.
  bool acquire(uint32_t index);
  // Gives a lease back; the adapter sleeps when the last one goes (unless it carries the display), at once or, with autosuspend, from service().
  // Thread-safe: two cores may call it in parallel, and the sleep happens exactly once on the last lease.
  void release(uint32_t index);
  uint32_t leases(uint32_t index) const { kernel::ScopedMutex g(lock_); return index < topology_.count() ? leases_[index] : 0; }
  // True while the adapter is powered down by this manager (D3hot or D3cold).
  bool asleep(uint32_t index) const { kernel::ScopedMutex g(lock_); return index < topology_.count() && level_[index] != GpuPowerLevel::On; }
  GpuPowerLevel power_level(uint32_t index) const { kernel::ScopedMutex g(lock_); return index < topology_.count() ? level_[index] : GpuPowerLevel::On; }
  // How many times the adapter was woken (for the boot log and the tests).
  uint32_t wake_count(uint32_t index) const { kernel::ScopedMutex g(lock_); return index < topology_.count() ? wakes_[index] : 0; }

  // --- Runtime power management.
  // 0 (the default) sleeps on the last release; otherwise service() sleeps an adapter that has had no lease for this long.
  void set_autosuspend_ms(uint32_t ms) { kernel::ScopedMutex g(lock_); autosuspend_ms_ = ms; }
  // The platform's way of cutting the adapter's power (not owned; must outlive the manager or be detached with nullptr). False for a bad index.
  bool attach_platform_power(uint32_t index, IGpuPlatformPower* power);
  IGpuPlatformPower* platform_power(uint32_t index) const { kernel::ScopedMutex g(lock_); return index < topology_.count() ? power_[index] : nullptr; }
  // Whether the adapter may lose its power while it sleeps: true when nothing on it keeps state the manager cannot restore (no driver, or a
  // driver that sets the device up again on every acquire). Off by default.
  void allow_cold(uint32_t index, bool allowed);
  // A driver uses this function of a GPU (its HD-audio controller, say): the GPU then never goes to D3cold, and the function is left awake.
  // False if the function is not a GPU's or one of its companions.
  bool mark_function_busy(PciAddress function, bool busy);
  uint32_t companion_count(uint32_t index) const { kernel::ScopedMutex g(lock_); return index < topology_.count() ? companions_[index].count : 0; }
  PciAddress companion(uint32_t index, uint32_t which) const;
  // Call now and then (about once a second) with a monotonic clock: sleeps the adapters that have been idle for the autosuspend time, and takes
  // asleep adapters that may lose power on to D3cold. Returns how many adapters changed level.
  uint32_t service(uint64_t now_ms);

  // Turns on the ASPM link states the GPU's link supports at both ends (and every function of the GPU accepts); kept across power cuts.
  // False when the GPU or its port has no PCI Express capability or nothing could be enabled.
  bool enable_aspm(uint32_t index);
  bool aspm_enabled(uint32_t index) const { kernel::ScopedMutex g(lock_); return index < topology_.count() && aspm_[index]; }
  // Disable the upstream port's link before cutting the GPU's power and retrain it after. False when the GPU has no upstream bridge.
  bool set_link_off_when_cold(uint32_t index, bool enabled);
  // The bridge above the adapter (its root port), when there is one.
  bool upstream_bridge(uint32_t index, PciAddress* out) const;

  // What the machine runs on, for select(). Thread-safe.
  void set_power_status(PowerSource source, uint8_t battery_percent, bool battery_saver);
  struct PowerStatus {
    PowerSource source = PowerSource::Unknown;
    uint8_t battery_percent = 100;
    bool battery_saver = false;
  };
  // What set_power_status() said last (for policies that must not wake a GPU on a low battery, such as GpuHotplug's polling). Thread-safe.
  PowerStatus power_status() const {
    kernel::ScopedMutex g(lock_);
    return PowerStatus{power_source_, battery_percent_, battery_saver_};
  }

 private:
  struct Companions {
    PciAddress function[kMaxCompanions] = {};
    bool busy[kMaxCompanions] = {};
    uint32_t count = 0;
  };
  // Called only with `lock_` held (by acquire/release/service), or before any other thread exists (initialize).
  bool wake(uint32_t index);
  void sleep(uint32_t index);
  bool go_cold(uint32_t index);
  bool cold_possible(uint32_t index) const;
  bool sleeps_when_idle(uint32_t index) const;
  void find_companions(uint32_t index);
  void save_config(PciAddress function, uint32_t* out) const;
  void restore_config(PciAddress function, const uint32_t* saved) const;
  void save_express(PciAddress function, uint32_t* out) const;
  void restore_express(PciAddress function, const uint32_t* saved) const;

  mutable kernel::Mutex lock_;   // serializes the lease count and level transitions across cores
  const IPciBus& pci_;
  DelayFn delay_;
  GpuTopology topology_;
  uint32_t leases_[GpuTopology::kMaxAdapters] = {};
  GpuPowerLevel level_[GpuTopology::kMaxAdapters] = {};
  uint32_t wakes_[GpuTopology::kMaxAdapters] = {};
  uint64_t idle_since_[GpuTopology::kMaxAdapters] = {};   // service()'s clock when the last lease went; 0 = not timed yet
  bool idle_pending_[GpuTopology::kMaxAdapters] = {};     // awake with no lease, waiting for the autosuspend time
  bool cold_allowed_[GpuTopology::kMaxAdapters] = {};
  IGpuPlatformPower* power_[GpuTopology::kMaxAdapters] = {};
  Companions companions_[GpuTopology::kMaxAdapters];
  // Config headers saved for D3cold: the GPU's and each companion's.
  uint32_t saved_[GpuTopology::kMaxAdapters][1 + kMaxCompanions][kSavedConfigDwords] = {};
  // The PCI Express capability's Device Control, Link Control, Device Control 2 and Link Control 2, per function, saved with the header.
  static constexpr uint32_t kSavedExpressDwords = 4;
  uint32_t saved_express_[GpuTopology::kMaxAdapters][1 + kMaxCompanions][kSavedExpressDwords] = {};
  PciAddress bridge_[GpuTopology::kMaxAdapters] = {};
  bool has_bridge_[GpuTopology::kMaxAdapters] = {};
  bool aspm_[GpuTopology::kMaxAdapters] = {};
  bool link_off_[GpuTopology::kMaxAdapters] = {};
  bool link_is_off_[GpuTopology::kMaxAdapters] = {};
  uint32_t autosuspend_ms_ = 0;
  PowerSource power_source_ = PowerSource::Unknown;
  uint8_t battery_percent_ = 100;
  bool battery_saver_ = false;
};

}  // namespace hardware
