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
#include "Utility/StringView.hpp"

// An owned, growable string: the characters live in one allocation from the
// IAllocator given, always followed by a NUL so c_str() can be handed to code that
// wants a C string. Move-only. Anything that can need memory returns false when
// the allocator is out of it, leaving the string unchanged - there are no
// exceptions - and every character access is checked.
//
//   String path(allocator);
//   if (!path.assign("/dev/") || !path.append(name) || !path.append_uint(index)) return false;
//   use(path.view());
class String {
 public:
  explicit String(allocators::IAllocator& allocator) : allocator_(&allocator) {}
  allocators::IAllocator& allocator() const { return *allocator_; }
  String(String&& other) : allocator_(other.allocator_), storage_(other.storage_), size_(other.size_) {
    other.storage_ = ArrayView<char>();
    other.size_ = 0;
  }
  String& operator=(String&& other) {
    if (this != &other) {
      allocator_->deallocate(storage_.data());
      allocator_ = other.allocator_;
      storage_ = other.storage_;
      size_ = other.size_;
      other.storage_ = ArrayView<char>();
      other.size_ = 0;
    }
    return *this;
  }
  String(const String&) = delete;
  String& operator=(const String&) = delete;
  ~String() { allocator_->deallocate(storage_.data()); }

  uint64_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  uint64_t capacity() const { return storage_.empty() ? 0 : storage_.size() - 1; }  // characters, not counting the NUL

  StringView view() const { return StringView(storage_.data(), size_); }
  operator StringView() const { return view(); }
  // NUL-terminated; never null (an empty string yields ""), valid until the next change.
  const char* c_str() const { return storage_.empty() ? "" : storage_.data(); }
  const char* data() const { return c_str(); }

  char operator[](uint64_t index) const {
    if (index >= size_) bounds_violation("String::operator[]", index, size_);
    return storage_[index];
  }
  // Overwrites one existing character (never the terminator).
  void set(uint64_t index, char c) {
    if (index >= size_) bounds_violation("String::set", index, size_);
    storage_[index] = c;
  }

  void clear() {
    size_ = 0;
    if (!storage_.empty()) storage_[0] = '\0';
  }

  // Makes room for `characters` characters. False if memory ran out.
  bool reserve(uint64_t characters) {
    if (characters <= capacity()) return true;
    if (characters >= ~uint64_t{0} - 1) return false;
    char* fresh = static_cast<char*>(allocator_->allocate(characters + 1));
    if (!fresh) return false;
    const ArrayView<char> next(fresh, characters + 1);
    if (size_) next.first(size_).copy_from(storage_.first(size_));
    next[size_] = '\0';
    allocator_->deallocate(storage_.data());
    storage_ = next;
    return true;
  }

  bool assign(StringView text) {
    if (!reserve(text.size())) return false;
    size_ = 0;
    return append(text);
  }
  bool append(StringView text) {
    if (text.empty()) return true;
    if (size_ > ~uint64_t{0} - text.size() - 1) return false;
    // `text` may point into this string: grow first (which keeps its bytes) only if needed.
    const uint64_t needed = size_ + text.size();
    if (needed > capacity() && !grow_for(needed, &text)) return false;
    storage_.subview(size_, text.size()).copy_from(text.chars());  // memmove semantics
    size_ = needed;
    storage_[size_] = '\0';
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
    if (value < 0) {
      if (!push_back('-')) return false;
      return append_uint(0 - static_cast<uint64_t>(value));
    }
    return append_uint(static_cast<uint64_t>(value));
  }
  // Inserts `text` before position `at` (at == size() appends). False if `at` is
  // beyond the end or memory ran out.
  bool insert(uint64_t at, StringView text) {
    if (at > size_) return false;
    String result(*allocator_);
    if (!result.reserve(size_ + text.size()) || !result.append(view().first(at)) || !result.append(text) ||
        !result.append(view().without_prefix(at)))
      return false;
    *this = static_cast<String&&>(result);
    return true;
  }
  // Removes `count` characters from `at` (clamped to the end). False if `at` is beyond it.
  bool erase(uint64_t at, uint64_t count) {
    if (at > size_) return false;
    if (count > size_ - at) count = size_ - at;
    if (count == 0) return true;
    storage_.subview(at, size_ - at - count).copy_from(storage_.subview(at + count, size_ - at - count));
    size_ -= count;
    storage_[size_] = '\0';
    return true;
  }
  // Shortens to `length` characters (no-op if already shorter).
  void truncate(uint64_t length) {
    if (length >= size_) return;
    size_ = length;
    storage_[size_] = '\0';
  }

  bool equals(StringView other) const { return view().equals(other); }
  bool operator==(StringView other) const { return view().equals(other); }
  bool operator!=(StringView other) const { return !view().equals(other); }

 private:
  // Grows to hold at least `needed` characters, geometrically. If `*keep` points
  // into the old buffer it is re-pointed into the new one.
  bool grow_for(uint64_t needed, StringView* keep) {
    uint64_t target = capacity() ? capacity() * 2 : 15;
    if (target < needed) target = needed;
    const char* old_begin = storage_.data();
    const bool inside = keep->data() >= old_begin && storage_.size() && keep->data() < old_begin + storage_.size();
    const uint64_t offset = inside ? static_cast<uint64_t>(keep->data() - old_begin) : 0;
    if (!reserve(target)) return false;
    if (inside) *keep = StringView(storage_.data() + offset, keep->size());
    return true;
  }

  allocators::IAllocator* allocator_;
  ArrayView<char> storage_;  // capacity() + 1 bytes: the text and its NUL
  uint64_t size_ = 0;
};
