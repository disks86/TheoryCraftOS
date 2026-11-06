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

// The I/O scheduler (src/IoScheduler): the cost model, the fair queue (weights, classes, deadlines, weight raising, domains, queue sets, the
// pool) on simulated time against expected values and a random model check, the depth controller's histogram and limits, and the whole
// scheduler with threads submitting to a simulated device of several slots at once - no slot over-committed, nothing lost, weights honoured
// (built with the address and undefined-behaviour sanitizers and, separately, ThreadSanitizer).
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "IoScheduler/FairQueue.hpp"
#include "IoScheduler/IoScheduler.hpp"
#include "IoScheduler/LatencyController.hpp"

using namespace iosched;

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int64_t> live{0};
  void* allocate(uint64_t size) override {
    if (size == 0) return nullptr;
    ++live;
    return aligned_alloc(64, (size + 63) / 64 * 64);
  }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override {
    if (p) {
      --live;
      free(p);
    }
  }
};

struct HostLock {
  std::mutex m;
  void lock() { m.lock(); }
  void unlock() { m.unlock(); }
};

IoPriority be(uint16_t weight = kDefaultWeight) { return IoPriority{IoClass::BestEffort, weight}; }
IoPriority rt() { return IoPriority{IoClass::Realtime, kDefaultWeight}; }
IoPriority idle() { return IoPriority{IoClass::Idle, kDefaultWeight}; }

FairQueueConfig plain_config() {
  FairQueueConfig c;
  c.raise_factor = 1;   // no weight raising unless a test wants it
  return c;
}

// Runs the picked request to completion at once (a device of depth 1 with no service time).
uint32_t run_one(FairQueue& q, uint64_t now, uint32_t domains = kAllDomains) {
  const uint32_t t = q.pick(domains, now);
  if (t == FairQueue::kNone) return t;
  q.commit(t, now);
  q.complete(t, now);
  return t;
}

void test_cost_model() {
  const IoCostModel nvme = IoCostModel::for_media(MediaKind::Nvme);
  CHECK(nvme.cost(IoOp::Read, 4096) == 2000 + 1300);
  CHECK(nvme.cost(IoOp::Read, 4097) == 2000 + 2 * 1300);
  CHECK(nvme.cost(IoOp::Read, 0) == 2000);
  CHECK(nvme.cost(IoOp::Write, 8192) == 4000 + 2 * 2000);
  CHECK(nvme.cost(IoOp::Flush, 0) == 100000);
  CHECK(nvme.cost(IoOp::Write, ~uint64_t{0}) == 4000 + 0xFFFFFFFFull * 2000);   // saturates, no wrap
  for (MediaKind k : {MediaKind::Nvme, MediaKind::SolidState, MediaKind::Rotational, MediaKind::Virtual, MediaKind::Removable}) {
    const IoCostModel m = IoCostModel::for_media(k);
    CHECK(m.cost(IoOp::Write, 65536) >= m.cost(IoOp::Read, 65536));   // writes never cheaper than reads
    CHECK(m.target_us[0] > 0 && m.target_us[0] < m.target_us[1]);
    CHECK(m.deadline_us[0] < m.deadline_us[1]);
  }

  CHECK(priority_from_cpu_priority(8).io_class == IoClass::BestEffort && priority_from_cpu_priority(8).weight == kDefaultWeight);
  CHECK(priority_from_cpu_priority(0).io_class == IoClass::Idle);
  CHECK(priority_from_cpu_priority(31).io_class == IoClass::Realtime);
  CHECK(priority_from_cpu_priority(16).weight > priority_from_cpu_priority(8).weight);
  CHECK(priority_from_cpu_priority(4).weight < priority_from_cpu_priority(8).weight);
  CHECK(clamp_priority(IoPriority{IoClass::BestEffort, 0}).weight == kMinWeight);
  CHECK(clamp_priority(IoPriority{IoClass::BestEffort, 5000}).weight == kMaxWeight);
  CHECK(clamp_priority(IoPriority{static_cast<IoClass>(9), 10}).io_class == IoClass::BestEffort);

  CHECK(vtime_before(~uint64_t{0} - 5, 3));    // across the wrap
  CHECK(!vtime_before(3, ~uint64_t{0} - 5));
  CHECK(vtime_max(~uint64_t{0} - 5, 3) == 3);
}

