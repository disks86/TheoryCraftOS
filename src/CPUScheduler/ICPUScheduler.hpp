#pragma once
#include <stdint.h>

namespace scheduling {

// Mirrors the design doc: high-performance work (rendering, compiling, ...)
// and efficiency work (background sync, ...) are queued separately.
enum class Queue : uint8_t { HighPerformance, Efficiency };

using TaskFunction = void (*)(void* context);

// Cooperative scheduling: a task runs until it returns. To "yield", a task
// submits its continuation and returns.
class ICPUScheduler {
 public:
  // Queues a task. Returns false if the queue is full or fn is null.
  virtual bool submit(TaskFunction fn, void* context,
                      Queue queue = Queue::HighPerformance) = 0;
  // Runs the next task, HighPerformance before Efficiency. False if none pending.
  virtual bool run_next() = 0;
  // Tasks waiting across both queues.
  virtual uint64_t pending() const = 0;

 protected:
  constexpr ICPUScheduler() = default;
  ~ICPUScheduler() = default;
};

}  // namespace scheduling
