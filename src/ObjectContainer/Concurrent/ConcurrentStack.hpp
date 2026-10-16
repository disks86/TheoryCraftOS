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
#include "ObjectContainer/Stack.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe Stack (last in, first out). Operations never wait: pop() returns
// false when the stack is empty, so a consumer polls or is told by other means
// that work has arrived. See ConcurrentList for what is and is not atomic.
template <class T>
class ConcurrentStack : public Synchronized<Stack<T>> {
  using Base = Synchronized<Stack<T>>;

 public:
  ConcurrentStack(allocators::IAllocator& allocator, const hardware::ICentralProcessingUnit& cpu)
      : Base(cpu, allocator) {}

  uint64_t count() const {
    return this->with([](const Stack<T>& s) { return s.count(); });
  }
  void clear() {
    this->with([](Stack<T>& s) { s.clear(); });
  }
  bool push(const T& value) {
    return this->with([&](Stack<T>& s) { return s.push(value); });
  }
  // Removes the top element, moving it into *out when out is given.
  bool pop(T* out = nullptr) {
    return this->with([&](Stack<T>& s) { return s.pop(out); });
  }
  // Copies the top element into *out; false when empty.
  bool top(T* out) const {
    return this->with([&](const Stack<T>& s) {
      const T* p = s.top();
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  // Top to bottom, under the lock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const Stack<T>& s) {
      for (const T& x : s) visitor(x);
    });
  }
};
