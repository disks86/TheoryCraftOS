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
#include "Utility/StringView.hpp"

// A small string that lives entirely inside the object (no allocator), laid out like
// folly's small-string optimisation: the LAST byte holds the number of unused
// characters, not a length. Short strings end in a NUL followed by padding, with the
// remaining count in the final byte; once the string fills the buffer that count is
// 0, so the final byte is itself the NUL terminator. Either way c_str() is a valid C
// string, and the size is just N - 1 - last byte.
//
// N is the buffer size in bytes (capacity is N - 1 characters), at most 256 so the
// count fits in a byte. Copyable. Anything that might not fit returns false and
// leaves the string unchanged; there are no exceptions.
//
//   FixedString<16> name;                  // up to 15 characters
//   if (!name.assign("eth") || !name.append_uint(0)) return false;
//   use(name.c_str());
template <uint64_t N>
class FixedString {
  static_assert(N >= 2 && N <= 256, "remaining-capacity byte needs 2 <= N <= 256");

 public:
  static constexpr uint64_t kCapacity = N - 1;

  constexpr FixedString() { set_size(0); }
  FixedString(StringView text) {
    set_size(0);
    assign(text);  // text beyond capacity is rejected, leaving this empty
  }

  constexpr uint64_t size() const { return kCapacity - static_cast<uint8_t>(buffer_[N - 1]); }
  constexpr bool empty() const { return size() == 0; }
  constexpr uint64_t capacity() const { return kCapacity; }
  constexpr bool full() const { return size() == kCapacity; }

  StringView view() const { return StringView(buffer_, size()); }
  operator StringView() const { return view(); }
  // NUL-terminated, valid until the next change. Never null.
  const char* c_str() const { return buffer_; }
  const char* data() const { return buffer_; }

  char operator[](uint64_t index) const {
    if (index >= size()) bounds_violation("FixedString::operator[]", index, size());
    return buffer_[index];
  }
  // Overwrites one existing character (never the terminator).
  void set(uint64_t index, char c) {
    if (index >= size()) bounds_violation("FixedString::set", index, size());
    buffer_[index] = c;
  }

  void clear() { set_size(0); }
  void truncate(uint64_t length) {
    if (length < size()) set_size(length);
  }

  bool assign(StringView text) {
    if (text.size() > kCapacity) return false;
    // `text` may point into this string, so move rather than copy.
    if (text.size()) __builtin_memmove(buffer_, text.data(), text.size());
    set_size(text.size());
    return true;
  }
  bool append(StringView text) {
    const uint64_t length = size();
    if (text.size() > kCapacity - length) return false;
    __builtin_memmove(buffer_ + length, text.data(), text.size());
    set_size(length + text.size());
    return true;
  }
  bool push_back(char c) { return append(StringView(&c, 1)); }
  // Appends the decimal digits of `value`.
  bool append_uint(uint64_t value) {
    char digits[20];
    uint32_t n = 0;
    do digits[n++] = static_cast<char>('0' + value % 10); while (value /= 10);
    for (uint32_t i = 0; i < n / 2; ++i) {
      const char t = digits[i];
      digits[i] = digits[n - 1 - i];
      digits[n - 1 - i] = t;
    }
    return append(StringView(digits, n));
  }
  bool append_int(int64_t value) {
    if (value >= 0) return append_uint(static_cast<uint64_t>(value));
    FixedString<N> backup = *this;
    if (!push_back('-') || !append_uint(0 - static_cast<uint64_t>(value))) {
      *this = backup;
      return false;
    }
    return true;
  }
  // Removes `count` characters from `at` (clamped to the end). False if `at` is beyond it.
  bool erase(uint64_t at, uint64_t count) {
    const uint64_t length = size();
    if (at > length) return false;
    if (count > length - at) count = length - at;
    __builtin_memmove(buffer_ + at, buffer_ + at + count, length - at - count);
    set_size(length - count);
    return true;
  }

  bool equals(StringView other) const { return view().equals(other); }
  bool operator==(StringView other) const { return view().equals(other); }
  bool operator!=(StringView other) const { return !view().equals(other); }

 private:
  // The one place the layout is written: terminator after the text, then the unused
  // count in the last byte (which is the terminator too when length == kCapacity).
  constexpr void set_size(uint64_t length) {
    buffer_[length] = '\0';
    buffer_[N - 1] = static_cast<char>(kCapacity - length);
  }

  char buffer_[N] = {};
};
