#include "CPUScheduler/Scheduler.hpp"

namespace scheduling {

bool Scheduler::submit(TaskFunction fn, void* context, Queue queue) {
  if (!fn) return false;
  Fifo& q = queue == Queue::HighPerformance ? high_ : efficiency_;
  if (q.count == kQueueCapacity) return false;
  q.tasks[(q.head + q.count) % kQueueCapacity] = {fn, context};
  ++q.count;
  return true;
}

bool Scheduler::run_next() {
  Fifo& q = high_.count ? high_ : efficiency_;
  if (!q.count) return false;
  Task task = q.tasks[q.head];
  q.head = (q.head + 1) % kQueueCapacity;
  --q.count;
  task.fn(task.context);  // may submit() more work, including itself
  return true;
}

uint64_t Scheduler::pending() const { return high_.count + efficiency_.count; }

void Scheduler::run() {
  while (run_next()) {}
}

void Scheduler::run_forever() {
  for (;;) {
    if (!run_next()) cpu_.halt();
  }
}

}  // namespace scheduling
