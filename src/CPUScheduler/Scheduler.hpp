#pragma once
#include "Allocators/IAllocator.hpp"
#include "CPUScheduler/ICPUScheduler.hpp"
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

namespace scheduling {

// Default scheduler: a cooperative, run-to-completion scheduler for one CPU
// with fixed-size FIFO queues (no heap). Strict priority: the efficiency queue
// only runs when the high-performance queue is empty. The allocator is held for
// future use (e.g. growable queues, task stacks); nothing is allocated yet.
class Scheduler final : public ICPUScheduler {
 public:
  static constexpr uint32_t kQueueCapacity = 64;

  constexpr Scheduler(hardware::ICentralProcessingUnit& cpu, allocators::IAllocator& allocator)
      : cpu_(cpu), allocator_(allocator) {}

  bool submit(TaskFunction fn, void* context, Queue queue = Queue::HighPerformance) override;
  bool run_next() override;
  uint64_t pending() const override;

  // Runs tasks until both queues are empty.
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

  hardware::ICentralProcessingUnit& cpu_;
  allocators::IAllocator& allocator_;
  Fifo high_;
  Fifo efficiency_;
};

}  // namespace scheduling
