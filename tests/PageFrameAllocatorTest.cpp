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

// Host tests for src/Allocators/PageFrameAllocator (LLFree) and the lock-free Allocator over it: frames of every size and alignment,
// runs across chunks, double frees, the low and high zones, and several threads at once (each its own "core", then all sharing one
// slot) allocating, writing, handing memory to each other and freeing, with everything accounted for at the end.
// `PageFrameAllocatorTest bench` also times the heap against a spin-locked one on 1..N threads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "Allocators/Allocator.hpp"

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

using allocators::Allocator;
using allocators::PageFrameAllocator;
constexpr uint64_t kFrame = 4096, kMiB = 1 << 20;

static thread_local uint32_t t_core = 0;
static uint32_t thread_core() { return t_core; }

struct FakeRam final : hardware::IRandomAccessMemory {
  void* region = nullptr;
  uint64_t bytes = 0;
  const char* name() const override { return "fake ram"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t total_bytes() const override { return bytes; }
  uint64_t usable_bytes() const override { return bytes; }
  void* base() const override { return region; }
};

static void* map_anywhere(uint64_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}
static void* map_at(uint64_t address, uint64_t bytes) {
  void* p = mmap(reinterpret_cast<void*>(address), bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}
static uint64_t addr(const void* p) { return reinterpret_cast<uint64_t>(p); }

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
  uint64_t below(uint64_t n) { return next() % n; }
};

// Single thread: shapes, alignment, runs, accounting, misuse.
static void test_frames_basic() {
  const uint64_t bytes = 70 * kMiB;   // (more than one 64 MiB tree)
  void* region = map_anywhere(bytes + 2 * kMiB);
  CHECK(region != nullptr);
  if (!region) return;
  // Start the region 1 MiB + one frame past a 2 MiB boundary, so the first chunk is partial.
  const uint64_t base = ((addr(region) + 2 * kMiB - 1) & ~(2 * kMiB - 1)) + kMiB + kFrame;
  PageFrameAllocator frames;
  CHECK(!frames.add_region(base, 100));   // smaller than a frame
  void* extra = nullptr;
  CHECK(frames.add_region(base, bytes - kMiB, 4000, &extra));
  CHECK(extra != nullptr && (addr(extra) & 63) == 0);
  CHECK(frames.area_count() == 1 && frames.free_bytes_low() == 0);
  const uint64_t initial = frames.free_bytes();
  CHECK(initial > bytes - 2 * kMiB && initial < bytes);

  struct Block { uint64_t at, frames; };
  std::vector<Block> blocks;
  const uint64_t shapes[][2] = {{1, 1}, {3, 1}, {64, 64}, {65, 1}, {200, 8}, {512, 512}, {513, 1}, {1500, 1}, {2048, 512}, {7, 4}, {1, 1024}};
  for (const auto& shape : shapes) {
    const uint64_t at = frames.allocate(shape[0], shape[1], false);
    CHECK(at != 0);
    if (!at) continue;
    CHECK(at >= base && at + shape[0] * kFrame <= base + bytes);
    CHECK(at % (shape[1] * kFrame) == 0);
    for (const Block& b : blocks) CHECK(at + shape[0] * kFrame <= b.at || b.at + b.frames * kFrame <= at);
    for (uint64_t f = 0; f < shape[0]; ++f) memset(reinterpret_cast<void*>(at + f * kFrame), 0xA5, 64);
    blocks.push_back({at, shape[0]});
  }
  uint64_t used = 0;
  for (const Block& b : blocks) used += b.frames * kFrame;
  CHECK(frames.free_bytes() == initial - used);

  // A frame of a range freed on its own, then the rest; a second free of either fails and changes nothing.
  const Block b = blocks[3];   // 65 frames
  CHECK(frames.free(b.at + 10 * kFrame, 1));
  CHECK(!frames.free(b.at + 10 * kFrame, 1));
  CHECK(!frames.free(b.at, b.frames));   // (frame 10 of it is free already)
  CHECK(frames.free(b.at, 10) && frames.free(b.at + 11 * kFrame, b.frames - 11));
  blocks.erase(blocks.begin() + 3);
  CHECK(!frames.free(base - kFrame, 1) && !frames.free(b.at + 1, 1));   // outside every area, not a frame start
  CHECK(frames.allocate(0, 1, false) == 0 && frames.allocate(1, 3, false) == 0);   // nothing, alignment not a power of two
  CHECK(frames.allocate(1, 1, true) == 0);   // no low area here
  for (const Block& x : blocks) CHECK(frames.free(x.at, x.frames));
  CHECK(frames.free_bytes() == initial);

  // Everything at once, a frame at a time: the whole area is handed out exactly once.
  std::vector<uint64_t> all;
  for (uint64_t at; (at = frames.allocate(1, 1, false));) all.push_back(at);
  CHECK(all.size() * kFrame == initial);
  CHECK(frames.free_bytes() == 0);
  std::vector<uint64_t> sorted = all;
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 1; i < sorted.size(); ++i) CHECK(sorted[i] != sorted[i - 1]);
  for (size_t i = 0; i < all.size(); i += 2) CHECK(frames.free(all[i], 1));
  CHECK(frames.allocate(2, 1, false) == 0);   // only single frames are left between used ones
  for (size_t i = 1; i < all.size(); i += 2) CHECK(frames.free(all[i], 1));
  CHECK(frames.free_bytes() == initial);
  // ... and then one run of nearly all of it.
  const uint64_t big = frames.allocate(initial / kFrame - 600, 1, false);
  CHECK(big != 0);
  if (big) CHECK(frames.free(big, initial / kFrame - 600));
  CHECK(frames.free_bytes() == initial);
  munmap(region, bytes + 2 * kMiB);
}

