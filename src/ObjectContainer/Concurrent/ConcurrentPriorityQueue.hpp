/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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
