#include "ConcurrentDemo.hpp"
#include "Allocators/ThreadSafeAllocator.hpp"
#include "ObjectContainer/Concurrent/ConcurrentDictionary.hpp"
#include "ObjectContainer/Concurrent/ConcurrentList.hpp"
#include "ObjectContainer/Concurrent/ConcurrentQueue.hpp"
#include "console.hpp"

namespace {
void report(const char* what, bool ok) {
  console::puts("  ");
  console::puts(what);
  console::puts(ok ? ": ok\n" : ": FAILED\n");
}

constexpr uint32_t kTasks = 16;
constexpr uint32_t kItemsPerTask = 200;
constexpr uint32_t kCounterKey = 0xFFFFFFFF;

struct Shared {
  ConcurrentQueue<uint32_t>* queue;
  ConcurrentDictionary<uint32_t, uint32_t>* dictionary;
  ConcurrentList<uint32_t>* list;
};
struct Job {
  Shared* shared;
  uint32_t index;
  volatile uint32_t failures;  // allocation failures seen by this task
};

// Runs on whichever core picks it up. It must not touch firmware services.
void fill_task(void* context) {
  auto* job = static_cast<Job*>(context);
  for (uint32_t i = 0; i < kItemsPerTask; ++i) {
    const uint32_t value = job->index * kItemsPerTask + i;
    bool ok = job->shared->queue->enqueue(value);
    ok &= job->shared->dictionary->put(value, value * 2);
    ok &= job->shared->list->push_back(value);
    ok &= job->shared->dictionary->update(kCounterKey, [](uint32_t& n) { ++n; });  // atomic read-modify-write
    if (!ok) job->failures = job->failures + 1;
  }
}
}  // namespace

bool run_concurrent_demo(allocators::Allocator& allocator, hardware::ICentralProcessingUnit& cpu,
                         scheduling::ICPUScheduler& scheduler) {
  const uint64_t free_before = allocator.free_bytes();
  bool all_ok = true;
  {
    allocators::ThreadSafeAllocator safe(allocator, cpu);
    ConcurrentQueue<uint32_t> queue(safe, cpu);
    ConcurrentDictionary<uint32_t, uint32_t> dictionary(safe, cpu);
    ConcurrentList<uint32_t> list(safe, cpu);
    dictionary.put(kCounterKey, 0);

    Shared shared{&queue, &dictionary, &list};
    Job jobs[kTasks];
    bool ok = true;
    for (uint32_t t = 0; t < kTasks; ++t) {
      jobs[t] = {&shared, t, 0};
      ok &= scheduler.submit(fill_task, &jobs[t]);
    }
    scheduler.run();  // all cores run fill_task concurrently
    uint32_t failures = 0;
    for (const Job& job : jobs) failures += job.failures;
    ok &= failures == 0;
    report("16 tasks filling queue, dictionary and list at once", ok);
    all_ok &= ok;

    constexpr uint32_t kTotal = kTasks * kItemsPerTask;
    uint64_t expected_sum = 0;
    for (uint32_t v = 0; v < kTotal; ++v) expected_sum += v;

    ok = queue.count() == kTotal && list.count() == kTotal && dictionary.count() == kTotal + 1;
    uint32_t counter = 0;
    ok &= dictionary.find(kCounterKey, &counter) && counter == kTotal;  // no lost updates
    uint64_t queue_sum = 0, dequeued = 0;
    uint32_t value;
    while (queue.dequeue(&value)) {
      queue_sum += value;
      ++dequeued;
    }
    uint64_t list_sum = 0;
    list.for_each([&](uint32_t v) { list_sum += v; });
    uint32_t doubled = 0;
    ok &= dequeued == kTotal && queue_sum == expected_sum && list_sum == expected_sum;
    ok &= dictionary.find(1234, &doubled) && doubled == 2468;
    report("nothing lost or duplicated (counts, sums, atomic counter)", ok);
    all_ok &= ok;
  }
  const bool freed = allocator.free_bytes() == free_before;
  report("no memory left allocated", freed);
  return all_ok && freed;
}