// Several threads with their own slots (or all on one), allocating and freeing frames with a tag written into each.
static void test_frames_threads(bool shared_slot) {
  const uint64_t bytes = 256 * kMiB;
  void* region = map_anywhere(bytes);
  CHECK(region != nullptr);
  if (!region) return;
  PageFrameAllocator frames;
  CHECK(frames.add_region(addr(region), bytes));
  if (!shared_slot) frames.set_core_source(thread_core);
  const uint64_t initial = frames.free_bytes();
  constexpr int kThreads = 8;
  // A hand-off box: threads put frames here for other threads to check and free.
  constexpr int kBox = 64;
  std::atomic<uint64_t> box[kBox];
  for (auto& b : box) b = 0;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      t_core = static_cast<uint32_t>(t);
      Rng rng(t + 1);
      struct Held { uint64_t at, frames; };
      std::vector<Held> held;
      auto stamp = [](uint64_t at, uint64_t n) { for (uint64_t f = 0; f < n; ++f) *reinterpret_cast<uint64_t*>(at + f * kFrame) = at ^ n ^ f; };
      auto verify = [](uint64_t at, uint64_t n) { for (uint64_t f = 0; f < n; ++f) if (*reinterpret_cast<uint64_t*>(at + f * kFrame) != (at ^ n ^ f)) return false; return true; };
      for (int step = 0; step < 20000; ++step) {
        const uint64_t r = rng.below(100);
        if (r < (held.size() < 200 ? 50 : 30) || held.empty()) {
          const uint64_t kind = rng.below(100);
          const uint64_t n = kind < 70 ? 1 + rng.below(4) : kind < 95 ? 1 + rng.below(64) : kind < 99 ? 65 + rng.below(448) : 513 + rng.below(1500);
          const uint64_t align = uint64_t{1} << rng.below(kind < 95 ? 4 : 10);
          const uint64_t at = frames.allocate(n, align, false);
          if (!at) continue;
          CHECK(at % (align * kFrame) == 0);
          stamp(at, n);
          held.push_back({at, n});
        } else if (r < 60) {
          // Hand one over (box entry: address | frames in the low 12 bits... frames up to 4095 fit since at is frame-aligned).
          const size_t i = rng.below(held.size());
          const uint64_t entry = held[i].at | held[i].frames;
          uint64_t expected = 0;
          if (held[i].frames < kFrame && box[rng.below(kBox)].compare_exchange_strong(expected, entry)) {
            held[i] = held.back();
            held.pop_back();
          }
        } else if (r < 70) {
          const uint64_t entry = box[rng.below(kBox)].exchange(0);
          if (entry) {
            const uint64_t at = entry & ~(kFrame - 1), n = entry & (kFrame - 1);
            CHECK(verify(at, n));
            CHECK(frames.free(at, n));
          }
        } else {
          const size_t i = rng.below(held.size());
          CHECK(verify(held[i].at, held[i].frames));
          CHECK(frames.free(held[i].at, held[i].frames));   // (no second free here: another thread may own the frames by now)
          held[i] = held.back();
          held.pop_back();
        }
      }
      for (const Held& h : held) {
        CHECK(verify(h.at, h.frames));
        CHECK(frames.free(h.at, h.frames));
      }
    });
  }
  for (std::thread& t : threads) t.join();
  for (auto& b : box) {
    const uint64_t entry = b.exchange(0);
    if (entry) CHECK(frames.free(entry & ~(kFrame - 1), entry & (kFrame - 1)));
  }
  CHECK(frames.free_bytes() == initial);
  // All the reservations can be taken back: one run of almost everything fits again.
  const uint64_t big = frames.allocate(initial / kFrame - 1024, 1, false);
  CHECK(big != 0);
  if (big) frames.free(big, initial / kFrame - 1024);
  munmap(region, bytes);
}

