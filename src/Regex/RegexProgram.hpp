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
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

// The pieces a compiled regular expression is made of, shared by the compiler,
// the matcher and the two front ends (regex::Regex, built at run time, and
// regex::StaticRegex, built by the compiler while the program itself is compiled).
namespace regex {

// ---- limits
constexpr uint32_t kMaxGroups = 32;                  // capture groups, group 0 (the whole match) included
constexpr uint32_t kLoopSlotBase = kMaxGroups * 2;   // slots below this are capture positions, above it loop guards
constexpr uint32_t kMaxLoops = 64;                   // loops over a body that can match nothing
constexpr uint32_t kMaxSlots = kLoopSlotBase + kMaxLoops;
constexpr uint32_t kMaxRepeat = 1000;                // the largest count in {n,m}
constexpr uint32_t kMaxNesting = 64;                 // nested groups
constexpr uint32_t kMaxLookDepth = 8;                // nested lookaheads
constexpr uint32_t kMaxProgram = 1u << 16;           // instructions
constexpr uint32_t kMaxRanges = 1u << 16;
constexpr uint32_t kMaxNameLength = 31;
constexpr uint32_t kMaxCodePoint = 0x10FFFF;
constexpr uint64_t kUnset = ~uint64_t{0};

enum class Flags : uint32_t {
  None = 0,
  IgnoreCase = 1,  // ASCII letters compare without regard to case
  Multiline = 2,   // ^ and $ also match at line breaks
  DotAll = 4,      // . also matches line breaks
  Utf8 = 8,        // pattern and input are UTF-8: one character is one code point, not one byte
};
constexpr Flags operator|(Flags a, Flags b) { return static_cast<Flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
constexpr bool has_flag(Flags set, Flags one) { return (static_cast<uint32_t>(set) & static_cast<uint32_t>(one)) != 0; }

enum class Error : uint8_t {
  None,
  TrailingBackslash,
  BadEscape,
  UnbalancedParen,
  UnterminatedClass,
  BadClassRange,
  NothingToRepeat,
  BadRepeat,          // {3,2}
  RepeatTooLarge,
  BadGroup,           // (?x
  BadGroupName,
  DuplicateGroupName,
  TooManyGroups,
  TooManyLoops,
  BadBackreference,
  ProgramTooLarge,
  NestingTooDeep,
  Unsupported,        // lookbehind
  BadUtf8,
  OutOfMemory,
};

constexpr const char* error_text(Error e) {
  switch (e) {
    case Error::None: return "no error";
    case Error::TrailingBackslash: return "pattern ends with a backslash";
    case Error::BadEscape: return "unknown or malformed escape";
    case Error::UnbalancedParen: return "unbalanced parenthesis";
    case Error::UnterminatedClass: return "character class is not closed";
    case Error::BadClassRange: return "invalid range in character class";
    case Error::NothingToRepeat: return "nothing to repeat";
    case Error::BadRepeat: return "invalid repeat count";
    case Error::RepeatTooLarge: return "repeat count too large";
    case Error::BadGroup: return "invalid group syntax";
    case Error::BadGroupName: return "invalid group name";
    case Error::DuplicateGroupName: return "duplicate group name";
    case Error::TooManyGroups: return "too many capture groups";
    case Error::TooManyLoops: return "too many loops over possibly empty text";
    case Error::BadBackreference: return "backreference to a group that does not exist";
    case Error::ProgramTooLarge: return "pattern compiles to too large a program";
    case Error::NestingTooDeep: return "pattern nests too deeply";
    case Error::Unsupported: return "unsupported syntax (lookbehind)";
    case Error::BadUtf8: return "pattern is not valid UTF-8";
    case Error::OutOfMemory: return "out of memory";
  }
  return "unknown error";
}

// ---- the program
// Jumps are relative to the instruction that holds them, so a piece of program can
// be moved or copied without fixing anything up; that is how repeats are built.
enum class Op : uint8_t {
  Match,       // success
  Char,        // a = code point
  CharFold,    // a = lower-case ASCII letter (either case matches)
  Any,         // anything but a line break
  AnyAll,      // anything
  Class,       // ranges[a .. a+b) contain the character
  ClassNot,    // ... do not contain it
  Split,       // try pc+a first, pc+b if that fails
  Jmp,         // pc += a
  Save,        // slots[a] = position
  TextBegin,
  TextEnd,
  LineBegin,
  LineEnd,
  WordBoundary,
  NotWordBoundary,
  Backref,     // a = group
  BackrefFold,
  Look,        // lookahead: the body follows, ends at LookEnd; execution resumes at pc+a. b = 1 for a negative one
  LookEnd,
  LoopEnter,   // slots[a] = position (guards a loop whose body can match nothing)
  LoopCheck,   // fails if the position is still slots[a]
};

struct Inst {
  Op op = Op::Match;
  int32_t a = 0;
  int32_t b = 0;
};

struct Range {
  uint32_t lo = 0;
  uint32_t hi = 0;
};

struct NameEntry {
  char text[kMaxNameLength + 1] = {};
  uint32_t length = 0;
  uint32_t group = 0;
};

// A compiled expression as the matcher sees it: views onto storage somebody else owns.
struct Program {
  ArrayView<const Inst> code;
  ArrayView<const Range> ranges;
  ArrayView<const NameEntry> names;
  uint32_t group_count = 1;  // including group 0
  Flags flags = Flags::None;

  // The index of a named group, or -1.
  constexpr int32_t group_index(StringView name) const {
    for (uint64_t i = 0; i < names.size(); ++i) {
      const NameEntry& e = names[i];
      if (e.length != name.size()) continue;
      bool same = true;
      for (uint64_t k = 0; k < e.length && same; ++k) same = e.text[k] == name[k];
      if (same) return static_cast<int32_t>(e.group);
    }
    return -1;
  }
};

// ---- storage
// The compiler and matcher only need a vector that can grow, shrink, and take an
// element in the middle; they come in two flavours with the same shape. Elements
// are plain data.

// Fixed capacity, no allocator: usable while the compiler runs.
template <class T, uint32_t N>
class FixedVec {
 public:
  constexpr uint64_t size() const { return size_; }
  constexpr bool oom() const { return false; }
  constexpr T& operator[](uint64_t i) {
    if (i >= size_) bounds_violation("FixedVec::operator[]", i, size_);
    return items_[i];
  }
  constexpr const T& operator[](uint64_t i) const {
    if (i >= size_) bounds_violation("FixedVec::operator[]", i, size_);
    return items_[i];
  }
  constexpr bool push(const T& v) {
    if (size_ >= N) return false;
    items_[size_++] = v;
    return true;
  }
  constexpr bool insert(uint64_t at, const T& v) {
    if (at > size_ || size_ >= N) return false;
    for (uint64_t i = size_; i > at; --i) items_[i] = items_[i - 1];
    items_[at] = v;
    ++size_;
    return true;
  }
  constexpr void truncate(uint64_t n) {
    if (n < size_) size_ = n;
  }
  constexpr const T* data() const { return items_; }
  constexpr ArrayView<const T> view() const { return ArrayView<const T>(items_, size_); }

 private:
  T items_[N ? N : 1] = {};
  uint64_t size_ = 0;
};

// Grows from an allocator, up to `limit` elements. Move-only.
template <class T>
class HeapVec {
 public:
  HeapVec(allocators::IAllocator& allocator, uint64_t limit) : allocator_(&allocator), limit_(limit) {}
  HeapVec(HeapVec&& o)
      : allocator_(o.allocator_), items_(o.items_), size_(o.size_), capacity_(o.capacity_), limit_(o.limit_), oom_(o.oom_) {
    o.items_ = nullptr;
    o.size_ = o.capacity_ = 0;
  }
  HeapVec& operator=(HeapVec&& o) {
    if (this != &o) {
      allocator_->deallocate(items_);
      allocator_ = o.allocator_;
      items_ = o.items_;
      size_ = o.size_;
      capacity_ = o.capacity_;
      limit_ = o.limit_;
      oom_ = o.oom_;
      o.items_ = nullptr;
      o.size_ = o.capacity_ = 0;
    }
    return *this;
  }
  HeapVec(const HeapVec&) = delete;
  HeapVec& operator=(const HeapVec&) = delete;
  ~HeapVec() { allocator_->deallocate(items_); }

  uint64_t size() const { return size_; }
  bool oom() const { return oom_; }  // the last refused push was for want of memory, not the limit
  T& operator[](uint64_t i) {
    if (i >= size_) bounds_violation("HeapVec::operator[]", i, size_);
    return items_[i];
  }
  const T& operator[](uint64_t i) const {
    if (i >= size_) bounds_violation("HeapVec::operator[]", i, size_);
    return items_[i];
  }
  bool push(const T& v) { return insert(size_, v); }
  bool insert(uint64_t at, const T& v) {
    if (at > size_ || size_ >= limit_ || !grow()) return false;
    if (at < size_) __builtin_memmove(items_ + at + 1, items_ + at, (size_ - at) * sizeof(T));
    items_[at] = v;
    ++size_;
    return true;
  }
  void truncate(uint64_t n) {
    if (n < size_) size_ = n;
  }
  const T* data() const { return items_; }
  ArrayView<const T> view() const { return ArrayView<const T>(items_, size_); }

 private:
  bool grow() {
    if (size_ < capacity_) return true;
    uint64_t target = capacity_ ? capacity_ * 2 : 16;
    if (target > limit_) target = limit_;
    if (target <= capacity_) return false;
    T* fresh = static_cast<T*>(allocator_->allocate(target * sizeof(T)));
    if (!fresh) {
      oom_ = true;
      return false;
    }
    if (size_) __builtin_memcpy(fresh, items_, size_ * sizeof(T));
    allocator_->deallocate(items_);
    items_ = fresh;
    capacity_ = target;
    return true;
  }

  allocators::IAllocator* allocator_;
  T* items_ = nullptr;
  uint64_t size_ = 0, capacity_ = 0, limit_;
  bool oom_ = false;
};

}  // namespace regex
