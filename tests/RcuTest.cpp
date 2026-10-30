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

// RCU tests: grace period semantics (reclamation waits for readers, nested readers, retire order, barrier,
// deleters that retire) and a multi-threaded stress run in which readers dereference a shared pointer while a
// writer keeps replacing and retiring it (the address sanitizer flags any use after free, ThreadSanitizer any
// race; the test is built with both by tests/run_host_tests.sh).
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <thread>
#include <vector>
#include "Kernel/Rcu.hpp"

using namespace kernel;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

// The scheduler is not part of this test: hand out a core number per thread, so readers spread over the shards.
namespace {
std::atomic<uint32_t> g_next_core{1};
thread_local uint32_t t_core = ~uint32_t{0};
}
uint32_t kernel::current_core() {
  if (t_core == ~uint32_t{0}) t_core = g_next_core++;
  return t_core;
}

namespace {
std::atomic<int> g_live{0};
std::atomic<int> g_freed{0};
std::vector<int> g_order;   // freed ids; only the single-threaded tests look at it

struct Item;
struct FreeItem {
  void operator()(Item* item) const;
};
struct Item : RcuObjectBase<Item, FreeItem> {
  std::atomic<uint64_t> a, b;   // always equal while alive; poisoned on free
  int id;
  explicit Item(uint64_t v, int i = 0) : a(v), b(v), id(i) { ++g_live; }
};
void FreeItem::operator()(Item* item) const {
  item->a = ~uint64_t{0};
  item->b = 0;
  g_order.push_back(item->id);
  --g_live;
  ++g_freed;
  delete item;
}

void drain(RcuDomain& d) {
  for (int i = 0; i < 100 && d.pending(); ++i) d.poll();
}

void test_reader_blocks_reclaim() {
  RcuDomain d;
  Item* shared = new Item(1);
  {
    RcuReader reader(d);
    Item* seen = rcu_dereference(&shared);
    rcu_assign_pointer(&shared, new Item(2));
    seen->retire(FreeItem{}, d);
    for (int i = 0; i < 10; ++i) CHECK(d.poll() == 0);   // the reader may still see `seen`
    CHECK(g_live == 2 && seen->a == 1 && seen->b == 1);
  }
  drain(d);
  CHECK(d.pending() == 0);
  CHECK(g_live == 1);
  rcu_retire(shared, FreeItem{}, d);
  d.barrier();
  CHECK(g_live == 0);
}

void test_no_readers_and_order() {
  RcuDomain d;
  g_order.clear();
  for (int i = 0; i < 5; ++i) (new Item(i, i))->retire(FreeItem{}, d);
  CHECK(d.pending() == 5);
  d.synchronize();
  CHECK(d.pending() == 0);
  CHECK((g_order == std::vector<int>{0, 1, 2, 3, 4}));   // callbacks run in retire order
  CHECK(g_live == 0);
}

void test_nested_and_late_readers() {
  RcuDomain d;
  Item* item = new Item(5);
  const uint32_t outer = d.lock();
  const uint32_t inner = d.lock();
  item->retire(FreeItem{}, d);
  d.unlock(inner);
  for (int i = 0; i < 10; ++i) d.poll();
  CHECK(g_live == 1);                  // the outer section still holds it up
  d.unlock(outer);
  drain(d);
  CHECK(g_live == 0);

  // A reader that is still inside when the domain is torn down would be a bug; here it leaves first.
  Item* a = new Item(6);
  a->retire(FreeItem{}, d);
  {
    RcuReader late(d);
    d.poll();
    d.poll();
  }
  d.barrier();
  CHECK(g_live == 0);
}

void test_barrier_after_late_reader_released() {
  RcuDomain d;
  std::atomic<bool> released{false}, entered{false};
  Item* item = new Item(9);
  std::thread reader([&] {
    RcuReader r(d);
    entered = true;
    while (!released) std::this_thread::yield();
  });
  while (!entered) std::this_thread::yield();
  item->retire(FreeItem{}, d);
  std::atomic<bool> done{false};
  std::thread waiter([&] { d.synchronize(); done = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(!done && g_live == 1);         // synchronize waits for the reader
  released = true;
  reader.join();
  waiter.join();
  CHECK(done && g_live == 0);
}

// Every shard holds up the grace period, and a section may end on another core than it began on.
void test_shards_and_migration() {
  RcuDomain d;
  constexpr int kReaders = 20;   // more than kShards: some share a shard
  uint32_t tokens[kReaders];
  uint32_t shards_seen = 0;
  for (int i = 0; i < kReaders; ++i) {
    std::thread([&, i] { tokens[i] = d.lock(); }).join();   // (each thread has its own "core")
    shards_seen |= 1u << (tokens[i] >> 1);
  }
  CHECK(shards_seen == 0xFFFFu);
  Item* item = new Item(4);
  item->retire(FreeItem{}, d);
  for (int i = 0; i < kReaders; ++i) {
    for (int k = 0; k < 5; ++k) d.poll();
    CHECK(g_live == 1);                       // still held by the readers left
    std::thread([&, i] { d.unlock(tokens[i]); }).join();   // (released from a different thread and core)
  }
  drain(d);
  CHECK(g_live == 0);
}

struct Chain : RcuObjectBase<Chain, void (*)(Chain*)> {
  RcuDomain* domain;
  int depth;
};
std::atomic<int> g_chain_freed{0};
void free_chain(Chain* c) {
  ++g_chain_freed;
  if (c->depth > 0) {
    Chain* next = new Chain;
    next->domain = c->domain;
    next->depth = c->depth - 1;
    next->retire(&free_chain, *c->domain);   // a deleter may retire more (it must not wait)
  }
  delete c;
}

void test_deleter_retires() {
  RcuDomain d;
  Chain* c = new Chain;
  c->domain = &d;
  c->depth = 3;
  c->retire(&free_chain, d);
  d.barrier();
  d.barrier();
  drain(d);
  CHECK(g_chain_freed == 4);
  CHECK(d.pending() == 0);
}

void test_auto_poll_and_destructor() {
  g_freed = 0;
  {
    RcuDomain d;
    for (int i = 0; i < 200; ++i) (new Item(i))->retire(FreeItem{}, d);   // retire alone drives reclamation past the threshold
    CHECK(g_freed > 100);
    CHECK(d.pending() < 200);
    (new Item(1))->retire(FreeItem{}, d);
    d.barrier();                       // teardown: the domain has no destructor
  }
  CHECK(g_live == 0);
}

void test_default_domain() {
  Item* item = new Item(3);
  {
    RcuReader reader;
    item->retire(FreeItem{});
    CHECK(g_live == 1);
  }
  rcu_synchronize();
  CHECK(g_live == 0);
  rcu_barrier();
}

// Readers hammer a shared pointer; the writer swaps it and retires the old one. Readers verify the invariant a == b
// and that the object is not poisoned, so a premature free is caught here as well as by the sanitizers.
void test_stress() {
  RcuDomain d;
  Item* shared = new Item(1);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};
  std::atomic<int> bad{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t] {
      uint64_t n = 0;
      while (!stop) {
        RcuReader reader(d);
        const Item* item = rcu_dereference(&shared);
        const uint64_t a = item->a.load(), b = item->b.load();
        if (a != b || a == ~uint64_t{0}) ++bad;
        if (t == 0) {                 // one reader nests and lingers
          RcuReader again(d);
          if (rcu_dereference(&shared)->a.load() == ~uint64_t{0}) ++bad;
          std::this_thread::yield();
        }
        ++n;
      }
      reads += n;
    });
  }
  std::thread syncer([&] {            // a second thread that blocks in synchronize() while the writer retires
    while (!stop) { d.synchronize(); std::this_thread::yield(); }
  });
  for (uint64_t v = 2; v < 20000; ++v) {
    Item* fresh = new Item(v);
    Item* old = shared;
    rcu_assign_pointer(&shared, fresh);
    old->retire(FreeItem{}, d);
  }
  stop = true;
  for (auto& r : readers) r.join();
  syncer.join();
  CHECK(bad == 0);
  CHECK(reads > 0);
  shared->retire(FreeItem{}, d);
  d.barrier();
  CHECK(g_live == 0);
  CHECK(d.pending() == 0);
}
}  // namespace

int main() {
  test_reader_blocks_reclaim();
  test_no_readers_and_order();
  test_nested_and_late_readers();
  test_barrier_after_late_reader_released();
  test_shards_and_migration();
  test_deleter_retires();
  test_auto_poll_and_destructor();
  test_default_domain();
  test_stress();
  if (g_failed) {
    printf("RcuTest: %d failure(s)\n", g_failed);
    return 1;
  }
  printf("RcuTest: all passed\n");
  return 0;
}
