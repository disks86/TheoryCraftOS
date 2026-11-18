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

// The machine's USB-C connectors: one UCSI policy manager kept up to date and published for every reader. One thread owns the UcsiCore and calls
// poll() (or run(), which loops on it): it starts the policy manager (retrying a few times if it does not answer), handles the connector changes
// when the firmware notified (Notify(device, 0x80), through IAcpiNotifyListener) and, for firmware that never notifies, looks at CCI every second
// and reads every connector again every half minute. A PPM that keeps timing out is dropped and started again later.
//
// What it read is published through RCU (read-mostly: it changes when something is plugged in): readers on any thread or core copy the current
// snapshot inside a read-side section, never blocking the poller; the poller publishes a new copy and retires the old one after a grace period.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AcpiEvents.hpp"
#include "Hardware/UsbTypeC/UcsiCore.hpp"
#include "Kernel/Rcu.hpp"

namespace hardware {

class UsbTypeCSystem final : public IUsbTypeC, public IAcpiNotifyListener {
 public:
  struct Options {
    uint32_t idle_check_ms = 1000;      // CCI looked at this often without a notification
    uint32_t full_refresh_ms = 30000;   // every connector read again this often
    uint32_t retry_ms = 30000;          // a policy manager that failed is tried again after this
    uint32_t max_start_attempts = 3;
    uint32_t max_failures = 3;          // commands timing out in a row before the policy manager is restarted
  };
  static constexpr uint32_t kNotifyValue = 0x80;

  UsbTypeCSystem(allocators::IAllocator& allocator, UcsiCore& core, IUcsiClock& clock, acpi::Node* device = nullptr)
      : UsbTypeCSystem(allocator, core, clock, device, Options()) {}
  UsbTypeCSystem(allocators::IAllocator& allocator, UcsiCore& core, IUcsiClock& clock, acpi::Node* device, const Options& options)
      : allocator_(allocator), core_(core), clock_(clock), device_(device), options_(options) {}
  ~UsbTypeCSystem();
  UsbTypeCSystem(const UsbTypeCSystem&) = delete;
  UsbTypeCSystem& operator=(const UsbTypeCSystem&) = delete;

  // --- The owner thread.
  // Starts the policy manager now. False if it did not answer (poll() tries again later).
  [[nodiscard]] bool start(uint64_t now_ms);
  void poll(uint64_t now_ms);
  // poll() in a loop until request_stop(), waking early when the firmware notifies. For a kernel thread: thread_main(this).
  void run();
  static void thread_main(void* system) { static_cast<UsbTypeCSystem*>(system)->run(); }
  void request_stop() { __atomic_store_n(&stop_, 1u, __ATOMIC_RELEASE); }
  bool running() const { return __atomic_load_n(&running_, __ATOMIC_ACQUIRE) != 0; }
  bool started() const { return __atomic_load_n(&ready_, __ATOMIC_ACQUIRE) != 0; }

  // --- Any thread.
  void on_notify(acpi::Node* device, uint32_t value) override;
  uint32_t notifications() const { return __atomic_load_n(&notifications_, __ATOMIC_RELAXED); }
  bool present() const override;
  uint32_t connector_count() const override;
  bool connector(uint32_t index, UsbTypeCConnector* out) const override;
  bool snapshot(UsbTypeCSnapshot* out) const override;
  uint32_t generation() const override;
  uint32_t publications() const { return __atomic_load_n(&publications_, __ATOMIC_RELAXED); }

 private:
  struct Published;
  struct FreePublished {
    void operator()(Published* p) const;
  };
  struct Published : kernel::RcuObjectBase<Published, FreePublished> {
    UsbTypeCSnapshot snapshot;
    allocators::IAllocator* allocator = nullptr;
  };
  void publish();
  void failed(UcsiCore::Result result, uint64_t now_ms);

  allocators::IAllocator& allocator_;
  UcsiCore& core_;
  IUcsiClock& clock_;
  acpi::Node* device_;
  Options options_;
  mutable kernel::RcuDomain domain_;
  Published* current_ = nullptr;          // (rcu_assign_pointer / rcu_dereference)
  // The owner's.
  UsbTypeCSnapshot work_;
  uint32_t generation_ = 0;
  uint32_t attempts_ = 0, failures_ = 0;
  uint64_t retry_at_ms_ = 0, last_check_ms_ = 0, last_full_ms_ = 0;
  bool dirty_ = false;                    // a publication that could not be allocated: tried again
  // Shared.
  uint32_t ready_ = 0, notify_pending_ = 0, notifications_ = 0, stop_ = 0, running_ = 0, publications_ = 0;
};

}  // namespace hardware
