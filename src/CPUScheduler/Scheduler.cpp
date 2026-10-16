#include "CPUScheduler/Scheduler.hpp"

namespace scheduling {

void Scheduler::lock() const {
  while (__atomic_test_and_set(&lock_, __ATOMIC_ACQUIRE)) cpu_.pause();
}

void Scheduler::unlock() const { __atomic_clear(&lock_, __ATOMIC_RELEASE); }

bool Scheduler::submit(TaskFunction fn, void* context, Queue queue) {
  if (!fn) return false;
  Fifo& q = queue == Queue::HighPerformance ? high_ : efficiency_;
  lock();
  bool ok = q.count < kQueueCapacity;
  if (ok) {
    q.tasks[(q.head + q.count) % kQueueCapacity] = {fn, context};
    ++q.count;
  }
  unlock();
  return ok;
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
  lock();
  const bool high = high_.count != 0;
  Fifo& q = high ? high_ : efficiency_;
  if (!q.count) {
    unlock();
    return false;
  }
  Task task = q.tasks[q.head];
  q.head = (q.head + 1) % kQueueCapacity;
  --q.count;
  unlock();

  const hardware::CoreKind preferred =
      high ? hardware::CoreKind::Performance : hardware::CoreKind::Efficiency;
  if (!dispatch(task, preferred)) task.fn(task.context);  // every other core is busy
  return true;
}

uint64_t Scheduler::pending() const {
  lock();
  uint64_t n = high_.count + efficiency_.count;
  unlock();
  return n;
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
