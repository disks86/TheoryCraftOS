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

// Bounds on a single search, so a pattern like (a*)*b on a long run of a's ends in
// an error instead of a hang.
struct Limits {
  uint64_t max_steps = 50'000'000;  // instructions executed over the whole search
  uint64_t max_stack = 1u << 18;    // backtracking frames alive at once (16 bytes each)
};

enum class Result : uint8_t {
  Matched,
  NoMatch,
  StepLimit,   // Limits::max_steps ran out
  StackLimit,  // Limits::max_stack (or memory for it) ran out
};

// Where the groups of a successful match are, as byte offsets into the input.
struct Match {
  uint32_t count = 0;  // groups, group 0 (the whole match) included
  uint64_t begins[kMaxGroups] = {};
  uint64_t ends[kMaxGroups] = {};

  constexpr bool has(uint32_t group) const { return group < count && begins[group] != kUnset && ends[group] != kUnset; }
  constexpr uint64_t begin(uint32_t group = 0) const { return begins[group]; }
  constexpr uint64_t end(uint32_t group = 0) const { return ends[group]; }
  constexpr uint64_t length(uint32_t group = 0) const { return has(group) ? ends[group] - begins[group] : 0; }
  // The text of a group (empty if it did not take part in the match).
  constexpr StringView text(StringView input, uint32_t group = 0) const {
    return has(group) ? StringView(input.data() + begins[group], ends[group] - begins[group]) : StringView();
  }
};

struct Frame {
  uint32_t kind = 0;  // 0: resume at pc with position `value`; 1: put `value` back in slot `index`
  uint32_t index = 0;
  uint64_t value = 0;
};

// Runs a Program over some text by backtracking. `Stack` is a FixedVec<Frame, N> or
// HeapVec<Frame>; it holds the choices still to be tried, so the depth of the search is
// bounded by it rather than by the call stack.
template <class Stack>
class Matcher {
 public:
  constexpr Matcher(const Program& program, StringView input, Stack& stack, Limits limits)
      : program_(program), input_(input.data()), size_(input.size()), stack_(stack), limits_(limits) {
    utf8_ = has_flag(program.flags, Flags::Utf8);
  }

  // Finds the first match starting at or after `from`. `anchored` only tries `from` itself;
  // `whole` additionally demands that the match runs to the end of the input.
  constexpr Result search(uint64_t from, bool anchored, bool whole, Match* out) {
    whole_ = whole;
    steps_ = 0;
    if (from > size_) return Result::NoMatch;
    // A program that must start with a fixed ASCII character can skip to the next one.
    int32_t first = -1;
    bool first_fold = false;
    if (!anchored && program_.code.size() > 0) {
      const Inst& i0 = program_.code[0];
      if ((i0.op == Op::Char || i0.op == Op::CharFold) && i0.a < 0x80) {
        first = i0.a;
        first_fold = i0.op == Op::CharFold;
      }
    }
    const uint32_t capture_slots = program_.group_count * 2;
    for (uint64_t start = from;;) {
      if (first >= 0) {
        while (start < size_ && !is_first(start, first, first_fold)) ++start;
        if (start >= size_) return Result::NoMatch;
      }
      stack_.truncate(0);
      for (uint32_t i = 0; i < capture_slots; ++i) slots_[i] = kUnset;
      uint64_t end = 0;
      const Run r = run(0, start, &end, 0);
      if (r == Run::Matched) {
        slots_[0] = start;
        slots_[1] = end;
        out->count = program_.group_count;
        for (uint32_t g = 0; g < program_.group_count; ++g) {
          out->begins[g] = slots_[g * 2];
          out->ends[g] = slots_[g * 2 + 1];
        }
        return Result::Matched;
      }
      if (r == Run::Steps) return Result::StepLimit;
      if (r == Run::StackFull) return Result::StackLimit;
      if (anchored || start >= size_) return Result::NoMatch;
      uint32_t cp, n;
      start += peek_char(start, &cp, &n) ? n : 1;
    }
  }

 private:
  enum class Run : uint8_t { Matched, Failed, Steps, StackFull };

  constexpr bool is_first(uint64_t at, int32_t first, bool fold) const {
    const int32_t c = static_cast<uint8_t>(input_[at]);
    return c == first || (fold && (c | 0x20) == first);
  }

