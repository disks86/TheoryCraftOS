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

// Host-side tests for ObjectContainer/Hive: stable element addresses, hole reuse, block release, cursor erase while walking, pointer
// validation, destructor accounting, allocation failure, and a randomized comparison against a plain model.
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "ObjectContainer/Hive.hpp"
#include "Utility/ArrayView.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// malloc-backed allocator that counts live allocations and can be told to fail after N more.
struct TestAllocator final : allocators::IAllocator {
  int64_t live = 0;
  int64_t fail_after = -1;
  void* allocate(uint64_t size) override {
    if (size == 0) return nullptr;
    if (fail_after == 0) return nullptr;
    if (fail_after > 0) --fail_after;
    ++live;
    return aligned_alloc(64, (size + 63) / 64 * 64);
  }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_alive = 0;
struct Tracked {
  uint64_t value;
  explicit Tracked(uint64_t v) : value(v) { ++g_alive; }
  Tracked(const Tracked& o) : value(o.value) { ++g_alive; }
  Tracked(Tracked&& o) : value(o.value) { ++g_alive; }
  ~Tracked() { --g_alive; }
};
struct Pinned {  // neither copyable nor movable
  int v;
  explicit Pinned(int x) : v(x) {}
  Pinned(const Pinned&) = delete;
};

static uint64_t sum_of(Hive<Tracked>& h) {
  uint64_t s = 0;
  for (Tracked& t : h) s += t.value;
  return s;
}

int main() {
  TestAllocator alloc;
  {
    Hive<Tracked> h(alloc);
    CHECK(h.empty() && h.count() == 0 && h.capacity() == 0);
    ObjectCursor c;
    CHECK(!h.first(&c));
    CHECK(h.begin() == h.end());

    // Stable addresses across growth through several blocks.
    std::vector<Tracked*> ptrs;
    for (uint64_t i = 0; i < 5000; ++i) {
      Tracked* t = h.emplace(i);
      CHECK(t && t->value == i);
      ptrs.push_back(t);
    }
    CHECK(h.count() == 5000 && g_alive == 5000 && h.capacity() >= 5000);
    for (uint64_t i = 0; i < 5000; ++i) CHECK(ptrs[i]->value == i && h.contains(ptrs[i]));
    CHECK(sum_of(h) == 5000ull * 4999 / 2);

    // Erase every other element; the survivors do not move.
    for (uint64_t i = 0; i < 5000; i += 2) CHECK(h.erase(ptrs[i]));
    CHECK(h.count() == 2500 && g_alive == 2500);
    for (uint64_t i = 1; i < 5000; i += 2) CHECK(ptrs[i]->value == i);
    CHECK(!h.erase(ptrs[0]));          // already erased
    CHECK(!h.contains(ptrs[0]));
    Tracked outsider(7);
    CHECK(!h.erase(&outsider) && !h.contains(&outsider));
    const uint64_t misaligned = reinterpret_cast<uint64_t>(ptrs[1]) + 1;
    CHECK(!h.contains(reinterpret_cast<Tracked*>(misaligned)));
    CHECK(!h.erase(static_cast<const Tracked*>(nullptr)));

    // Holes are refilled before capacity grows.
    const uint64_t cap = h.capacity();
    for (uint64_t i = 0; i < 2500; ++i) CHECK(h.insert(Tracked(100000 + i)));
    CHECK(h.count() == 5000 && h.capacity() == cap);

    // Walk and count; erase while walking.
    uint64_t seen = 0;
    for (Tracked& t : h) { (void)t; ++seen; }
    CHECK(seen == 5000);
    for (bool more = h.first(&c); more;) more = (h.element(c)->value >= 100000) ? h.erase(&c) : h.next(&c);
    CHECK(h.count() == 2500);
    for (Tracked& t : h) CHECK(t.value < 5000 && t.value % 2 == 1);

    // Erase everything: all but one block is released.
    for (bool more = h.first(&c); more;) more = h.erase(&c);
    CHECK(h.empty() && g_alive == 1 /* outsider */);
    CHECK(h.capacity() == Hive<Tracked>::kMaxBlock || h.capacity() < 5000);
    CHECK(h.first(&c) == false);
    Tracked* again = h.emplace(9);
    CHECK(again && again->value == 9 && h.count() == 1);
    h.clear();
    CHECK(h.count() == 0 && h.capacity() == 0 && alloc.live == 0);
  }
  CHECK(g_alive == 0 && alloc.live == 0);

  // Destructor destroys what is left.
  {
    Hive<Tracked> h(alloc);
    for (uint64_t i = 0; i < 100; ++i) h.emplace(i);
    CHECK(g_alive == 100);
  }
  CHECK(g_alive == 0 && alloc.live == 0);

  // Non-movable elements.
  {
    Hive<Pinned> h(alloc);
    Pinned* a = h.emplace(1);
    Pinned* b = h.emplace(2);
    CHECK(a && b && a != b && a->v == 1 && b->v == 2);
  }

  // reserve() and allocation failure.
  {
    Hive<uint32_t> h(alloc);
    CHECK(h.reserve(3000) && h.capacity() >= 3000 && h.count() == 0);
    const uint64_t cap = h.capacity();
    for (uint32_t i = 0; i < 3000; ++i) CHECK(h.insert(i));
    CHECK(h.capacity() == cap);
    Hive<uint32_t> small(alloc);
    alloc.fail_after = 0;
    CHECK(small.emplace(1u) == nullptr && small.count() == 0 && !small.reserve(10));
    alloc.fail_after = 2;  // the block and its slots fit, the bitmap does not
    CHECK(small.emplace(1u) == nullptr);
    alloc.fail_after = -1;
    const int64_t before = alloc.live;
    CHECK(small.emplace(5u) && *small.begin() == 5u);
    CHECK(alloc.live > before);
  }
  CHECK(alloc.live == 0);

  // Randomized comparison against a model.
  {
    Hive<Tracked> h(alloc);
    std::vector<Tracked*> model;
    uint64_t rng = 0x9e3779b97f4a7c15ull, expect_sum = 0;
    auto rand = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    for (int step = 0; step < 200000; ++step) {
      if (model.empty() || rand() % 100 < 52) {
        uint64_t v = rand() % 1000000;
        Tracked* t = h.emplace(v);
        CHECK(t);
        model.push_back(t);
        expect_sum += v;
      } else {
        size_t i = rand() % model.size();
        expect_sum -= model[i]->value;
        CHECK(h.erase(model[i]));
        model[i] = model.back();
        model.pop_back();
      }
      if (step % 5000 == 0) CHECK(sum_of(h) == expect_sum && h.count() == model.size());
    }
    CHECK(sum_of(h) == expect_sum && h.count() == model.size() && uint64_t(g_alive) == model.size());
  }
  CHECK(g_alive == 0 && alloc.live == 0);

  if (g_failed) { printf("HiveTest: %d check(s) failed\n", g_failed); return 1; }
  printf("HiveTest: all passed\n");
  return 0;
}
