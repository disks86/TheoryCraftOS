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

// Monitors plugged into a sleeping GPU. A hybrid laptop's discrete GPU sleeps while idle (D3hot, or D3cold with its power cut: GpuManager), and
// while it does its own hot-plug detection is dark: a monitor plugged into an HDMI or DisplayPort port wired to it goes unnoticed. Linux
// notices it two ways, and so does this:
//
//  - ACPI. The firmware routes the port's hot-plug line to a chipset GPIO (or lets the sleeping GPU raise a PME) and its event method says
//    Notify on the GPU's Device (0x81 "output status change" as the ACPI video extensions have it, 0x80, or 0x02 device wake), on one of the
//    GPU's output Devices, or on its root port (0x00 bus check, 0x01 device check, 0x02). watch_acpi() finds those nodes (GpuAcpiNamespace);
//    the hot-plug object is an IAcpiNotifyListener (AcpiEvents::add_listener). A notification only marks the GPU; service() then takes a lease
//    (waking it), has its probe read the connectors again (GpuPool::refresh_outputs), gives the lease back and tells the listeners.
//  - Polling, as DRM polls connectors that cannot interrupt: off by default (set_poll_policy). When on, every watched GPU whose probe can read
//    outputs is looked at every period_ms (10 s by default). The trade-off: each look wakes the GPU, and it then stays up for the manager's
//    autosuspend time (5 s), so a 10 s period keeps a discrete GPU powered about half of the time; on battery below min_battery_percent (or with
//    battery saver) a sleeping GPU is not woken for it.
//
// A GPU with a display driver never sleeps (the driver holds a lease), so its own hot-plug works and SystemMonitors polls its monitor source; a
// notification for it is passed on to the listeners unchecked (GpuOutputCheck::CannotTell), and so is one for a GPU whose probe cannot see
// outputs: the listeners look themselves (SystemMonitors::poll, the HD-audio codec's pins).
//
// Listeners (IGpuOutputListener) hear outputs_changed(gpu) once per change: main.cpp has the GPU's HD-audio controller read its pins and ELD
// again (SystemAudioDevices::connections_changed) and the desktop look for monitors (SystemMonitors::poll).
//
// Threads: on_notify() may come from any thread (it only takes a short lock and sets a bit); service() from one or several (passes are
// serialized); listeners may be added and removed from any thread. Listeners are called from service() with no lock of this class held but
// the dispatch lock: a listener must not call remove_listener() or service() itself (add_listener() is fine). remove_listener() returns once no
// call to that listener is in flight.
#pragma once
#include <stdint.h>
#include "Hardware/Acpi/AcpiEvents.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/Acpi/GpuAcpiNamespace.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPool.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/Optional.hpp"

namespace hardware {

class IGpuOutputListener {
 public:
  // A monitor came or went on GPU `gpu_index` (a pool index), or the firmware said one may have and nothing here could tell. Called from
  // GpuHotplug::service().
  virtual void outputs_changed(uint32_t gpu_index) = 0;

 protected:
  IGpuOutputListener() = default;
  ~IGpuOutputListener() = default;
};

struct GpuOutputPollPolicy {
  static constexpr uint32_t kDefaultPeriodMs = 10000;
  static constexpr uint32_t kMinPeriodMs = 1000;   // (a shorter period is raised to this: the GPU would never sleep)
  bool enabled = false;                            // off by default: each look wakes the GPU
  uint32_t period_ms = kDefaultPeriodMs;
  uint8_t min_battery_percent = kDiscreteMinBatteryPercent;   // on battery below this (or with battery saver) a sleeping GPU is not woken to look
};

// What a watched ACPI node is to its GPU; it decides which Notify values mean "look at the outputs".
enum class GpuAcpiRole : uint8_t {
  Device,         // the GPU's own Device: 0x02 (device wake, a PME from the sleeping GPU), 0x80, 0x81
  Bridge,         // its root port: 0x00 (bus check), 0x01 (device check), 0x02
  Output,         // one of the GPU's output Devices (_DOD children): 0x80, 0x81
  OtherAdapter,   // another GPU's Device that reports the hybrid machine's output changes (the integrated GPU's ACPI video): 0x81 only
};

class GpuHotplug final : public IAcpiNotifyListener {
 public:
  static constexpr uint32_t kMaxListeners = 8;
  static constexpr uint32_t kMaxWatches = 32;   // ACPI nodes over all GPUs
  static constexpr uint32_t kMaxOutputsPerGpu = 6;

  explicit GpuHotplug(GpuPool& pool) : pool_(pool) {}
  GpuHotplug(const GpuHotplug&) = delete;
  GpuHotplug& operator=(const GpuHotplug&) = delete;

