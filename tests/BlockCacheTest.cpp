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

// The block cache (src/BlockCache): ARC's lists against a reference model, scan resistance, the cache against a model of the disk with
// random reads, writes, flushes, discards and write zeroes over a small cache (so pages are evicted and written back all the time), coalesced
// writeback, device errors, and threads reading and writing at once with a flusher, a device that completes out of order and checks that no
// two writes of the same block are on it at once (built with the address and undefined-behaviour sanitizers and, separately, ThreadSanitizer).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>
#include "BlockCache/ArcShard.hpp"
#include "BlockCache/BlockCache.hpp"

using namespace blockcache;

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

struct HostEnv {
  struct Mutex {
    std::mutex m;
    void lock() { m.lock(); }
    void unlock() { m.unlock(); }
  };
  struct Condition {
    std::condition_variable_any cv;
    bool wait(Mutex& m, uint64_t us) { return cv.wait_for(m, std::chrono::microseconds(us)) == std::cv_status::no_timeout; }
    void notify_all() { cv.notify_all(); }
  };
  static uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
};

using Cache = BlockCache<HostEnv>;

// ---------------------------------------------------------------- ARC

// ARC as the paper has it (with REPLACE only when the cache is full), on standard containers.
struct ReferenceArc {
  uint32_t c, p = 0;
  std::list<uint64_t> t1, t2, b1, b2;   // front = MRU
  explicit ReferenceArc(uint32_t capacity) : c(capacity) {}
  static bool take(std::list<uint64_t>& l, uint64_t x) {
    for (auto it = l.begin(); it != l.end(); ++it)
      if (*it == x) { l.erase(it); return true; }
    return false;
  }
  void replace(bool in_b2) {
    if (!t1.empty() && (t1.size() > p || (in_b2 && t1.size() == p))) {
      b1.push_front(t1.back());
      t1.pop_back();
    } else {
      b2.push_front(t2.back());
      t2.pop_back();
    }
  }
  void access(uint64_t x) {
    if (take(t1, x) || take(t2, x)) { t2.push_front(x); return; }
    if (take(b1, x)) {
      const uint32_t d = b1.size() + 1 >= b2.size() ? 1 : static_cast<uint32_t>(b2.size() / (b1.size() + 1));
      p = p + d > c ? c : p + d;
      if (t1.size() + t2.size() >= c) replace(false);
      t2.push_front(x);
      return;
    }
    if (take(b2, x)) {
      const uint32_t d = b2.size() + 1 >= b1.size() ? 1 : static_cast<uint32_t>(b1.size() / (b2.size() + 1));
      p = p > d ? p - d : 0;
      if (t1.size() + t2.size() >= c) replace(true);
      t2.push_front(x);
      return;
    }
    bool made_room = false;
    if (t1.size() + b1.size() >= c) {
      if (t1.size() < c) b1.pop_back();
      else { t1.pop_back(); made_room = true; }
    } else if (t1.size() + t2.size() + b1.size() + b2.size() >= 2 * c) {
      b2.pop_back();
    }
    if (!made_room && t1.size() + t2.size() >= c) replace(false);
    t1.push_front(x);
  }
};

void test_arc_model() {
  TestAllocator allocator;
  std::mt19937_64 rng(7);
  for (uint32_t capacity : {1u, 2u, 3u, 8u, 33u}) {
    ArcShard arc(allocator);
    CHECK(arc.initialize(capacity, 64));
    ReferenceArc ref(capacity);
    for (int step = 0; step < 20000; ++step) {
      // A mix of a hot set, a warm set and one-off pages.
      const uint32_t r = rng() % 10;
      const uint64_t page = r < 5 ? rng() % (capacity + 1) : r < 8 ? 100 + rng() % (3 * capacity + 1) : 100000 + rng() % 100000;
      bool inserted;
      const uint32_t i = arc.access(page, &inserted);
      ref.access(page);
      CHECK(i != kNone);
      if (i == kNone) break;
      arc.entry(i).valid = 1;   // (it holds something, as a read would leave it)
      CHECK(arc.list_size(ArcList::T1) == ref.t1.size());
      CHECK(arc.list_size(ArcList::T2) == ref.t2.size());
      CHECK(arc.list_size(ArcList::B1) == ref.b1.size());
      CHECK(arc.list_size(ArcList::B2) == ref.b2.size());
      CHECK(arc.target() == ref.p);
      CHECK(arc.resident() <= capacity && arc.resident() + arc.ghosts() <= 2 * capacity);
      if (g_failed) { printf("  capacity %u step %d page %llu\n", capacity, step, (unsigned long long)page); return; }
    }
    // Every resident page of the model is found, every ghost is not.
    for (uint64_t x : ref.t1) CHECK(arc.find(x) != kNone);
    for (uint64_t x : ref.t2) CHECK(arc.find(x) != kNone);
    for (uint64_t x : ref.b1) CHECK(arc.find(x) == kNone);
  }
  CHECK(allocator.live == 0);
}

