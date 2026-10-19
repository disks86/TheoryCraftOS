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
#include "ObjectContainer/List.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe List: every operation takes a SpinLock, so cores may add, remove
// and read at the same time. Values are copied in and out - there are no
// references into the list, since another core could move or destroy the
// element the moment the lock is released.
//
// Single operations are atomic. A sequence (check the count, then insert) is not
// unless you hold the lock yourself through access() or with(). for_each() runs
// the callback under the lock: keep it short and do not call back into this list
// from it. T must be copyable (and movable, as for List).
//
// WARNING: Do not attempt to acquire the same lock recursively (e.g., calling 
// methods on the same ConcurrentList from within a callback passed to for_each())
// as this will cause deadlock.
template <class T>
class ConcurrentList : public Synchronized<List<T>> {
  using Base = Synchronized<List<T>>;

 public:
  ConcurrentList(allocators::IAllocator& allocator, const hardware::ICentralProcessingUnit& cpu)
      : Base(cpu, allocator) {}

  uint64_t count() const {
    return this->with([](const List<T>& l) { return l.count(); });
  }
  void clear() {
    this->with([](List<T>& l) { l.clear(); });
  }
  bool reserve(uint64_t capacity) {
    return this->with([&](List<T>& l) { return l.reserve(capacity); });
  }
  bool push_back(const T& value) {
    return this->with([&](List<T>& l) { return l.push_back(value); });
  }
  bool insert(uint64_t index, const T& value) {
    return this->with([&](List<T>& l) { return l.insert(index, value); });
  }
  bool remove_at(uint64_t index) {
    return this->with([&](List<T>& l) { return l.remove_at(index); });
  }
  // Copies the element at `index` into *out; false if the index is out of range.
  bool get(uint64_t index, T* out) const {
    return this->with([&](const List<T>& l) {
      const T* p = l.get(index);
      if (p) *out = *p;
      return p != nullptr;
    });
  }
  // Replaces the element at `index`.
  bool set(uint64_t index, const T& value) {
    return this->with([&](List<T>& l) {
      T* p = l.get(index);
      if (p) *p = value;
      return p != nullptr;
    });
  }
  // Calls visitor(element) for each element, in order, under the lock.
  // WARNING: Do not attempt to acquire the same lock recursively from within 
  // this visitor callback as this will cause deadlock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const List<T>& l) {
      for (const T& x : l) visitor(x);
    });
  }
};
