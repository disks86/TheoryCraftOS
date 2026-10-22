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
#include "Regex/RegexProgram.hpp"

namespace regex {

// Turns pattern text into a Program, writing into the three vectors it is given
// (FixedVec while compiling at compile time, HeapVec at run time - this is the
// same code either way).
//
// Syntax (ECMAScript-like):
//   literals, .  [set] [^set] [a-z]  \d \D \w \W \s \S  \n \r \t \f \v \0 \xHH \uHHHH \u{H...}
//   ^ $ \b \B \A \z      (? ... ): groups  (...) (?:...) (?<name>...)  \1 \k<name>
//   a|b   * + ? {n} {n,} {n,m} and the lazy forms *? +? ?? {n,m}?
//   lookahead (?=...) (?!...)
// Not supported: lookbehind, possessive quantifiers, inline flags, Unicode properties.
//
// The parser is one pass that emits code directly: a quantifier is applied by
// inserting a Split in front of the piece it follows and copying that piece,
// which relative jumps make a plain copy.
template <class CodeVec, class RangeVec, class NameVec>
class Compiler {
 public:
  constexpr Compiler(CodeVec& code, RangeVec& ranges, NameVec& names) : code_(code), ranges_(ranges), names_(names) {}

  constexpr Error compile(StringView pattern, Flags flags) { return compile(pattern.data(), pattern.size(), flags); }
  // The same from a pointer and length (what a template-argument string gives, which StringView's null check cannot take in a constant expression).
  constexpr Error compile(const char* text, uint64_t length, Flags flags) {
    text_ = text;
    length_ = length;
    pos_ = 0;
    flags_ = flags;
    utf8_ = has_flag(flags, Flags::Utf8);
    fold_ = has_flag(flags, Flags::IgnoreCase);
    error_ = Error::None;
    groups_ = 1;
    loops_ = 0;
    look_depth_ = 0;
    bool nullable = false;
    if (!parse_alternation(&nullable, 0)) return error_;
    if (pos_ != length_) {  // a ')' with nothing open
      fail(Error::UnbalancedParen);
      return error_;
    }
    if (!emit(Op::Match)) return error_;
    return Error::None;
  }

  constexpr uint32_t group_count() const { return groups_; }
  constexpr Flags flags() const { return flags_; }