void test_arc_scan_resistance() {
  TestAllocator allocator;
  ArcShard arc(allocator);
  CHECK(arc.initialize(100, 64));
  bool inserted;
  // A working set of 60 pages used twice, then a scan of 10000 pages used once: the working set stays.
  for (int round = 0; round < 2; ++round)
    for (uint64_t p = 0; p < 60; ++p) arc.access(p, &inserted);
  for (uint64_t p = 1000; p < 11000; ++p) arc.access(p, &inserted);
  uint32_t kept = 0;
  for (uint64_t p = 0; p < 60; ++p) kept += arc.find(p) != kNone;
  CHECK(kept == 60);
  // Busy and dirty pages are not evicted; with all of them busy there is no room.
  ArcShard small(allocator);
  CHECK(small.initialize(4, 64));
  for (uint64_t p = 0; p < 4; ++p) {
    const uint32_t i = small.access(p, &inserted);
    if (p == 0) small.entry(i).loading = true;
    if (p == 1) small.entry(i).writeback = true;
    if (p >= 2) small.mark_dirty(i, 1, p, 0);
  }
  CHECK(small.access(10, &inserted) == kNone);
  CHECK(small.dirty_pages() == 2);
  small.clear_dirty(small.find(2), 1);
  CHECK(small.dirty_pages() == 1);
  const uint32_t n = small.access(10, &inserted);
  CHECK(n != kNone && inserted && small.find(2) == kNone && small.find(0) != kNone && small.find(1) != kNone);
}

// ---------------------------------------------------------------- the disk

// A sparse disk of `blocks` blocks. Unwritten blocks read as a function of their number. Optionally slow, failing, and checking that no two
// writes of one block are in flight at once.
struct MemDisk final : IBlockBackend {
  uint32_t bs;
  uint64_t blocks;
  std::mutex mutex;
  std::unordered_map<uint64_t, std::vector<uint8_t>> data;
  std::vector<std::atomic<uint8_t>> writing;
  std::atomic<bool> fail_writes{false}, fail_reads{false};
  std::atomic<uint32_t> delay_us{0};
  std::atomic<uint64_t> reads{0}, writes{0}, write_bytes{0}, flushes{0}, overlaps{0}, discards{0}, zeroes{0};
  std::atomic<uint64_t> largest_write{0};
  MemDisk(uint32_t block_size, uint64_t block_count, bool track) : bs(block_size), blocks(block_count), writing(track ? block_count : 0) {}

