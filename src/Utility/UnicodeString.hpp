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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "UnicodeStringView.hpp"
#include "Utility/Bounds.hpp"

// An owned, growable Unicode string: the characters live in one allocation from the
// IAllocator given, always followed by a NUL so c_str() can be handed to code that
// wants a C string. Move-only. Anything that can need memory returns false when
// the allocator is out of it, leaving the string unchanged - there are no
// exceptions - and every character access is checked.
//
//   UnicodeString path(allocator);
//   if (!path.append(U"example")) { /* handle allocation failure */ }
//   const char16_t* c_string = path.c_str();
//
class UnicodeString {
 public:
  // Create an empty string with no characters, using the given allocator.
  explicit UnicodeString(IAllocator& allocator);

  // Move constructor and assignment.
  UnicodeString(UnicodeString&& other) noexcept;
  UnicodeString& operator=(UnicodeString&& other) noexcept;

  // Non-copyable but move-only: there are no copy constructors or assignment operators.
  UnicodeString(const UnicodeString&) = delete;
  UnicodeString& operator=(const UnicodeString&) = delete;

  ~UnicodeString();

  // Returns true if the string has no characters (same as empty()).
  bool operator!() const { return empty(); }

  // Returns the number of characters in the string. Includes any NUL terminator
  // which is stored in the allocation but not counted here.
  uint64_t size() const { return size_; }

  // Returns true if there are no characters (size() == 0).
  bool empty() const { return size_ == 0; }

  // The character at position i, checked for bounds. Unlike other
  // checked accessors, it is not an error to access one position past the end:
  // the NUL terminator is considered part of the string. That also means that
  // a valid range is [0, size_].
  char16_t at(uint64_t i) const { return data_[i]; }

  // The character at position i, checked for bounds with assertion checking.
  char16_t operator[](uint64_t i) const { return data_[i]; }

  // Concatenate the given string to the end of this one. Returns false if
  // allocation fails or if the new size would exceed uint64_t's maximum.
  bool append(UnicodeStringView view);

  // Concatenate a single character at the end of the string, returning true on
  // success. Fails with false when allocation fails or if the new size would
  // exceed uint64_t's maximum.
  bool append(char16_t c);

  // Returns a pointer to NUL-terminated character data. Valid for read-only use.
  const char16_t* c_str() const { return data_; }

  // Returns a view of the string's contents.
  UnicodeStringView view() const { return UnicodeStringView(data_, size_); }

  // The string's allocator, to match String's interface.
  IAllocator& allocator() const { return allocator_; }

  // Swap this string with another.
  void swap(UnicodeString& other) {
    std::swap(allocator_, other.allocator_);
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
  }

 private:
  IAllocator& allocator_;
  char16_t* data_ = nullptr;
  uint64_t size_ = 0; 
  uint64_t capacity_ = 0;

  // Allocate new storage, or reallocate if there's not enough space.
  bool grow(uint64_t new_capacity);

  // Copy the contents of other string to this one, with size and capacity.
  void copy_from(const UnicodeString& other);
};

inline void swap(UnicodeString& a, UnicodeString& b) { a.swap(b); }