  // The character at `at`: its code point and byte length. False at the end of the input.
  constexpr bool peek_char(uint64_t at, uint32_t* cp, uint32_t* len) const {
    if (at >= size_) return false;
    const uint8_t lead = static_cast<uint8_t>(input_[at]);
    *cp = lead;
    *len = 1;
    if (!utf8_ || lead < 0xC0 || lead >= 0xF8) return true;
    const uint32_t n = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : 1;
    if (at + n >= size_) return true;  // truncated: a stray byte
    uint32_t v = lead & (0x3F >> n);
    for (uint32_t i = 1; i <= n; ++i) {
      const uint8_t c = static_cast<uint8_t>(input_[at + i]);
      if ((c & 0xC0) != 0x80) return true;  // malformed: treat the lead byte as a character
      v = (v << 6) | (c & 0x3F);
    }
    *cp = v;
    *len = n + 1;
    return true;
  }
  // The character that ends at `at`: its code point and byte length. False at the start of the input.
  constexpr bool peek_before(uint64_t at, uint32_t* cp, uint32_t* len) const {
    if (at == 0) return false;
    uint64_t i = at - 1;
    if (utf8_)
      while (i > 0 && at - i < 4 && (static_cast<uint8_t>(input_[i]) & 0xC0) == 0x80) --i;
    uint32_t c, n;
    if (peek_char(i, &c, &n) && i + n == at) {
      *cp = c;
      *len = n;
      return true;
    }
    *cp = static_cast<uint8_t>(input_[at - 1]);  // not a whole sequence: the last byte alone
    *len = 1;
    return true;
  }
  static constexpr bool is_line_break(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
  static constexpr bool is_word(uint32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
  constexpr bool word_at(uint64_t at) const { return at < size_ && is_word(static_cast<uint8_t>(input_[at])); }

  constexpr bool in_class(const Inst& inst, uint32_t cp) const {
    for (int32_t i = 0; i < inst.b; ++i) {
      const Range& r = program_.ranges[static_cast<uint64_t>(inst.a + i)];
      if (cp >= r.lo && cp <= r.hi) return true;
    }
    return false;
  }

  constexpr bool push(uint32_t kind, uint32_t index, uint64_t value) {
    if (stack_.size() >= limits_.max_stack) return false;
    Frame f;
    f.kind = kind;
    f.index = index;
    f.value = value;
    return stack_.push(f);
  }
  constexpr void set_slot(uint32_t slot, uint64_t value) { slots_[slot] = value; }

  // Executes from `pc` at `pos` until Match/LookEnd (Matched) or until every choice made
  // since entry has been tried (Failed). On Matched the frames pushed since entry are
  // still on the stack (the capture undo records matter to the caller).
  constexpr Run run(uint32_t pc, uint64_t pos, uint64_t* end, uint32_t depth) {
    const uint64_t base = stack_.size();
    for (;;) {
      bool ok = true;
      if (++steps_ > limits_.max_steps) return Run::Steps;
      const Inst& inst = program_.code[pc];
      uint32_t cp = 0, n = 0;
      switch (inst.op) {
        case Op::Match:
          if (whole_ && depth == 0 && pos != size_) {
            ok = false;
            break;
          }
          *end = pos;
          return Run::Matched;
        case Op::LookEnd:
          *end = pos;
          return Run::Matched;
        case Op::Char:
          ok = peek_char(pos, &cp, &n) && cp == static_cast<uint32_t>(inst.a);
          if (ok) { pos += n; ++pc; }
          break;
        case Op::CharFold:
          ok = peek_char(pos, &cp, &n) && (cp | 0x20) == static_cast<uint32_t>(inst.a) && cp < 0x80;
          if (ok) { pos += n; ++pc; }
          break;
        case Op::Any:
          ok = peek_char(pos, &cp, &n) && !is_line_break(cp);
          if (ok) { pos += n; ++pc; }
          break;
        case Op::AnyAll:
          ok = peek_char(pos, &cp, &n);
          if (ok) { pos += n; ++pc; }
          break;
        case Op::Class:
        case Op::ClassNot:
          ok = peek_char(pos, &cp, &n) && in_class(inst, cp) == (inst.op == Op::Class);
          if (ok) { pos += n; ++pc; }
          break;
        case Op::CharBack:
          ok = peek_before(pos, &cp, &n) && cp == static_cast<uint32_t>(inst.a);
          if (ok) { pos -= n; ++pc; }
          break;
        case Op::CharFoldBack:
          ok = peek_before(pos, &cp, &n) && (cp | 0x20) == static_cast<uint32_t>(inst.a) && cp < 0x80;
          if (ok) { pos -= n; ++pc; }
          break;
        case Op::AnyBack:
          ok = peek_before(pos, &cp, &n) && !is_line_break(cp);
          if (ok) { pos -= n; ++pc; }
          break;
        case Op::AnyAllBack:
          ok = peek_before(pos, &cp, &n);
          if (ok) { pos -= n; ++pc; }
          break;
        case Op::ClassBack:
        case Op::ClassNotBack:
          ok = peek_before(pos, &cp, &n) && in_class(inst, cp) == (inst.op == Op::ClassBack);
          if (ok) { pos -= n; ++pc; }
          break;
        case Op::BackrefBack:
        case Op::BackrefFoldBack: {
          const uint64_t s = slots_[inst.a * 2], e = slots_[inst.a * 2 + 1];
          if (s != kUnset && e != kUnset && e > s) {
            const uint64_t len = e - s;
            ok = pos >= len;
            for (uint64_t i = 0; ok && i < len; ++i) {
              const uint8_t x = static_cast<uint8_t>(input_[s + i]), y = static_cast<uint8_t>(input_[pos - len + i]);
              ok = x == y || (inst.op == Op::BackrefFoldBack && x < 0x80 && y < 0x80 && is_letter(x) && (x | 0x20) == (y | 0x20));
            }
            if (ok) pos -= len;
          }
          ++pc;
          break;
        }
        case Op::Split:
          if (!push(0, pc + static_cast<uint32_t>(inst.b), pos)) return Run::StackFull;
          pc += static_cast<uint32_t>(inst.a);
          break;
        case Op::Jmp: pc += static_cast<uint32_t>(inst.a); break;
        case Op::Save:
        case Op::LoopEnter:
          if (!push(1, static_cast<uint32_t>(inst.a), slots_[inst.a])) return Run::StackFull;
          slots_[inst.a] = pos;
          ++pc;
          break;
        case Op::LoopCheck:
          ok = slots_[inst.a] != pos;
          ++pc;
          break;
        case Op::TextBegin: ok = pos == 0; ++pc; break;
        case Op::TextEnd: ok = pos == size_; ++pc; break;
        case Op::LineBegin: ok = pos == 0 || is_line_break(static_cast<uint8_t>(input_[pos - 1])); ++pc; break;
        case Op::LineEnd: ok = pos == size_ || is_line_break(static_cast<uint8_t>(input_[pos])); ++pc; break;
        case Op::WordBoundary: ok = (pos > 0 && word_at(pos - 1)) != word_at(pos); ++pc; break;
        case Op::NotWordBoundary: ok = (pos > 0 && word_at(pos - 1)) == word_at(pos); ++pc; break;
        case Op::Backref:
        case Op::BackrefFold: {
          const uint64_t s = slots_[inst.a * 2], e = slots_[inst.a * 2 + 1];
          if (s != kUnset && e != kUnset && e > s) {  // a group that has not matched (yet) matches nothing
            const uint64_t len = e - s;
            ok = pos + len <= size_ && pos + len >= pos;
            for (uint64_t i = 0; ok && i < len; ++i) {
              const uint8_t x = static_cast<uint8_t>(input_[s + i]), y = static_cast<uint8_t>(input_[pos + i]);
              ok = x == y || (inst.op == Op::BackrefFold && x < 0x80 && y < 0x80 && is_letter(x) && (x | 0x20) == (y | 0x20));
            }
            if (ok) pos += len;
          }
          ++pc;
          break;
        }
        case Op::Look: {
          uint64_t ignored = 0;
          const uint64_t look_base = stack_.size();
          const Run r = run(pc + 1, pos, &ignored, depth + 1);
          if (r == Run::Steps || r == Run::StackFull) return r;
          const bool negative = inst.b != 0;
          if (r == Run::Matched) {
            if (negative) {
              unwind(look_base);
              ok = false;
            } else {
              keep_undo_only(look_base);  // no going back into a lookahead, but its captures must be undone
            }
          } else {
            ok = negative;
          }
          if (ok) pc += static_cast<uint32_t>(inst.a);
          break;
        }
      }
      if (ok) continue;
      // Backtrack to the most recent choice.
      for (;;) {
        if (stack_.size() <= base) return Run::Failed;
        const Frame f = stack_[stack_.size() - 1];
        stack_.truncate(stack_.size() - 1);
        if (f.kind == 1) {
          slots_[f.index] = f.value;
          continue;
        }
        pc = f.index;
        pos = f.value;
        break;
      }
    }
  }

  static constexpr bool is_letter(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

  // Pops frames down to `to`, undoing the capture changes among them.
  constexpr void unwind(uint64_t to) {
    while (stack_.size() > to) {
      const Frame f = stack_[stack_.size() - 1];
      stack_.truncate(stack_.size() - 1);
      if (f.kind == 1) slots_[f.index] = f.value;
    }
  }
  // Drops the choices above `from` but keeps the records that undo capture changes.
  constexpr void keep_undo_only(uint64_t from) {
    uint64_t w = from;
    for (uint64_t i = from; i < stack_.size(); ++i) {
      const Frame f = stack_[i];
      if (f.kind == 1) stack_[w++] = f;
    }
    stack_.truncate(w);
  }

  const Program& program_;
  const char* input_;
  uint64_t size_;
  Stack& stack_;
  Limits limits_;
  bool utf8_ = false;
  bool whole_ = false;
  uint64_t steps_ = 0;
  uint64_t slots_[kMaxSlots] = {};
};

}  // namespace regex
