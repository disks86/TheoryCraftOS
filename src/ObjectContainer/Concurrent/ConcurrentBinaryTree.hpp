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
#include "ObjectContainer/BinaryTree.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe BinaryTree (ordered set): every operation takes a SpinLock and
// values are copied in and out. See ConcurrentList for what is and is not atomic.
//
// WARNING: Do not attempt to acquire the same lock recursively - this can lead to deadlock if the underlying synchronization primitive does not support re-entrancy. (e.g., calling 
// methods on the same ConcurrentBinaryTree from within a callback passed to for_each())
// as this will cause deadlock.
// 
// WARNING: Do not attempt to acquire the same lock recursively from within 
// any visitor callback passed to for_each() as this will cause deadlock.
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
  // WARNING: Do not attempt to acquire the same lock recursively from within 
  // this visitor callback as this will cause deadlock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const Inner& t) {
      for (const T& x : t) visitor(x);
    });
  }
};
