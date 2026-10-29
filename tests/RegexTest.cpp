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

// Host-side tests for the regular expression engine: patterns checked at compile time (static_assert)
// and the same behaviour through the run-time Regex.
#include <stdio.h>
#include <stdlib.h>
#include "Regex/Regex.hpp"

struct Violation {};
void bounds_violation(const char*, uint64_t, uint64_t) { throw Violation{}; }

struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

using namespace regex;
using namespace regex::literals;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

// ---- compile time
static_assert(compile<"abc">().matches("abc"));
static_assert(!compile<"abc">().matches("abcd"));
static_assert(!compile<"abc">().matches("ab"));
static_assert(compile<"a|b|c">().matches("b") && !compile<"a|b|c">().matches("d"));
static_assert(compile<"a*">().matches("") && compile<"a*">().matches("aaaa") && !compile<"a*">().matches("aab"));
static_assert(compile<"a+b?">().matches("aaab") && !compile<"a+b?">().matches("b"));
static_assert(compile<"[a-c]+\\d{2,3}">().matches("abcab12") && compile<"[a-c]+\\d{2,3}">().matches("c999") && !compile<"[a-c]+\\d{2,3}">().matches("c9999"));
static_assert(compile<"(ab)\\1">().matches("abab") && !compile<"(ab)\\1">().matches("abba"));
static_assert(compile<"\\w+@\\w+\\.com">().contains("mail bob@example.com now"));
static_assert(compile<"hello", Flags::IgnoreCase>().matches("HeLLo"));
static_assert(compile<"(a*)*b">().matches("aaab"));          // an empty iteration does not loop forever
static_assert(compile<"(?:a?)*b">().matches("b"));
static_assert("x(?=y)"_re.contains("xy") && !"x(?=y)"_re.contains("xz") && "x(?!y)"_re.contains("xz"));
static_assert(try_compile<"(">().error() == Error::UnbalancedParen);
static_assert(try_compile<"a**">().error() == Error::NothingToRepeat);
static_assert(try_compile<"[z-a]">().error() == Error::BadClassRange);
static_assert(try_compile<"a{3,2}">().error() == Error::BadRepeat);
static_assert(try_compile<"\\2(a)">().error() == Error::BadBackreference);
static_assert("(?<=a)b"_re.contains("ab") && !"(?<=a)b"_re.contains("cb") && "(?<!a)b"_re.contains("cb") && !"(?<!a)b"_re.contains("ab"));   // lookbehind
static_assert(compile<"(?<=foo|ba)x">().contains("bax") && compile<"(?<=\\d{2})x">().contains("12x") && !compile<"(?<=\\d{2})x">().contains("1x"));
static_assert(compile<"\\p{Lu}\\p{Ll}+", Flags::Utf8 | Flags::Unicode>().matches("Hello") && !compile<"\\p{Lu}", Flags::Utf8 | Flags::Unicode>().contains("hello"));
static_assert(compile<"[\\P{L}]", Flags::Utf8 | Flags::Unicode>().matches("1") && !compile<"[\\P{L}]", Flags::Utf8 | Flags::Unicode>().matches("a"));
static_assert(compile<"[^\\p{White_Space}\\p{Pattern_Syntax}]+", Flags::Utf8 | Flags::Unicode>().matches("abc") && !compile<"[^\\p{White_Space}\\p{Pattern_Syntax}]+", Flags::Utf8 | Flags::Unicode>().matches("a-c"));
static_assert(try_compile<"\\p{Nope}", Flags::Utf8 | Flags::Unicode>().error() == Error::BadProperty);
static_assert(compile<"(?<year>\\d+)-(?<month>\\d+)">().group_index("month") == 2);
static_assert(compile<"(a)(b)(c)">().group_count() == 4);

constexpr bool date_ok() {
  constexpr auto date = compile<"(\\d{4})-(\\d{2})-(\\d{2})">();
  Match m;
  const StringView text = "on 2026-10-21.";
  if (date.search(text, &m) != Result::Matched) return false;
  return m.begin() == 3 && m.end() == 13 && m.text(text, 1).equals("2026") && m.text(text, 3).equals("21");
}
static_assert(date_ok());

