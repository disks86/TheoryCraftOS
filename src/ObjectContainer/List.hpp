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
#include "Utility/ArrayView.hpp"

// A growable array: elements sit next to each other in one allocation, so
// indexing is O(1) and appending is amortized O(1). Growth doubles the
// capacity: the elements are moved into a new allocation and the old one is
// freed, so references and pointers into the list do not survive an insertion.
// T must be move-constructible and move-assignable. Every allocation goes
// through the IAllocator given.
template <class T>
class List final : public IObjectContainer<T> {
 public:
  explicit List(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~List() {
    clear();
    if (storage_.data()) {
      allocator_.deallocate(storage_.data());
    }
  }

  uint64_t count() const override { return size_; }
  uint64_t capacity() const { return storage_.size(); }

  // Destroys the elements; the memory is kept for reuse.
  void clear() override {
    for (uint64_t i = 0; i < size_; ++i) storage_[i].~T();
    size_ = 0;
  }

  // Makes room for at least `capacity` elements. False if memory ran out.
  bool reserve(uint64_t capacity) {
    if (capacity <= storage_.size()) return true;
    T* fresh = container_support::allocate_array<T>(allocator_, capacity);
    if (!fresh) return false;
    
    // Move existing elements to new allocation
    for (uint64_t i = 0; i < size_; ++i) {
      new (&fresh[i]) T(container_support::move(storage_[i]));
      storage_[i].~T();
    }
    
    // Deallocate old memory, but only if it's not null
    if (storage_.data()) {
      allocator_.deallocate(storage_.data());
    }
    storage_ = ArrayView<T>(fresh, capacity);
    return true;
  }

  bool push_back(const T& value) { return insert(size_, value); }
  bool push_back(T&& value) { return insert(size_, container_support::move(value)); }

  // Inserts before `index` (index == count() appends), shifting later elements up.
  bool insert(uint64_t index, const T& value) { return emplace_at(index, value); }
  bool insert(uint64_t index, T&& value) { return emplace_at(index, container_support::move(value)); }

  // Removes the element at `index`, shifting later elements down.
  bool remove_at(uint64_t index) {
    if (index >= size_) return false;
    
    // Move elements to fill the gap
    for (uint64_t i = index; i + 1 < size_; ++i) {
      storage_[i] = container_support::move(storage_[i + 1]);
    }
    
    // Destroy the last element 
    storage_[size_ - 1].~T();
    --size_;
    return true;
  }

  // Bounds-checked access; nullptr when out of range.
  T* get(uint64_t index) { return index < size_ ? &storage_[index] : nullptr; }
  const T* get(uint64_t index) const { return index < size_ ? &storage_[index] : nullptr; }
  
  // Checked access: an index at or above count() stops the OS (bounds_violation)
  // rather than touch memory that holds no element. Use get() if it may be out of range.
  T& operator[](uint64_t index) {
    if (index >= size_) bounds_violation("List::operator[]", index, size_);
    return storage_[index];
  }
  const T& operator[](uint64_t index) const {
    if (index >= size_) bounds_violation("List::operator[]", index, size_);
    return storage_[index];
  }
  
  // The elements as a view (valid until the list grows or shrinks).
  ArrayView<T> view() { return storage_.first(size_); }
  ArrayView<const T> view() const { return ArrayView<const T>(storage_).first(size_); }

  // --- IObjectContainer iteration: the cursor's `index` is the element's index.
  bool first(ObjectCursor* cursor) const override {
    if (size_ == 0) return false;
    cursor->index = 0;
    return true;
  }
  bool next(ObjectCursor* cursor) const override {
    if (cursor->index + 1 >= size_) return false;
    ++cursor->index;
    return true;
  }
  T* element(const ObjectCursor& cursor) const override { return &storage_[cursor.index]; }

 private:
  template <class U>
  bool emplace_at(uint64_t index, U&& value) {
    if (index > size_) return false;
    
    // Check for potential double allocation when growing
    if (size_ == storage_.size() && !reserve(storage_.size() ? storage_.size() * 2 : 8)) 
      return false;
      
    // Open a gap: the last element moves into raw memory, the rest assign upward.
    if (index < size_) {
      new (&storage_[size_]) T(container_support::move(storage_[size_ - 1]));
      for (uint64_t i = size_ - 1; i > index; --i) 
        storage_[i] = container_support::move(storage_[i - 1]);
      storage_[index] = static_cast<U&&>(value);
    } else {
      new (&storage_[index]) T(static_cast<U&&>(value));
    }
    ++size_;
    return true;
  }

  allocators::IAllocator& allocator_;
  ArrayView<T> storage_;  // the allocation (the first size_ elements are alive); every index is checked against it
  uint64_t size_ = 0;
};