// The heap: zones, sizes, alignment, tags, double frees and reclaim, on one thread.
static void test_heap_basic() {
  void* low = map_at(0x30000000ull, 16 * kMiB);
  void* high = map_anywhere(32 * kMiB);
  CHECK(low && high);
  if (!low || !high) return;
  FakeRam ram;
  ram.region = low;
  ram.bytes = 16 * kMiB;
  Allocator a(ram);
  CHECK(a.initialize() && a.add_region(high, 32 * kMiB));
  const uint64_t low_free = a.free_bytes_low(), high_free = a.free_bytes_high();
  for (uint64_t size = 1; size <= 20000; size += size < 300 ? 1 : 97) {
    void* p = a.allocate(size);
    CHECK(p != nullptr && addr(p) >= addr(high));
    if (!p) continue;
    const uint64_t natural = size & -size;
    CHECK(addr(p) % (natural < 64 ? (natural < 16 ? 16 : natural) : 64) == 0 || size <= 16);
    memset(p, 0x3C, size);
    a.deallocate(p);
  }
  for (uint64_t alignment = 16; alignment <= 64 * 1024; alignment *= 2) {
    for (uint64_t size : {1ull, 100ull, 4096ull, 5000ull, 70000ull}) {
      void* p = a.allocate_aligned(size, alignment);
      void* d = a.allocate_dma(size, alignment);
      CHECK(p && addr(p) % alignment == 0 && addr(p) >= addr(high));
      CHECK(d && addr(d) % alignment == 0 && addr(d) + size <= 0x30000000ull + 16 * kMiB && addr(d) >= 0x30000000ull);
      if (p) memset(p, 1, size);
      if (d) memset(d, 2, size);
      a.deallocate(p);
      a.deallocate(d);
    }
  }
  CHECK(!a.allocate(0) && !a.allocate_aligned(10, 0) && !a.allocate_aligned(10, 48) && !a.allocate_dma(10, 3));
  a.trim();
  CHECK(a.free_bytes_low() == low_free && a.free_bytes_high() == high_free);

  // Frees that are not ours, or not the start of an allocation, or second ones, are ignored.
  int on_stack = 0;
  a.deallocate(&on_stack);
  void* big = a.allocate(100 * 1024);
  CHECK(big && addr(big) % kFrame == 0);
  a.deallocate(static_cast<uint8_t*>(big) + 64);
  a.deallocate(big);
  a.deallocate(big);
  void* small = a.allocate(200);
  a.deallocate(static_cast<uint8_t*>(small) + 8);
  a.deallocate(small);
  a.deallocate(small);
  void* s1 = a.allocate(200);
  void* s2 = a.allocate(200);
  CHECK(s1 && s2 && s1 != s2);
  a.deallocate(s1);
  a.deallocate(s2);
  a.trim();
  CHECK(a.free_bytes() == low_free + high_free);
  munmap(low, 16 * kMiB);
  munmap(high, 32 * kMiB);
}

