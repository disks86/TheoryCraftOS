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
#include "ObjectContainer/ContainerSupport.hpp"
#include "ObjectContainer/IObjectContainer.hpp"

// First in, first out. A ring buffer: enqueue and dequeue are amortized O(1) and
// never shift elements. When it fills, the elements are moved into a buffer of
// twice the size, so references into the queue do not survive an enqueue.
// Iteration goes from the front to the back: the order of dequeuing.
// T must be move-constructible and move-assignable. Every allocation goes
// through the IAllocator given.
template <class T>
class Queue final : public IObjectContainer<T> {
 public:
  explicit Queue(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Queue() {
    clear();
    allocator_.deallocate(data_);
  }

  uint64_t count() const override { return size_; }

  void clear() override {
    for (uint64_t i = 0; i < size_; ++i) slot(i)->~T();
    size_ = 0;
    head_ = 0;
  }

  bool reserve(uint64_t capacity) {
    if (capacity <= capacity_) return true;
    T* fresh = container_support::allocate_array<T>(allocator_, capacity);
    if (!fresh) return false;
    for (uint64_t i = 0; i < size_; ++i) {  // unwrap into logical order
      new (&fresh[i]) T(container_support::move(*slot(i)));
      slot(i)->~T();
    }
    allocator_.deallocate(data_);
    data_ = fresh;
    capacity_ = capacity;
    head_ = 0;
    return true;
  }

  bool enqueue(const T& value) { return emplace(value); }
  bool enqueue(T&& value) { return emplace(container_support::move(value)); }

  // The oldest / newest element, or nullptr when empty.
  T* front() { return size_ ? slot(0) : nullptr; }
  T* back() { return size_ ? slot(size_ - 1) : nullptr; }
  const T* front() const { return size_ ? slot(0) : nullptr; }
  const T* back() const { return size_ ? slot(size_ - 1) : nullptr; }

  // Removes the oldest element, moving it into *out first when out is given.
  bool dequeue(T* out = nullptr) {
    if (!size_) return false;
    T* oldest = slot(0);
    if (out) *out = container_support::move(*oldest);
    oldest->~T();
    head_ = (head_ + 1) % capacity_;
    --size_;
    return true;
  }

  // --- IObjectContainer iteration: front to back; `index` is the position from the front.
  bool first(ObjectCursor* cursor) const override {
    if (!size_) return false;
    cursor->index = 0;
    return true;
  }
  bool next(ObjectCursor* cursor) const override {
    if (cursor->index + 1 >= size_) return false;
    ++cursor->index;
    return true;
  }
  T* element(const ObjectCursor& cursor) const override { return slot(cursor.index); }

 private:
  T* slot(uint64_t logical) const { return &data_[(head_ + logical) % capacity_]; }

  template <class U>
  bool emplace(U&& value) {
    if (size_ == capacity_ && !reserve(capacity_ ? capacity_ * 2 : 8)) return false;
    new (slot(size_)) T(static_cast<U&&>(value));
    ++size_;
    return true;
  }

  allocators::IAllocator& allocator_;
  T* data_ = nullptr;
  uint64_t capacity_ = 0;
  uint64_t head_ = 0;  // index in data_ of the oldest element
  uint64_t size_ = 0;
};