// Two backlogged queues of weights 100 and 300 share the device 1:3.
void test_weights() {
  TestAllocator a;
  {
    FairQueue q(a);
    CHECK(q.initialize(256, plain_config()));
    uint64_t now = 1000;
    for (int i = 0; i < 100; ++i) {
      CHECK(q.enqueue(1, be(100), IoOp::Read, 4096, now) != FairQueue::kNone);
      CHECK(q.enqueue(2, be(300), IoOp::Read, 4096, now) != FairQueue::kNone);
    }
    uint32_t served[3] = {};
    // Serve 100 requests; keep both backlogged by refilling the queue that was served.
    for (int i = 0; i < 100; ++i) {
      const uint32_t t = q.pick(kAllDomains, now);
      CHECK(t != FairQueue::kNone);
      IssuerState s1, s2;
      q.issuer_state(1, &s1);
      q.issuer_state(2, &s2);
      q.commit(t, now);
      IssuerState a1, a2;
      q.issuer_state(1, &a1);
      q.issuer_state(2, &a2);
      const uint64_t who = a1.queued < s1.queued ? 1 : 2;
      ++served[who];
      q.complete(t, now);
      q.enqueue(who, be(who == 1 ? 100 : 300), IoOp::Read, 4096, now);
      now += 1;
    }
    CHECK(served[1] + served[2] == 100);
    CHECK(served[1] >= 22 && served[1] <= 28);   // 25 expected
    IssuerState s1, s2;
    CHECK(q.issuer_state(1, &s1) && q.issuer_state(2, &s2));
    CHECK(s1.effective_weight == 100 && s2.effective_weight == 300);
    CHECK(!s1.raised && !s2.raised);
    // Drain.
    while (run_one(q, now) != FairQueue::kNone) {}
    CHECK(q.queued() == 0 && q.in_flight() == 0);
    CHECK(q.statistics().enqueued == q.statistics().completed);
  }
  CHECK(a.live == 0);
}

// Cheaper requests: a queue issuing 4 KiB reads against one issuing 1 MiB reads at the same weight gets the same device time, so many more requests.
void test_cost_fairness() {
  TestAllocator a;
  FairQueue q(a);
  CHECK(q.initialize(256, plain_config()));
  uint64_t now = 1;
  for (int i = 0; i < 64; ++i) {
    q.enqueue(1, be(), IoOp::Read, 4096, now);
    q.enqueue(2, be(), IoOp::Read, 1 << 20, now);
  }
  for (int i = 0; i < 60; ++i) run_one(q, now);
  IssuerState s1, s2;
  q.issuer_state(1, &s1);
  q.issuer_state(2, &s2);
  CHECK(s1.queued < 64 && s2.queued < 64);
  CHECK(64 - s1.queued > 10 * (64 - s2.queued));   // the small reader went many times more often
  const double ratio = static_cast<double>(s1.served_cost) / static_cast<double>(s2.served_cost);
  CHECK(ratio > 0.4 && ratio < 2.5);
}

void test_classes() {
  TestAllocator a;
  FairQueue q(a);
  FairQueueConfig c = plain_config();
  c.idle_grace_us = 2000;
  CHECK(q.initialize(256, c));
  uint64_t now = 10;
  for (int i = 0; i < 5; ++i) {
    q.enqueue(1, idle(), IoOp::Read, 4096, now);
    q.enqueue(2, be(), IoOp::Read, 4096, now);
    q.enqueue(3, rt(), IoOp::Read, 4096, now);
  }
  // Realtime first, then BestEffort.
  for (int i = 0; i < 5; ++i) {
    const uint32_t t = q.pick(kAllDomains, now);
    q.commit(t, now);
    IssuerState s;
    q.issuer_state(3, &s);
    CHECK(s.queued == static_cast<uint32_t>(4 - i));
    q.complete(t, now);
  }
  for (int i = 0; i < 5; ++i) CHECK(run_one(q, now) != FairQueue::kNone);
  IssuerState s;
  q.issuer_state(2, &s);
  CHECK(s.queued == 0);
  // Idle waits for the grace time after the last BestEffort completion.
  CHECK(q.pick(kAllDomains, now) == FairQueue::kNone);
  CHECK(q.idle_wait_until(now) == now + 2000);
  CHECK(q.pick(kAllDomains, now + 1999) == FairQueue::kNone);
  CHECK(q.pick(kAllDomains, now + 2000) != FairQueue::kNone);
  CHECK(q.idle_wait_until(now + 2000) == 0);
  now += 2000;
  // While Idle runs, a BestEffort request goes first.
  q.enqueue(2, be(), IoOp::Read, 4096, now);
  const uint32_t t = q.pick(kAllDomains, now);
  q.commit(t, now);
  q.issuer_state(2, &s);
  CHECK(s.in_flight == 1);
  q.complete(t, now);
  // ...and puts the Idle class back to waiting.
  CHECK(q.pick(kAllDomains, now + 10) == FairQueue::kNone);
  while (q.queued()) {
    now += 2000;
    run_one(q, now);
  }
  CHECK(q.in_flight() == 0);
}

