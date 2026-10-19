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
#include "ObjectContainer/LinkedList.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe LinkedList: every operation takes a SpinLock. Values are copied in
// and out; see ConcurrentList for what is and is not atomic. For erase-while-
// walking use access() and the usual c
//
// WARNING: Do not attempt to acquire the same lock recursively - this can lead to deadlock if the underlying synchronization primitive does not support re-entrancy.ursor loop with the lock held.
//
// WARNING: Do not attempt to acquire the same lock recursively (e.g., calling 
// methods on the same ConcurrentLinkedList from within a callback passed to for_each())
// as this will cause deadlock.
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
  // Calls visitor(element) for each element, in order, under the lock.
  // WARNING: Do not attempt to acquire the same lock recursively from within 
  // this visitor callback as this will cause deadlock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const LinkedList<T>& l) {
      for (const T& x : l) visitor(x);
    });
  }
};
