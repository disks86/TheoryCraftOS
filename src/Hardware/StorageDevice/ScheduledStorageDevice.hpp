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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "IoScheduler/IoScheduler.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A storage device whose requests go through the I/O scheduler (IoScheduler/IoScheduler.hpp, docs/IoScheduling.md). SystemStorageDevices
// puts one in front of every device it finds, so everything that reads or writes a disk - file systems, the swap of a program's image, the
// page cache - shares the device by the I/O priorities of the threads asking (set_thread_io_priority; by default from their CPU priority).
// The calling thread runs its own request on the device once the scheduler lets it, so a request that can go at once costs nothing but a
// compare-and-swap, and a waiting one sleeps on its own event until the request before it finishes. Everything else is the device's.
class ScheduledStorageDevice final : public IStorageDevice {
 public:
  explicit ScheduledStorageDevice(allocators::IAllocator& allocator) : scheduler_(allocator) {}

  // Sets up the scheduler for `device` (initialized already: its info() says the medium and depth) and takes it over. False, with `device`
  // left as it was, if there was no memory.
  bool attach(UniquePointer<IStorageDevice>& device);

  const char* name() const override { return inner_->name(); }
  bool initialize() override { return true; }   // (attach() takes a device that is up)
  void shutdown() override { inner_->shutdown(); }
  // The device's own facts, then the scheduler's.
  void describe(IPropertySink& sink) const override;
  bool present() const override { return inner_->present(); }
  bool can_eject() const override { return inner_->can_eject(); }
  bool eject() override { return inner_->eject(); }
  bool ejected() const override { return inner_->ejected(); }
  StorageInfo info() const override { return inner_->info(); }
  StorageStatistics statistics() const override { return inner_->statistics(); }

  bool read(uint64_t lba, ByteView buffer) override;
  bool write(uint64_t lba, ConstByteView buffer) override;
  bool flush() override;
  bool discard(uint64_t lba, uint64_t blocks) override;

  iosched::IoSchedulerStatistics scheduler_statistics() const { return scheduler_.statistics(); }
  IStorageDevice& device() { return *inner_; }

 private:
  // The scheduler's lock: the kernel's wait-list lock, a spinlock that masks interrupts on this core while held (kernel::detail::lock_list),
  // so a completion may one day come from an interrupt handler.
  class Lock {
   public:
    void lock() { flags_ = kernel::detail::lock_list(&list_); }
    void unlock() { kernel::detail::unlock_list(&list_, flags_); }

   private:
    kernel::WaitList list_;   // (only its lock word is used)
    uint64_t flags_ = 0;
  };

  // Waits for the scheduler's go, runs `transfer()` on the device and tells the scheduler it is done.
  template <class Transfer>
  bool run(iosched::IoOp op, uint64_t bytes, Transfer&& transfer);

  UniquePointer<IStorageDevice> inner_;
  mutable iosched::IoScheduler<Lock> scheduler_;   // (its statistics take its lock)
};

}  // namespace hardware
