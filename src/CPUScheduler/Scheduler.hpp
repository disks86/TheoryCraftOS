#pragma once
#include "Allocators/IAllocator.hpp"
#include "CPUScheduler/ICPUScheduler.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Synchronization/SpinLock.hpp"
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

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
