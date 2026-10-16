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
#include "Utility/ArrayView.hpp"

// A non-owning view of text: a pointer and a length, with no NUL terminator
// required (and none assumed beyond the length). Use it wherever a function used to
// take `const char*` and then walk to the terminator: the length is known up front
// and every access is checked against it, like ArrayView.
//
//   StringView name = "example.com";           // from a literal or any C string
//   StringView label;
//   if (name.split_once('.', &label, &rest)) ...
//
// A view into a String or buffer is invalidated when that changes, as with pointers.
// Characters are bytes (ASCII/UTF-8 is not interpreted).
class StringView {
 public:
  static constexpr uint64_t npos = ~uint64_t{0};

  constexpr StringView() = default;
  constexpr StringView(const char* data, uint64_t length) : data_(data), size_(data ? length : 0) {}
  // A NUL-terminated C string (measured once, here). nullptr is the empty string.
  constexpr StringView(const char* text) : data_(text), size_(text ? measure(text) : 0) {}
  // A literal or char array: the text up to the first NUL, within the array.
  template <uint64_t N>
  constexpr StringView(const char (&array)[N]) : data_(array), size_(measure(array, N)) {}
  explicit StringView(ConstByteView bytes) : data_(reinterpret_cast<const char*>(bytes.data())), size_(bytes.size()) {}

  constexpr const char* data() const { return data_; }
  constexpr uint64_t size() const { return size_; }
  constexpr bool empty() const { return size_ == 0; }
  constexpr const char* begin() const { return data_; }
  constexpr const char* end() const { return data_ + size_; }

  // Checked access: an index at or past the end stops the OS.
  char operator[](uint64_t index) const {
    if (index >= size_) bounds_violation("StringView::operator[]", index, size_);
    return data_[index];
  }
  bool try_get(uint64_t index, char* out) const {
    if (index >= size_) return false;
    *out = data_[index];
    return true;
  }
  char front() const { return (*this)[0]; }
  char back() const { return (*this)[size_ - 1]; }

  // The characters as bytes / as an ArrayView.
  ConstByteView bytes() const { return ConstByteView(reinterpret_cast<const uint8_t*>(data_), size_); }
  ArrayView<const char> chars() const { return ArrayView<const char>(data_, size_); }

  // Sub-views (checked: a range outside the text stops the OS) ...
  StringView substr(uint64_t position, uint64_t length) const { return StringView(chars().subview(position, length)); }
  StringView substr(uint64_t position) const { return StringView(chars().subview(position)); }
  StringView first(uint64_t count) const { return substr(0, count); }
  StringView last(uint64_t count) const { return StringView(chars().last(count)); }
  // ... or non-aborting.
  bool try_substr(uint64_t position, uint64_t length, StringView* out) const {
    ArrayView<const char> part;
    if (!chars().try_subview(position, length, &part)) return false;
    *out = StringView(part);
    return true;
  }
  // Everything after the first `count` characters / without the last `count` (clamped, never fails).
  StringView without_prefix(uint64_t count) const { return count >= size_ ? StringView() : StringView(data_ + count, size_ - count); }
  StringView without_suffix(uint64_t count) const { return count >= size_ ? StringView() : StringView(data_, size_ - count); }
  // Without leading/trailing blanks (space, tab, CR, LF).
  StringView trimmed() const {
    uint64_t a = 0, b = size_;
    while (a < b && is_blank(data_[a])) ++a;
    while (b > a && is_blank(data_[b - 1])) --b;
    return StringView(data_ + a, b - a);
  }

  bool starts_with(StringView prefix) const { return prefix.size_ <= size_ && StringView(data_, prefix.size_).equals(prefix); }
  bool ends_with(StringView suffix) const {
    return suffix.size_ <= size_ && StringView(data_ + (size_ - suffix.size_), suffix.size_).equals(suffix);
  }
  bool starts_with(char c) const { return size_ > 0 && data_[0] == c; }
  bool ends_with(char c) const { return size_ > 0 && data_[size_ - 1] == c; }