// A request whose deadline has passed goes first, whatever the classes say.
void test_deadlines() {
  TestAllocator a;
  FairQueue q(a);
  CHECK(q.initialize(512, plain_config()));
  const IoCostModel m = IoCostModel::for_media(MediaKind::SolidState);
  uint64_t now = 100;
  q.enqueue(2, be(), IoOp::Write, 4096, now);           // BestEffort write: deadline m.deadline_us[1]
  for (int i = 0; i < 200; ++i) q.enqueue(1, rt(), IoOp::Read, 4096, now);
  now += 10;
  CHECK(!q.expired_dispatch(run_one(q, now)));
  now = 100 + m.deadline_us[1];                          // the write's deadline (the realtime reads' passed long ago, at deadline/4)
  const uint32_t t = q.pick(kAllDomains, now);
  CHECK(t != FairQueue::kNone && q.domain(t) == IoDomain::Read);   // expired realtime reads are older: they go first
  // Only writes free (the controller is out of read slots): the write goes, as expired.
  const uint32_t w = q.pick(domain_bit(IoDomain::Write), now);
  CHECK(w != FairQueue::kNone && q.domain(w) == IoDomain::Write);
  q.commit(w, now);
  CHECK(q.expired_dispatch(w));
  q.complete(w, now);
  CHECK(q.statistics().expired >= 1);
  while (run_one(q, now) != FairQueue::kNone) {}
  CHECK(q.queued() == 0);

  // An Idle request does not wait for ever behind a busy BestEffort issuer: once its deadline is the earliest passed one, it goes. Here the
  // BestEffort backlog is 50 ms deep, so that is 50 ms after its own deadline at the latest.
  now += 1000000;
  const uint64_t idle_start = now;
  for (int i = 0; i < 50; ++i) q.enqueue(5, be(), IoOp::Read, 4096, now);
  q.enqueue(6, idle(), IoOp::Read, 4096, now);
  bool idle_served = false;
  for (int i = 0; i < 600 && !idle_served; ++i) {
    now += 1000;   // 1 ms a request
    const uint32_t t2 = q.pick(kAllDomains, now);
    q.commit(t2, now);
    IssuerState s;
    q.issuer_state(6, &s);
    idle_served = s.in_flight == 1;
    q.complete(t2, now);
    q.enqueue(5, be(), IoOp::Read, 4096, now);
  }
  CHECK(idle_served);
  CHECK(now - idle_start >= uint64_t{m.deadline_us[0]} * 10);   // (not before: BestEffort always had something queued)
  CHECK(now - idle_start <= uint64_t{m.deadline_us[0]} * 10 + 51000);
}

