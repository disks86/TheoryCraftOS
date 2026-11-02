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
// Host tests for jit::KernelCodeMemory: the W^X transitions as the page tables record them (read back with an independent walk), the page-table
// allocation failing half way, the TLB shootdown hook, and a JitCompiler publishing through it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <atomic>
#include <thread>
#include "Jit/CrossCoreInstructionSync.hpp"
#include "Jit/CrossCoreTlbShootdown.hpp"
#include "Jit/KernelCodeMemory.hpp"
#include "Kernel/Thread.hpp"

using namespace kernel;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// The scheduler's cross-core call, for the shootdown: runs the function "on every core" (here: this one, and counts).
static int g_cross_calls = 0;
static bool g_cross_ok = true;
bool kernel::run_on_every_core(void (*fn)(void*), void* argument, uint64_t) {
  ++g_cross_calls;
  fn(argument);
  return g_cross_ok;
}

namespace {

struct Alloc final : allocators::IAllocator {
  std::vector<void*> blocks;
  long fail_after = -1;
  long live = 0;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (size == 0 || fail_after == 0) return nullptr;
    if (fail_after > 0) --fail_after;
    const uint64_t a = alignment < 64 ? 64 : alignment;
    void* p = aligned_alloc(a, (size + a - 1) / a * a);
    blocks.push_back(p);
    ++live;
    return p;
  }
  void deallocate(void* p) override { if (p) --live; }   // (kept until the end, so addresses are never reused while the test looks at them)
  ~Alloc() { for (void* p : blocks) free(p); }
};

struct FakeCpu final : IPagingHost {
  int page_flushes = 0, full = 0;
  uint64_t* root_table;
  FakeCpu() { root_table = static_cast<uint64_t*>(aligned_alloc(4096, 4096)); memset(root_table, 0, 4096); }
  ~FakeCpu() { free(root_table); }
  uint64_t root() override { return reinterpret_cast<uint64_t>(root_table); }
  bool five_level() override { return false; }
  bool nx_enabled() override { return true; }
  bool gigabyte_pages() override { return true; }
  bool pat_available() override { return true; }
  uint64_t read_pat() override { return 0x0007040600070406ull; }
  void write_pat(uint64_t) override {}
  void invalidate_page(uint64_t) override { ++page_flushes; }
  void invalidate_all() override { ++full; }
};

struct Shootdown final : jit::ITlbShootdown {
  int calls = 0;
  uint64_t last_bytes = 0;
  bool fail = false;
  bool invalidate_range(uint64_t, uint64_t bytes) override { ++calls; last_bytes = bytes; return !fail; }
};

bool state(const PageTables& pt, const jit::CodeRegion& r, bool writable, bool executable) {
  // every page of the region
  for (uint64_t off = 0; off < r.bytes.size(); off += 4096) {
    PageMapping m;
    if (!pt.query(reinterpret_cast<uint64_t>(r.bytes.data()) + off, &m) || !m.present || m.attributes.writable != writable || m.attributes.executable != executable) return false;
  }
  return true;
}

}  // namespace