 private:
  // ---- emitting
  constexpr bool fail(Error e) {
    if (error_ == Error::None) error_ = e;
    return false;
  }
  constexpr bool fail_full() { return fail(code_.oom() || ranges_.oom() || names_.oom() ? Error::OutOfMemory : Error::ProgramTooLarge); }
  constexpr bool emit(Op op, int32_t a = 0, int32_t b = 0) {
    Inst i;
    i.op = op;
    i.a = a;
    i.b = b;
    return code_.push(i) || fail_full();
  }
  constexpr bool insert_at(uint64_t at, Op op, int32_t a, int32_t b) {
    Inst i;
    i.op = op;
    i.a = a;
    i.b = b;
    return code_.insert(at, i) || fail_full();
  }
  // Appends a copy of code_[from, from+count).
  constexpr bool append_copy(uint64_t from, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
      const Inst inst = code_[from + i];
      if (!code_.push(inst)) return fail_full();
    }
    return true;
  }
  constexpr bool emit_char(uint32_t cp) {
    if (fold_ && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')))
      return emit(Op::CharFold, static_cast<int32_t>(cp | 0x20));
    return emit(Op::Char, static_cast<int32_t>(cp));
  }
  constexpr bool add_range(uint32_t lo, uint32_t hi) {
    Range r;
    r.lo = lo;
    r.hi = hi;
    if (!ranges_.push(r)) return fail_full();
    if (fold_) {  // the other case of any letters in the range
      uint32_t a = lo > 'a' ? lo : 'a', b = hi < 'z' ? hi : 'z';
      if (a <= b && !push_range(a - 32, b - 32)) return false;
      a = lo > 'A' ? lo : 'A';
      b = hi < 'Z' ? hi : 'Z';
      if (a <= b && !push_range(a + 32, b + 32)) return false;
    }
    return true;
  }
  constexpr bool push_range(uint32_t lo, uint32_t hi) {
    Range r;
    r.lo = lo;
    r.hi = hi;
    return ranges_.push(r) || fail_full();
  }
  // \d \w \s and their complements, as ranges.
  constexpr bool add_set(char kind) {
    switch (kind) {
      case 'd': return add_range('0', '9');
      case 'D': return add_range(0, '0' - 1) && add_range('9' + 1, kMaxCodePoint);
      case 'w': return add_range('0', '9') && add_range('A', 'Z') && add_range('_', '_') && add_range('a', 'z');
      case 'W':
        return add_range(0, '0' - 1) && add_range('9' + 1, 'A' - 1) && add_range('Z' + 1, '_' - 1) && add_range('_' + 1, 'a' - 1) &&
               add_range('z' + 1, kMaxCodePoint);
      case 's': return add_range(9, 13) && add_range(' ', ' ');
      default: return add_range(0, 8) && add_range(14, 31) && add_range('!', kMaxCodePoint);  // 'S'
    }
  }

  // ---- reading the pattern
  constexpr bool at_end() const { return pos_ >= length_; }
  constexpr char peek(uint64_t ahead = 0) const { return pos_ + ahead < length_ ? text_[pos_ + ahead] : '\0'; }
  constexpr bool accept(char c) {
    if (pos_ < length_ && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  // The next character: a byte, or a whole UTF-8 sequence in Utf8 mode.
  constexpr bool read_char(uint32_t* cp) {
    const uint8_t lead = static_cast<uint8_t>(text_[pos_]);
    if (!utf8_ || lead < 0x80) {
      *cp = lead;
      ++pos_;
      return true;
    }
    uint32_t n = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    if (n == 0 || lead >= 0xF8) return fail(Error::BadUtf8);
    uint32_t v = lead & (0x3F >> n);
    for (uint32_t i = 1; i <= n; ++i) {
      if (pos_ + i >= length_) return fail(Error::BadUtf8);
      const uint8_t c = static_cast<uint8_t>(text_[pos_ + i]);
      if ((c & 0xC0) != 0x80) return fail(Error::BadUtf8);
      v = (v << 6) | (c & 0x3F);
    }
    pos_ += n + 1;
    *cp = v;
    return true;
  }
  static constexpr int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  constexpr bool read_hex(uint32_t digits, uint32_t* out) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < digits; ++i) {
      const int h = hex_value(peek());
      if (h < 0) return fail(Error::BadEscape);
      v = (v << 4) | static_cast<uint32_t>(h);
      ++pos_;
    }
    *out = v;
    return true;
  }
  static constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
  static constexpr bool is_name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }

