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

// Host-side tests for the session history (entries, state, documents, traversal) and the hubs pages share (per-origin storage, events between pages).
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "Browser/PageHub.hpp"
#include "Browser/SessionHistory.hpp"
#include "Utility/Move.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::string addr(SessionHistory& h) { return h.current() ? std::string(h.current()->address.c_str()) : "-"; }

static void test_history() {
  TestAllocator a;
  {
    SessionHistory h(a);
    CHECK(h.empty() && h.index() == -1 && !h.can_go(-1) && !h.can_go(1) && h.current() == nullptr && !h.go(1));
    CHECK(!h.push_state("/x", "", false, false));   // (nothing to push onto)
    CHECK(h.navigate("http://a/1", 1) && h.navigate("http://a/2", 2) && h.navigate("http://a/3", 3));
    CHECK(h.count() == 3 && h.index() == 2 && addr(h) == "http://a/3" && h.can_go(-2) && !h.can_go(-3) && !h.can_go(1));
    CHECK(h.go(-1) && addr(h) == "http://a/2" && h.can_go(1) && h.can_go(-1));
    // a new page drops the forward entries
    CHECK(h.navigate("http://a/4", 4) && h.count() == 3 && addr(h) == "http://a/4" && !h.can_go(1));
    // pushState entries belong to the document of the entry they were made from
    CHECK(h.push_state("http://a/4?x", "{\"s\":1}", true, false));
    CHECK(h.count() == 4 && h.current()->document == 4 && h.current()->has_state && std::string(h.current()->state.c_str()) == "{\"s\":1}");
    CHECK(h.push_state("http://a/4?y", "", false, false) && h.current()->document == 4 && !h.current()->has_state);
    CHECK(h.push_state("http://a/4?y#z", "\"r\"", true, true) && h.count() == 5 && addr(h) == "http://a/4?y#z" && h.current()->has_state);
    CHECK(h.push_fragment("http://a/4?y#frag") && h.current()->document == 4 && h.count() == 6);
    // going back through them
    CHECK(h.go(-1) && h.go(-1) && addr(h) == "http://a/4?x" && h.current()->document == 4 && h.go(-1) && h.current()->document == 4 && h.go(-1) && h.current()->document == 2);
    // replace_current and renumbering (the document was loaded again)
    h.replace_current("http://a/2b", 9);
    CHECK(addr(h) == "http://a/2b" && h.current()->document == 9);
    h.renumber_document(4, 12);
    CHECK(h.at(2).document == 12 && h.at(3).document == 12 && h.at(1).document == 9);
    // the oldest entries go when there are too many
    SessionHistory many(a);
    for (uint32_t i = 0; i < SessionHistory::kMaxEntries + 20; ++i) many.navigate(("http://a/" + std::to_string(i)).c_str(), i + 1);
    CHECK(many.count() == SessionHistory::kMaxEntries && many.index() == SessionHistory::kMaxEntries - 1 && std::string(many.at(0).address.c_str()) == "http://a/20");
    // a state that is too large is refused
    std::string big(SessionHistory::kMaxStateBytes + 1, 'x');
    CHECK(!many.push_state("http://a/big", StringView(big.data(), big.size()), true, false));
    many.clear();
    CHECK(many.empty() && many.index() == -1);
  }
  CHECK(a.live == 0);
}

static void test_hubs() {
  TestAllocator a;
  {
    StorageHub hub(a);
    StorageArea& one = hub.area("https://one.example");
    StorageArea& two = hub.area("https://two.example");
    one.set("k", "v1");
    two.set("k", "v2");
    StringView v;
    CHECK(&one != &two && &hub.area("https://one.example") == &one && one.get("k", &v) && v.equals("v1") && two.get("k", &v) && v.equals("v2") && hub.origin_count() == 2);
    // (the areas stay where they are as more origins come)
    for (int i = 0; i < 50; ++i) hub.area(("https://o" + std::to_string(i) + ".example").c_str());
    CHECK(&hub.area("https://one.example") == &one && one.get("k", &v) && v.equals("v1"));

    PageHub ph(a);
    const uint32_t p1 = ph.add_page(), p2 = ph.add_page(), p3 = ph.add_page();
    CHECK(p1 != p2 && p2 != p3);
    uint64_t seen2 = ph.serial(), seen3 = ph.serial(), seen1 = ph.serial();
    HubEvent e(a);
    e.source = p1;
    e.origin.assign("https://one.example");
    e.name.assign("k");
    e.first.assign("old");
    e.second.assign("new");
    ph.post(util::move(e));
    HubEvent got(a);
    CHECK(ph.pending(p2, "https://one.example", seen2) && !ph.pending(p1, "https://one.example", seen1) && !ph.pending(p3, "https://two.example", seen3));
    CHECK(ph.next(p2, "https://one.example", &seen2, &got) && std::string(got.name.c_str()) == "k" && std::string(got.first.c_str()) == "old" && std::string(got.second.c_str()) == "new" && got.source == p1);
    CHECK(!ph.next(p2, "https://one.example", &seen2, &got) && !ph.pending(p2, "https://one.example", seen2));
    CHECK(!ph.next(p1, "https://one.example", &seen1, &got));   // (a page does not hear itself)
    CHECK(!ph.next(p3, "https://two.example", &seen3, &got));   // (nor other origins)
    // a page that was away sees what happened since, up to the backlog
    uint64_t late = 0;
    for (int i = 0; i < 300; ++i) {
      HubEvent f(a);
      f.source = p1;
      f.origin.assign("https://one.example");
      f.name.assign(std::to_string(i).c_str());
      f.kind = i % 2 ? HubEvent::Kind::Broadcast : HubEvent::Kind::Storage;
      ph.post(util::move(f));
    }
    int count = 0;
    while (ph.next(p2, "https://one.example", &late, &got)) ++count;
    CHECK(count == static_cast<int>(PageHub::kBacklog));
  }
  CHECK(a.live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_history();
  test_hubs();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("history tests passed\n");
  return 0;
}
