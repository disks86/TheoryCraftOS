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
#include "Regex/RegexCompiler.hpp"
#include "Regex/RegexMatcher.hpp"
#include "Utility/String.hpp"

// Regular expressions, built either while the program is compiled or while it runs.
//
//   // At compile time: the pattern is checked and turned into a table by the C++ compiler,
//   // nothing is allocated, and it can be used in a constant expression.
//   constexpr auto date = regex::compile<"(?<y>\\d{4})-(\\d{2})-(\\d{2})">();
//   static_assert(date.matches("2026-10-21"));
//   regex::Match m;
//   if (date.search(text, &m) == regex::Result::Matched) use(m.text(text, 1));
//
//   // At run time: for patterns that arrive as data (a settings file, a script's RegExp).
//   regex::Regex re(allocator);
//   if (re.compile(user_pattern, regex::Flags::IgnoreCase) != regex::Error::None) ...
//   re.search(text, &m);
//
// Both run the same compiler and the same matcher. The matcher backtracks (so it has
// backreferences and lazy quantifiers) under a step and a stack limit: a search that would
// run away stops with Result::StepLimit rather than hang. See RegexCompiler.hpp for the syntax.
namespace regex {

// ---- compile-time front end

// A string literal usable as a template argument.
template <uint64_t N>
struct ConstString {
  char text[N] = {};
  constexpr ConstString(const char (&literal)[N]) {
    for (uint64_t i = 0; i < N; ++i) text[i] = literal[i];
  }
  static constexpr uint64_t length() { return N - 1; }
  constexpr StringView view() const { return StringView(text); }  // the array form: up to the first NUL
};

// A regular expression held in fixed-size tables, so it needs no allocator and can be a
// constexpr variable. Made by regex::compile<>.
template <uint32_t CodeSize, uint32_t RangeSize, uint32_t NameSize>
class StaticRegex {
 public:
  // The backtracking stack of the calls that are not given one (frames of 16 bytes).
  static constexpr uint32_t kDefaultStack = 256;

  constexpr Error error() const { return error_; }
  constexpr bool valid() const { return error_ == Error::None; }
  constexpr Program program() const {
    Program p;
    p.code = code_.view();
    p.ranges = ranges_.view();
    p.names = names_.view();
    p.group_count = groups_;
    p.flags = flags_;
    return p;
  }
  constexpr uint32_t group_count() const { return groups_; }
  constexpr int32_t group_index(StringView name) const { return program().group_index(name); }

  // Does the whole of `input` match?
  constexpr bool matches(StringView input) const {
    Match m;
    return match(input, &m) == Result::Matched;
  }
  // The same, with the groups and with limits.
  constexpr Result match(StringView input, Match* out, Limits limits = {}) const {
    FixedVec<Frame, kDefaultStack> stack;
    return run(input, 0, true, true, out, stack, limits);
  }
  // Does a match start at `at` (not necessarily reaching the end)?
  constexpr Result match_prefix(StringView input, Match* out, uint64_t at = 0, Limits limits = {}) const {
    FixedVec<Frame, kDefaultStack> stack;
    return run(input, at, true, false, out, stack, limits);
  }
  // Finds the first match at or after `from`.
  constexpr Result search(StringView input, Match* out, uint64_t from = 0, Limits limits = {}) const {
    FixedVec<Frame, kDefaultStack> stack;
    return run(input, from, false, false, out, stack, limits);
  }
  constexpr bool contains(StringView input) const {
    Match m;
    return search(input, &m) == Result::Matched;
  }
  // As search(), with a stack the caller provides (for inputs that need more than kDefaultStack frames).
  template <class Stack>
  constexpr Result search_with(Stack& stack, StringView input, Match* out, uint64_t from = 0, Limits limits = {}) const {
    return run(input, from, false, false, out, stack, limits);
  }

  // Filled in by compile<>().
  FixedVec<Inst, CodeSize> code_;
  FixedVec<Range, RangeSize> ranges_;
  FixedVec<NameEntry, NameSize> names_;
  uint32_t groups_ = 1;
  Flags flags_ = Flags::None;
  Error error_ = Error::None;

