#pragma once
#include "ObjectContainer/LinkedList.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe LinkedList: every operation takes a SpinLock. Values are copied in
// and out; see ConcurrentList for what is and is not atomic. For erase-while-
// walking use access() and the usual cursor loop with the lock held.
template <class T>
class ConcurrentLinkedList : public Synchronized<LinkedList<T>> {
  using Base = Synchronized<LinkedList<T>>;

 public:
  ConcurrentLinkedList(allocators::IAllocator& allocator, const hardware::ICentralProcessingUnit& cpu)
      : Base(cpu, allocator) {}

  uint64_t count() const {
    return this->with([](const LinkedList<T>& l) { return l.count(); });
  }
  void clear() {
    this->with([](LinkedList<T>& l) { l.clear(); });
  }
  bool push_back(const T& value) {
    return this->with([&](LinkedList<T>& l) { return l.push_back(value); });
  }
  bool push_front(const T& value) {
    return this->with([&](LinkedList<T>& l) { return l.push_front(value); });
  }
  // Removes the first / last element, copying it into *out when out is given.
  bool pop_front(T* out = nullptr) {
    return this->with([&](LinkedList<T>& l) {
      T* p = l.front();
      if (!p) return false;
      if (out) *out = *p;
      return l.pop_front();
    });
  }
  bool pop_back(T* out = nullptr) {
    return this->with([&](LinkedList<T>& l) {
      T* p = l.back();
      if (!p) return false;
      if (out) *out = *p;
      return l.pop_back();
    });
  }
  // Copies the first / last element into *out; false when empty.
  bool front(T* out) const {
    return this->with([&](const LinkedList<T>& l) {
      const T* p = l.front();
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  bool back(T* out) const {
    return this->with([&](const LinkedList<T>& l) {
      const T* p = l.back();
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const LinkedList<T>& l) {
      for (const T& x : l) visitor(x);
    });
  }
};
