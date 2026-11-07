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

// The network's socket lookup table (src/Network/RcuSocketTable.hpp): it grows and shrinks with what it holds and keeps every entry findable
// across each resize; a resize that runs out of memory leaves it as it was; and, with real threads (built with both the address sanitizer and
// ThreadSanitizer by tests/run_host_tests.sh), readers looking up a fixed set of sockets never miss one while a writer churns thousands of
// others through the table, forcing it to resize again and again.
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <thread>
#include <vector>
#include "Network/RcuSocketTable.hpp"

using namespace network;

static std::atomic<int> g_failed{0};
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

// (an out-of-bounds access is a failed test, not a kernel panic)
[[noreturn]] void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  printf("bounds violation: %s index %llu size %llu\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

// (as in RcuTest: a core number per thread, so readers spread over the domain's shards)
namespace {
std::atomic<uint32_t> g_next_core{1};
thread_local uint32_t t_core = ~uint32_t{0};
}  // namespace
uint32_t kernel::current_core() {
  if (t_core == ~uint32_t{0}) t_core = g_next_core++;
  return t_core;
}

namespace {

// Counts what is live; can be told to fail one allocation, or all of them.
struct TestAllocator final : allocators::IAllocator {
  std::atomic<long> live{0};
  std::atomic<long> fail_in{-1};   // the allocation after this many more fails (-1: none)
  std::atomic<bool> refuse{false};
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size || refuse) return nullptr;
    if (fail_in >= 0 && fail_in-- == 0) return nullptr;
    ++live;
    if (alignment < 16) alignment = 16;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override {
    if (!p) return;
    --live;
    free(p);
  }
};

struct FakeSocket {
  explicit FakeSocket(uint32_t i) : id(i) {}
  uint32_t id;
};

using Table = RcuSocketTable<FakeSocket, 64>;

SocketKey key_for(uint32_t id) {
  SocketKey k;
  k.local_port = static_cast<uint16_t>(id);
  k.remote_port = static_cast<uint16_t>(id >> 16);
  k.remote = IpAddress(Ipv4Address::make(10, 0, uint8_t(id >> 8), uint8_t(id)));
  return k;
}
uint32_t hash_for(uint32_t id) {
  const SocketKey k = key_for(id);
  return hash_connection(k.local_port, k.remote_port, k.remote);
}
bool same_key(const SocketKey& a, const SocketKey& b) {
  return a.local_port == b.local_port && a.remote_port == b.remote_port && a.remote.ipv4 == b.remote.ipv4;
}
SharedPointer<FakeSocket> lookup(const Table& table, uint32_t id) {
  const SocketKey want = key_for(id);
  return table.find(hash_for(id), [&](const SocketKey& k, const FakeSocket&) { return same_key(k, want); });
}

void test_grow_and_shrink() {
  TestAllocator alloc;
  {
    Table table(alloc);
    CHECK(table.bucket_count() == 0 && !lookup(table, 1));   // (nothing exists before the first insert)
    constexpr uint32_t kCount = 5000;
    std::vector<SharedPointer<FakeSocket>> sockets;
    uint32_t largest = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
      sockets.push_back(SharedPointer<FakeSocket>::make(alloc, i));
      CHECK(table.insert(hash_for(i), key_for(i), sockets.back()));
      if (table.bucket_count() > largest) largest = table.bucket_count();
    }
    CHECK(table.count() == kCount);
    CHECK(largest == 4096 && table.bucket_count() == 4096);   // (doubled whenever it held two entries a bucket)
    bool all = true;
    for (uint32_t i = 0; i < kCount; ++i) {
      const SharedPointer<FakeSocket> s = lookup(table, i);
      all = all && s && s->id == i;
    }
    CHECK(all);
    CHECK(!lookup(table, kCount + 7));
    CHECK(table.any(hash_for(10), [](const SocketKey& k, const FakeSocket& s) { return k.local_port == 10 && s.id == 10; }));
    for (uint32_t i = 0; i < kCount; i += 2) CHECK(table.remove(hash_for(i), sockets[i].get()));
    CHECK(!table.remove(hash_for(0), sockets[0].get()));
    all = true;
    for (uint32_t i = 0; i < kCount; ++i) all = all && bool(lookup(table, i)) == (i % 2 == 1);
    CHECK(all);
    for (uint32_t i = 1; i < kCount; i += 2) CHECK(table.remove(hash_for(i), sockets[i].get()));
    CHECK(table.count() == 0 && table.bucket_count() == 64);   // (halved below one entry in four buckets, never under the least)
  }
  CHECK(alloc.live == 0);   // (the vector went first, the table's nodes and arrays with it: nothing left)
}