 private:
  template <class Stack>
  constexpr Result run(StringView input, uint64_t from, bool anchored, bool whole, Match* out, Stack& stack, Limits limits) const {
    if (error_ != Error::None) return Result::NoMatch;
    const Program p = program();
    Matcher<Stack> matcher(p, input, stack, limits);
    return matcher.search(from, anchored, whole, out);
  }
};

namespace detail {

constexpr uint32_t kScratchCode = 4096, kScratchRanges = 2048, kScratchNames = 32;

struct Sizes {
  Error error = Error::None;
  uint32_t code = 1, ranges = 1, names = 1;
};

// A first pass into scratch tables finds out how large the real ones need to be.
template <uint32_t ScratchCode>
constexpr Sizes measure(const char* pattern, uint64_t length, Flags flags) {
  FixedVec<Inst, ScratchCode> code;
  FixedVec<Range, kScratchRanges> ranges;
  FixedVec<NameEntry, kScratchNames> names;
  Compiler<decltype(code), decltype(ranges), decltype(names)> compiler(code, ranges, names);
  Sizes s;
  s.error = compiler.compile(pattern, length, flags);
  s.code = static_cast<uint32_t>(code.size() ? code.size() : 1);
  s.ranges = static_cast<uint32_t>(ranges.size() ? ranges.size() : 1);
  s.names = static_cast<uint32_t>(names.size() ? names.size() : 1);
  return s;
}

template <ConstString Pattern, Flags F, uint32_t ScratchCode>
constexpr Sizes sizes_of = measure<ScratchCode>(Pattern.text, Pattern.length(), F);

template <ConstString Pattern, Flags F, uint32_t ScratchCode>
constexpr auto build() {
  constexpr Sizes s = sizes_of<Pattern, F, ScratchCode>;
  StaticRegex<s.code, s.ranges, s.names> re;
  Compiler<decltype(re.code_), decltype(re.ranges_), decltype(re.names_)> compiler(re.code_, re.ranges_, re.names_);
  re.error_ = compiler.compile(Pattern.text, Pattern.length(), F);
  re.groups_ = compiler.group_count();
  re.flags_ = F;
  return re;
}

}  // namespace detail

// Compiles `Pattern` while the program is compiled; a pattern that is not valid (or
// compiles to more than `ScratchCode` instructions) is a compile error.
//   constexpr auto word = regex::compile<"\\b\\w+\\b", regex::Flags::IgnoreCase>();
template <ConstString Pattern, Flags F = Flags::None, uint32_t ScratchCode = detail::kScratchCode>
consteval auto compile() {
  static_assert(detail::sizes_of<Pattern, F, ScratchCode>.error == Error::None,
                "invalid regular expression (use regex::try_compile<> and error() to see why)");
  return detail::build<Pattern, F, ScratchCode>();
}
// As compile<>, but an invalid pattern gives an expression whose error() says what was wrong.
template <ConstString Pattern, Flags F = Flags::None, uint32_t ScratchCode = detail::kScratchCode>
consteval auto try_compile() {
  return detail::build<Pattern, F, ScratchCode>();
}

namespace literals {
template <ConstString Pattern>
consteval auto operator""_re() {
  return compile<Pattern>();
}
}  // namespace literals

// ---- run-time front end

// A regular expression compiled from text at run time, with its tables in memory from the
// allocator it was given. Move-only.
class Regex {
 public:
  explicit Regex(allocators::IAllocator& allocator);

  // Compiles `pattern`, replacing whatever was compiled before. On failure the expression
  // is left empty (valid() is false) and matching finds nothing.
  Error compile(StringView pattern, Flags flags = Flags::None);

  bool valid() const { return valid_; }
  Error error() const { return error_; }
  Program program() const;
  uint32_t group_count() const { return groups_; }
  int32_t group_index(StringView name) const { return program().group_index(name); }

  // Does the whole of `input` match?
  Result match(StringView input, Match* out, Limits limits = {}) const;
  // Does a match start at `at` (not necessarily reaching the end)?
  Result match_prefix(StringView input, Match* out, uint64_t at = 0, Limits limits = {}) const;
  // Finds the first match at or after `from`.
  Result search(StringView input, Match* out, uint64_t from = 0, Limits limits = {}) const;
  // True if the expression matches anywhere in `input`.
  bool contains(StringView input) const;

  // Copies `input` to `out` with the first match (or, if `all`, every match) replaced by
  // `replacement`, in which $& is the match, $1..$9 are groups, $<name> a named group, $$ a dollar.
  // False if memory ran out or a search hit its limit (`out` is then incomplete).
  bool replace(StringView input, StringView replacement, bool all, String* out, Limits limits = {}) const;

 private:
  allocators::IAllocator* allocator_;
  HeapVec<Inst> code_;
  HeapVec<Range> ranges_;
  HeapVec<NameEntry> names_;
  uint32_t groups_ = 1;
  Flags flags_ = Flags::None;
  Error error_ = Error::None;
  bool valid_ = false;
};

}  // namespace regex