  static uint8_t initial(uint64_t lba, uint32_t off) { return static_cast<uint8_t>(lba * 31 + off * 7 + 1); }
  void get(uint64_t lba, uint8_t* out) {
    auto it = data.find(lba);
    if (it == data.end()) for (uint32_t i = 0; i < bs; ++i) out[i] = initial(lba, i);
    else memcpy(out, it->second.data(), bs);
  }
  void pause() {
    const uint32_t d = delay_us.load();
    if (d) std::this_thread::sleep_for(std::chrono::microseconds(rand() % (d + 1)));
  }
  bool read(uint64_t lba, ByteView buffer) override {
    if (buffer.size() % bs || lba + buffer.size() / bs > blocks) return false;
    pause();
    if (fail_reads) return false;
    ++reads;
    std::lock_guard<std::mutex> g(mutex);
    for (uint64_t b = 0; b < buffer.size() / bs; ++b) get(lba + b, buffer.data() + b * bs);
    return true;
  }
  bool write(uint64_t lba, ConstByteView buffer) override {
    if (buffer.size() % bs || lba + buffer.size() / bs > blocks) return false;
    const uint64_t n = buffer.size() / bs;
    if (!writing.empty())
      for (uint64_t b = 0; b < n; ++b) if (writing[lba + b].exchange(1)) ++overlaps;
    pause();
    bool ok = !fail_writes;
    if (ok) {
      std::lock_guard<std::mutex> g(mutex);
      for (uint64_t b = 0; b < n; ++b) data[lba + b].assign(buffer.data() + b * bs, buffer.data() + (b + 1) * bs);
      ++writes;
      write_bytes += buffer.size();
      uint64_t big = largest_write.load();
      while (buffer.size() > big && !largest_write.compare_exchange_weak(big, buffer.size())) {}
    }
    if (!writing.empty()) for (uint64_t b = 0; b < n; ++b) writing[lba + b].store(0);
    return ok;
  }
  bool flush() override { ++flushes; return true; }
  bool fill(uint64_t lba, uint64_t n, uint8_t value) {
    if (lba + n > blocks) return false;
    std::lock_guard<std::mutex> g(mutex);
    for (uint64_t b = 0; b < n; ++b) data[lba + b].assign(bs, value);
    return true;
  }
  bool discard(uint64_t lba, uint64_t n) override { ++discards; return fill(lba, n, 0xDD); }
  bool write_zeroes(uint64_t lba, uint64_t n) override { ++zeroes; return fill(lba, n, 0); }
  std::vector<uint8_t> raw(uint64_t lba, uint64_t n) {
    std::vector<uint8_t> out(n * bs);
    std::lock_guard<std::mutex> g(mutex);
    for (uint64_t b = 0; b < n; ++b) get(lba + b, out.data() + b * bs);
    return out;
  }
};

BlockCacheConfig small_config(uint32_t bs, uint64_t blocks, uint64_t capacity_bytes, uint32_t shards) {
  BlockCacheConfig c;
  c.block_size = bs;
  c.block_count = blocks;
  c.page_bytes = 4096;
  c.capacity_bytes = capacity_bytes;
  c.shards = shards;
  c.max_write_bytes = 64 << 10;
  c.max_read_bytes = 32 << 10;
  return c;
}

// Random operations against a model of the disk, on a cache much smaller than the disk.
void test_model(uint32_t bs, uint64_t blocks, uint32_t shards, uint64_t seed) {
  TestAllocator allocator;
  MemDisk disk(bs, blocks, true);
  {
    Cache cache(allocator);
    CHECK(cache.initialize(disk, small_config(bs, blocks, 64 * 4096, shards)));
    std::vector<uint8_t> model(blocks * bs);
    for (uint64_t b = 0; b < blocks; ++b)
      for (uint32_t i = 0; i < bs; ++i) model[b * bs + i] = MemDisk::initial(b, i);
    std::mt19937_64 rng(seed);
    std::vector<uint8_t> buf;
    for (int step = 0; step < 6000 && !g_failed; ++step) {
      const uint32_t op = rng() % 100;
      const uint64_t n = 1 + (rng() % 6 == 0 ? rng() % 200 : rng() % 12);
      const uint64_t lba = rng() % (blocks - n + 1);
      buf.assign(n * bs, 0);
      if (op < 45) {
        CHECK(cache.read(lba, ByteView(buf.data(), buf.size())));
        CHECK(memcmp(buf.data(), &model[lba * bs], buf.size()) == 0);
        if (g_failed) printf("  read step %d lba %llu n %llu\n", step, (unsigned long long)lba, (unsigned long long)n);
      } else if (op < 85) {
        for (auto& x : buf) x = static_cast<uint8_t>(rng());
        CHECK(cache.write(lba, ConstByteView(buf.data(), buf.size())));
        memcpy(&model[lba * bs], buf.data(), buf.size());
      } else if (op < 90) {
        CHECK(cache.flush());
        CHECK(disk.raw(0, blocks) == model);
      } else if (op < 94) {
        CHECK(cache.discard(lba, n));
        memset(&model[lba * bs], 0xDD, n * bs);   // (what this disk does with a discard)
      } else if (op < 97) {
        CHECK(cache.write_zeroes(lba, n));
        memset(&model[lba * bs], 0, n * bs);
      } else {
        cache.write_back_due(HostEnv::now_us() + 10000000);
      }
    }
    CHECK(cache.flush());
    CHECK(disk.raw(0, blocks) == model);
    CHECK(disk.overlaps == 0);
    const BlockCacheStatistics s = cache.statistics();
    CHECK(s.read_hits > 0 && s.read_misses > 0 && s.evictions > 0 && s.writebacks > 0);
    CHECK(s.dirty_pages == 0 && s.resident_pages <= s.capacity_pages);
    // Bad requests.
    uint8_t one[4096];
    CHECK(!cache.read(blocks, ByteView(one, bs)));
    CHECK(!cache.read(0, ByteView(one, bs - 1)));
    CHECK(!cache.write(blocks - 1, ConstByteView(one, 2ull * bs)));
    CHECK(!cache.discard(blocks, 1));
  }
  CHECK(allocator.live == 0);
}

