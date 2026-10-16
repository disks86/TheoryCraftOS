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

#include "CPUScheduler/Scheduler.hpp"

namespace scheduling {

Scheduler::Scheduler(hardware::ICentralProcessingUnit& cpu, allocators::IAllocator& allocator)
    : cpu_(cpu), lock_(cpu), high_(allocator), efficiency_(allocator) {
  high_.reserve(kInitialCapacity);  // best effort: the queues also grow on demand
  efficiency_.reserve(kInitialCapacity);
}

bool Scheduler::submit(TaskFunction fn, void* context, TaskQueue queue) {
  if (!fn) return false;
  ScopedLock guard(lock_);
  return (queue == TaskQueue::HighPerformance ? high_ : efficiency_).enqueue(Task{fn, context});
}

bool Scheduler::dispatch(const Task& task, hardware::CoreKind preferred) {
  const uint32_t self = cpu_.current_core();
  const uint32_t cores = cpu_.core_count();
  uint32_t fallback = cores;  // first free core of the other kind
  for (uint32_t core = 0; core < cores; ++core) {
    if (core == self || cpu_.core_busy(core)) continue;
    if (cpu_.core_kind(core) == preferred) {
      if (cpu_.start_on_core(core, task.fn, task.context)) return true;
    } else if (fallback == cores) {
      fallback = core;
    }
  }
  return fallback != cores && cpu_.start_on_core(fallback, task.fn, task.context);
}

bool Scheduler::run_next() {
  Task task;
  bool high;
  {
    ScopedLock guard(lock_);
    high = !high_.empty();
    if (!(high ? high_ : efficiency_).dequeue(&task)) return false;
  }

  const hardware::CoreKind preferred =
      high ? hardware::CoreKind::Performance : hardware::CoreKind::Efficiency;
  if (!dispatch(task, preferred)) task.fn(task.context);  // every other core is busy
  return true;
}

uint64_t Scheduler::pending() const {
  ScopedLock guard(lock_);
  return high_.count() + efficiency_.count();
}

bool Scheduler::any_core_busy() {
  const uint32_t self = cpu_.current_core();
  for (uint32_t core = 0; core < cpu_.core_count(); ++core)
    if (core != self && cpu_.core_busy(core)) return true;
  return false;
}

void Scheduler::run() {
  // Running tasks may submit more work, so only stop once nothing is queued
  // and nothing is still executing elsewhere.
  for (;;) {
    if (run_next()) continue;
    if (!any_core_busy()) break;
    cpu_.pause();
  }
}

void Scheduler::run_forever() {
  for (;;) {
    if (!run_next()) cpu_.halt();
  }
}

}  // namespace scheduling