  // Position of the first occurrence at or after `from`, or npos.
  uint64_t find(char c, uint64_t from = 0) const {
    for (uint64_t i = from; i < size_; ++i)
      if (data_[i] == c) return i;
    return npos;
  }
  uint64_t find(StringView needle, uint64_t from = 0) const {
    if (needle.size_ > size_) return npos;
    for (uint64_t i = from; i + needle.size_ <= size_; ++i)
      if (StringView(data_ + i, needle.size_).equals(needle)) return i;
    return npos;
  }
  uint64_t rfind(char c) const {
    for (uint64_t i = size_; i > 0; --i)
      if (data_[i - 1] == c) return i - 1;
    return npos;
  }
  bool contains(char c) const { return find(c) != npos; }
  bool contains(StringView needle) const { return find(needle) != npos; }

  // Splits at the first `separator`: *before gets the text ahead of it, *after the
  // text behind it. False (outputs untouched) if there is no separator.
  bool split_once(char separator, StringView* before, StringView* after) const {
    const uint64_t at = find(separator);
    if (at == npos) return false;
    const StringView head(data_, at), tail(data_ + at + 1, size_ - at - 1);
    *before = head;
    *after = tail;
    return true;
  }
  // Takes the text up to the next `separator` off the front into *token (the whole
  // rest if there is none). False once nothing is left. For `while (rest.next_token(',', &t))`.
  bool next_token(char separator, StringView* token) {
    if (empty()) return false;
    const uint64_t at = find(separator);
    if (at == npos) {
      *token = *this;
      *this = StringView();
    } else {
      *token = StringView(data_, at);
      *this = StringView(data_ + at + 1, size_ - at - 1);
    }
    return true;
  }

  bool equals(StringView other) const {
    if (size_ != other.size_) return false;
    for (uint64_t i = 0; i < size_; ++i)
      if (data_[i] != other.data_[i]) return false;
    return true;
  }
  // ASCII letters compare without regard to case.
  bool equals_ignore_case(StringView other) const {
    if (size_ != other.size_) return false;
    for (uint64_t i = 0; i < size_; ++i)
      if (lower(data_[i]) != lower(other.data_[i])) return false;
    return true;
  }
  // <0, 0, >0 like strcmp, comparing as unsigned bytes; a prefix sorts first.
  int compare(StringView other) const {
    const uint64_t n = size_ < other.size_ ? size_ : other.size_;
    for (uint64_t i = 0; i < n; ++i) {
      const unsigned char a = data_[i], b = other.data_[i];
      if (a != b) return a < b ? -1 : 1;
    }
    return size_ == other.size_ ? 0 : (size_ < other.size_ ? -1 : 1);
  }
  bool operator==(StringView other) const { return equals(other); }
  bool operator!=(StringView other) const { return !equals(other); }
  bool operator<(StringView other) const { return compare(other) < 0; }

  // Parses a whole-string unsigned decimal number: no sign, no blanks, no overflow.
  bool parse_uint(uint64_t* out) const {
    if (empty()) return false;
    uint64_t value = 0;
    for (char c : *this) {
      if (c < '0' || c > '9') return false;
      const uint64_t digit = static_cast<uint64_t>(c - '0');
      if (value > (~uint64_t{0} - digit) / 10) return false;
      value = value * 10 + digit;
    }
    *out = value;
    return true;
  }

  // Copies the text into `out` followed by a NUL. False (nothing written) if it does not fit.
  bool copy_to(ArrayView<char> out) const {
    if (size_ >= out.size()) return false;
    if (size_) out.first(size_).copy_from(chars());
    out[size_] = '\0';
    return true;
  }

 private:
  explicit constexpr StringView(ArrayView<const char> chars) : data_(chars.data()), size_(chars.size()) {}

  static constexpr uint64_t measure(const char* text) {
    uint64_t n = 0;
    while (text[n]) ++n;
    return n;
  }
  static constexpr uint64_t measure(const char* array, uint64_t capacity) {
    uint64_t n = 0;
    while (n < capacity && array[n]) ++n;
    return n;
  }
  static constexpr bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
  static constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

  const char* data_ = nullptr;
  uint64_t size_ = 0;
};