void test_geometry() {
  TestAllocator allocator;
  MemDisk disk(512, 100, false);
  Cache cache(allocator);
  BlockCacheConfig c = small_config(512, 100, 1 << 20, 1);
  c.page_bytes = 1000;
  CHECK(!cache.initialize(disk, c));    // (not a multiple of the block)
  c.page_bytes = 64 * 1024;
  CHECK(!cache.initialize(disk, c));    // (128 blocks a page)
  c.page_bytes = 4096;
  c.shards = 3;
  CHECK(!cache.initialize(disk, c));
  c.shards = 0;
  CHECK(cache.initialize(disk, c));
  CHECK(cache.shard_count() >= 1);
  // 4 KiB blocks: one block a page.
  MemDisk big(4096, 300, true);
  Cache cache4(allocator);
  CHECK(cache4.initialize(big, small_config(4096, 300, 16 * 4096, 2)));
  std::vector<uint8_t> a(4096 * 20, 0x42), b(4096 * 20);
  CHECK(cache4.write(280, ConstByteView(a.data(), a.size())));
  CHECK(cache4.read(280, ByteView(b.data(), b.size())) && a == b);
  CHECK(cache4.flush() && big.raw(280, 20) == a);
}

// Pages written one at a time reach the device as long writes; a read of a whole region is one device read per run.
void test_coalescing() {
  TestAllocator allocator;
  MemDisk disk(512, 1 << 16, true);
  Cache cache(allocator);
  BlockCacheConfig c = small_config(512, 1 << 16, 4 << 20, 4);
  c.max_write_bytes = 256 << 10;
  c.max_read_bytes = 128 << 10;
  c.dirty_percent = 100;
  CHECK(cache.initialize(disk, c));
  std::vector<uint8_t> block(4096);
  // 1 MiB in 4 KiB writes, backwards (so it is not the order of the writes that makes the runs).
  for (int64_t p = 255; p >= 0; --p) {
    for (auto& x : block) x = static_cast<uint8_t>(p);
    CHECK(cache.write(static_cast<uint64_t>(p) * 8, ConstByteView(block.data(), block.size())));
  }
  CHECK(disk.writes == 0);   // (all still in the cache)
  CHECK(cache.flush());
  CHECK(disk.writes <= 8 && disk.largest_write == (256u << 10));
  printf("  1 MiB of 4 KiB writes went out as %llu device writes\n", (unsigned long long)disk.writes.load());
  const uint64_t reads_before = disk.reads;
  std::vector<uint8_t> region(512 << 10);
  CHECK(cache.read(4096, ByteView(region.data(), region.size())));   // (not cached yet: 512 KiB in runs of 128 KiB)
  CHECK(disk.reads - reads_before == 4);
  // A read of the cached megabyte goes nowhere.
  std::vector<uint8_t> mb(1 << 20);
  const uint64_t reads_mid = disk.reads;
  CHECK(cache.read(0, ByteView(mb.data(), mb.size())));
  CHECK(disk.reads == reads_mid);
  for (uint64_t p = 0; p < 256; ++p) CHECK(mb[p * 4096] == static_cast<uint8_t>(p));
  // Partial pages: blocks 1 and 3 of a page dirty, 2 not valid: two writes, not one that would put stale data on block 2.
  Cache fresh(allocator);
  MemDisk d2(512, 1024, true);
  CHECK(fresh.initialize(d2, small_config(512, 1024, 1 << 20, 1)));
  uint8_t x[512];
  memset(x, 0x11, sizeof(x));
  CHECK(fresh.write(1, ConstByteView(x, 512)));
  memset(x, 0x33, sizeof(x));
  CHECK(fresh.write(3, ConstByteView(x, 512)));
  CHECK(fresh.flush() && d2.writes == 2);
  std::vector<uint8_t> raw = d2.raw(2, 1);
  CHECK(raw[0] == MemDisk::initial(2, 0));
}

