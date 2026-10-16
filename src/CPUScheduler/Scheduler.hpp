#pragma once
#include "Allocators/IAllocator.hpp"
#include "CPUScheduler/ICPUScheduler.hpp"
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

namespace scheduling {

// Default scheduler: cooperative and run-to-completion, spread across every
// core the CPU interface reports. Fixed-size FIFO queues (no heap). Strict
// priority: the efficiency queue only starts when the high-performance queue is
// empty. High-performance tasks prefer Performance cores and efficiency tasks
// prefer Efficiency cores, falling back to any free core, then to this core.
// Call run()/run_next() from one core; submit() may be called from any. The allocator is held for
// future use (e.g. growable queues, task stacks); nothing is allocated yet.
class Scheduler final : public ICPUScheduler {
 public:
  static constexpr uint32_t kQueueCapacity = 64;

  constexpr Scheduler(hardware::ICentralProcessingUnit& cpu, allocators::IAllocator& allocator)
      : cpu_(cpu), allocator_(allocator) {}

  bool submit(TaskFunction fn, void* context, Queue queue = Queue::HighPerformance) override;
  bool run_next() override;
  uint64_t pending() const override;

  // Runs tasks until both queues are empty and every started task has finished.
  void run();
  // Runs tasks forever, halting the CPU whenever there is nothing to do.
  [[noreturn]] void run_forever();

 private:
  struct Task {
    TaskFunction fn;
    void* context;
  };
  struct Fifo {
    Task tasks[kQueueCapacity];
    uint32_t head = 0;
    uint32_t count = 0;
  };

  void lock() const;
  void unlock() const;
  bool dispatch(const Task& task, hardware::CoreKind preferred);
  bool any_core_busy();

  hardware::ICentralProcessingUnit& cpu_;
  allocators::IAllocator& allocator_;
  mutable volatile uint32_t lock_ = 0;  // guards both queues
  Fifo high_;
  Fifo efficiency_;
};

}  // namespace scheduling