// BFQ's low-latency heuristic: a reader back from a long pause is raised, one that streams is not.
void test_weight_raising() {
  TestAllocator a;
  FairQueue q(a);
  FairQueueConfig c;   // raising on (factor 10)
  c.interactive_gap_us = 20000;
  c.raise_duration_us = 200000;
  c.raise_budget = c.cost.cost(IoOp::Read, 4096) * 20;   // 20 small reads
  CHECK(q.initialize(512, c));
  uint64_t now = 1000;
  // The streamer: a write-heavy backlog (writes are never raised).
  for (int i = 0; i < 200; ++i) q.enqueue(1, be(), IoOp::Write, 65536, now);
  IssuerState s;
  q.issuer_state(1, &s);
  CHECK(!s.raised && s.effective_weight == kDefaultWeight);
  for (int i = 0; i < 20; ++i) run_one(q, now += 100);

  // The interactive reader appears: raised, and its read goes within a couple of picks.
  q.enqueue(2, be(), IoOp::Read, 4096, now);
  q.issuer_state(2, &s);
  CHECK(s.raised && s.effective_weight == 10 * kDefaultWeight);
  int picks = 0;
  for (;;) {
    ++picks;
    const uint32_t t = q.pick(kAllDomains, now += 100);
    q.commit(t, now);
    IssuerState r;
    q.issuer_state(2, &r);
    q.complete(t, now);
    if (r.queued == 0) break;
  }
  CHECK(picks <= 2);
  CHECK(q.statistics().raised == 1);

  // Reading back to back (no gap) it stays one busy period and loses the raise once it used up the budget.
  for (int i = 0; i < 40; ++i) {
    q.enqueue(2, be(), IoOp::Read, 4096, now += 10);
    for (;;) {
      const uint32_t t = q.pick(kAllDomains, now += 10);
      q.commit(t, now);
      IssuerState r;
      q.issuer_state(2, &r);
      q.complete(t, now);
      if (r.queued == 0) break;
    }
  }
  q.issuer_state(2, &s);
  CHECK(!s.raised);
  // Back after a pause, but its last busy period was long (40 reads > budget): no raise.
  now += 50000;
  q.enqueue(2, be(), IoOp::Read, 4096, now);
  q.issuer_state(2, &s);
  CHECK(!s.raised);
  while (true) {
    IssuerState r;
    q.issuer_state(2, &r);
    if (r.queued == 0) break;
    run_one(q, now += 10);
  }
  // A soft real-time reader: a few reads, a pause, again: raised every time.
  for (int round = 0; round < 3; ++round) {
    now += 30000;
    q.enqueue(3, be(), IoOp::Read, 4096, now);
    q.issuer_state(3, &s);
    CHECK(s.raised);
    while (true) {
      IssuerState r;
      q.issuer_state(3, &r);
      if (r.queued == 0) break;
      run_one(q, now += 10);
    }
  }
  // A raise also ends with time.
  now += 30000;
  q.enqueue(3, be(), IoOp::Read, 4096, now);
  q.issuer_state(3, &s);
  CHECK(s.raised);
  for (int i = 0; i < 2; ++i) q.enqueue(1, be(), IoOp::Write, 65536, now);
  now += c.raise_duration_us + 1;
  while (q.queued()) run_one(q, now += 10);
  q.issuer_state(3, &s);
  CHECK(!s.raised);
}

void test_domains() {
  TestAllocator a;
  FairQueue q(a);
  CHECK(q.initialize(64, plain_config()));
  q.enqueue(1, be(), IoOp::Read, 4096, 1);
  q.enqueue(1, be(), IoOp::Write, 4096, 2);
  q.enqueue(1, be(), IoOp::Flush, 0, 3);
  CHECK(q.queued_in(IoDomain::Read) == 1 && q.queued_in(IoDomain::Write) == 1 && q.queued_in(IoDomain::Other) == 1);
  CHECK(q.pick(0, 10) == FairQueue::kNone);
  uint32_t t = q.pick(domain_bit(IoDomain::Other), 10);
  CHECK(t != FairQueue::kNone && q.domain(t) == IoDomain::Other);
  t = q.pick(domain_bit(IoDomain::Write) | domain_bit(IoDomain::Other), 10);
  CHECK(q.domain(t) == IoDomain::Write);   // the oldest of the free domains
  q.commit(t, 10);
  CHECK(q.queued_in(IoDomain::Write) == 0 && q.in_flight() == 1);
  t = q.pick(kAllDomains, 10);
  CHECK(q.domain(t) == IoDomain::Read);
  q.commit(t, 10);
  q.complete(t, 11);
  CHECK(q.in_flight() == 1);
  // commit/complete of tickets in the wrong state change nothing.
  q.complete(t, 12);
  q.commit(t, 12);
  q.complete(12345, 12);
  CHECK(q.in_flight() == 1 && q.queued() == 1);
}

void test_sets_and_pool() {
  TestAllocator a;
  {
    FairQueue q(a);
    CHECK(q.initialize(1024, plain_config()));
    // More issuers than queues: the extra ones share the queues of their class, nothing is lost.
    for (uint64_t i = 0; i < 600; ++i) CHECK(q.enqueue(1000 + i, be(), IoOp::Read, 4096, 5) != FairQueue::kNone);
    CHECK(q.statistics().overflowed >= 600 - FairQueue::kIssuerQueues);
    uint32_t n = 0;
    while (run_one(q, 6) != FairQueue::kNone) ++n;
    CHECK(n == 600 && q.queued() == 0 && q.in_flight() == 0);
    // Idle queues are reused by new issuers.
    const uint64_t overflowed = q.statistics().overflowed;
    for (uint64_t i = 0; i < 200; ++i) q.enqueue(5000 + i, be(), IoOp::Read, 4096, 7);
    CHECK(q.statistics().overflowed - overflowed < 40);   // only sets that got more than 4 of the 200
    while (run_one(q, 8) != FairQueue::kNone) {}
  }
  {
    FairQueue q(a);
    CHECK(q.initialize(4, plain_config()));
    for (int i = 0; i < 4; ++i) CHECK(q.enqueue(1, be(), IoOp::Read, 4096, 1) != FairQueue::kNone);
    CHECK(q.enqueue(1, be(), IoOp::Read, 4096, 1) == FairQueue::kNone);
    CHECK(q.statistics().pool_full == 1);
    const uint32_t t = q.pick(kAllDomains, 1);
    q.commit(t, 1);
    CHECK(q.enqueue(1, be(), IoOp::Read, 4096, 1) == FairQueue::kNone);   // in flight still holds its slot
    q.complete(t, 2);
    CHECK(q.enqueue(1, be(), IoOp::Read, 4096, 2) != FairQueue::kNone);
  }
  {
    FairQueue q(a);
    CHECK(!q.initialize(0, plain_config()));
    CHECK(q.enqueue(1, be(), IoOp::Read, 1, 1) == FairQueue::kNone);
    CHECK(q.pick(kAllDomains, 1) == FairQueue::kNone);
  }
  CHECK(a.live == 0);
}