// The heap from several threads: each its own core (or all one), objects passed between threads and freed by the receiver.
static void test_heap_threads(bool shared_slot) {
  void* low = map_at(0x31000000ull, 64 * kMiB);
  void* high = map_anywhere(512 * kMiB);
  CHECK(low && high);
  if (!low || !high) return;
  FakeRam ram;
  ram.region = low;
  ram.bytes = 64 * kMiB;
  Allocator a(ram);
  CHECK(a.initialize() && a.add_region(high, 512 * kMiB));
  if (!shared_slot) a.set_core_source(thread_core);
  const uint64_t initial = a.free_bytes();
  constexpr int kThreads = 8, kBox = 256;
  struct Item { std::atomic<void*> p{nullptr}; std::atomic<uint64_t> size{0}; };
  static int filling;
  void* const kFilling = &filling;   // (a slot being filled)
  std::vector<Item> box(kBox);
  auto fill = [](void* p, uint64_t size) {
    uint8_t* b = static_cast<uint8_t*>(p);
    const uint8_t tag = static_cast<uint8_t>(addr(p) >> 4 ^ size);
    for (uint64_t i = 0; i < size; i += 13) b[i] = tag;
    b[size - 1] = tag;
  };
  auto check = [](void* p, uint64_t size) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    const uint8_t tag = static_cast<uint8_t>(addr(p) >> 4 ^ size);
    for (uint64_t i = 0; i < size; i += 13) if (b[i] != tag) return false;
    return b[size - 1] == tag;
  };
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      t_core = static_cast<uint32_t>(t);
      Rng rng(100 + t);
      struct Held { void* p; uint64_t size; };
      std::vector<Held> held;
      for (int step = 0; step < 60000; ++step) {
        const uint64_t r = rng.below(100);
        if (r < (held.size() < 400 ? 48 : 30) || held.empty()) {
          const uint64_t k = rng.below(1000);
          const uint64_t size = k < 800 ? 1 + rng.below(256) : k < 980 ? 1 + rng.below(16384) : k < 998 ? 16385 + rng.below(200000) : 1 + rng.below(3 * kMiB);
          const uint64_t which = rng.below(10);
          const uint64_t alignment = uint64_t{16} << rng.below(9);
          void* p = which < 7 ? a.allocate(size) : which < 9 ? a.allocate_aligned(size, alignment) : a.allocate_dma(size, alignment);
          CHECK(p != nullptr);
          if (!p) continue;
          if (which >= 7) CHECK(addr(p) % alignment == 0);
          if (which == 9) CHECK(addr(p) >= addr(low) && addr(p) + size <= addr(low) + 64 * kMiB);
          fill(p, size);
          held.push_back({p, size});
        } else if (r < 62) {
          const size_t i = rng.below(held.size());
          Item& slot = box[rng.below(kBox)];
          void* expected = nullptr;
          if (slot.p.compare_exchange_strong(expected, kFilling)) {   // (claim the slot, then fill it in)
            slot.size.store(held[i].size);
            slot.p.store(held[i].p);
            held[i] = held.back();
            held.pop_back();
          }
        } else if (r < 76) {
          Item& slot = box[rng.below(kBox)];
          void* p = slot.p.load();
          if (p && p != kFilling) {
            const uint64_t size = slot.size.load();
            if (slot.p.compare_exchange_strong(p, nullptr)) {
              CHECK(check(p, size));
              a.deallocate(p);
            }
          }
        } else {
          const size_t i = rng.below(held.size());
          CHECK(check(held[i].p, held[i].size));
          a.deallocate(held[i].p);
          held[i] = held.back();
          held.pop_back();
        }
        if (step % 20000 == 19999 && t == 0) a.trim();   // (a reclaim while the others keep going)
      }
      for (const Held& h : held) {
        CHECK(check(h.p, h.size));
        a.deallocate(h.p);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  for (Item& slot : box)
    if (void* p = slot.p.exchange(nullptr)) a.deallocate(p);
  a.trim();
  CHECK(a.free_bytes() == initial);
  munmap(low, 64 * kMiB);
  munmap(high, 512 * kMiB);
}

// Throughput: alloc/free pairs of small objects on 1..N threads, this heap against the same heap behind one std::mutex (what the
// old spin-locked front did).
static void bench() {
  void* high = map_anywhere(2048 * kMiB);
  FakeRam ram;
  ram.region = high;
  ram.bytes = 2048 * kMiB;
  Allocator a(ram);
  if (!high || !a.initialize()) return;
  a.set_core_source(thread_core);
  std::mutex lock;
  const unsigned max_threads = std::thread::hardware_concurrency() < 16 ? std::thread::hardware_concurrency() : 16;
  for (int locked = 0; locked < 2; ++locked) {
    for (unsigned n = 1; n <= max_threads; n *= 2) {
      constexpr int kOps = 2000000;
      auto start = std::chrono::steady_clock::now();
      std::vector<std::thread> threads;
      for (unsigned t = 0; t < n; ++t) {
        threads.emplace_back([&, t] {
          t_core = t;
          void* keep[64] = {};
          Rng rng(t + 7);
          for (int i = 0; i < kOps; ++i) {
            const int k = static_cast<int>(rng.below(64));
            const uint64_t size = 16 + rng.below(240);
            if (locked) {
              std::lock_guard<std::mutex> g(lock);
              a.deallocate(keep[k]);
              keep[k] = a.allocate(size);
            } else {
              a.deallocate(keep[k]);
              keep[k] = a.allocate(size);
            }
          }
          for (void* p : keep) a.deallocate(p);
        });
      }
      for (std::thread& t : threads) t.join();
      const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      printf("%-12s %2u thread(s): %6.1f M alloc+free/s\n", locked ? "mutex-locked" : "lock-free", n, n * kOps / s / 1e6);
    }
  }
  munmap(high, 2048 * kMiB);
}

int main(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "bench")) {
    bench();
    return 0;
  }
  test_frames_basic();
  test_frames_threads(false);
  test_frames_threads(true);
  test_heap_basic();
  test_heap_threads(false);
  test_heap_threads(true);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed.load());
    return 1;
  }
  printf("page frame allocator tests passed\n");
  return 0;
}