  // ---- escapes
  // After a backslash that is not a class shorthand, assertion or backreference:
  // the character it stands for.
  constexpr bool read_escaped_char(uint32_t* cp) {
    if (at_end()) return fail(Error::TrailingBackslash);
    const char c = peek();
    switch (c) {
      case 'n': ++pos_; *cp = '\n'; return true;
      case 'r': ++pos_; *cp = '\r'; return true;
      case 't': ++pos_; *cp = '\t'; return true;
      case 'f': ++pos_; *cp = '\f'; return true;
      case 'v': ++pos_; *cp = '\v'; return true;
      case '0':
        ++pos_;
        if (is_digit(peek())) return fail(Error::BadEscape);
        *cp = 0;
        return true;
      case 'x': ++pos_; return read_hex(2, cp);
      case 'u':
        ++pos_;
        if (accept('{')) {
          uint32_t v = 0, n = 0;
          while (hex_value(peek()) >= 0) {
            v = (v << 4) | static_cast<uint32_t>(hex_value(peek()));
            ++pos_;
            if (++n > 6 || v > kMaxCodePoint) return fail(Error::BadEscape);
          }
          if (n == 0 || !accept('}')) return fail(Error::BadEscape);
          *cp = v;
          return true;
        }
        return read_hex(4, cp);
      default:
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c)) return fail(Error::BadEscape);
        return read_char(cp);  // an escaped punctuation mark stands for itself
    }
  }

  // ---- grammar
  // alternation := branch ('|' branch)*
  constexpr bool parse_alternation(bool* nullable, uint32_t depth) {
    if (depth > kMaxNesting) return fail(Error::NestingTooDeep);
    uint64_t branch_start = code_.size();
    int64_t last_jmp = -1;  // the jumps to the end form a chain through their `a` fields until it is known
    bool any_nullable = false;
    for (;;) {
      bool n = false;
      if (!parse_branch(&n, depth)) return false;
      any_nullable = any_nullable || n;
      if (!accept('|')) break;
      if (!insert_at(branch_start, Op::Split, 1, 0)) return false;
      const int64_t jmp = static_cast<int64_t>(code_.size());
      code_[branch_start].b = static_cast<int32_t>(jmp + 1 - static_cast<int64_t>(branch_start));
      if (!emit(Op::Jmp, last_jmp >= 0 ? static_cast<int32_t>(jmp - last_jmp) : 0)) return false;
      last_jmp = jmp;
      branch_start = code_.size();
    }
    const int64_t end = static_cast<int64_t>(code_.size());
    while (last_jmp >= 0) {
      const int32_t back = code_[last_jmp].a;
      code_[last_jmp].a = static_cast<int32_t>(end - last_jmp);
      last_jmp = back ? last_jmp - back : -1;
    }
    *nullable = any_nullable;
    return true;
  }

  // branch := term*
  constexpr bool parse_branch(bool* nullable, uint32_t depth) {
    bool all = true;
    while (!at_end() && peek() != '|' && peek() != ')') {
      bool n = false;
      if (!parse_term(&n, depth)) return false;
      all = all && n;
    }
    *nullable = all;
    return true;
  }

  // term := atom quantifier?
  constexpr bool parse_term(bool* nullable, uint32_t depth) {
    const uint64_t start = code_.size();
    bool repeatable = true;
    bool atom_nullable = false;
    const char c = peek();
    switch (c) {
      case '*':
      case '+':
      case '?': return fail(Error::NothingToRepeat);
      case '{': {
        uint32_t lo, hi;
        uint64_t length;
        if (read_braces(pos_, &lo, &hi, &length)) return fail(Error::NothingToRepeat);
        ++pos_;
        if (!emit_char('{')) return false;
        break;
      }
      case '.':
        ++pos_;
        if (!emit(has_flag(flags_, Flags::DotAll) ? Op::AnyAll : Op::Any)) return false;
        break;
      case '^':
        ++pos_;
        if (!emit(has_flag(flags_, Flags::Multiline) ? Op::LineBegin : Op::TextBegin)) return false;
        repeatable = false;
        break;
      case '$':
        ++pos_;
        if (!emit(has_flag(flags_, Flags::Multiline) ? Op::LineEnd : Op::TextEnd)) return false;
        repeatable = false;
        break;
      case '[':
        ++pos_;
        if (!parse_class()) return false;
        break;
      case '(':
        ++pos_;
        if (!parse_group(&atom_nullable, &repeatable, depth)) return false;
        break;
      case '\\':
        ++pos_;
        if (!parse_escape(&atom_nullable, &repeatable)) return false;
        break;
      default: {
        uint32_t cp;
        if (!read_char(&cp) || !emit_char(cp)) return false;
        break;
      }
    }
    const char q = peek();
    if (q == '*' || q == '+' || q == '?' || (q == '{' && quantifier_ahead())) {
      if (!repeatable) return fail(Error::NothingToRepeat);
      return parse_quantifier(start, atom_nullable, nullable);
    }
    *nullable = atom_nullable || !repeatable;  // an assertion matches the empty string
    return true;
  }

  // \ ... outside a class
  constexpr bool parse_escape(bool* nullable, bool* repeatable) {
    if (at_end()) return fail(Error::TrailingBackslash);
    const char c = peek();
    switch (c) {
      case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
        ++pos_;
        const uint64_t first = ranges_.size();
        if (!add_set(c)) return false;
        return emit(Op::Class, static_cast<int32_t>(first), static_cast<int32_t>(ranges_.size() - first));
      }
      case 'b': ++pos_; *repeatable = false; return emit(Op::WordBoundary);
      case 'B': ++pos_; *repeatable = false; return emit(Op::NotWordBoundary);
      case 'A': ++pos_; *repeatable = false; return emit(Op::TextBegin);
      case 'z': ++pos_; *repeatable = false; return emit(Op::TextEnd);
      case 'k': {
        ++pos_;
        if (!accept('<')) return fail(Error::BadGroupName);
        const uint64_t name_start = pos_;
        while (!at_end() && peek() != '>') ++pos_;
        if (at_end()) return fail(Error::BadGroupName);
        const int32_t g = find_name(name_start, pos_ - name_start);
        ++pos_;
        if (g < 0) return fail(Error::BadBackreference);
        *nullable = true;
        return emit(fold_ ? Op::BackrefFold : Op::Backref, g);
      }
      default: break;
    }
    if (c >= '1' && c <= '9') {
      uint32_t n = 0;
      while (is_digit(peek())) {
        n = n * 10 + static_cast<uint32_t>(peek() - '0');
        if (n >= kMaxGroups) return fail(Error::BadBackreference);
        ++pos_;
      }
      if (n >= groups_) return fail(Error::BadBackreference);
      *nullable = true;
      return emit(fold_ ? Op::BackrefFold : Op::Backref, static_cast<int32_t>(n));
    }
    uint32_t cp;
    return read_escaped_char(&cp) && emit_char(cp);
  }

  // After '[': the set up to and including ']'.
  constexpr bool parse_class() {
    const bool negate = accept('^');
    const uint64_t first = ranges_.size();
    for (;;) {
      if (at_end()) return fail(Error::UnterminatedClass);
      if (accept(']')) break;
      uint32_t lo;
      char set = 0;
      if (!parse_class_atom(&lo, &set)) return false;
      if (set) {
        if (!add_set(set)) return false;
        continue;
      }
      uint32_t hi = lo;
      if (peek() == '-' && peek(1) != ']' && pos_ + 1 < length_) {
        ++pos_;
        char hi_set = 0;
        if (!parse_class_atom(&hi, &hi_set)) return false;
        if (hi_set || hi < lo) return fail(Error::BadClassRange);
      }
      if (!add_range(lo, hi)) return false;
    }
    return emit(negate ? Op::ClassNot : Op::Class, static_cast<int32_t>(first), static_cast<int32_t>(ranges_.size() - first));
  }
  constexpr bool parse_class_atom(uint32_t* cp, char* set) {
    if (!accept('\\')) return read_char(cp);
    if (at_end()) return fail(Error::TrailingBackslash);
    const char c = peek();
    if (c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S') {
      ++pos_;
      *set = c;
      return true;
    }
    if (c == 'b') {
      ++pos_;
      *cp = 8;  // backspace, inside a class
      return true;
    }
    if (c == '-') {
      ++pos_;
      *cp = '-';
      return true;
    }
    return read_escaped_char(cp);
  }

  // After '(': a group.
  constexpr bool parse_group(bool* nullable, bool* repeatable, uint32_t depth) {
    if (accept('?')) {
      if (accept(':')) return parse_group_body(nullable, depth);
      if (peek() == '=' || peek() == '!') {
        const bool negate = peek() == '!';
        ++pos_;
        if (look_depth_ >= kMaxLookDepth) return fail(Error::NestingTooDeep);
        ++look_depth_;
        const uint64_t look = code_.size();
        if (!emit(Op::Look, 0, negate ? 1 : 0)) return false;
        bool inner = false;
        if (!parse_alternation(&inner, depth + 1)) return false;
        if (!accept(')')) return fail(Error::UnbalancedParen);
        if (!emit(Op::LookEnd)) return false;
        code_[look].a = static_cast<int32_t>(code_.size() - look);
        --look_depth_;
        *nullable = true;
        *repeatable = false;
        return true;
      }
      if (accept('<')) {
        if (peek() == '=' || peek() == '!') return fail(Error::Unsupported);
        const uint64_t name_start = pos_;
        if (!is_name_start(peek())) return fail(Error::BadGroupName);
        while (!at_end() && peek() != '>') {
          const char c = peek();
          if (!is_name_start(c) && !is_digit(c)) return fail(Error::BadGroupName);
          ++pos_;
        }
        if (at_end()) return fail(Error::BadGroupName);
        const uint64_t name_length = pos_ - name_start;
        ++pos_;
        if (name_length > kMaxNameLength) return fail(Error::BadGroupName);
        if (find_name(name_start, name_length) >= 0) return fail(Error::DuplicateGroupName);
        if (groups_ >= kMaxGroups) return fail(Error::TooManyGroups);
        NameEntry entry;
        for (uint64_t i = 0; i < name_length; ++i) entry.text[i] = text_[name_start + i];
        entry.length = static_cast<uint32_t>(name_length);
        entry.group = groups_;
        if (!names_.push(entry)) return fail_full();
        return parse_capture(nullable, depth);
      }
      return fail(Error::BadGroup);
    }
    return parse_capture(nullable, depth);
  }
  constexpr bool parse_capture(bool* nullable, uint32_t depth) {
    if (groups_ >= kMaxGroups) return fail(Error::TooManyGroups);
    const uint32_t g = groups_++;
    if (!emit(Op::Save, static_cast<int32_t>(g * 2)) || !parse_group_body(nullable, depth)) return false;
    return emit(Op::Save, static_cast<int32_t>(g * 2 + 1));
  }
  // The alternation and closing parenthesis of a group.
  constexpr bool parse_group_body(bool* nullable, uint32_t depth) {
    if (!parse_alternation(nullable, depth + 1)) return false;
    return accept(')') || fail(Error::UnbalancedParen);
  }
  constexpr int32_t find_name(uint64_t start, uint64_t length) const {
    for (uint64_t i = 0; i < names_.size(); ++i) {
      const NameEntry& e = names_[i];
      if (e.length != length) continue;
      bool same = true;
      for (uint64_t k = 0; k < length && same; ++k) same = e.text[k] == text_[start + k];
      if (same) return static_cast<int32_t>(e.group);
    }
    return -1;
  }

  // ---- quantifiers
  // {n} {n,} {n,m} starting at text_[at] == '{'. On success *length is its size.
  constexpr bool read_braces(uint64_t at, uint32_t* lo, uint32_t* hi, uint64_t* length) const {
    uint64_t i = at + 1;
    auto number = [&](uint32_t* out) {
      if (i >= length_ || !is_digit(text_[i])) return false;
      uint64_t v = 0;
      while (i < length_ && is_digit(text_[i])) {
        v = v * 10 + static_cast<uint64_t>(text_[i] - '0');
        if (v > 0xFFFFFF) v = 0xFFFFFF;  // saturate: reported as too large by the caller
        ++i;
      }
      *out = static_cast<uint32_t>(v);
      return true;
    };
    if (!number(lo)) return false;
    *hi = *lo;
    if (i < length_ && text_[i] == ',') {
      ++i;
      if (i < length_ && text_[i] == '}') *hi = ~uint32_t{0};  // unbounded
      else if (!number(hi)) return false;
    }
    if (i >= length_ || text_[i] != '}') return false;
    *length = i + 1 - at;
    return true;
  }
  constexpr bool quantifier_ahead() const {
    uint32_t lo, hi;
    uint64_t length;
    return read_braces(pos_, &lo, &hi, &length);
  }

  constexpr bool parse_quantifier(uint64_t start, bool body_nullable, bool* nullable) {
    uint32_t lo = 0, hi = 0;
    const char q = peek();
    if (q == '*') { ++pos_; lo = 0; hi = ~uint32_t{0}; }
    else if (q == '+') { ++pos_; lo = 1; hi = ~uint32_t{0}; }
    else if (q == '?') { ++pos_; lo = 0; hi = 1; }
    else {
      uint64_t length = 0;
      read_braces(pos_, &lo, &hi, &length);
      pos_ += length;
    }
    const bool greedy = !accept('?');
    constexpr uint32_t kInfinite = ~uint32_t{0};
    if (hi != kInfinite && hi < lo) return fail(Error::BadRepeat);
    if (lo > kMaxRepeat || (hi != kInfinite && hi > kMaxRepeat)) return fail(Error::RepeatTooLarge);
    *nullable = body_nullable || lo == 0;
    return apply_repeat(start, lo, hi, greedy, body_nullable);
  }

  // Repeats code_[start, end) lo..hi times.
  constexpr bool apply_repeat(uint64_t start, uint32_t lo, uint32_t hi, bool greedy, bool body_nullable) {
    constexpr uint32_t kInfinite = ~uint32_t{0};
    const uint64_t length = code_.size() - start;
    if (hi == 0) {
      code_.truncate(start);
      return true;
    }
    if (hi == kInfinite) {
      if (lo >= 1 && !body_nullable) {
        // e{n,} = e^(n-1) followed by "e, then maybe again".
        uint64_t last = start;
        for (uint32_t i = 1; i < lo; ++i) {
          last = code_.size();
          if (!append_copy(start, length)) return false;
        }
        const int32_t back = static_cast<int32_t>(last) - static_cast<int32_t>(code_.size());
        return emit(Op::Split, greedy ? back : 1, greedy ? 1 : back);
      }
      // e{n,} = e^n e*, with the star over its own copy (or the body itself when n is 0).
      uint64_t star = start;
      if (lo >= 1) {
        for (uint32_t i = 1; i < lo; ++i)
          if (!append_copy(start, length)) return false;
        star = code_.size();
        if (!append_copy(start, length)) return false;
      }
      return wrap_star(star, greedy, body_nullable);
    }
    // Finite: lo required copies, then hi-lo optional ones, nested so that each one
    // is only tried if the one before it matched. The Splits jump to the end; until it
    // is known they form a chain through their `b` fields.
    int64_t last_split = -1;
    uint32_t optional = hi - lo;
    if (lo == 0) {
      if (!insert_at(start, Op::Split, 0, 0)) return false;
      last_split = static_cast<int64_t>(start);
      --optional;
    } else {
      for (uint32_t i = 1; i < lo; ++i)
        if (!append_copy(start, length)) return false;
    }
    for (uint32_t i = 0; i < optional; ++i) {
      const int64_t at = static_cast<int64_t>(code_.size());
      if (!emit(Op::Split, 0, last_split >= 0 ? static_cast<int32_t>(at - last_split) : 0)) return false;
      last_split = at;
      if (!append_copy(start + (lo == 0 ? 1 : 0), length)) return false;
    }
    const int64_t end = static_cast<int64_t>(code_.size());
    while (last_split >= 0) {
      const int32_t back = code_[last_split].b;
      const int32_t to_end = static_cast<int32_t>(end - last_split);
      code_[last_split].a = greedy ? 1 : to_end;
      code_[last_split].b = greedy ? to_end : 1;
      last_split = back ? last_split - back : -1;
    }
    return true;
  }

  // Wraps code_[at, end) in a loop that may run zero times. A body that can match
  // nothing is guarded so that an empty iteration ends the loop instead of spinning.
  constexpr bool wrap_star(uint64_t at, bool greedy, bool body_nullable) {
    const int32_t length = static_cast<int32_t>(code_.size() - at);
    if (!body_nullable) {
      // Split exit, body, Jmp back
      if (!insert_at(at, Op::Split, greedy ? 1 : length + 2, greedy ? length + 2 : 1)) return false;
      return emit(Op::Jmp, -(length + 1));
    }
    if (loops_ >= kMaxLoops) return fail(Error::TooManyLoops);
    const int32_t slot = static_cast<int32_t>(kLoopSlotBase + loops_++);
    // Split, LoopEnter, body, LoopCheck, Jmp back
    if (!insert_at(at, Op::LoopEnter, slot, 0)) return false;
    if (!insert_at(at, Op::Split, greedy ? 1 : length + 4, greedy ? length + 4 : 1)) return false;
    return emit(Op::LoopCheck, slot) && emit(Op::Jmp, -(length + 3));
  }

 public:
  constexpr uint32_t loop_count() const { return loops_; }

 private:
  CodeVec& code_;
  RangeVec& ranges_;
  NameVec& names_;
  const char* text_ = nullptr;
  uint64_t length_ = 0, pos_ = 0;
  Flags flags_ = Flags::None;
  bool utf8_ = false, fold_ = false;
  Error error_ = Error::None;
  uint32_t groups_ = 1, loops_ = 0, look_depth_ = 0;
};

}  // namespace regex
