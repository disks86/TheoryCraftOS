#pragma once
#include "ObjectContainer/BinaryTree.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe BinaryTree (ordered set): every operation takes a SpinLock and
// values are copied in and out. See ConcurrentList for what is and is not atomic.
template <class T, class Compare = DefaultLess<T>>
class ConcurrentBinaryTree : public Synchronized<BinaryTree<T, Compare>> {
  using Inner = BinaryTree<T, Compare>;
  using Base = Synchronized<Inner>;

 public:
  using InsertResult = typename Inner::InsertResult;

  ConcurrentBinaryTree(allocators::IAllocator& allocator, const hardware::ICentralProcessingUnit& cpu,
                       Compare compare = Compare())
      : Base(cpu, allocator, compare) {}

  uint64_t count() const {
    return this->with([](const Inner& t) { return t.count(); });
  }
  void clear() {
    this->with([](Inner& t) { t.clear(); });
  }
  InsertResult insert(const T& value) {
    return this->with([&](Inner& t) { return t.insert(value); });
  }
  bool remove(const T& value) {
    return this->with([&](Inner& t) { return t.remove(value); });
  }
  bool contains(const T& value) const {
    return this->with([&](const Inner& t) { return t.contains(value); });
  }
  // Copies the stored value equivalent to `value` into *out.
  bool find(const T& value, T* out) const {
    return this->with([&](const Inner& t) {
      const T* p = t.find(value);
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  // Copies the smallest / largest value into *out; false when empty.
  bool min(T* out) const {
    return this->with([&](const Inner& t) {
      const T* p = t.min();
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  bool max(T* out) const {
    return this->with([&](const Inner& t) {
      const T* p = t.max();
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  uint32_t height() const {
    return this->with([](const Inner& t) { return t.height(); });
  }
  // In-order (ascending), under the lock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const Inner& t) {
      for (const T& x : t) visitor(x);
    });
  }
};