// ---- run time
static Regex build(TestAllocator& a, const char* pattern, Flags f = Flags::None) {
  Regex re(a);
  const Error e = re.compile(pattern, f);
  if (e != Error::None) printf("compile of %s failed: %s\n", pattern, error_text(e));
  return re;
}
static bool full(TestAllocator& a, const char* pattern, const char* input, Flags f = Flags::None) {
  Regex re = build(a, pattern, f);
  return re.valid() && re.match(input, nullptr) == Result::Matched;
}
// The text of group `g` of the first match, or "<none>".
static bool found(TestAllocator& a, const char* pattern, const char* input, const char* expect, uint32_t g = 0, Flags f = Flags::None) {
  Regex re = build(a, pattern, f);
  Match m;
  if (!re.valid() || re.search(input, &m) != Result::Matched) return expect == nullptr;
  if (!expect) return false;
  return m.text(input, g).equals(expect);
}
static Error error_of(TestAllocator& a, const char* pattern, Flags f = Flags::None) {
  Regex re(a);
  return re.compile(pattern, f);
}

int main() {
  TestAllocator a;

  // ---- basics
  CHECK(full(a, "", ""));
  CHECK(full(a, "abc", "abc") && !full(a, "abc", "abd") && !full(a, "abc", "ab"));
  CHECK(full(a, "a.c", "abc") && !full(a, "a.c", "a\nc") && full(a, "a.c", "a\nc", Flags::DotAll));
  CHECK(full(a, "a|bc|d", "bc") && full(a, "a|bc|d", "d") && !full(a, "a|bc|d", "b"));
  CHECK(full(a, "a|", "") && full(a, "|a", "a") && full(a, "(|a)b", "b"));
  CHECK(full(a, "colou?r", "color") && full(a, "colou?r", "colour") && !full(a, "colou?r", "colouur"));
  CHECK(full(a, "a\\.b", "a.b") && !full(a, "a\\.b", "axb"));
  CHECK(full(a, "\\x41\\u0042\\u{43}\\t", "ABC\t"));
  CHECK(full(a, "a{", "a{") && full(a, "a{x}", "a{x}") && full(a, "}]", "}]"));

  // ---- quantifiers
  CHECK(full(a, "a{3}", "aaa") && !full(a, "a{3}", "aa") && !full(a, "a{3}", "aaaa"));
  CHECK(full(a, "a{2,}", "aa") && full(a, "a{2,}", "aaaaaa") && !full(a, "a{2,}", "a"));
  CHECK(full(a, "a{2,4}", "aa") && full(a, "a{2,4}", "aaaa") && !full(a, "a{2,4}", "aaaaa"));
  CHECK(full(a, "a{0}b", "b") && full(a, "a{0,2}b", "aab") && !full(a, "a{0,2}b", "aaab"));
  CHECK(full(a, "(ab){2,3}", "ababab") && !full(a, "(ab){2,3}", "ab"));
  CHECK(full(a, "(?:a|bc){3}", "abca"));
  CHECK(found(a, "a+", "baaac", "aaa") && found(a, "a+?", "baaac", "a") && found(a, "a*?b", "aab", "aab"));
  CHECK(found(a, "<.+>", "<a><b>", "<a><b>") && found(a, "<.+?>", "<a><b>", "<a>"));
  CHECK(found(a, "a{2,3}", "aaaa", "aaa") && found(a, "a{2,3}?", "aaaa", "aa"));
  CHECK(found(a, "x??y", "xy", "xy") && found(a, "x??", "x", ""));
  CHECK(full(a, "(a*)*b", "aaab") && full(a, "(a?)+b", "b") && full(a, "(a|b*)*c", "abbac") && full(a, "(?:)*x", "x"));
  CHECK(full(a, "(a*)+", "") && full(a, "(a*){2,3}", "aaa"));

  // ---- classes
  CHECK(full(a, "[abc]+", "abccba") && !full(a, "[abc]+", "abd"));
  CHECK(full(a, "[^abc]+", "xyz") && !full(a, "[^abc]+", "xaz"));
  CHECK(full(a, "[a-fA-F0-9]+", "dEaDbeef09") && !full(a, "[a-f]+", "g"));
  CHECK(full(a, "[\\d_]+", "12_3") && full(a, "[\\w-]+", "ab-c_d") && full(a, "[a\\-z]+", "a-z"));
  CHECK(full(a, "[\\D]+", "abc") && !full(a, "[\\D]", "5") && full(a, "[^\\s]+", "abc") && !full(a, "[^\\s]", " "));
  CHECK(full(a, "[]*", "") && !full(a, "[]", "a") && full(a, "[^]", "\n"));
  CHECK(full(a, "\\d+", "0123") && !full(a, "\\d", "a") && full(a, "\\D+", "ab") && full(a, "\\s+", " \t\r\n") && full(a, "\\S+", "ab"));
  CHECK(full(a, "\\w+", "ab_9") && !full(a, "\\w", "-") && full(a, "\\W+", "-+ "));
  CHECK(full(a, "[\\b]", "\b") && full(a, "[a-]", "-") && full(a, "[-a]", "-"));

  // ---- anchors and boundaries
  CHECK(found(a, "^a", "ab", "a") && found(a, "^a", "ba", nullptr) && found(a, "a$", "ba", "a") && found(a, "a$", "ab", nullptr));
  CHECK(found(a, "^b", "a\nb", nullptr) && found(a, "^b", "a\nb", "b", 0, Flags::Multiline) && found(a, "a$", "a\nb", "a", 0, Flags::Multiline));
  CHECK(found(a, "\\bfoo\\b", "a foo b", "foo") && found(a, "\\bfoo\\b", "afoob", nullptr) && found(a, "\\Boo", "foo", "oo"));
  CHECK(found(a, "\\Aa", "a", "a") && found(a, "a\\z", "a", "a") && found(a, "a\\z", "a\n", nullptr));
  CHECK(full(a, "^$", "") && full(a, "^abc$", "abc"));

  // ---- groups, backreferences
  {
    Regex re = build(a, "(a)(b)?(c)");
    Match m;
    CHECK(re.search("xac", &m) == Result::Matched && m.count == 4 && m.has(1) && !m.has(2) && m.text("xac", 3).equals("c") && m.begin() == 1 && m.end() == 3);
    CHECK(m.length(2) == 0 && m.text("xac", 2).empty());
  }
  CHECK(found(a, "(a)|(b)", "b", "b", 2) && found(a, "(a)|(b)", "b", "", 1));
  CHECK(found(a, "(\\w+) \\1", "say hello hello there", "hello hello") && found(a, "(\\w+) \\1", "say hello world", nullptr));
  CHECK(full(a, "(a)\\1", "aa", Flags::None) && !full(a, "(a)\\1", "aA") && full(a, "(a)\\1", "aA", Flags::IgnoreCase));
  CHECK(full(a, "(a*)b\\1", "aabaa") && full(a, "(a)?\\1b", "b"));    // an unmatched group matches empty
  CHECK(full(a, "(?<q>['\"]).*?\\k<q>", "'it'") && !full(a, "(?<q>['\"]).*?\\k<q>", "'it\""));
  {
    Regex re = build(a, "(?<year>\\d+)-(?<month>\\d+)");
    CHECK(re.group_index("year") == 1 && re.group_index("month") == 2 && re.group_index("day") == -1 && re.group_count() == 3);
  }
  CHECK(full(a, "(?:ab)+", "ababab"));
  CHECK(found(a, "((a)(b))", "ab", "a", 2) && found(a, "((a)(b))", "ab", "ab", 1));
  CHECK(found(a, "(a|ab)(c|bcd)(d*)", "abcd", "a", 1));

  // ---- lookahead
  CHECK(found(a, "foo(?=bar)", "foobar", "foo") && found(a, "foo(?=bar)", "foobaz", nullptr));
  CHECK(found(a, "foo(?!bar)", "foobar foobaz", "foo") && found(a, "foo(?!bar)", "foobar", nullptr));
  CHECK(found(a, "(?=(\\d+))\\d", "42", "42", 1));                 // captures made in a positive lookahead stay
  CHECK(found(a, "(?!(a))b", "b", "", 1));                         // ... and none escape a negative one
  CHECK(full(a, "(?=.*\\d)(?=.*[a-z]).{4,}", "ab3d") && !full(a, "(?=.*\\d)(?=.*[a-z]).{4,}", "abcd"));

  // ---- case folding
  CHECK(full(a, "abc", "AbC", Flags::IgnoreCase) && full(a, "[a-c]+", "ABC", Flags::IgnoreCase) && full(a, "[^a-c]", "d", Flags::IgnoreCase));
  CHECK(!full(a, "[^a-c]", "B", Flags::IgnoreCase) && !full(a, "abc", "AbC"));
  CHECK(found(a, "x", "aXb", "X", 0, Flags::IgnoreCase));

  // ---- UTF-8
  CHECK(full(a, "caf\xC3\xA9", "caf\xC3\xA9", Flags::Utf8) && full(a, "caf.", "caf\xC3\xA9", Flags::Utf8) && !full(a, "caf.", "caf\xC3\xA9") );
  CHECK(full(a, "[\xC3\xA0-\xC3\xBF]+", "\xC3\xA9\xC3\xA8", Flags::Utf8));
  CHECK(full(a, "\\u00e9", "\xC3\xA9", Flags::Utf8) && full(a, "\\u{1F600}", "\xF0\x9F\x98\x80", Flags::Utf8));
  CHECK(full(a, ".", "\xF0\x9F\x98\x80", Flags::Utf8) && !full(a, ".", "\xF0\x9F\x98\x80"));
  CHECK(found(a, "\xC3\xA9", "ab\xC3\xA9", "\xC3\xA9", 0, Flags::Utf8));
  CHECK(error_of(a, "\xC3", Flags::Utf8) == Error::BadUtf8 && error_of(a, "\xFF", Flags::Utf8) == Error::BadUtf8);
  CHECK(full(a, "[^a]", "\xC3\xA9", Flags::Utf8));

  // ---- search, prefixes, positions
  {
    Regex re = build(a, "o+");
    Match m;
    CHECK(re.search("foo boo", &m) == Result::Matched && m.begin() == 1 && m.end() == 3);
    CHECK(re.search("foo boo", &m, 3) == Result::Matched && m.begin() == 5 && m.end() == 7);
    CHECK(re.search("foo boo", &m, 7) == Result::NoMatch && re.search("foo", &m, 99) == Result::NoMatch);
    CHECK(re.match_prefix("foo", &m, 1) == Result::Matched && m.end() == 3 && re.match_prefix("foo", &m, 0) == Result::NoMatch);
    CHECK(re.match("foo", &m) == Result::NoMatch && re.match("ooo", &m) == Result::Matched && re.contains("xxo") && !re.contains("xxx"));
  }
  {
    Regex re = build(a, "a", Flags::IgnoreCase);   // exercises the first-character skip
    Match m;
    CHECK(re.search("bbbbA", &m) == Result::Matched && m.begin() == 4);
  }
  {
    Regex re = build(a, "x*");
    Match m;
    CHECK(re.search("abc", &m) == Result::Matched && m.begin() == 0 && m.end() == 0);
    CHECK(re.search("", &m) == Result::Matched);
  }

  // ---- replace
  {
    String out(a);
    Regex re = build(a, "(\\w+)@(\\w+)");
    CHECK(re.replace("a@b and c@d", "$2:$1", true, &out) && out.equals("b:a and d:c"));
    out.clear();
    CHECK(re.replace("a@b and c@d", "<$&>", false, &out) && out.equals("<a@b> and c@d"));
    out.clear();
    CHECK(re.replace("a@b", "$$ $9 $", true, &out) && out.equals("$ $9 $"));
    Regex named = build(a, "(?<x>\\d)");
    out.clear();
    CHECK(named.replace("a1b2", "[$<x>]", true, &out) && out.equals("a[1]b[2]"));
    Regex empty = build(a, "x*");
    out.clear();
    CHECK(empty.replace("abc", "-", true, &out) && out.equals("-a-b-c-"));
    Regex none = build(a, "z");
    out.clear();
    CHECK(none.replace("abc", "-", true, &out) && out.equals("abc"));
    Regex u = build(a, "x*", Flags::Utf8);
    out.clear();
    CHECK(u.replace("\xC3\xA9", "-", true, &out) && out.equals("-\xC3\xA9-"));
  }

  // ---- errors
  CHECK(error_of(a, "(") == Error::UnbalancedParen && error_of(a, ")") == Error::UnbalancedParen && error_of(a, "(a))") == Error::UnbalancedParen);
  CHECK(error_of(a, "[a") == Error::UnterminatedClass && error_of(a, "a\\") == Error::TrailingBackslash);
  CHECK(error_of(a, "*") == Error::NothingToRepeat && error_of(a, "+a") == Error::NothingToRepeat && error_of(a, "{2}") == Error::NothingToRepeat);
  CHECK(error_of(a, "^*") == Error::NothingToRepeat && error_of(a, "\\b+") == Error::NothingToRepeat && error_of(a, "(?=a)*") == Error::NothingToRepeat);
  CHECK(error_of(a, "a{2,1}") == Error::BadRepeat && error_of(a, "a{1001}") == Error::RepeatTooLarge && error_of(a, "a{99999999999}") == Error::RepeatTooLarge);
  CHECK(error_of(a, "\\q") == Error::BadEscape && error_of(a, "\\xZ1") == Error::BadEscape && error_of(a, "\\u12") == Error::BadEscape && error_of(a, "\\00") == Error::BadEscape);
  CHECK(error_of(a, "(?x)") == Error::BadGroup && error_of(a, "(?<1a>x)") == Error::BadGroupName && error_of(a, "(?<a>x)|(?<a>y)") == Error::None   /* (a name may be used again: ES2025) */);
  CHECK(error_of(a, "\\1") == Error::BadBackreference && error_of(a, "(a)\\2") == Error::BadBackreference && error_of(a, "\\k<n>") == Error::BadBackreference);
  CHECK(error_of(a, "(?<=a)") == Error::None && error_of(a, "(?<!a)") == Error::None);   // (lookbehind compiles)
  CHECK(found(a, "(?<=(\\d)(\\d))x", "12x", "x", 0));
  CHECK(error_of(a, "[b-a]") == Error::BadClassRange && full(a, "[\\d-z]+", "1-z"));  // after a shorthand the dash is a plain character
  {
    String many(a);
    for (int i = 0; i < 40; ++i) many.append("(a)");
    CHECK(error_of(a, many.c_str()) == Error::TooManyGroups);
    String deep(a);
    for (int i = 0; i < 100; ++i) deep.append("(?:");
    deep.append("a");
    for (int i = 0; i < 100; ++i) deep.append(")");
    CHECK(error_of(a, deep.c_str()) == Error::NestingTooDeep);
    String looks(a);
    for (int i = 0; i < 20; ++i) looks.append("(?=");
    CHECK(error_of(a, looks.c_str()) == Error::NestingTooDeep);
    CHECK(error_of(a, "((a{1000}){1000}){1000}") == Error::ProgramTooLarge);
    String loops(a);
    for (int i = 0; i < 70; ++i) loops.append("(?:a*)*");
    CHECK(error_of(a, loops.c_str()) == Error::TooManyLoops);
    CHECK(error_of(a, "(") == Error::UnbalancedParen);
  }
  {
    Regex re(a);
    CHECK(re.compile("a+") == Error::None && re.valid());
    CHECK(re.compile("(") != Error::None && !re.valid() && re.contains("a") == false);  // a failed compile leaves it empty
    CHECK(re.compile("b") == Error::None && re.contains("b") && !re.contains("a"));
  }

  // ---- limits
  {
    Regex re = build(a, "(a*)*b");
    String as(a);
    for (int i = 0; i < 40; ++i) as.push_back('a');
    Match m;
    Limits small;
    small.max_steps = 100000;
    // Nested quantifiers over a long run with no way to finish: stops at the budget, not by hanging.
    Regex evil = build(a, "(a|aa)+$");
    String more(a);
    for (int i = 0; i < 60; ++i) more.push_back('a');
    more.push_back('b');
    CHECK(evil.search(more.view(), &m, 0, small) == Result::StepLimit);
    Limits tiny;
    tiny.max_stack = 8;
    Regex star = build(a, ".*x");
    CHECK(star.search(as.view(), &m, 0, tiny) == Result::StackLimit);
    CHECK(re.search(as.view(), &m) != Result::Matched);   // exponential: ends in NoMatch or the step limit, never a hang
  }
  {
    // A long input needs a long stack, which the heap gives it.
    Regex re = build(a, "(?:a|b)*c");
    String big(a);
    for (int i = 0; i < 100000; ++i) big.push_back(i % 2 ? 'a' : 'b');
    big.push_back('c');
    Match m;
    CHECK(re.match(big.view(), &m) == Result::Matched && m.end() == big.size());
    // The compile-time one has a small fixed stack and says so.
    constexpr auto fixed = compile<"(?:a|b)*c">();
    CHECK(fixed.match(big.view(), &m) == Result::StackLimit);
    FixedVec<Frame, 1> tiny;
    HeapVec<Frame> heap(a, 1u << 20);
    CHECK(fixed.search_with(heap, big.view(), &m) == Result::Matched);
  }

  // ---- memory
  {
    a.fail = true;
    Regex re(a);
    CHECK(re.compile("abc") == Error::OutOfMemory && !re.valid());
    a.fail = false;
    CHECK(re.compile("(abc)") == Error::None);
    a.fail = true;
    Match m;
    CHECK(re.search("abc", &m) == Result::StackLimit);   // the stack could not be allocated
    a.fail = false;
  }
  CHECK(a.live == 0);

  if (g_failed) {
    printf("RegexTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("RegexTest: all passed\n");
  return 0;
}