// Random operations against a model of the counts; every ticket is dispatched and completed exactly once.
void test_random_model() {
  TestAllocator a;
  FairQueue q(a);
  CHECK(q.initialize(128, FairQueueConfig{}));
  uint64_t rng = 0x12345678;
  auto next = [&rng] {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  std::vector<int> state(128, 0);   // 0 free, 1 queued, 2 in flight
  std::vector<uint32_t> flying;
  uint32_t queued = 0;
  uint64_t now = 1;
  for (int step = 0; step < 200000; ++step) {
    now += next() % 50;
    const uint32_t r = next() % 10;
    if (r < 4) {
      const IoClass c = static_cast<IoClass>(next() % 3);
      const IoOp op = static_cast<IoOp>(next() % 3);
      const uint32_t t = q.enqueue(next() % 40, IoPriority{c, static_cast<uint16_t>(1 + next() % 1000)}, op, next() % (1 << 20), now);
      if (t != FairQueue::kNone) {
        CHECK(t < 128 && state[t] == 0);
        state[t] = 1;
        ++queued;
      } else {
        CHECK(queued + flying.size() == 128);
      }
    } else if (r < 7) {
      const uint32_t mask = static_cast<uint32_t>(next() % 8);
      const uint32_t t = q.pick(mask, now);
      if (t != FairQueue::kNone) {
        CHECK(state[t] == 1 && (mask & domain_bit(q.domain(t))));
        q.commit(t, now);
        state[t] = 2;
        --queued;
        flying.push_back(t);
      }
    } else if (!flying.empty()) {
      const uint32_t i = static_cast<uint32_t>(next() % flying.size());
      const uint32_t t = flying[i];
      flying[i] = flying.back();
      flying.pop_back();
      q.complete(t, now);
      state[t] = 0;
    }
    CHECK(q.queued() == queued && q.in_flight() == flying.size());
    if (g_failed > 20) return;
  }
  // Everything drains (deadlines and the idle grace guarantee it).
  for (int i = 0; i < 1000000 && (q.queued() || !flying.empty()); ++i) {
    now += 1000;
    const uint32_t t = q.pick(kAllDomains, now);
    if (t != FairQueue::kNone) q.commit(t, now), flying.push_back(t);
    if (!flying.empty()) {
      q.complete(flying.back(), now);
      flying.pop_back();
    }
  }
  CHECK(q.queued() == 0 && q.in_flight() == 0);
  CHECK(q.statistics().enqueued == q.statistics().completed);
}

void test_latency_controller() {
  CHECK(LatencyController::bucket_of(0, 1000) == 0);
  CHECK(LatencyController::bucket_of(249, 1000) == 0);
  CHECK(LatencyController::bucket_of(250, 1000) == 1);
  CHECK(LatencyController::bucket_of(999, 1000) == 3);
  CHECK(LatencyController::bucket_of(1000, 1000) == 4);
  CHECK(LatencyController::bucket_of(1999, 1000) == 4);
  CHECK(LatencyController::bucket_of(3999, 1000) == 5);
  CHECK(LatencyController::bucket_of(7999, 1000) == 6);
  CHECK(LatencyController::bucket_of(8000, 1000) == 7);
  CHECK(LatencyController::bucket_of(~uint64_t{0}, 1000) == 7);
  CHECK(LatencyController::bucket_of(5, 0) == 6);   // (a target of 0 counts as 1 us)

  const IoCostModel m = IoCostModel::for_media(MediaKind::Nvme);   // read target 1000 us, write 5000 us
  LatencyController c;
  c.initialize(64, m, 100000, 0);
  CHECK(c.limit(IoDomain::Read) == 64 && c.limit(IoDomain::Write) == 48 && c.limit(IoDomain::Other) == 16);
  // Reads too slow while writes run: the writes are throttled, window after window, down to 1.
  uint64_t now = 0;
  uint32_t expected = 48;
  for (int w = 0; w < 8; ++w) {
    for (int i = 0; i < 50; ++i) c.record(IoDomain::Read, 3000, now);
    for (int i = 0; i < 50; ++i) c.record(IoDomain::Write, 2000, now);
    c.adjust(now += 100000);
    expected = expected / 2 ? expected / 2 : 1;
    CHECK(c.limit(IoDomain::Write) == expected);
    CHECK(c.limit(IoDomain::Read) == 64);      // reads over target but under 8x: not throttled themselves
    CHECK(c.limit(IoDomain::Other) == 16);     // no flush traffic: left alone
  }
  CHECK(c.statistics().last_p90_bucket[0] == 5);
  // Reads fine again and writes waiting for slots: writes grow back by a quarter (at least one) per window, up to the cap.
  for (int w = 0; w < 40; ++w) {
    for (int i = 0; i < 50; ++i) c.record(IoDomain::Read, 200, now);
    for (int i = 0; i < 50; ++i) c.record(IoDomain::Write, 2000, now);
    c.note_throttled(IoDomain::Write);
    c.adjust(now += 100000);
  }
  CHECK(c.limit(IoDomain::Write) == 48);
  // Writes alone and 8x over their target: they lose a quarter of their own depth.
  for (int i = 0; i < 50; ++i) c.record(IoDomain::Write, 50000, now);
  c.adjust(now += 100000);
  CHECK(c.limit(IoDomain::Write) == 36);
  CHECK(c.limit(IoDomain::Read) == 64);      // more important than writes: untouched
  // Too few samples say nothing: an idle domain recovers.
  for (int i = 0; i < 3; ++i) c.record(IoDomain::Write, 50000, now);
  c.adjust(now += 100000);
  CHECK(c.limit(IoDomain::Write) == 45);
  // record() ends the window by itself.
  const uint64_t windows = c.statistics().windows;
  c.record(IoDomain::Read, 10, now + 50000);
  CHECK(c.statistics().windows == windows);
  c.record(IoDomain::Read, 10, now + 100000);
  CHECK(c.statistics().windows == windows + 1);

  LatencyController one;
  one.initialize(1, m, 100000, 0);
  CHECK(one.limit(IoDomain::Read) == 1 && one.limit(IoDomain::Write) == 1 && one.limit(IoDomain::Other) == 1);
  for (int i = 0; i < 50; ++i) one.record(IoDomain::Read, 100000, 0);
  for (int i = 0; i < 50; ++i) one.record(IoDomain::Write, 100000, 0);
  one.adjust(1);
  CHECK(one.limit(IoDomain::Read) == 1 && one.limit(IoDomain::Write) == 1);   // never below 1
}

uint64_t now_us() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void spin_us(uint64_t us) {
  const uint64_t until = now_us() + us;
  while (now_us() < until) {}
}

struct HostWaiter final : IoWaiter {
  std::atomic<bool> granted{false};
  void grant() override { granted.store(true, std::memory_order_release); }   // (the last thing that touches the waiter)
};

// Runs one request through the scheduler the way the kernel wrapper does: submit, wait (kicking when the hint runs out), run, complete.
template <class Device>
void do_request(IoScheduler<HostLock>& s, uint64_t issuer, IoPriority p, IoOp op, uint64_t bytes, Device&& device) {
  HostWaiter waiter;
  IoTicket ticket;
  SubmitResult r;
  while ((r = s.submit(issuer, p, op, bytes, waiter, &ticket, now_us())) == SubmitResult::Full) std::this_thread::yield();
  if (r == SubmitResult::Queued) {
    uint64_t look = now_us() + s.wait_hint_us(now_us());
    while (!waiter.granted.load(std::memory_order_acquire)) {
      if (now_us() >= look) {
        s.kick(now_us());
        look = now_us() + s.wait_hint_us(now_us());
      }
      std::this_thread::yield();
    }
  }
  device(ticket.domain);
  s.complete(ticket, now_us());
}

// Many threads, mixed classes and operations, a device of 4 slots: never more than 4 on it, nothing lost, everything accounted for.
void test_threads_stress() {
  TestAllocator a;
  {
    IoScheduler<HostLock> s(a);
    IoSchedulerConfig c;
    c.depth = 4;
    c.capacity = 24;   // fewer than the threads' requests at times: Full gets exercised
    c.window_us = 2000;
    c.fair.cost = IoCostModel::for_media(MediaKind::Nvme);
    c.fair.idle_grace_us = 200;
    CHECK(s.initialize(c, now_us()));
    std::atomic<int> on_device{0}, peak{0};
    std::atomic<int> per_domain[kDomainCount] = {};
    std::atomic<uint64_t> done{0};
    constexpr int kThreads = 24, kRequests = 1500;
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&, i] {
        uint64_t rng = 0x9E37 + i * 7919;
        for (int n = 0; n < kRequests; ++n) {
          rng = rng * 6364136223846793005ull + 1442695040888963407ull;
          const IoOp op = static_cast<IoOp>((rng >> 33) % 3);
          const IoPriority p = i % 8 == 0 ? rt() : i % 8 == 1 ? idle() : be(static_cast<uint16_t>(50 + 50 * (i % 4)));
          do_request(s, static_cast<uint64_t>(i % 12), p, op, 4096u << ((rng >> 40) % 5), [&](IoDomain d) {
            const int now_on = ++on_device;
            int old = peak.load();
            while (now_on > old && !peak.compare_exchange_weak(old, now_on)) {}
            CHECK(now_on <= 4);
            ++per_domain[domain_index(d)];
            if ((rng >> 50) % 4 == 0) spin_us(5);
            --per_domain[domain_index(d)];
            --on_device;
          });
          ++done;
        }
      });
    }
    for (std::thread& t : threads) t.join();
    CHECK(done == uint64_t{kThreads} * kRequests);
    const IoSchedulerStatistics st = s.statistics();
    CHECK(st.queued == 0 && st.in_flight == 0);
    CHECK(st.fair.enqueued == st.fair.completed && st.fair.dispatched == st.fair.completed);
    CHECK(st.fair.enqueued + st.bypassed == done);
    CHECK(peak <= 4);
    printf("  stress: %llu requests, %llu bypassed, %llu queued, %llu expired, peak %d on device, write limit %u\n",
           (unsigned long long)done.load(), (unsigned long long)st.bypassed, (unsigned long long)st.fair.enqueued,
           (unsigned long long)st.fair.expired, peak.load(), st.depth.limit[1]);
  }
  CHECK(a.live == 0);
}

