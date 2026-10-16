#pragma once
#include "ObjectContainer/Comparison.hpp"
#include "ObjectContainer/List.hpp"

// A queue that always hands out its highest-priority element next, kept in a
// binary max-heap: push and pop take O(log n), top is O(1). Priority comes from
// the "less" functor: less(a, b) true means a has LOWER priority than b, so with
// the default operator< the largest value is served first (pass a greater-than
// functor for smallest-first). Elements of equal priority come out in no
// particular order.
//
// Iteration visits the elements in heap order, which is NOT sorted - only the
// first one is guaranteed to be the top. Pop them to get priority order.
// T must be move-constructible and move-assignable. Every allocation goes
// through the IAllocator given.
template <class T, class Less = DefaultLess<T>>
class PriorityQueue final : public IObjectContainer<T> {
 public:
  explicit PriorityQueue(allocators::IAllocator& allocator, Less less = Less())
      : items_(allocator), less_(less) {}

  uint64_t count() const override { return items_.count(); }
  void clear() override { items_.clear(); }

  bool reserve(uint64_t capacity) { return items_.reserve(capacity); }

  bool push(const T& value) { return add(value); }
  bool push(T&& value) { return add(container_support::move(value)); }

  // The highest-priority element, or nullptr when empty.
  const T* top() const { return items_.get(0); }

  // Removes the highest-priority element, moving it into *out first when out is given.
  bool pop(T* out = nullptr) {
    const uint64_t n = items_.count();
    if (n == 0) return false;
    if (out) *out = container_support::move(items_[0]);
    if (n > 1) items_[0] = container_support::move(items_[n - 1]);
    items_.remove_at(n - 1);
    if (n > 2) sift_down(0);
    return true;
  }

  // --- IObjectContainer iteration: heap order; `index` is the heap slot.
  bool first(ObjectCursor* cursor) const override {
    if (items_.empty()) return false;
    cursor->index = 0;
    return true;
  }
  bool next(ObjectCursor* cursor) const override {
    if (cursor->index + 1 >= items_.count()) return false;
    ++cursor->index;
    return true;
  }
  T* element(const ObjectCursor& cursor) const override {
    return const_cast<T*>(items_.get(cursor.index));
  }

 private:
  template <class U>
  bool add(U&& value) {
    if (!items_.push_back(static_cast<U&&>(value))) return false;
    // Sift the new last element up past every lower-priority parent.
    uint64_t child = items_.count() - 1;
    while (child > 0) {
      const uint64_t parent = (child - 1) / 2;
      if (!less_(items_[parent], items_[child])) break;
      swap(parent, child);
      child = parent;
    }
    return true;
  }

  void sift_down(uint64_t parent) {
    const uint64_t n = items_.count();
    for (;;) {
      uint64_t best = parent;
      const uint64_t left = 2 * parent + 1, right = left + 1;
      if (left < n && less_(items_[best], items_[left])) best = left;
      if (right < n && less_(items_[best], items_[right])) best = right;
      if (best == parent) return;
      swap(parent, best);
      parent = best;
    }
  }

  void swap(uint64_t a, uint64_t b) {
    T temp(container_support::move(items_[a]));
    items_[a] = container_support::move(items_[b]);
    items_[b] = container_support::move(temp);
  }

  List<T> items_;
  Less less_;
};
