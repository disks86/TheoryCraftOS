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
#include "ArrayView.hpp"
#include "Utility/Bounds.hpp"

// A view into a Unicode string: just a pointer and a size. Immutable.
//
//   UnicodeStringView view = path.view();
//   for (uint64_t i = 0; i < view.size(); ++i) {
//     char16_t c = view[i];
//     // ... use c ...
//   }
//
class UnicodeStringView {
 public:
  // Create an empty view.
  constexpr UnicodeStringView() = default;

  // Create a view of the given NUL-terminated character array, using strlen for
  // the size. The character should be '\0' to make a valid string. Not allowed
  // to pass a null pointer.
  constexpr UnicodeStringView(const char16_t* data);

  // Create a view of the given character array, of the specified size.
  // Not allowed to pass a null pointer.
  constexpr UnicodeStringView(const char16_t* data, uint64_t size);

  // Returns true if the view is empty (size() == 0).
  constexpr bool empty() const { return size_ == 0; }

  // Returns the number of characters in the view. Does not include any NUL terminator.
  constexpr uint64_t size() const { return size_; }

  // The character at position i, checked for bounds with assertion checking.
  constexpr char16_t operator[](uint64_t i) const { return data_[i]; }

  // Returns a pointer to the character data. Valid for read-only use.
  constexpr const char16_t* data() const { return data_; }

  // Returns a pointer to the character data. Valid for read-only use.
  constexpr const char16_t* c_str() const { return data_; }

  // Returns a view of the string's contents as an ArrayView.
  constexpr ArrayView<const char16_t> array_view() const { return ArrayView<const char16_t>(data_, size_); }

 private:
  const char16_t* data_ = nullptr;
  uint64_t size_ = 0;
};

// Make a view of the given NUL-terminated character array.
//
//   UnicodeStringView view = UnicodeStringView::from_c_str("hello");
//   // ... use view ...
//
inline constexpr UnicodeStringView UnicodeStringView::from_c_str(const char16_t* data) {
  return UnicodeStringView(data, Bounds::strlen(data));
}

// Constructors implementation
inline constexpr UnicodeStringView::UnicodeStringView(const char16_t* data) : data_(data), size_(Bounds::strlen(data)) {}

inline constexpr UnicodeStringView::UnicodeStringView(const char16_t* data, uint64_t size) : data_(data), size_(size) {}