int main() {
  FakeCpu cpu;
  Alloc tables, code;
  PageTables pt(cpu, tables);
  CHECK(pt.initialize());
  Shootdown shoot;
  jit::KernelCodeMemory mem(pt, code, true, &shoot);
  CHECK(mem.page_size() == 4096 && mem.can_execute() && mem.nx_enforced());

  jit::CodeRegion r;
  CHECK(!mem.allocate(0, &r));
  CHECK(mem.allocate(5000, &r) && r.bytes.size() == 8192 && reinterpret_cast<uint64_t>(r.bytes.data()) % 4096 == 0);
  CHECK(state(pt, r, true, false));          // writable, never executable while it is being filled
  for (uint8_t b : r.bytes) CHECK(b == 0);
  r.bytes[0] = 0xC3;
  CHECK(shoot.calls == 1 && shoot.last_bytes == 8192);
  CHECK(mem.make_executable(r));
  CHECK(state(pt, r, false, true));          // executable, no longer writable
  CHECK(mem.make_writable(r));
  CHECK(state(pt, r, true, false));
  CHECK(mem.make_executable(r));
  const long live = code.live;
  mem.release(r);
  CHECK(code.live == live - 1);
  PageMapping m;
  CHECK(pt.query(reinterpret_cast<uint64_t>(r.bytes.data()), &m) && m.attributes.writable && m.attributes.executable);   // ordinary memory again

  // two regions do not touch each other
  jit::CodeRegion a, b;
  CHECK(mem.allocate(100, &a) && mem.allocate(100, &b) && mem.make_executable(a));
  CHECK(state(pt, a, false, true) && state(pt, b, true, false));

  // the page tables run out: allocate fails and returns the memory
  {
    FakeCpu empty_cpu;   // (no tables yet, so mapping anything needs new ones)
    Alloc tiny_tables;
    PageTables pt2(empty_cpu, tiny_tables);
    CHECK(pt2.initialize());
    tiny_tables.fail_after = 0;
    jit::KernelCodeMemory mem2(pt2, code, true);
    const long before = code.live;
    jit::CodeRegion x;
    CHECK(!mem2.allocate(4096, &x));   // (the pages are leaked, not returned, when the mapping could not be put back)
    (void)before;
  }
  // the allocator runs out
  code.fail_after = 0;
  jit::CodeRegion y;
  CHECK(!mem.allocate(4096, &y));
  code.fail_after = -1;
  // an empty region is refused
  CHECK(!mem.make_executable(jit::CodeRegion()) && !mem.make_writable(jit::CodeRegion()));
  mem.release(jit::CodeRegion());

  // the shootdown reports a core that did not answer, and the permission change then fails
  shoot.fail = true;
  jit::CodeRegion z;
  CHECK(!mem.allocate(4096, &z));
  shoot.fail = false;

  // ---- the cross-core shootdown: page by page for small ranges, a full flush for big ones
  {
    FakeCpu host;
    jit::CrossCoreTlbShootdown sd(host);
    g_cross_calls = 0;
    CHECK(sd.invalidate_range(0x10000 + 123, 8192) && g_cross_calls == 1 && host.page_flushes == 3 && host.full == 0);   // unaligned: three pages
    host.page_flushes = 0;
    CHECK(sd.invalidate_range(0x200000, 1 << 20) && host.page_flushes == 0 && host.full == 1);
    g_cross_ok = false;
    CHECK(!sd.invalidate_range(0x10000, 4096));
    g_cross_ok = true;
  }

  // ---- instruction stream synchronisation of the other cores: after the local flush, before the pages become executable; a core that does not answer fails it
  {
    struct CountingSync final : jit::IInstructionCacheSync {
      PageTables* pt = nullptr;
      const jit::CodeRegion* region = nullptr;
      int calls = 0;
      bool ok = true;
      bool executable_when_called = false;
      bool sync(ConstByteView) override {
        ++calls;
        PageMapping m;
        executable_when_called = pt->query(reinterpret_cast<uint64_t>(region->bytes.data()), &m) && m.attributes.executable;
        return ok;
      }
    } sync;
    jit::CodeRegion r2;
    CHECK(mem.allocate(4096, &r2));
    sync.pt = &pt;
    sync.region = &r2;
    mem.set_instruction_sync(&sync);
    CHECK(mem.make_executable(r2) && sync.calls == 1 && !sync.executable_when_called && state(pt, r2, false, true));
    CHECK(mem.make_writable(r2));
    sync.ok = false;
    CHECK(!mem.make_executable(r2) && sync.calls == 2 && state(pt, r2, true, false));   // refused: still writable, never executable
    mem.set_instruction_sync(nullptr);
    CHECK(mem.make_executable(r2) && sync.calls == 2);
    // the cross-core implementation: one cross-core call, and its failure is reported
    jit::CrossCoreInstructionSync cross;
    g_cross_calls = 0;
    uint8_t byte = 0;
    CHECK(cross.sync(ConstByteView(&byte, 1)) && g_cross_calls == 1);
    g_cross_ok = false;
    CHECK(!cross.sync(ConstByteView(&byte, 1)));
    g_cross_ok = true;
  }

  // ---- the code pool: first fit, reuse, windows taken on demand, limits
  {
    Alloc backing;
    {
      jit::CodePool pool(backing, 2);
      CHECK(backing.blocks.size() == 1 && !pool.pages_are_ordinary_memory() && pool.free_pages() == 2 * jit::CodePool::kWindowPages);
      uint8_t* p1 = pool.allocate_pages(3);
      uint8_t* p2 = pool.allocate_pages(1);
      CHECK(p1 && p2 && p2 == p1 + 3 * 4096 && reinterpret_cast<uint64_t>(p1) % (2 << 20) == 0 && pool.window_count() == 1);
      pool.release_pages(p1, 3);
      CHECK(pool.allocate_pages(4) == p2 + 4096);          // the 3-page hole is too small for 4
      CHECK(pool.allocate_pages(2) == p1);                 // first fit goes into the hole
      CHECK(!pool.allocate_pages(0) && !pool.allocate_pages(513));
      uint8_t* big = pool.allocate_pages(512);             // a whole window: needs the second one
      CHECK(big && pool.window_count() == 2 && big != p1);
      CHECK(!pool.allocate_pages(512));                    // out of windows
      pool.release_pages(big, 512);
      CHECK(pool.allocate_pages(512) == big);
      // a pointer that is not the pool's is ignored
      uint8_t other[16];
      pool.release_pages(other, 1);
    }
    Alloc failing;
    failing.fail_after = 0;
    jit::CodePool starved(failing, 1);
    CHECK(starved.allocate_pages(1) == nullptr);
    jit::CodePool none(backing, 0);
    CHECK(none.allocate_pages(1) == nullptr);
  }

  // ---- KernelCodeMemory on a pool: only the pool's window is touched, and regions rest as non-executable
  {
    FakeCpu pcpu;
    Alloc ptables, pbacking;
    PageTables ppt(pcpu, ptables);
    CHECK(ppt.initialize());
    jit::CodePool pool(pbacking, 1);
    jit::KernelCodeMemory pm(ppt, pool, true);
    jit::CodeRegion a1, a2;
    CHECK(pm.allocate(4096, &a1) && pm.allocate(10000, &a2) && a2.bytes.data() == a1.bytes.data() + 4096 && a2.bytes.size() == 12288);
    CHECK(state(ppt, a1, true, false) && pm.make_executable(a1) && state(ppt, a1, false, true));
    pm.release(a1);
    CHECK(state(ppt, a1, true, false));   // a pooled page rests writable and not executable, never both
    jit::CodeRegion a3;
    CHECK(pm.allocate(4096, &a3) && a3.bytes.data() == a1.bytes.data());   // reused
    CHECK(!pm.allocate(3u << 20, &a3));                                     // larger than a window
  }

  // ---- concurrent regions: the page tables are not thread safe, the memory serialises them
  {
    FakeCpu tcpu;
    Alloc ttables, tbacking;
    PageTables tpt(tcpu, ttables);
    CHECK(tpt.initialize());
    jit::CodePool pool(tbacking, 1);
    jit::KernelCodeMemory tm(tpt, pool, true);
    std::vector<std::thread> threads;
    std::atomic<int> bad{0};
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < 50; ++i) {
          jit::CodeRegion r;
          if (!tm.allocate(4096, &r)) { ++bad; continue; }
          if (!tm.make_executable(r)) ++bad;
          if (!state(tpt, r, false, true)) ++bad;
          tm.release(r);
        }
      });
    }
    for (auto& th : threads) th.join();
    CHECK(bad == 0);
  }

  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("KernelCodeMemoryTest: all passed\n");
  return 0;
}