// The flusher writes old data back by itself; a writer over the dirty limit writes back itself.
void test_expiry_and_throttle() {
  TestAllocator allocator;
  MemDisk disk(512, 1 << 16, true);
  Cache cache(allocator);
  BlockCacheConfig c = small_config(512, 1 << 16, 1 << 20, 4);
  c.expire_us = 1000000;
  CHECK(cache.initialize(disk, c));
  std::vector<uint8_t> block(4096, 7);
  CHECK(cache.write(0, ConstByteView(block.data(), block.size())));
  const uint64_t now = HostEnv::now_us();
  CHECK(!cache.writeback_due(now));
  CHECK(cache.write_back_due(now) == 0 && disk.writes == 0);
  CHECK(cache.writeback_due(now + 2000000));
  CHECK(cache.write_back_due(now + 2000000) == 1 && disk.writes == 1 && cache.dirty_pages() == 0);
  // 1 MiB cache in 4 shards (64 pages each, limit 16): writing 2 MiB makes the writer write back as it goes.
  std::vector<uint8_t> two(2 << 20, 9);
  CHECK(cache.write(8, ConstByteView(two.data(), two.size())));
  const BlockCacheStatistics s = cache.statistics();
  CHECK(s.throttled > 0);
  CHECK(s.dirty_pages <= 4 * 16 + 4);
  std::vector<uint8_t> back(two.size());
  CHECK(cache.read(8, ByteView(back.data(), back.size())) && back == two);
  int notified = 0;
  cache.set_dirty_notifier([](void* n) { ++*static_cast<int*>(n); }, &notified);
  for (uint64_t p = 0; p < 40; ++p) CHECK(cache.write(10000 + p * 8, ConstByteView(block.data(), block.size())));
  CHECK(notified > 0);
}

void test_errors() {
  TestAllocator allocator;
  MemDisk disk(512, 4096, true);
  {
    Cache cache(allocator);
    CHECK(cache.initialize(disk, small_config(512, 4096, 1 << 20, 2)));
    std::vector<uint8_t> a(8192, 0x5C), b(8192);
    CHECK(cache.write(100, ConstByteView(a.data(), a.size())));
    disk.fail_writes = true;
    CHECK(!cache.flush());
    CHECK(cache.dirty_pages() > 0);   // (kept, to be written again)
    CHECK(cache.read(100, ByteView(b.data(), b.size())) && a == b);
    disk.fail_writes = false;
    CHECK(cache.flush() && disk.raw(100, 16) == a && cache.dirty_pages() == 0);
    // Writing back without a device flush (eject's way).
    std::vector<uint8_t> c2(4096, 0x77);
    CHECK(cache.write(500, ConstByteView(c2.data(), c2.size())));
    const uint64_t flushes = disk.flushes;
    CHECK(cache.write_back_all() && disk.flushes == flushes && disk.raw(500, 8) == c2);
    // A failed read leaves nothing behind; the next one reads again.
    disk.fail_reads = true;
    CHECK(!cache.read(3000, ByteView(b.data(), b.size())));
    disk.fail_reads = false;
    CHECK(cache.read(3000, ByteView(b.data(), b.size())) && b == disk.raw(3000, 16));
    // A device that is gone: everything dropped.
    CHECK(cache.write(200, ConstByteView(a.data(), a.size())));
    cache.drop_all();
    CHECK(cache.dirty_pages() == 0 && cache.statistics().resident_pages == 0);
    // A trim of a huge range on a cache that holds a few pages: only those are looked at.
    CHECK(cache.write(10, ConstByteView(a.data(), a.size())));
    CHECK(cache.discard(0, 4096));
    CHECK(cache.flush() && disk.raw(10, 1)[0] == 0xDD);
  }
  CHECK(allocator.live == 0);
}

