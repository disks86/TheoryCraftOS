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

// Hazard pointer tests: slot handling, deferred reclamation, and a multi-threaded stress run in which readers
// dereference a shared pointer while a writer keeps replacing and retiring it (the address sanitizer flags
// any use after free, ThreadSanitizer any race; the test is built with both by tests/run_host_tests.sh).
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <thread>
#include <vector>
#include "Kernel/HazardPointer.hpp"

using namespace kernel;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

namespace {
std::atomic<int> g_live{0};
std::atomic<int> g_freed{0};

struct Item {
  HazardNode node;
  std::atomic<uint64_t> a, b;   // always equal while the item is alive; poisoned on free
  explicit Item(uint64_t v) : a(v), b(v) { ++g_live; }
};

void free_item(void* p) {
  auto* item = static_cast<Item*>(p);
  item->a = ~uint64_t{0};
  item->b = 0;
  --g_live;
  ++g_freed;
  delete item;
}

void retire_item(HazardDomain<>& d, Item* item) { d.retire(item, item->node, free_item); }

void test_slots() {
  HazardDomain<4> d;
  uint32_t s[4];
  for (auto& x : s) { x = d.acquire_slot(); CHECK(x != HazardDomain<4>::kNoSlot); }
  CHECK(s[0] != s[1] && s[1] != s[2] && s[2] != s[3] && s[0] != s[2] && s[0] != s[3] && s[1] != s[3]);
  CHECK(d.acquire_slot() == HazardDomain<4>::kNoSlot);
  d.release_slot(s[2]);
  CHECK(d.acquire_slot() == s[2]);
  BasicHazardGuard<HazardDomain<4>> none(d, BasicHazardGuard<HazardDomain<4>>::NoWait{});
  CHECK(!none.valid());
  Item* null_item = nullptr;
  CHECK(none.protect(&null_item) == nullptr);
}

void test_deferred_reclaim() {
  HazardDomain<> d;
  Item* shared = new Item(7);
  Item* const* location = &shared;
  {
    HazardGuard guard(d);
    Item* seen = guard.protect(location);
    CHECK(seen == shared);
    Item* old = shared;
    shared = new Item(8);
    retire_item(d, old);
    CHECK(d.scan() == 0);               // protected: stays
    CHECK(d.pending() == 1);
    CHECK(seen->a == 7 && seen->b == 7);
    guard.reset();
    CHECK(d.scan() == 1);               // protection dropped: reclaimed
    CHECK(d.pending() == 0);
  }
  {
    HazardGuard guard(d);
    guard.protect(location);
    Item* old = shared;
    shared = nullptr;
    retire_item(d, old);
    CHECK(d.scan() == 0);
  }                                      // guard destroyed
  CHECK(d.scan() == 1);
  CHECK(g_live == 0);
}

void test_null_and_destructor() {
  g_live = 0;
  {
    HazardDomain<> d;
    Item* none = nullptr;
    HazardGuard guard(d);
    CHECK(guard.protect(&none) == nullptr);
    for (int i = 0; i < 5; ++i) retire_item(d, new Item(i));
    CHECK(d.pending() == 5);
  }                                      // ~HazardDomain drains (guard died first)
  CHECK(g_live == 0);
}

void test_auto_scan() {
  g_live = 0;
  HazardDomain<4> d;
  for (int i = 0; i < 100; ++i) {
    auto* item = new Item(i);
    d.retire(item, item->node, free_item);
  }
  CHECK(d.pending() < 100);              // the threshold scan ran by itself
  CHECK(g_live == static_cast<int>(d.pending()));
  d.drain();
  CHECK(g_live == 0);
}

void test_stress() {
  g_live = 0;
  g_freed = 0;
  constexpr int kReaders = 4;
  constexpr int kWrites = 20000;
  HazardDomain<> d;
  Item* shared = new Item(1);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};
  std::atomic<int> torn{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        HazardGuard guard(d);
        Item* item = guard.protect(&shared);
        if (item) {
          const uint64_t a = item->a.load(), b = item->b.load();
          if (a != b || a == ~uint64_t{0}) ++torn;   // freed under us
        }
        ++reads;
      }
    });
  }
  std::thread writer([&] {
    for (int i = 2; i < kWrites; ++i) {
      Item* fresh = new Item(static_cast<uint64_t>(i));
      Item* old = __atomic_exchange_n(&shared, fresh, __ATOMIC_ACQ_REL);
      retire_item(d, old);
    }
    stop = true;
  });
  writer.join();
  for (auto& t : readers) t.join();
  CHECK(torn == 0);
  CHECK(reads > 0);
  Item* last = shared;
  shared = nullptr;
  retire_item(d, last);
  d.drain();
  CHECK(g_live == 0);
  CHECK(g_freed == kWrites - 1);
}

// Two scanners and two retirers at once: every object is reclaimed exactly once.
void test_concurrent_scan() {
  g_live = 0;
  g_freed = 0;
  HazardDomain<> d;
  constexpr int kPerThread = 10000;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        retire_item(d, new Item(i));
        if (t & 1) d.scan();
      }
    });
  }
  for (auto& t : threads) t.join();
  d.drain();
  CHECK(g_live == 0);
  CHECK(g_freed == 4 * kPerThread);
}
}  // namespace

int main() {
  test_slots();
  test_deferred_reclaim();
  test_null_and_destructor();
  test_auto_scan();
  test_stress();
  test_concurrent_scan();
  if (g_failed) { printf("%d FAILED\n", g_failed); return 1; }
  printf("HazardPointerTest: all passed\n");
  return 0;
}
