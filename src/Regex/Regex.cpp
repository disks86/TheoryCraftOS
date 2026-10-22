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

#include "Regex/Regex.hpp"

namespace regex {

Regex::Regex(allocators::IAllocator& allocator)
    : allocator_(&allocator), code_(allocator, kMaxProgram), ranges_(allocator, kMaxRanges), names_(allocator, kMaxGroups) {}

Error Regex::compile(StringView pattern, Flags flags) {
  HeapVec<Inst> code(*allocator_, kMaxProgram);
  HeapVec<Range> ranges(*allocator_, kMaxRanges);
  HeapVec<NameEntry> names(*allocator_, kMaxGroups);
  Compiler<HeapVec<Inst>, HeapVec<Range>, HeapVec<NameEntry>> compiler(code, ranges, names);
  const Error e = compiler.compile(pattern, flags);
  if (e != Error::None) {
    valid_ = false;
    error_ = e;
    code_.truncate(0);
    ranges_.truncate(0);
    names_.truncate(0);
    return e;
  }
  code_ = static_cast<HeapVec<Inst>&&>(code);
  ranges_ = static_cast<HeapVec<Range>&&>(ranges);
  names_ = static_cast<HeapVec<NameEntry>&&>(names);
  groups_ = compiler.group_count();
  flags_ = flags;
  error_ = Error::None;
  valid_ = true;
  return Error::None;
}

Program Regex::program() const {
  Program p;
  p.code = code_.view();
  p.ranges = ranges_.view();
  p.names = names_.view();
  p.group_count = groups_;
  p.flags = flags_;
  return p;
}

namespace {

Result run(const Regex& re, allocators::IAllocator& allocator, StringView input, uint64_t from, bool anchored, bool whole, Match* out,
           Limits limits) {
  if (!re.valid()) return Result::NoMatch;
  HeapVec<Frame> stack(allocator, limits.max_stack);
  const Program p = re.program();
  Matcher<HeapVec<Frame>> matcher(p, input, stack, limits);
  Match scratch;
  return matcher.search(from, anchored, whole, out ? out : &scratch);
}

}  // namespace

Result Regex::match(StringView input, Match* out, Limits limits) const {
  return run(*this, *allocator_, input, 0, true, true, out, limits);
}
Result Regex::match_prefix(StringView input, Match* out, uint64_t at, Limits limits) const {
  return run(*this, *allocator_, input, at, true, false, out, limits);
}
Result Regex::search(StringView input, Match* out, uint64_t from, Limits limits) const {
  return run(*this, *allocator_, input, from, false, false, out, limits);
}
bool Regex::contains(StringView input) const {
  Match m;
  return search(input, &m) == Result::Matched;
}

bool Regex::replace(StringView input, StringView replacement, bool all, String* out, Limits limits) const {
  uint64_t copied = 0, from = 0;
  while (from <= input.size()) {
    Match m;
    const Result r = search(input, &m, from, limits);
    if (r == Result::NoMatch) break;
    if (r != Result::Matched) return false;
    if (!out->append(input.substr(copied, m.begin() - copied))) return false;
    for (uint64_t i = 0; i < replacement.size(); ++i) {
      const char c = replacement[i];
      if (c != '$' || i + 1 >= replacement.size()) {
        if (!out->push_back(c)) return false;
        continue;
      }
      const char n = replacement[i + 1];
      if (n == '$') {
        if (!out->push_back('$')) return false;
        ++i;
      } else if (n == '&') {
        if (!out->append(m.text(input, 0))) return false;
        ++i;
      } else if (n >= '0' && n <= '9') {
        const uint32_t g = static_cast<uint32_t>(n - '0');
        if (g == 0 || g >= m.count) {
          if (!out->push_back('$')) return false;  // not a group: left as written
          continue;
        }
        if (!out->append(m.text(input, g))) return false;
        ++i;
      } else if (n == '<') {
        const uint64_t close = replacement.find('>', i + 2);
        const int32_t g = close == StringView::npos ? -1 : group_index(replacement.substr(i + 2, close - i - 2));
        if (g < 0) {
          if (!out->push_back('$')) return false;
          continue;
        }
        if (!out->append(m.text(input, static_cast<uint32_t>(g)))) return false;
        i = close;
      } else if (!out->push_back('$')) {
        return false;
      }
    }
    copied = m.end();
    if (!all) break;
    // Continue after the match; after an empty one, a character further so it cannot repeat forever.
    from = m.end() > m.begin() ? m.end() : m.end() + 1;
    if (m.end() == m.begin() && m.end() < input.size() && has_flag(flags_, Flags::Utf8)) {
      while (from < input.size() && (static_cast<uint8_t>(input[from]) & 0xC0) == 0x80) ++from;
    }
  }
  return out->append(input.substr(copied < input.size() ? copied : input.size()));
}

}  // namespace regex