// Two groups of threads, weights 100 and 400, keep a device of depth 1 saturated: the second gets about four times the service.
void test_threads_weights() {
  TestAllocator a;
  IoScheduler<HostLock> s(a);
  IoSchedulerConfig c;
  c.depth = 1;
  c.capacity = 64;
  c.fair.raise_factor = 1;
  CHECK(s.initialize(c, now_us()));
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> served[2] = {};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      const int group = i % 2;
      while (!stop.load()) {
        do_request(s, 100 + group, be(group ? 400 : 100), IoOp::Read, 4096, [&](IoDomain) { spin_us(20); });
        ++served[group];
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  stop = true;
  for (std::thread& t : threads) t.join();
  const double ratio = static_cast<double>(served[1].load()) / static_cast<double>(served[0].load() ? served[0].load() : 1);
  printf("  weights: %llu vs %llu requests (ratio %.2f, 4 wanted)\n", (unsigned long long)served[0].load(), (unsigned long long)served[1].load(), ratio);
  CHECK(ratio > 2.5 && ratio < 6.0);
  CHECK(s.statistics().in_flight == 0);
}

// Realtime threads against BestEffort ones on a saturated device: a realtime request has at most the one on the device ahead of it, a best
// effort one waits its turn behind the others. Counted in requests that went to the device in between (wall-clock waits on the host are
// mostly the host's own thread scheduling).
void test_threads_latency() {
  TestAllocator a;
  IoScheduler<HostLock> s(a);
  IoSchedulerConfig c;
  c.depth = 1;
  c.capacity = 64;
  CHECK(s.initialize(c, now_us()));
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> sequence{0};
  std::atomic<uint64_t> ahead_sum[2] = {}, count[2] = {};
  std::vector<std::thread> threads;
  for (int i = 0; i < 13; ++i) {
    threads.emplace_back([&, i] {
      const int realtime = i == 0;
      while (!stop.load()) {
        const uint64_t before = sequence.load();
        uint64_t ahead = 0;
        do_request(s, 200 + i, realtime ? rt() : be(), realtime ? IoOp::Read : IoOp::Write, 65536, [&](IoDomain) {
          ahead = sequence.fetch_add(1) - before;
          spin_us(20);
        });
        ahead_sum[realtime] += ahead;
        ++count[realtime];
        if (realtime) std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop = true;
  for (std::thread& t : threads) t.join();
  const double rt_ahead = static_cast<double>(ahead_sum[1]) / static_cast<double>(count[1] ? count[1].load() : 1);
  const double be_ahead = static_cast<double>(ahead_sum[0]) / static_cast<double>(count[0] ? count[0].load() : 1);
  printf("  latency: %.2f requests ahead of a realtime one on average, %.2f ahead of a best effort one\n", rt_ahead, be_ahead);
  CHECK(count[1] > 10);
  CHECK(rt_ahead < 2.0);
  CHECK(be_ahead > 4.0);
}

// A flash drive queues internally: here every request takes longer the more are on the device. Forty threads flood it with writes while two
// read; the depth controller sees the reads over their target and throttles the writes' share of the 32 slots.
void test_threads_depth_control() {
  TestAllocator a;
  IoScheduler<HostLock> s(a);
  IoSchedulerConfig c;
  c.depth = 32;
  c.capacity = 128;
  c.window_us = 20000;
  c.fair.cost = IoCostModel::for_media(MediaKind::Nvme);
  c.fair.cost.target_us[0] = 300;    // reads: 300 us
  c.fair.cost.target_us[1] = 20000;
  CHECK(s.initialize(c, now_us()));
  const uint32_t cap = s.controller().cap(IoDomain::Write);
  std::atomic<bool> stop{false};
  std::atomic<int> on_device{0};
  std::atomic<uint32_t> lowest{cap};
  std::vector<std::thread> threads;
  for (int i = 0; i < 42; ++i) {
    threads.emplace_back([&, i] {
      const bool reader = i < 2;
      while (!stop.load()) {
        do_request(s, 300 + i, be(), reader ? IoOp::Read : IoOp::Write, 16384, [&](IoDomain) {
          const int n = ++on_device;
          std::this_thread::sleep_for(std::chrono::microseconds(20 * n));
          --on_device;
        });
        const uint32_t limit = s.controller().limit(IoDomain::Write);
        uint32_t old = lowest.load();
        while (limit < old && !lowest.compare_exchange_weak(old, limit)) {}
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  stop = true;
  for (std::thread& t : threads) t.join();
  const IoSchedulerStatistics st = s.statistics();
  printf("  depth control: write limit cap %u, lowest %u, now %u; %llu windows, %llu shrinks, %llu grows\n", cap, lowest.load(), st.depth.limit[1],
         (unsigned long long)st.depth.windows, (unsigned long long)st.depth.shrinks, (unsigned long long)st.depth.grows);
  CHECK(st.depth.shrinks > 0);
  CHECK(lowest < cap / 2);
  CHECK(st.depth.limit[0] == 32);   // reads keep the whole depth
  CHECK(st.in_flight == 0 && st.queued == 0);
}

// One thread and a free device: every request takes the lock-free path.
void test_bypass() {
  TestAllocator a;
  IoScheduler<HostLock> s(a);
  IoSchedulerConfig c;
  c.depth = 2;
  CHECK(s.initialize(c, now_us()));
  for (int i = 0; i < 100; ++i) do_request(s, 1, be(), IoOp::Read, 4096, [](IoDomain) {});
  IoSchedulerStatistics st = s.statistics();
  CHECK(st.bypassed == 100 && st.fair.enqueued == 0);
  // Idle requests always queue (and with the device quiet, go at once).
  do_request(s, 1, idle(), IoOp::Read, 4096, [](IoDomain) {});
  st = s.statistics();
  CHECK(st.fair.enqueued == 1 && st.fair.completed == 1);
  // No bypass when configured off.
  IoScheduler<HostLock> t(a);
  c.bypass = false;
  CHECK(t.initialize(c, now_us()));
  do_request(t, 1, be(), IoOp::Read, 4096, [](IoDomain) {});
  CHECK(t.statistics().bypassed == 0 && t.statistics().fair.completed == 1);
}

}  // namespace

int main() {
  test_cost_model();
  test_weights();
  test_cost_fairness();
  test_classes();
  test_deadlines();
  test_weight_raising();
  test_domains();
  test_sets_and_pool();
  test_random_model();
  test_latency_controller();
  test_bypass();
  test_threads_stress();
  test_threads_weights();
  test_threads_latency();
  test_threads_depth_control();
  if (g_failed) {
    printf("IoSchedulerTest: %d FAILED\n", g_failed.load());
    return 1;
  }
  printf("IoSchedulerTest: all passed\n");
  return 0;
}
