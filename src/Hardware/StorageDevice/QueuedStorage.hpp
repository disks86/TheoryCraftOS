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

// What the storage drivers that keep many commands on the device at once (NVMe, virtio-blk, AHCI with NCQ) share: the interrupt vectors
// they take their completions on, a pool of command slots, and the wait for one command's completion - sleeping until the completion
// interrupt wakes it, looking at the queue itself when the interrupt is late, or polling when there are no interrupts.
#pragma once
#include <stdint.h>
#include "Hardware/DeviceProperties.hpp"
#include "Kernel/Interrupts.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

// x86: a vector of kVectorStorageFirst.. for a completion interrupt, or 0 when all are taken (and on other architectures).
uint8_t claim_storage_vector();
void release_storage_vector(uint8_t vector);

// The time stamp counter in microseconds (0 until it is calibrated).
uint64_t storage_now_us();

// Up to 64 command slots, handed out lock-free. A slot is what a request holds while it is on the device: its buffer, its descriptors or
// tag, its completion.
class SlotPool {
 public:
  static constexpr uint32_t kMaxSlots = 64;

  void reset(uint32_t count);   // all `count` free (count <= kMaxSlots)
  uint32_t count() const { return count_; }
  // A free slot; waits (yields) while none is. The I/O scheduler keeps to the device's queue depth, so only callers that bypass it wait.
  uint32_t acquire();
  bool try_acquire(uint32_t* slot);
  void release(uint32_t slot);
  // Every slot taken and none handed out until the next reset (shutdown).
  void close() { __atomic_store_n(&free_, 0, __ATOMIC_RELEASE); }

 private:
  uint64_t free_ = 0;   // atomic: bit n while slot n is free
  uint32_t count_ = 0;
};

// The completion of the command in one slot. The submitter arms it before the command goes to the device; the reaper (an interrupt
// handler, or any waiter looking at the queue) records the status under the queue's lock and wakes the waiter after letting the lock go.
// A wake can be stale (the slot's previous use): the waiter always checks `completed` itself.
struct CommandCompletion {
  kernel::Event done;
  uint32_t completed = 0;   // atomic
  uint16_t status = 0;
  bool lost = false;        // it timed out: the device may still use the slot's memory, so the slot is never handed out again

  void arm() {
    __atomic_store_n(&completed, 0, __ATOMIC_RELAXED);
    done.reset();
  }
  void record(uint16_t result) {
    status = result;
    __atomic_store_n(&completed, 1, __ATOMIC_RELEASE);
  }
  void wake() { done.set(); }
  bool is_complete() const { return __atomic_load_n(&completed, __ATOMIC_ACQUIRE) != 0; }

  // Waits for record(). With `sleep` (completion interrupts are on, and this thread may sleep) on the event, calling `reap()` whenever a
  // kInterruptWaitUs wait times out; otherwise calling `reap()` in a loop. False after `timeout_us` without a completion (`lost` is set).
  static constexpr uint64_t kInterruptWaitUs = 2000;
  template <class Reap>
  bool wait(bool sleep, uint64_t timeout_us, Reap&& reap) {
    const uint64_t start = storage_now_us();
    while (!is_complete()) {
      if (sleep) {
        if (!done.wait(kInterruptWaitUs)) reap();
        else if (!is_complete()) done.reset();   // (a stale wake)
      } else {
        reap();
        if (is_complete()) break;
        if (kernel::running()) kernel::yield();
        else kernel::cpu_relax();
      }
      if (storage_now_us() - start > timeout_us) {
        reap();
        if (is_complete()) break;
        lost = true;
        return false;
      }
    }
    return true;
  }
};

// The properties every such driver shows: its queues and depth, how completions arrive, and the interrupts each queue's core took.
struct QueueReport {
  uint32_t core = 0;
  uint64_t interrupts = 0;
};
void describe_queues(IPropertySink& sink, uint32_t queue_count, uint32_t depth, bool interrupts, const QueueReport* queues);

// Whether a waiter may sleep for its completion interrupt: interrupts are on for the device, threads run, and this one is not in a section
// with interrupts masked.
inline bool may_sleep_for_interrupt(bool interrupts) { return interrupts && kernel::running() && kernel::irq_enabled(); }

}  // namespace hardware
