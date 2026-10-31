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
    : cpu_(cpu), lock_(cpu), high_(allocator), efficiency_(allocator), allocator_(allocator) {
  high_.reserve(kInitialCapacity);  // best effort: the queues also grow on demand
  efficiency_.reserve(kInitialCapacity);
}

bool Scheduler::submit(TaskFunction fn, void* context, TaskQueue queue) {
  if (!fn) return false;
  ScopedLock guard(lock_);
  return (queue == TaskQueue::HighPerformance ? high_ : efficiency_).enqueue(Task{fn, context});
}

struct Scheduler::Job {
  Scheduler* scheduler;
  TaskFunction fn;
  void* context;
};

void Scheduler::job_main(void* argument) {
  Job* job = static_cast<Job*>(argument);
  const Job copy = *job;
  copy.scheduler->allocator_.deallocate(job);
  copy.fn(copy.context);
  __atomic_sub_fetch(&copy.scheduler->active_, 1, __ATOMIC_SEQ_CST);
}

bool Scheduler::run_next() {
  Task task;
  bool high;
  {
    ScopedLock guard(lock_);
    high = !high_.empty();
    if (!(high ? high_ : efficiency_).dequeue(&task)) return false;
  }

  if (kernel::running()) {
    Job* job = static_cast<Job*>(allocator_.allocate(sizeof(Job)));
    if (job) {
      *job = {this, task.fn, task.context};
      __atomic_add_fetch(&active_, 1, __ATOMIC_SEQ_CST);
      const hardware::CoreKind preferred = high ? hardware::CoreKind::Performance : hardware::CoreKind::Efficiency;
      kernel::Thread* thread = kernel::spawn(job_main, job, "task", -1, static_cast<int8_t>(preferred));
      if (thread) {
        kernel::detach(thread);
        return true;
      }
      __atomic_sub_fetch(&active_, 1, __ATOMIC_SEQ_CST);
      allocator_.deallocate(job);
    }
  }
  task.fn(task.context);  // no threads (yet) or no memory for one: run it here
  return true;
}

uint64_t Scheduler::pending() const {
  ScopedLock guard(lock_);
  return high_.count() + efficiency_.count();
}

void Scheduler::run() {
  // Running tasks may submit more work, so only stop once nothing is queued
  // and no task thread is still executing.
  for (;;) {
    if (run_next()) continue;
    if (__atomic_load_n(&active_, __ATOMIC_SEQ_CST) == 0) {
      // Everything has run; give the finished threads' stacks back before returning.
      if (kernel::running()) while (kernel::reap_zombies() != 0) kernel::yield();
      break;
    }
    kernel::yield();
    cpu_.pause();
  }
}

void Scheduler::run_forever() {
  for (;;) {
    if (!run_next()) kernel::sleep_us(1000);
  }
}

}  // namespace scheduling
