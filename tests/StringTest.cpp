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

// Host-side tests for StringView and String.
#include <stdio.h>
#include <stdlib.h>
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

struct Violation {};
void bounds_violation(const char*, uint64_t, uint64_t) { throw Violation{}; }

// An allocator that can be told to fail, and counts live blocks.
struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define STOPS(expr) do { bool stopped_ = false; try { (void)(expr); } catch (Violation&) { stopped_ = true; } \
  if (!stopped_) { printf("FAILED line %d: no violation: %s\n", __LINE__, #expr); ++g_failed; } } while (0)

int main() {
  // ---- construction
  StringView hello = "hello";
  CHECK(hello.size() == 5 && !hello.empty() && hello[0] == 'h' && hello.back() == 'o');
  CHECK(StringView().empty() && StringView(nullptr).empty() && StringView(nullptr, 5).empty());
  CHECK(StringView("abcdef", 3).size() == 3);                       // not NUL-terminated at the view's end
  char buffer[8] = {'a', 'b', 'c', 0, 'x', 'y', 'z', 0};
  CHECK(StringView(buffer).size() == 3);                            // an array: up to the first NUL
  char unterminated[3] = {'a', 'b', 'c'};
  CHECK(StringView(unterminated).size() == 3);                      // an array without a NUL stays inside it
  STOPS(hello[5]);
  STOPS(StringView().front());
  char c;
  CHECK(hello.try_get(4, &c) && c == 'o' && !hello.try_get(5, &c));

  // ---- sub-views
  CHECK(hello.substr(1, 3).equals("ell") && hello.substr(2).equals("llo") && hello.first(2).equals("he") && hello.last(2).equals("lo"));
  CHECK(hello.substr(5).empty());
  STOPS(hello.substr(6));
  STOPS(hello.substr(2, 4));
  STOPS(hello.last(6));
  StringView part;
  CHECK(hello.try_substr(1, 4, &part) && part.equals("ello") && !hello.try_substr(1, 5, &part) && !hello.try_substr(~0ull, 1, &part));
  CHECK(hello.without_prefix(2).equals("llo") && hello.without_prefix(9).empty() && hello.without_suffix(2).equals("hel") && hello.without_suffix(9).empty());
  CHECK(StringView("  \t x y \r\n").trimmed().equals("x y") && StringView("   ").trimmed().empty());

  // ---- searching and comparing
  CHECK(hello.starts_with("he") && !hello.starts_with("hex") && !hello.starts_with("hello!") && hello.starts_with('h') && !StringView().starts_with('h'));
  CHECK(hello.ends_with("llo") && !hello.ends_with("ello!") && hello.ends_with('o'));
  CHECK(hello.find('l') == 2 && hello.find('l', 3) == 3 && hello.find('z') == StringView::npos && hello.find('l', 9) == StringView::npos);
  CHECK(hello.find("lo") == 3 && hello.find("") == 0 && hello.find("hello!") == StringView::npos && hello.rfind('l') == 3 && hello.rfind('z') == StringView::npos);
  CHECK(hello.contains('e') && hello.contains("ell") && !hello.contains("elo"));
  CHECK(hello == "hello" && hello != "help" && hello.equals_ignore_case("HeLLo") && !hello.equals_ignore_case("hell"));
  CHECK(hello.compare("hello") == 0 && hello.compare("help") < 0 && hello.compare("hell") > 0 && StringView("a") < StringView("ab"));
  CHECK(StringView("\xff").compare("a") > 0);  // bytes compare as unsigned

  // ---- splitting and numbers
  StringView before, after;
  CHECK(StringView("key=value=x").split_once('=', &before, &after) && before.equals("key") && after.equals("value=x"));
  CHECK(!StringView("novalue").split_once('=', &before, &after) && before.equals("key"));  // untouched on failure
  CHECK(StringView("a=").split_once('=', &before, &after) && before.equals("a") && after.empty());
  StringView rest = "a,bb,,c", token;
  int tokens = 0; bool shape = true;
  while (rest.next_token(',', &token)) { ++tokens; if (tokens == 1) shape &= token.equals("a"); if (tokens == 3) shape &= token.empty(); if (tokens == 4) shape &= token.equals("c"); }
  CHECK(tokens == 4 && shape && !rest.next_token(',', &token));
  uint64_t number;
  CHECK(StringView("12345").parse_uint(&number) && number == 12345 && StringView("0").parse_uint(&number) && number == 0);
  CHECK(StringView("18446744073709551615").parse_uint(&number) && number == ~0ull);
  CHECK(!StringView("18446744073709551616").parse_uint(&number) && !StringView("").parse_uint(&number) && !StringView("-1").parse_uint(&number) &&
        !StringView("1 ").parse_uint(&number) && !StringView("12a").parse_uint(&number) && !StringView("99999999999999999999999").parse_uint(&number));

  // ---- copy_to
  char out[6];
  CHECK(hello.copy_to(ArrayView<char>(out)) && out[5] == 0 && out[0] == 'h');
  char tiny[5];
  CHECK(!hello.copy_to(ArrayView<char>(tiny)));  // no room for the NUL
  CHECK(StringView("").copy_to(ArrayView<char>(tiny)) && tiny[0] == 0);

  // ---- String
  TestAllocator alloc;
  {
    String s(alloc);
    CHECK(s.empty() && s.size() == 0 && s.c_str()[0] == 0 && s.view().empty() && alloc.live == 0);
    CHECK(s.assign("hello") && s.size() == 5 && s == "hello" && s.c_str()[5] == 0);
    CHECK(s.append(", world") && s == "hello, world" && s[7] == 'w');
    CHECK(s.push_back('!') && s == "hello, world!");
    STOPS(s[13]);
    s.set(0, 'J'); CHECK(s == "Jello, world!");
    STOPS(s.set(13, 'x'));
    CHECK(s.append_uint(0) && s.append_uint(1234567890123ull) && s == "Jello, world!01234567890123");
    s.clear(); CHECK(s.empty() && s.c_str()[0] == 0 && s.capacity() > 0);
    CHECK(s.append_int(-42) && s == "-42" && s.append_int(0) && s == "-420");
    CHECK(s.assign("") && s.empty());
    // growth keeps content, many appends
    String big(alloc);
    for (int i = 0; i < 1000; ++i) CHECK(big.push_back((char)('a' + i % 26)));
    CHECK(big.size() == 1000 && big[999] == (char)('a' + 999 % 26) && big.view().first(3).equals("abc") && big.c_str()[1000] == 0);
    // appending part of itself, across a reallocation
    String self(alloc); CHECK(self.assign("abc"));
    for (int i = 0; i < 12; ++i) CHECK(self.append(self.view()));
    CHECK(self.size() == 3 * 4096 && self.view().first(6).equals("abcabc") && self.view().last(3).equals("abc"));
    String self2(alloc); CHECK(self2.assign("0123456789"));
    CHECK(self2.append(self2.view().substr(2, 5)) && self2 == "012345678923456");
    // insert / erase / truncate
    String e(alloc); CHECK(e.assign("hello"));
    CHECK(e.insert(0, ">> ") && e == ">> hello" && e.insert(e.size(), "!") && e == ">> hello!" && e.insert(5, "--") && e == ">> he--llo!");
    CHECK(!e.insert(99, "x") && e == ">> he--llo!");
    CHECK(e.erase(5, 2) && e == ">> hello!" && e.erase(0, 3) && e == "hello!" && e.erase(4, 99) && e == "hell" && !e.erase(5, 1) && e == "hell");
    e.truncate(2); CHECK(e == "he" && e.c_str()[2] == 0); e.truncate(10); CHECK(e == "he");
    // move
    String moved(static_cast<String&&>(e));
    CHECK(moved == "he" && e.empty() && e.c_str()[0] == 0);
    String assigned(alloc); CHECK(assigned.assign("replaced"));
    assigned = static_cast<String&&>(moved);
    CHECK(assigned == "he" && moved.empty());
    // conversions
    StringView v = assigned; CHECK(v.equals("he"));
  }
  CHECK(alloc.live == 0);  // everything freed

  // ---- allocation failure leaves the string alone
  {
    String s(alloc); CHECK(s.assign("keep")); const uint64_t cap = s.capacity();
    alloc.fail = true;
    CHECK(!s.append("this text is much longer than the capacity of the string so it must grow and growing fails"));
    CHECK(s == "keep" && s.capacity() == cap);
    CHECK(!s.insert(1, "this text is much longer than the capacity of the string so it must grow and growing fails") && s == "keep");
    CHECK(!s.reserve(10000) && s == "keep");
    alloc.fail = false;
    CHECK(s.append(" ok") && s == "keep ok");
  }
  CHECK(alloc.live == 0);

  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("string ok\n");
  return 0;
}
