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

// Host-side tests for the asynchronous I/O requests and the completion queue (src/Process/IoRequest.*): completion in the order calls finish, taking by owner, data ownership,
// removing a process's requests, a randomized comparison against a plain model, and several threads adding, completing, taking and posting at the same time (the system lock
// of the kernel is a mutex here).
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "Process/IoRequest.hpp"
#include "Utility/AllocatedView.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct CountingAllocator final : allocators::IAllocator {
  std::atomic<int64_t> live{0};
  void* allocate(uint64_t size) override { if (size == 0) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { if (size == 0) return nullptr; ++live; return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { if (!p) return; --live; free(p); }
};

using namespace process::io;

IoRequest request(uint64_t owner, uint64_t thread, uint64_t iosb = 0) {
  IoRequest r;
  r.owner = owner;
  r.thread = thread;
  r.status_block = iosb;
  return r;
}

ByteView bytes(CountingAllocator& a, uint32_t n, uint8_t v) {
  ByteView view = allocate_view<uint8_t>(a, n);
  for (uint32_t i = 0; i < n; ++i) view[i] = v;
  return view;
}

void test_basic() {
  CountingAllocator a;
  {
    IoRequestTable t(a);
    IoRequest* r1 = t.add(request(1, 10, 0x1000));
    IoRequest* r2 = t.add(request(1, 11, 0x2000));
    IoRequest* r3 = t.add(request(2, 20, 0x3000));
    CHECK(r1 && r2 && r3 && r1->id != r2->id && r2->id != r3->id);
    const uint32_t id1 = r1->id, id2 = r2->id, id3 = r3->id;
    CHECK(t.count() == 3 && t.pending_count() == 3 && t.ready() == 0);
    CHECK(t.find(id2) == r2 && t.find(0) == nullptr && t.find(id3 + 100) == nullptr);
    IoRequest out[4];
    CHECK(t.take_completed(1, out, 4) == 0);      // nothing finished yet
    // finish in the order 3, 2, 1
    CHECK(t.complete(id3, 5, 77));
    CHECK(t.complete(id2, 0xC0000120u, 0, bytes(a, 8, 0xAB)));
    CHECK(!t.complete(id2, 1, 1));      // only once
    CHECK(!t.complete(9999, 1, 1));
    CHECK(t.complete(id1, 0, 12, bytes(a, 4, 0xCD)));
    CHECK(t.ready() == 3 && t.pending_count() == 0);
    CHECK(t.take_completed(1, out, 1) == 1 && out[0].id == id2 && out[0].status == 0xC0000120u && out[0].data.size() == 8 && out[0].data[3] == 0xAB);      // owner 1's oldest
    t.release_data(out[0].data);
    CHECK(t.take_completed(1, out, 4) == 1 && out[0].id == id1 && out[0].information == 12 && out[0].data.size() == 4);
    t.release_data(out[0].data);
    CHECK(t.take_completed(1, out, 4) == 0);
    CHECK(t.take_completed(2, out, 4) == 1 && out[0].id == id3 && out[0].information == 77 && out[0].data.empty());
    CHECK(t.count() == 0 && t.ready() == 0);
  }
  CHECK(a.live == 0);
}

void test_remove_owner() {
  CountingAllocator a;
  {
    IoRequestTable t(a);
    std::vector<uint32_t> ids;
    for (int i = 0; i < 40; ++i) ids.push_back(t.add(request(i % 2 ? 7 : 8, 1))->id);
    t.complete(ids[1], 0, 1, bytes(a, 16, 1));
    t.complete(ids[3], 0, 1, bytes(a, 16, 2));
    t.complete(ids[2], 0, 1, bytes(a, 16, 3));      // (owner 8's)
    CHECK(t.ready() == 3 && t.pending_count() == 37);
    IoRequest out[16];
    uint32_t total = 0, finished = 0;
    for (uint32_t n; (n = t.remove_owner(7, out, 16)) != 0;) {
      total += n;
      for (uint32_t i = 0; i < n; ++i) { CHECK(out[i].owner == 7); CHECK(out[i].data.empty()); if (out[i].completed) ++finished; }
    }
    CHECK(total == 20 && finished == 2);
    CHECK(t.count() == 20 && t.ready() == 1 && t.pending_count() == 19);
    IoRequest taken[4];
    CHECK(t.take_completed(8, taken, 4) == 1 && taken[0].id == ids[2]);      // the stale ids of the removed ones are skipped
    t.release_data(taken[0].data);
    CHECK(t.take_completed(7, taken, 4) == 0);
    t.erase(ids[0]);
    CHECK(t.count() == 18 && t.pending_count() == 18);
  }
  CHECK(a.live == 0);
}

void test_limit() {
  CountingAllocator a;
  IoRequestTable t(a);
  uint32_t added = 0;
  for (uint32_t i = 0; i < IoRequestTable::kMaxRequests + 10; ++i) if (t.add(request(1, 1))) ++added;
  CHECK(added == IoRequestTable::kMaxRequests);
}

void test_queue() {
  CountingAllocator a;
  {
    CompletionQueue q(a);
    Packet p;
    CHECK(!q.remove(&p) && q.depth() == 0);
    for (uint64_t i = 0; i < 100; ++i) { Packet k; k.key = i; k.context = i * 3; k.status = uint32_t(i); k.information = i + 5; CHECK(q.post(k)); }
    CHECK(q.depth() == 100);
    CHECK(q.remove(&p) && p.key == 0 && p.context == 0 && p.information == 5);
    Packet many[10];
    CHECK(q.remove_many(many, 10) == 10 && many[0].key == 1 && many[9].key == 10);
    CHECK(q.depth() == 89);
    q.clear();
    CHECK(q.depth() == 0 && !q.remove(&p));
  }
  CHECK(a.live == 0);
}

// A random run against a plain model: add, complete (with data), take by owner, remove an owner.
void test_random(uint32_t seed) {
  CountingAllocator a;
  {
    IoRequestTable t(a);
    struct Model { uint64_t owner; bool done; uint64_t order; };
    std::map<uint32_t, Model> model;
    uint64_t order = 0;
    srand(seed);
    for (int step = 0; step < 4000; ++step) {
      const int what = rand() % 10;
      if (what < 4) {
        IoRequest* r = t.add(request(1 + rand() % 3, 1));
        if (r) model[r->id] = {r->owner, false, 0};
      } else if (what < 7 && !model.empty()) {
        auto it = model.begin();
        std::advance(it, rand() % model.size());
        ByteView data = rand() % 2 ? bytes(a, 1 + rand() % 40, 9) : ByteView();
        const bool ok = t.complete(it->first, 1, 2, data);
        if (!ok) t.release_data(data);      // (a request that is over already does not take the data)
        CHECK(ok == !it->second.done);
        if (ok) { it->second.done = true; it->second.order = ++order; }
      } else if (what < 9) {
        const uint64_t owner = 1 + rand() % 3;
        IoRequest out[3];
        const uint32_t n = t.take_completed(owner, out, 3);
        std::vector<std::pair<uint64_t, uint32_t>> expected;
        for (auto& kv : model) if (kv.second.owner == owner && kv.second.done) expected.push_back({kv.second.order, kv.first});
        std::sort(expected.begin(), expected.end());
        CHECK(n == (expected.size() < 3 ? expected.size() : 3));
        for (uint32_t i = 0; i < n; ++i) { CHECK(out[i].id == expected[i].second); t.release_data(out[i].data); model.erase(out[i].id); }
      } else {
        const uint64_t owner = 1 + rand() % 3;
        IoRequest out[8];
        for (uint32_t n; (n = t.remove_owner(owner, out, 8)) != 0;) for (uint32_t i = 0; i < n; ++i) { CHECK(model.count(out[i].id) == 1); model.erase(out[i].id); }
        for (auto& kv : model) CHECK(kv.second.owner != owner);
      }
      uint32_t ready = 0, pending = 0;
      for (auto& kv : model) (kv.second.done ? ready : pending)++;
      CHECK(t.ready() == ready && t.pending_count() == pending && t.count() == model.size());
    }
  }
  CHECK(a.live == 0);
}

struct SystemLock {
  std::mutex m;
  std::condition_variable cv;
};

// Threads add requests of their own process, a completer finishes them in a shuffled order and every thread takes its own: each request is seen exactly once, in the order it
// finished, with its data intact.
void test_threads() {
  CountingAllocator a;
  {
    SystemLock lock;
    IoRequestTable table(a);
    constexpr int kOwners = 4, kPerOwner = 400;
    std::atomic<int> taken_total{0};
    std::mutex ids_lock;
    std::vector<uint32_t> to_complete;
    std::atomic<bool> adding{true};
    std::vector<std::thread> threads;
    std::vector<std::set<uint32_t>> seen(kOwners + 1);
    for (int o = 1; o <= kOwners; ++o) {
      threads.emplace_back([&, o] {
        int added = 0, got = 0;
        uint64_t last_sequence = 0;
        while (got < kPerOwner) {
          if (added < kPerOwner) {
            std::unique_lock<std::mutex> hold(lock.m);
            IoRequest r = request(o, o * 100, 0x1000 + added);
            IoRequest* made = table.add(r);
            if (made) { const uint32_t id = made->id; hold.unlock(); std::lock_guard<std::mutex> g(ids_lock); to_complete.push_back(id); ++added; }
          }
          IoRequest out[5];
          uint32_t n;
          {
            std::unique_lock<std::mutex> hold(lock.m);
            n = table.take_completed(o, out, 5);
          }
          for (uint32_t i = 0; i < n; ++i) {
            CHECK(out[i].owner == uint64_t(o));
            CHECK(out[i].sequence > last_sequence);
            last_sequence = out[i].sequence;
            CHECK(out[i].data.size() == 6 && out[i].data[5] == uint8_t(out[i].id));
            CHECK(seen[o].insert(out[i].id).second);
            std::unique_lock<std::mutex> hold(lock.m);
            table.release_data(out[i].data);
          }
          got += int(n);
          taken_total += int(n);
          if (n == 0) std::this_thread::yield();
        }
      });
    }
    std::thread completer([&] {
      int done = 0;
      srand(5);
      while (done < kOwners * kPerOwner) {
        uint32_t id = 0;
        {
          std::lock_guard<std::mutex> g(ids_lock);
          if (!to_complete.empty()) { const size_t k = size_t(rand()) % to_complete.size(); id = to_complete[k]; to_complete[k] = to_complete.back(); to_complete.pop_back(); }
        }
        if (id == 0) { std::this_thread::yield(); continue; }
        std::unique_lock<std::mutex> hold(lock.m);
        CHECK(table.complete(id, 0, 6, bytes(a, 6, uint8_t(id))));
        ++done;
      }
    });
    completer.join();
    for (std::thread& t : threads) t.join();
    CHECK(taken_total == kOwners * kPerOwner);
    CHECK(table.count() == 0 && table.ready() == 0 && table.pending_count() == 0);
  }
  CHECK(a.live == 0);
}

// Producers post packets, consumers wait for them on the condition variable: all arrive once, in the order of each producer.
void test_queue_threads() {
  CountingAllocator a;
  {
    SystemLock lock;
    CompletionQueue q(a);
    constexpr int kProducers = 4, kPackets = 2000, kConsumers = 3;
    std::atomic<int> consumed{0};
    std::vector<std::vector<uint64_t>> got(kConsumers);
    std::vector<std::thread> threads;
    for (int c = 0; c < kConsumers; ++c) {
      threads.emplace_back([&, c] {
        for (;;) {
          std::unique_lock<std::mutex> hold(lock.m);
          Packet p;
          while (!q.remove(&p)) {
            if (consumed >= kProducers * kPackets) return;
            lock.cv.wait_for(hold, std::chrono::milliseconds(5));
          }
          ++consumed;
          got[c].push_back(p.key * kPackets + p.context);
          if (consumed >= kProducers * kPackets) lock.cv.notify_all();
        }
      });
    }
    for (int pr = 0; pr < kProducers; ++pr) {
      threads.emplace_back([&, pr] {
        for (int i = 0; i < kPackets; ++i) {
          std::unique_lock<std::mutex> hold(lock.m);
          Packet p; p.key = uint64_t(pr); p.context = uint64_t(i);
          CHECK(q.post(p));
          lock.cv.notify_one();
        }
      });
    }
    for (std::thread& t : threads) t.join();
    CHECK(consumed == kProducers * kPackets);
    std::set<uint64_t> all;
    for (auto& v : got) {
      std::vector<int> last(kProducers, -1);
      for (uint64_t k : v) {
        const int pr = int(k / kPackets), i = int(k % kPackets);
        CHECK(i > last[pr]);      // (each producer's packets leave in the order they came, per consumer)
        last[pr] = i;
        all.insert(k);
      }
    }
    CHECK(all.size() == size_t(kProducers) * kPackets);
    CHECK(q.depth() == 0);
  }
  CHECK(a.live == 0);
}

}  // namespace

int main() {
  test_basic();
  test_remove_owner();
  test_limit();
  test_queue();
  for (uint32_t seed = 1; seed <= 20; ++seed) test_random(seed);
  test_threads();
  test_queue_threads();
  if (g_failed) { printf("IoRequestTest: %d failed\n", g_failed.load()); return 1; }
  printf("IoRequestTest: all passed\n");
  return 0;
}
