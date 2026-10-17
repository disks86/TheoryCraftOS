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

// Host-side tests for src/Allocators/Allocator: the low (below 4 GiB) and high pools, DMA allocations, regions that straddle 4 GiB, and a
// randomized check that nothing overlaps and everything coalesces back. The test maps real memory at the addresses it needs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <vector>
#include "Allocators/Allocator.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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

// Memory at a chosen address (the kernel runs with RAM identity-mapped, so the addresses are what the allocator sees).
static void* map_at(uint64_t address, uint64_t bytes) {
  void* p = mmap(reinterpret_cast<void*>(address), bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

constexpr uint64_t kLow = 0x30000000ull, kHigh = 0x300000000ull, kMiB = 1 << 20;
constexpr uint64_t kFourGiB = 1ull << 32;
static bool is_low(const void* p) { return reinterpret_cast<uint64_t>(p) < kFourGiB; }

int main() {
  void* low = map_at(kLow, 8 * kMiB);
  void* high = map_at(kHigh, 8 * kMiB);
  void* straddle = map_at(kFourGiB - kMiB, 2 * kMiB);   // 1 MiB below 4 GiB and 1 MiB above
  if (!low || !high || !straddle) {
    printf("cannot map memory at the needed addresses here; skipped\n");
    return 0;
  }
  FakeRam ram;
  ram.region = low;
  ram.bytes = 8 * kMiB;
  allocators::Allocator a(ram);
  CHECK(a.initialize());
  const uint64_t low_free = a.free_bytes_low();
  CHECK(low_free > 7 * kMiB && a.free_bytes_high() == 0 && a.free_bytes() == low_free);

  // With only a low pool everything comes from it.
  void* p = a.allocate(1000);
  CHECK(p && is_low(p) && !a.allocate(0));
  a.deallocate(p);
  CHECK(a.free_bytes() == low_free);

  // A high region: ordinary allocations go there first, DMA allocations stay low.
  CHECK(a.add_region(high, 8 * kMiB));
  const uint64_t high_free = a.free_bytes_high();
  CHECK(high_free > 7 * kMiB && a.free_bytes_low() == low_free);
  void* q = a.allocate(4096);
  void* r = a.allocate_aligned(100, 4096);
  void* d = a.allocate_dma(4096, 4096);
  void* d2 = a.allocate_dma(10, 16);
  CHECK(q && !is_low(q) && r && !is_low(r) && (reinterpret_cast<uint64_t>(r) & 4095) == 0);
  CHECK(d && is_low(d) && (reinterpret_cast<uint64_t>(d) & 4095) == 0 && d2 && is_low(d2));
  memset(q, 1, 4096);
  memset(d, 2, 4096);
  a.deallocate(q);
  a.deallocate(r);
  a.deallocate(d);
  a.deallocate(d2);
  CHECK(a.free_bytes_low() == low_free && a.free_bytes_high() == high_free);

  // When the high pool cannot satisfy a request the low pool does; DMA never uses the high one.
  void* big = a.allocate(7 * kMiB + 512 * 1024);
  CHECK(big && !is_low(big));
  void* spill = a.allocate(2 * kMiB);
  CHECK(spill && is_low(spill));
  a.deallocate(spill);
  void* dma_big = a.allocate_dma(7 * kMiB, 4096);
  CHECK(dma_big && is_low(dma_big));
  CHECK(!a.allocate_dma(2 * kMiB, 4096));            // low is nearly used up; the free high memory does not count
  CHECK(a.allocate(100 * 1024));                     // an ordinary allocation could still use the rest of the high pool or the low one
  a.deallocate(big);
  a.deallocate(dma_big);

  // A region crossing 4 GiB is split into a low and a high run.
  {
    FakeRam ram2;
    ram2.region = straddle;
    ram2.bytes = 2 * kMiB;
    allocators::Allocator b(ram2);
    CHECK(b.initialize());
    CHECK(b.free_bytes_low() > 900 * 1024 && b.free_bytes_high() > 900 * 1024);
    void* hi = b.allocate(512 * 1024);
    void* lo = b.allocate_dma(512 * 1024, 64);
    CHECK(hi && !is_low(hi) && lo && is_low(lo) && reinterpret_cast<uint64_t>(lo) + 512 * 1024 <= kFourGiB);
    b.deallocate(hi);
    b.deallocate(lo);
    // a second region added to the other one
    CHECK(!b.add_region(nullptr, 1000) && !b.add_region(straddle, 64));
  }

  // Random use of all three calls: nothing overlaps, contents survive, everything coalesces back.
  {
    FakeRam ram3;
    void* l3 = map_at(kLow + 16 * kMiB, 4 * kMiB);
    void* h3 = map_at(kHigh + 16 * kMiB, 4 * kMiB);
    CHECK(l3 && h3);
    ram3.region = l3;
    ram3.bytes = 4 * kMiB;
    allocators::Allocator c(ram3);
    CHECK(c.initialize() && c.add_region(h3, 4 * kMiB));
    const uint64_t before = c.free_bytes();
    struct Block { uint8_t* p; uint64_t size; uint8_t tag; bool dma; };
    std::vector<Block> blocks;
    uint32_t seed = 4242;
    auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
    for (int step = 0; step < 20000; ++step) {
      if (blocks.size() > 40 || (!blocks.empty() && rnd() % 100 < 45)) {
        const size_t i = rnd() % blocks.size();
        Block b = blocks[i];
        for (uint64_t k = 0; k < b.size; k += 61) CHECK(b.p[k] == b.tag);
        c.deallocate(b.p);
        blocks[i] = blocks.back();
        blocks.pop_back();
      } else {
        const uint64_t size = 1 + rnd() % 30000;
        const uint32_t kind = rnd() % 3;
        const uint64_t alignment = uint64_t{16} << (rnd() % 9);
        Block b{nullptr, size, static_cast<uint8_t>(rnd()), kind == 2};
        b.p = static_cast<uint8_t*>(kind == 0 ? c.allocate(size) : kind == 1 ? c.allocate_aligned(size, alignment) : c.allocate_dma(size, alignment));
        if (!b.p) continue;
        if (kind == 1) CHECK((reinterpret_cast<uint64_t>(b.p) & (alignment - 1)) == 0);
        if (kind == 2) CHECK(is_low(b.p) && reinterpret_cast<uint64_t>(b.p) + size <= kFourGiB && (reinterpret_cast<uint64_t>(b.p) & (alignment - 1)) == 0);
        memset(b.p, b.tag, size);
        blocks.push_back(b);
      }
    }
    for (const Block& b : blocks) {
      for (uint64_t k = 0; k < b.size; k += 61) CHECK(b.p[k] == b.tag);
      c.deallocate(b.p);
    }
    CHECK(c.free_bytes() == before);
    // with everything freed one allocation can take a whole pool again
    void* whole = c.allocate(3 * kMiB + 512 * 1024);
    CHECK(whole);
    c.deallocate(whole);
  }
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("allocator tests passed\n");
  return 0;
}
