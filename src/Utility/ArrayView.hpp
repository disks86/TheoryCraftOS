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
#include <type_traits>
#include "Utility/Bounds.hpp"
#include "Utility/Slice.hpp"

// A pointer and a count: a non-owning window onto `size` consecutive elements
// that someone else owns (an array, a buffer, a DMA ring, part of a packet).
// Wherever the old code passed `T* data, uint32_t length` as two arguments, pass
// one ArrayView instead: the length can no longer be forgotten, mismatched or
// trusted from the wrong source.
//
//   - operator[], subview(), first(), ... are CHECKED: going outside the view
//     stops the OS (bounds_violation) instead of touching foreign memory.
//   - try_get(), try_subview(), copy_from() and ByteReader report failure with a
//     return value, for data you do not control (network packets, USB descriptors).
//   - Iteration (range-for) stays inside the view by construction.
//
// A view does not own its memory and never extends it: the usual rules for
// pointers apply to its lifetime. ArrayView<const T> views read-only memory.
template <class T>
class ArrayView {
 public:
  using Element = T;

  constexpr ArrayView() = default;
  constexpr ArrayView(T* data, uint64_t size) : data_(data), size_(data ? size : 0) {}  // a null view is empty
  template <uint64_t N>
  constexpr ArrayView(T (&array)[N]) : data_(array), size_(N) {}
  // ArrayView<U> -> ArrayView<const U>
  template <class U>
    requires(std::is_same_v<const U, T> && !std::is_same_v<U, T>)
  constexpr ArrayView(const ArrayView<U>& other) : data_(other.data()), size_(other.size()) {}

  constexpr T* data() const { return data_; }
  constexpr uint64_t size() const { return size_; }
  constexpr uint64_t size_bytes() const { return size_ * sizeof(T); }
  constexpr bool empty() const { return size_ == 0; }
  constexpr bool in_range(uint64_t index) const { return index < size_; }

  // Iteration.
  constexpr T* begin() const { return data_; }
  constexpr T* end() const { return data_ + size_; }

  // Checked element access.
  T& operator[](uint64_t index) const {
    if (index >= size_) bounds_violation("ArrayView::operator[]", index, size_);
    return data_[index];
  }
  T& front() const { return (*this)[0]; }
  T& back() const { return (*this)[size_ - 1]; }  // an empty view stops the OS here (index wraps past size)

  // Element access that reports failure.
  bool try_get(uint64_t index, std::remove_const_t<T>* out) const {
    if (index >= size_) return false;
    *out = data_[index];
    return true;
  }
  T* try_at(uint64_t index) const { return index < size_ ? data_ + index : nullptr; }

  // Sub-views. The checked forms stop the OS when the range is not inside the
  // view; the try_ forms return false.
  ArrayView subview(Slice range) const {
    ArrayView result;
    if (!try_subview(range, &result)) bounds_violation("ArrayView::subview", range.start + range.length, size_);
    return result;
  }
  ArrayView subview(uint64_t offset, uint64_t count) const { return subview(Slice(offset, count)); }
  ArrayView subview(uint64_t offset) const {
    if (offset > size_) bounds_violation("ArrayView::subview", offset, size_);
    return ArrayView(data_ + offset, size_ - offset);
  }
  ArrayView operator[](Slice range) const { return subview(range); }
  ArrayView first(uint64_t count) const { return subview(0, count); }
  ArrayView last(uint64_t count) const {
    if (count > size_) bounds_violation("ArrayView::last", count, size_);
    return ArrayView(data_ + (size_ - count), count);
  }
  // Without the first `count` elements.
  ArrayView dropped(uint64_t count) const { return subview(count); }

  bool try_subview(Slice range, ArrayView* out) const {
    if (!range.fits_in(size_)) return false;
    *out = ArrayView(data_ + range.start, range.length);
    return true;
  }
  bool try_subview(uint64_t offset, uint64_t count, ArrayView* out) const {
    return try_subview(Slice(offset, count), out);
  }
  // The longest prefix of at most `count` elements (never fails).
  ArrayView at_most(uint64_t count) const { return ArrayView(data_, count < size_ ? count : size_); }

  // Reinterprets the memory as another element type. The size must be a whole
  // number of U and the address suitably aligned, or the OS stops. For turning a
  // byte buffer into a header struct or a DMA region into a descriptor array.
  template <class U>
  ArrayView<U> cast() const {
    ArrayView<U> result;
    if (!try_cast<U>(&result)) bounds_violation("ArrayView::cast", size_bytes(), sizeof(U));
    return result;
  }
  template <class U>
  bool try_cast(ArrayView<U>* out) const {
    if (size_bytes() % sizeof(U) != 0 || reinterpret_cast<uint64_t>(data_) % alignof(U) != 0) return false;
    *out = ArrayView<U>(reinterpret_cast<U*>(data_), size_bytes() / sizeof(U));
    return true;
  }

  // Copies `source` to the start of this view. False (and nothing is copied) if
  // it does not fit. Overlapping memory is handled.
  template <class U>
    requires(std::is_same_v<std::remove_const_t<U>, std::remove_const_t<T>> && !std::is_const_v<T>)
  bool copy_from(ArrayView<U> source) const {
    if (source.size() > size_) return false;
    __builtin_memmove(data_, source.data(), source.size() * sizeof(T));
    return true;
  }
  // Fills the whole view.
  void fill(const T& value) const
    requires(!std::is_const_v<T>)
  {
    for (uint64_t i = 0; i < size_; ++i) data_[i] = value;
  }
  // Same length and equal contents (bytewise: for plain data only).
  template <class U>
  bool equals(ArrayView<U> other) const {
    return other.size() == size_ && (size_ == 0 || __builtin_memcmp(data_, other.data(), size_ * sizeof(T)) == 0);
  }

 private:
  T* data_ = nullptr;
  uint64_t size_ = 0;
};

using ByteView = ArrayView<uint8_t>;
using ConstByteView = ArrayView<const uint8_t>;

// A view of one object's bytes, e.g. a header struct to fill from a packet.
template <class T>
ByteView bytes_of(T& object) {
  return ByteView(reinterpret_cast<uint8_t*>(&object), sizeof(T));
}
template <class T>
ConstByteView bytes_of(const T& object) {
  return ConstByteView(reinterpret_cast<const uint8_t*>(&object), sizeof(T));
}

// The characters of a NUL-terminated string (without the NUL) as bytes. The
// string must really be terminated; the view is only as long as its text.
inline ConstByteView text_bytes(const char* text) {
  uint64_t length = 0;
  while (text[length]) ++length;
  return ConstByteView(reinterpret_cast<const uint8_t*>(text), length);
}
