#pragma once
#include "ObjectContainer/Queue.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe Queue (first in, first out): any number of producers and
// consumers. Operations never wait: dequeue() returns false when the queue is
// empty. See ConcurrentList for what is and is not atomic.
template <class T>
class ConcurrentQueue : public Synchronized<Queue<T>> {
  using Base = Synchronized<Queue<T>>;

 public:
  ConcurrentQueue(allocators::IAllocator& allocator, const hardware::ICentralProcessingUnit& cpu)
      : Base(cpu, allocator) {}

  uint64_t count() const {
    return this->with([](const Queue<T>& q) { return q.count(); });
  }
  void clear() {
    this->with([](Queue<T>& q) { q.clear(); });
  }
  bool enqueue(const T& value) {
    return this->with([&](Queue<T>& q) { return q.enqueue(value); });
  }
  // Removes the oldest element, moving it into *out when out is given.
  bool dequeue(T* out = nullptr) {
    return this->with([&](Queue<T>& q) { return q.dequeue(out); });
  }
  // Copies the oldest / newest element into *out; false when empty.
  bool front(T* out) const {
    return this->with([&](const Queue<T>& q) {
      const T* p = q.front();
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  bool back(T* out) const {
    return this->with([&](const Queue<T>& q) {
      const T* p = q.back();
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  // Front to back, under the lock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const Queue<T>& q) {
      for (const T& x : q) visitor(x);
    });
  }
};
