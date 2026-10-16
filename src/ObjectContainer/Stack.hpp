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

// Last in, first out. Built on List, so push and pop take amortized O(1) and the
// elements live in one array; references into the stack do not survive a push.
// Iteration goes from the top down: the order the elements would be popped in.
// T must be move-constructible and move-assignable. Every allocation goes
// through the IAllocator given.
template <class T>
class Stack final : public IObjectContainer<T> {
 public:
  explicit Stack(allocators::IAllocator& allocator) : items_(allocator) {}

  uint64_t count() const override { return items_.count(); }
  void clear() override { items_.clear(); }

  bool reserve(uint64_t capacity) { return items_.reserve(capacity); }
  bool push(const T& value) { return items_.push_back(value); }
  bool push(T&& value) { return items_.push_back(container_support::move(value)); }

  // The top element, or nullptr when empty.
  T* top() { return items_.get(items_.count() - 1); }
  const T* top() const { return items_.get(items_.count() - 1); }

  // Removes the top element, moving it into *out first when out is given.
  bool pop(T* out = nullptr) {
    T* t = top();
    if (!t) return false;
    if (out) *out = container_support::move(*t);
    return items_.remove_at(items_.count() - 1);
  }

  // --- IObjectContainer iteration: top to bottom; `index` is the element's slot.
  bool first(ObjectCursor* cursor) const override {
    if (items_.empty()) return false;
    cursor->index = items_.count() - 1;
    return true;
  }
  bool next(ObjectCursor* cursor) const override {
    if (cursor->index == 0) return false;
    --cursor->index;
    return true;
  }
  T* element(const ObjectCursor& cursor) const override {
    return const_cast<T*>(items_.get(cursor.index));
  }

 private:
  List<T> items_;
};