// Out of memory in the middle of a resize: the table keeps its old array, every entry is still there, and the half-built copy is freed.
void test_resize_out_of_memory() {
  TestAllocator alloc;
  {
    Table table(alloc);
    std::vector<SharedPointer<FakeSocket>> sockets;
    for (uint32_t i = 0; i < 131; ++i) sockets.push_back(SharedPointer<FakeSocket>::make(alloc, i));
    for (uint32_t i = 0; i < 128; ++i) CHECK(table.insert(hash_for(i), key_for(i), sockets[i]));
    CHECK(table.bucket_count() == 64);
    const long before = alloc.live;
    // the next insert grows the table to 128 buckets: the new array (2 allocations) and 38 copies are made, the 39th copy fails
    alloc.fail_in = 40;
    CHECK(table.insert(hash_for(128), key_for(128), sockets[128]));   // (the resize gives up; the insert's own node is made)
    CHECK(table.bucket_count() == 64 && table.count() == 129);
    CHECK(alloc.fail_in == -1 && alloc.live == before + 1);   // (only the new node: the half-built array and its copies were freed)
    alloc.refuse = true;
    CHECK(!table.insert(hash_for(129), key_for(129), sockets[129]));   // no memory at all: refused, the table unchanged
    alloc.refuse = false;
    bool all = true;
    for (uint32_t i = 0; i < 129; ++i) all = all && lookup(table, i) && lookup(table, i)->id == i;
    CHECK(all && !lookup(table, 129));
    CHECK(table.insert(hash_for(129), key_for(129), sockets[129]) && table.bucket_count() == 128);   // (memory again: it grows now)
    CHECK(lookup(table, 0) && lookup(table, 129) && table.count() == 130);
  }
  CHECK(alloc.live == 0);
}

// Readers look up 1000 sockets that stay in the table the whole time, while a writer adds and removes 20000 others in waves, so the table
// grows and shrinks under them: no reader ever misses one of the 1000 or gets a wrong one, and nothing is used after it is freed.
void test_concurrent_resizes() {
  TestAllocator alloc;
  {
    Table table(alloc);
    constexpr uint32_t kStable = 1000, kChurn = 20000, kWaves = 6;
    std::vector<SharedPointer<FakeSocket>> stable;
    for (uint32_t i = 0; i < kStable; ++i) {
      stable.push_back(SharedPointer<FakeSocket>::make(alloc, i));
      CHECK(table.insert(hash_for(i), key_for(i), stable.back()));
    }
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> lookups{0}, misses{0}, wrong{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r)
      readers.emplace_back([&, r] {
        uint32_t i = r * 251;
        uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          i = (i + 1) % kStable;
          const SharedPointer<FakeSocket> s = lookup(table, i);
          if (!s) ++misses;
          else if (s->id != i) ++wrong;
          ++n;
        }
        lookups += n;
      });
    uint32_t resizes = 0, last = table.bucket_count(), peak = last;
    for (uint32_t wave = 0; wave < kWaves; ++wave) {
      std::vector<SharedPointer<FakeSocket>> churn;
      for (uint32_t i = 0; i < kChurn; ++i) {
        const uint32_t id = 100000 + wave * kChurn + i;
        churn.push_back(SharedPointer<FakeSocket>::make(alloc, id));
        table.insert(hash_for(id), key_for(id), churn.back());
        if (table.bucket_count() != last) ++resizes, last = table.bucket_count();
        if (last > peak) peak = last;
      }
      for (uint32_t i = 0; i < kChurn; ++i) {
        table.remove(hash_for(100000 + wave * kChurn + i), churn[i].get());
        if (table.bucket_count() != last) ++resizes, last = table.bucket_count();
      }
    }
    stop = true;
    for (std::thread& t : readers) t.join();
    printf("concurrent resizes: %u resizes (up to %u buckets), %llu lookups, %llu missed, %llu wrong\n", resizes, peak,
           (unsigned long long)lookups.load(), (unsigned long long)misses.load(), (unsigned long long)wrong.load());
    CHECK(resizes >= 4 * kWaves && peak == 16384);   // (21000 entries at most: doubled up to 16384 buckets)
    CHECK(misses == 0 && wrong == 0 && lookups > 1000);
    // (back to 1000 entries: halved each time it fell below a quarter - under 1024 of 4096 buckets - and 1000 is not below a quarter of 2048)
    CHECK(table.count() == kStable && table.bucket_count() == 2048);
  }
  CHECK(alloc.live == 0);
}

}  // namespace

int main() {
  test_grow_and_shrink();
  test_resize_out_of_memory();
  test_concurrent_resizes();
  if (g_failed) {
    printf("RcuSocketTableTest: %d failed\n", g_failed.load());
    return 1;
  }
  printf("RcuSocketTableTest: all passed\n");
  return 0;
}
