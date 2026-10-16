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
#include "CPUScheduler/ICPUScheduler.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Synchronization/SpinLock.hpp"
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

namespace scheduling {

// Default scheduler: cooperative and run-to-completion, spread across every
// core the CPU interface reports. The two task queues are ring buffers drawn
// from the allocator, so they grow on demand instead of rejecting work after a
// fixed number of tasks (submit() fails only if the allocator is out of
// memory). Strict priority: the efficiency queue only starts when the
// high-performance queue is empty. High-performance tasks prefer Performance
// cores and efficiency tasks prefer Efficiency cores, falling back to any free
// core, then to this core. Call run()/run_next() from one core; submit() may be
// called from any.
//
// Both queues are guarded by one SpinLock, and they allocate under it, so the
// allocator is called from whichever core submits. It must therefore be safe to
// call from several cores - pass a ThreadSafeAllocator shared with the rest of
// the system. Room for kInitialCapacity tasks per queue is reserved up front so
// that ordinary submission never allocates.
class Scheduler final : public ICPUScheduler {
 public:
  static constexpr uint32_t kInitialCapacity = 64;

  Scheduler(hardware::ICentralProcessingUnit& cpu, allocators::IAllocator& allocator);

  bool submit(TaskFunction fn, void* context, TaskQueue queue = TaskQueue::HighPerformance) override;
  bool run_next() override;
  uint64_t pending() const override;

  // Runs tasks until both queues are empty and every started task has finished.
  void run() override;
  // Runs tasks forever, halting the CPU whenever there is nothing to do.
  [[noreturn]] void run_forever();

 private:
  struct Task {
    TaskFunction fn;
    void* context;
  };

  bool dispatch(const Task& task, hardware::CoreKind preferred);
  bool any_core_busy();

  hardware::ICentralProcessingUnit& cpu_;
  mutable SpinLock lock_;  // guards both queues
  Queue<Task> high_;
  Queue<Task> efficiency_;
};

}  // namespace scheduling