  // GPU `index` is looked after: polled (when the policy is on) and checked on request_check(). False for a bad index. watch_acpi() and
  // watch_node() imply it.
  [[nodiscard]] bool watch(uint32_t index);
  bool watched(uint32_t index) const;
  // Finds GPU `index`'s ACPI Device and root port (GpuAcpiNamespace::find_gpu; `bridge` is the PCI bridge above it, when known) and the
  // Devices below the GPU's (its outputs), and watches them. Returns the nodes found (also kept for acpi_nodes()); with none the GPU is still
  // watched for polling.
  GpuAcpiNodes watch_acpi(uint32_t index, acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& bridge = {});
  // One node for GPU `index`. False for a bad index, a null node or a full table.
  [[nodiscard]] bool watch_node(uint32_t index, acpi::Node* node, GpuAcpiRole role);
  // What watch_acpi() found for GPU `index` (for arming the nodes' wake events: AcpiEvents::enable_wake).
  GpuAcpiNodes acpi_nodes(uint32_t index) const;
  // True if Notify(`role` node, `value`) means the outputs may have changed.
  static bool is_output_notification(GpuAcpiRole role, uint32_t value);

  // IAcpiNotifyListener: marks the GPUs whose nodes this is. Any thread; never blocks for long, never touches the hardware.
  void on_notify(acpi::Node* device, uint32_t value) override;
  // As if the firmware had notified GPU `index` (a driver that learnt of a hot plug some other way). False for a GPU not watched.
  bool request_check(uint32_t index);

  void set_poll_policy(const GpuOutputPollPolicy& policy);
  GpuOutputPollPolicy poll_policy() const;

  // Does the work: every GPU notified since the last call, and every one due for a poll, is checked (GpuPool::refresh_outputs: a lease, the
  // probe's look, the lease back), and the listeners hear of each change. `now_ms` is a monotonic clock in milliseconds. Call it from a thread
  // that may wait for a GPU to wake (the main loop does, next to GpuManager::service). Returns the number of changes reported.
  uint32_t service(uint64_t now_ms);

  // False if the list is full or `listener` is null. The same listener twice is one entry.
  [[nodiscard]] bool add_listener(IGpuOutputListener* listener);
  // Returns once no call to `listener` is in flight (do not call it from a listener).
  void remove_listener(IGpuOutputListener* listener);

  // Statistics (boot log, tests).
  uint32_t notifications() const { return __atomic_load_n(&notifications_, __ATOMIC_RELAXED); }   // notifications that marked a GPU
  uint32_t checks() const { return __atomic_load_n(&checks_, __ATOMIC_RELAXED); }                 // refresh_outputs() calls
  uint32_t changes() const { return __atomic_load_n(&changes_, __ATOMIC_RELAXED); }               // outputs_changed() rounds
  uint32_t polls_skipped() const { return __atomic_load_n(&skipped_, __ATOMIC_RELAXED); }        // polls not made on a low battery
  uint32_t wake_failures() const { return __atomic_load_n(&wake_failures_, __ATOMIC_RELAXED); }

 private:
  struct Watch {
    acpi::Node* node = nullptr;
    uint32_t gpu = 0;
    GpuAcpiRole role = GpuAcpiRole::Device;
  };
  void dispatch(uint32_t index);
  void count(uint32_t* counter) { __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED); }

  GpuPool& pool_;
  mutable kernel::Mutex lock_;          // the watch table, the policy and the poll times (short holds, no calls out)
  Watch watches_[kMaxWatches];
  uint32_t watch_count_ = 0;
  GpuAcpiNodes nodes_[GpuTopology::kMaxAdapters];
  uint32_t watched_ = 0;                // bit per GPU
  GpuOutputPollPolicy policy_;
  uint64_t next_poll_ms_[GpuTopology::kMaxAdapters] = {};
  bool poll_timed_[GpuTopology::kMaxAdapters] = {};
  uint64_t last_now_ms_ = 0;
  uint32_t pending_ = 0;                // bit per GPU notified and not yet checked (atomic)

  kernel::Mutex service_lock_;          // one service() pass at a time
  kernel::Mutex dispatch_lock_;         // held while listeners are called (remove_listener waits on it)
  kernel::Mutex listeners_lock_;        // the listener list
  IGpuOutputListener* listeners_[kMaxListeners] = {};

  uint32_t notifications_ = 0, checks_ = 0, changes_ = 0, skipped_ = 0, wake_failures_ = 0;
};

}  // namespace hardware
