#pragma once
#include "ObjectContainer/PriorityQueue.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe PriorityQueue: any number of producers and consumers; pop() always
// removes the highest-priority element present at that moment and returns false
// when empty (it never waits). See ConcurrentList for what is and is not atomic.
template <class T, class Less = DefaultLess<T>>
class ConcurrentPriorityQueue : public Synchronized<PriorityQueue<T, Less>> {
  using Inner = PriorityQueue<T, Less>;
  using Base = Synchronized<Inner>;

 public:
  ConcurrentPriorityQueue(allocators::IAllocator& allocator, const hardware::ICentralProcessingUnit& cpu,
                          Less less = Less())
      : Base(cpu, allocator, less) {}

  uint64_t count() const {
    return this->with([](const Inner& q) { return q.count(); });
  }
  void clear() {
    this->with([](Inner& q) { q.clear(); });
  }
  bool push(const T& value) {
    return this->with([&](Inner& q) { return q.push(value); });
  }
  bool pop(T* out = nullptr) {
    return this->with([&](Inner& q) { return q.pop(out); });
  }
  // Copies the highest-priority element into *out; false when empty.
  bool top(T* out) const {
    return this->with([&](const Inner& q) {
      const T* p = q.top();
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  // In heap order (not sorted), under the lock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const Inner& q) {
      for (const T& x : q) visitor(x);
    });
  }
};
