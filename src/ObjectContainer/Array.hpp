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
#include "ObjectContainer/IObjectContainer.hpp"
#include "Utility/ArrayView.hpp"

// An array whose length is part of its type: Array<int, 8> always holds exactly
// eight ints, stored inside the object itself - no allocator, no allocation, so
// it can live on the stack, in a static, or inside another object. All N elements
// exist from construction (value-initialized: zero for numbers, the default
// constructor for classes). count() is always N; clear() puts every element back
// to T{} rather than removing it. Indexing is checked like List and ArrayView.
//
//   Array<uint32_t, 4> lanes;
//   lanes.fill(7);
//   lanes[2] = 9;
//   for (uint32_t& lane : lanes) ...          // range-for, like every container
//   ArrayView<uint32_t> v = lanes.view();     // hand it to code that takes a view
//
// T must be default-constructible and assignable. Like the other containers it
// cannot be copied (the IObjectContainer base forbids it); pass a view or a reference.
template <class T, uint64_t N>
class Array final : public IObjectContainer<T> {
 public:
  static constexpr uint64_t kSize = N;

  constexpr Array() = default;

  uint64_t count() const override { return N; }
  // Resets every element to T{}; the length does not change.
  void clear() override {
    for (uint64_t i = 0; i < N; ++i) items_[i] = T{};
  }

  // Sets every element to `value`.
  void fill(const T& value) {
    for (uint64_t i = 0; i < N; ++i) items_[i] = value;
  }

  // Checked access: an index at or above N stops the OS (bounds_violation).
  T& operator[](uint64_t index) {
    if (index >= N) bounds_violation("Array::operator[]", index, N);
    return items_[index];
  }
  const T& operator[](uint64_t index) const {
    if (index >= N) bounds_violation("Array::operator[]", index, N);
    return items_[index];
  }
  // Unchecked-by-stopping access: nullptr when the index is out of range.
  T* get(uint64_t index) { return index < N ? &items_[index] : nullptr; }
  const T* get(uint64_t index) const { return index < N ? &items_[index] : nullptr; }

  T& front() { return items_[0]; }
  const T& front() const { return items_[0]; }
  T& back() { return items_[N - 1]; }
  const T& back() const { return items_[N - 1]; }

  // The elements as a view; valid for the life of the array.
  ArrayView<T> view() { return ArrayView<T>(items_, N); }
  ArrayView<const T> view() const { return ArrayView<const T>(items_, N); }
  T* data() { return items_; }
  const T* data() const { return items_; }

  // --- IObjectContainer iteration: the cursor's `index` is the element's index.
  bool first(ObjectCursor* cursor) const override {
    if (N == 0) return false;
    cursor->index = 0;
    return true;
  }
  bool next(ObjectCursor* cursor) const override {
    if (cursor->index + 1 >= N) return false;
    ++cursor->index;
    return true;
  }
  T* element(const ObjectCursor& cursor) const override { return const_cast<T*>(&items_[cursor.index]); }

 private:
  T items_[N ? N : 1] = {};
};