// Threads at once: each writes its own region and checks it, all read a shared region that only one changes, a flusher runs, and
// discards and zeroing come from one more. The device completes in random order and at random speed.
void test_threads() {
  TestAllocator allocator;
  constexpr uint32_t kThreads = 8;
  constexpr uint64_t kRegion = 2048;   // blocks of 512 B
  constexpr uint64_t kBlocks = kRegion * (kThreads + 2);
  MemDisk disk(512, kBlocks, true);
  disk.delay_us = 30;
  Cache cache(allocator);
  BlockCacheConfig c = small_config(512, kBlocks, 512 * 4096, 8);
  c.expire_us = 2000;
  CHECK(cache.initialize(disk, c));
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> ops{0};
  std::thread flusher([&] {
    while (!stop) {
      cache.write_back_due(HostEnv::now_us());
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  });
  std::vector<std::thread> threads;
  for (uint32_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(1000 + t);
      const uint64_t base = kRegion * t;
      std::vector<uint8_t> model(kRegion * 512);
      for (uint64_t b = 0; b < kRegion; ++b)
        for (uint32_t i = 0; i < 512; ++i) model[b * 512 + i] = MemDisk::initial(base + b, i);
      std::vector<uint8_t> buf;
      for (int step = 0; step < 1500 && !g_failed; ++step) {
        const uint64_t n = 1 + rng() % 40;
        const uint64_t off = rng() % (kRegion - n + 1);
        buf.resize(n * 512);
        const uint32_t op = rng() % 100;
        if (op < 40) {
          for (auto& x : buf) x = static_cast<uint8_t>(rng());
          CHECK(cache.write(base + off, ConstByteView(buf.data(), buf.size())));
          memcpy(&model[off * 512], buf.data(), buf.size());
        } else if (op < 80) {
          CHECK(cache.read(base + off, ByteView(buf.data(), buf.size())));
          CHECK(memcmp(buf.data(), &model[off * 512], buf.size()) == 0);
        } else if (op < 95) {
          // The shared region: only thread 0 writes it (whole 4 KiB pages of one byte value), the others check each page is one value.
          const uint64_t shared = kRegion * kThreads;
          const uint64_t page = rng() % (kRegion / 8);
          buf.resize(4096);
          if (t == 0) {
            memset(buf.data(), static_cast<int>(rng() % 250 + 1), 4096);
            CHECK(cache.write(shared + page * 8, ConstByteView(buf.data(), 4096)));
          } else {
            CHECK(cache.read(shared + page * 8, ByteView(buf.data(), 4096)));
            bool same = true;
            if (buf[0] != MemDisk::initial(shared + page * 8, 0))
              for (uint32_t i = 1; i < 4096; ++i) same &= buf[i] == buf[0];
            CHECK(same);
          }
        } else if (op < 98) {
          CHECK(cache.discard(base + off, n));
          memset(&model[off * 512], 0xDD, n * 512);
        } else {
          CHECK(cache.flush());
        }
        ++ops;
      }
      CHECK(cache.flush());
      CHECK(disk.raw(base, kRegion) == model);
    });
  }
  for (auto& th : threads) th.join();
  stop = true;
  flusher.join();
  CHECK(disk.overlaps == 0);
  const BlockCacheStatistics s = cache.statistics();
  printf("  threads: %llu operations, %llu hits, %llu misses, %llu writebacks (%llu KiB average), %llu by writers, %llu ghost hits\n",
         (unsigned long long)ops.load(), (unsigned long long)s.read_hits, (unsigned long long)s.read_misses,
         (unsigned long long)s.writebacks, (unsigned long long)(s.writebacks ? s.writeback_bytes / s.writebacks / 1024 : 0),
         (unsigned long long)s.throttled, (unsigned long long)(s.ghost_hits[0] + s.ghost_hits[1]));
}

// Several readers of the same missing page: one device read, all get the data.
void test_shared_miss() {
  TestAllocator allocator;
  MemDisk disk(512, 1 << 14, false);
  disk.delay_us = 2000;
  Cache cache(allocator);
  CHECK(cache.initialize(disk, small_config(512, 1 << 14, 1 << 20, 4)));
  std::vector<std::thread> threads;
  std::atomic<int> good{0};
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&] {
      uint8_t b[4096];
      if (cache.read(800, ByteView(b, sizeof(b))) && b[1] == MemDisk::initial(800, 1)) ++good;
    });
  for (auto& th : threads) th.join();
  CHECK(good == 8);
  CHECK(disk.reads <= 3);   // (one, unless a thread came after it finished and... then it hit; a few at most when threads start late)
}

}  // namespace

int main() {
  test_arc_model();
  test_arc_scan_resistance();
  test_geometry();
  test_model(512, 3001, 4, 1);
  test_model(512, 4096, 1, 2);
  test_model(2048, 1500, 2, 3);
  test_coalescing();
  test_expiry_and_throttle();
  test_errors();
  test_shared_miss();
  test_threads();
  if (g_failed) {
    printf("BlockCacheTest: %d FAILED\n", g_failed.load());
    return 1;
  }
  printf("BlockCacheTest: all passed\n");
  return 0;
}
