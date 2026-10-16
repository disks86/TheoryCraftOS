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
#include <new>
#include "Allocators/IAllocator.hpp"
#include "Utility/Move.hpp"

template <class T>
class SharedPointer;

// Sole owner of one object allocated from an IAllocator. The object is
// destroyed and its memory returned to that allocator when the pointer is
// destroyed, reset or assigned over. Move-only.
//
// Ownership can move to a pointer to a base class (UniquePointer<Base> from a
// UniquePointer<Derived>). The destructor that runs is always the concrete
// type's - the pointer remembers how it was created - so the base needs no
// virtual destructor (the interfaces in this project deliberately have none).
template <class T>
class UniquePointer {
 public:
  constexpr UniquePointer() = default;
  constexpr UniquePointer(decltype(nullptr)) {}
  ~UniquePointer() { reset(); }

  UniquePointer(const UniquePointer&) = delete;
  UniquePointer& operator=(const UniquePointer&) = delete;

  UniquePointer(UniquePointer&& other) { take(other); }
  UniquePointer& operator=(UniquePointer&& other) {
    if (this != &other) {
      reset();
      take(other);
    }
    return *this;
  }

  // From a pointer to a derived class.
  template <class U>
    requires requires(U* u) { [](T*) {}(u); }
  UniquePointer(UniquePointer<U>&& other) {
    take(other);
  }
  template <class U>
    requires requires(U* u) { [](T*) {}(u); }
  UniquePointer& operator=(UniquePointer<U>&& other) {
    reset();
    take(other);
    return *this;
  }

  // Constructs a T from `args` in memory from `allocator`. Null if it ran out.
  template <class... Args>
  static UniquePointer make(allocators::IAllocator& allocator, Args&&... args) {
    return make_as<T>(allocator, util::forward<Args>(args)...);
  }

  // Constructs a U (T or a class derived from it) and owns it as a T.
  template <class U, class... Args>
  static UniquePointer make_as(allocators::IAllocator& allocator, Args&&... args) {
    static_assert(alignof(U) <= 64, "type is over-aligned for the allocator");
    void* memory = allocator.allocate(sizeof(U));
    if (!memory) return UniquePointer();
    UniquePointer result;
    result.ptr_ = new (memory) U(util::forward<Args>(args)...);
    result.object_ = memory;
    result.destroy_ = &destroy<U>;
    result.allocator_ = &allocator;
    return result;
  }

  // Destroys the owned object (if any), leaving the pointer null.
  void reset() {
    if (ptr_) destroy_(allocator_, object_);
    ptr_ = nullptr;
    object_ = nullptr;
    destroy_ = nullptr;
    allocator_ = nullptr;
  }

  void swap(UniquePointer& other) {
    UniquePointer temp(util::move(other));
    other = util::move(*this);
    *this = util::move(temp);
  }

  T* get() const { return ptr_; }
  T& operator*() const { return *ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  bool operator==(decltype(nullptr)) const { return ptr_ == nullptr; }

 private:
  template <class U>
  friend class UniquePointer;
  template <class U>
  friend class SharedPointer;

  using Destroy = void (*)(allocators::IAllocator*, void*);

  template <class U>
  static void destroy(allocators::IAllocator* allocator, void* object) {
    static_cast<U*>(object)->~U();
    allocator->deallocate(object);
  }

  template <class U>
  void take(UniquePointer<U>& other) {
    ptr_ = other.ptr_;  // implicit derived-to-base conversion (adjusts the address if needed)
    object_ = other.object_;
    destroy_ = other.destroy_;
    allocator_ = other.allocator_;
    other.ptr_ = nullptr;
    other.object_ = nullptr;
    other.destroy_ = nullptr;
    other.allocator_ = nullptr;
  }

  T* ptr_ = nullptr;
  void* object_ = nullptr;  // what the allocator handed out (may differ from ptr_ for a base)
  Destroy destroy_ = nullptr;
  allocators::IAllocator* allocator_ = nullptr;
};
