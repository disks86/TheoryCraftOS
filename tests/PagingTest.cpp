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

// Host-side tests for src/Kernel/Paging: the page-table walker against a pretend CPU. The "physical" addresses are the addresses of real tables in this
// process, so the code under test follows real pointers; the checks read the tables back with an independent walk.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include "Kernel/Paging.hpp"
#include "Kernel/Rcu.hpp"

using namespace kernel;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::mutex lock;   // (the threaded tests allocate from several threads)
  std::unordered_set<void*> blocks;   // handed out and not given back
  long fail_after = -1;   // allocations that still succeed (-1: all)
  std::atomic<int> returned{0};   // blocks given back (freed at once, so the address sanitizer sees any later use)
  std::atomic<int> foreign{0};    // deallocate() of something this allocator did not hand out (a firmware table, say)
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    std::lock_guard<std::mutex> guard(lock);
    if (fail_after == 0) return nullptr;
    if (fail_after > 0) --fail_after;
    void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
    blocks.insert(p);
    return p;
  }
  void deallocate(void* p) override {
    std::lock_guard<std::mutex> guard(lock);
    if (!blocks.erase(p)) {
      ++foreign;
      return;
    }
    free(p);
    ++returned;
  }
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

struct FakeShootdown final : ITlbShootdown {
  std::atomic<int> calls{0}, everything{0};
  bool fail = false;
  bool invalidate_range(uint64_t, uint64_t bytes) override {
    ++calls;
    if (bytes >= (1ull << 47)) ++everything;
    return !fail;
  }
};

struct FakeCpu final : IPagingHost {
  uint64_t* root_table;
  bool five = false, nx = true, gb = true, pat = true;
  uint64_t pat_msr = 0x0007040600070406ull;
  std::atomic<int> full_flushes{0}, writes{0};
  std::atomic<int> edits_begun{0}, edits_ended{0};
  bool may_wait = true;
  uint64_t begin_table_edit() override { ++edits_begun; return 0x1234; }
  bool can_wait() override { return may_wait; }
  void end_table_edit(uint64_t token) override { if (token == 0x1234) ++edits_ended; }
  FakeCpu() { root_table = static_cast<uint64_t*>(aligned_alloc(4096, 4096)); memset(root_table, 0, 4096); }
  std::vector<void*> owned;   // tables the "firmware" made
  ~FakeCpu() { free(root_table); for (void* p : owned) free(p); }
  uint64_t root() override { return reinterpret_cast<uint64_t>(root_table); }
  bool five_level() override { return five; }
  bool nx_enabled() override { return nx; }
  bool gigabyte_pages() override { return gb; }
  bool pat_available() override { return pat; }
  uint64_t read_pat() override { return pat_msr; }
  void write_pat(uint64_t v) override { pat_msr = v; ++writes; }
  void invalidate_page(uint64_t) override {}
  void invalidate_all() override { ++full_flushes; }
};

constexpr uint64_t kMiB = 1 << 20, kGiB = 1ull << 30;
constexpr uint64_t kP = 1, kRW = 2, kPS = 0x80;

// An independent reading of the tables: the leaf entry that translates `address`, and how big its page is.
struct Leaf { bool present; uint64_t entry; uint64_t size; };
static Leaf walk(uint64_t* root, uint64_t address) {
  uint64_t* t = root;
  for (int level = 4; level >= 1; --level) {
    const uint64_t e = t[(address >> (12 + 9 * (level - 1))) & 511];
    if (!(e & kP)) return {false, 0, 0};
    if (level == 1 || (level <= 3 && (e & kPS))) return {true, e, 1ull << (12 + 9 * (level - 1))};
    t = reinterpret_cast<uint64_t*>(e & 0x000FFFFFFFFFF000ull);
  }
  return {false, 0, 0};
}

// Tables made by PageTables (the software bit 9 on the entry that points to them) that can be reached from `table`.
static int owned_tables(const uint64_t* table, int level) {
  int n = 0;
  for (int i = 0; i < 512; ++i) {
    const uint64_t e = table[i];
    if (!(e & kP) || level == 1 || (level <= 3 && (e & kPS))) continue;
    n += ((e >> 9) & 1) + owned_tables(reinterpret_cast<const uint64_t*>(e & 0x000FFFFFFFFFF000ull), level - 1);
  }
  return n;
}

// What the firmware leaves behind: the first 4 GiB mapped 1:1 with 1 GiB write-back pages, the way OVMF does it.
static void firmware_map(FakeCpu& cpu, int gigabytes) {
  uint64_t* pdpt = static_cast<uint64_t*>(aligned_alloc(4096, 4096));
  cpu.owned.push_back(pdpt);
  memset(pdpt, 0, 4096);
  for (int i = 0; i < gigabytes; ++i) pdpt[i] = (static_cast<uint64_t>(i) << 30) | kP | kRW | kPS;
  cpu.root_table[0] = reinterpret_cast<uint64_t>(pdpt) | kP | kRW;
}

int main() {
  // ---- a firmware-style identity map, then device registers in the middle of a 1 GiB page
  {
    FakeCpu cpu;
    TestAllocator alloc;
    firmware_map(cpu, 4);
    PageTables pt(cpu, alloc);
    CHECK(pt.initialize());
    PageMapping m;
    CHECK(pt.query(0xC0001000, &m) && m.present && m.page_size == kGiB && m.physical == 0xC0000000 && m.attributes.cache == CacheMode::WriteBack && m.attributes.writable);
    CHECK(pt.query(0x100000000ull, &m) && !m.present);
    CHECK(pt.is_mapped(0x1000, 3 * kGiB) && !pt.is_mapped(3 * kGiB, 2 * kGiB));

    PageAttributes uc;
    uc.cache = CacheMode::Uncached;
    uc.executable = false;
    CHECK(pt.map(0xFEE00000, 4096, uc));
    CHECK(cpu.edits_begun >= 1 && cpu.edits_begun == cpu.edits_ended);   // every change to the tables is bracketed (the firmware's tables may be read-only)
    CHECK(pt.query(0xFEE00000, &m) && m.present && m.page_size == 4096 && m.physical == 0xFEE00000 && m.attributes.cache == CacheMode::Uncached && !m.attributes.executable && m.attributes.writable);
    CHECK(pt.query(0xFEDFF000, &m) && m.page_size == 2 * kMiB && m.attributes.cache == CacheMode::WriteBack && m.attributes.executable);   // neighbours keep their mapping...
    CHECK(pt.query(0xFEE01000, &m) && m.physical == 0xFEE01000 && m.attributes.cache == CacheMode::WriteBack);
    CHECK(pt.query(0xC0000000, &m) && m.page_size == 2 * kMiB && m.physical == 0xC0000000);   // ...and the rest of the split gigabyte is 2 MiB pages
    CHECK(pt.query(0x80000000, &m) && m.page_size == kGiB);   // other gigabytes are untouched
    CHECK(cpu.full_flushes >= 1);
    // everything in the split gigabyte still translates to itself
    for (uint64_t a = 0xC0000000; a < 0x100000000ull; a += 0x1F3000 + 0x123) {
      Leaf l = walk(cpu.root_table, a);
      CHECK(l.present && ((l.entry & 0x000FFFFFFFFFF000ull) & ~(l.size - 1)) == (a & ~(l.size - 1)));
    }
    // an aligned 4 MiB window uses 2 MiB pages
    CHECK(pt.map(0xE0000000, 4 * kMiB, uc));
    CHECK(pt.query(0xE0100000, &m) && m.page_size == 2 * kMiB && m.attributes.cache == CacheMode::Uncached);
    // mapping again changes nothing about where things are
    CHECK(pt.map(0xFEE00000, 4096, uc) && pt.query(0xFEE00000, &m) && m.physical == 0xFEE00000);
  }

  // ---- memory above 4 GiB that the firmware did not map: a 64-bit BAR
  {
    FakeCpu cpu;
    TestAllocator alloc;
    firmware_map(cpu, 4);
    PageTables pt(cpu, alloc);
    CHECK(pt.initialize());
    PageAttributes uc;
    uc.cache = CacheMode::Uncached;
    const size_t tables_before = alloc.blocks.size();
    CHECK(pt.map(0x280000000ull, 256 * kMiB, uc));
    PageMapping m;
    CHECK(pt.query(0x280000000ull, &m) && m.present && m.page_size == 2 * kMiB && m.attributes.cache == CacheMode::Uncached);
    CHECK(pt.query(0x280000000ull + 256 * kMiB - 4096, &m) && m.present && !pt.query(0x280000000ull + 256 * kMiB, &m) == false && !m.present);
    CHECK(alloc.blocks.size() - tables_before == 1);   // one page directory for the whole window: 2 MiB pages, no page tables
    CHECK(pt.is_mapped(0x280000000ull, 256 * kMiB));
    // a window of whole gigabytes uses 1 GiB pages
    CHECK(pt.map(0x4000000000ull, 2 * kGiB, uc));
    CHECK(pt.query(0x4040000000ull, &m) && m.page_size == kGiB && m.physical == 0x4040000000ull);
    // without 1 GiB pages in the CPU it falls back to 2 MiB ones
    cpu.gb = false;
    PageTables pt2(cpu, alloc);
    CHECK(pt2.initialize() && pt2.map(0x8000000000ull, kGiB, uc) && pt2.query(0x8000000000ull, &m) && m.page_size == 2 * kMiB && pt2.is_mapped(0x8000000000ull, kGiB));
    // an odd size and offset reach out to whole pages
    CHECK(pt.map(0x300000800ull, 5000, uc) && pt.query(0x300000000ull, &m) && m.page_size == 4096 && pt.query(0x300001000ull, &m) && m.present && pt.query(0x300002000ull, &m) && !m.present);
  }

  // ---- map_missing leaves what is there alone
  {
    FakeCpu cpu;
    TestAllocator alloc;
    firmware_map(cpu, 4);
    PageTables pt(cpu, alloc);
    CHECK(pt.initialize());
    PageAttributes wb, uc;
    uc.cache = CacheMode::Uncached;
    CHECK(pt.map(0x200004000ull, 4 * 4096, wb));
    CHECK(pt.map_missing(0x200000000ull, 4 * kMiB, uc));
    PageMapping m;
    CHECK(pt.query(0x200004000ull, &m) && m.attributes.cache == CacheMode::WriteBack);   // kept
    CHECK(pt.query(0x200003000ull, &m) && m.attributes.cache == CacheMode::Uncached && m.page_size == 4096);
    CHECK(pt.query(0x200008000ull, &m) && m.attributes.cache == CacheMode::Uncached);
    CHECK(pt.query(0x200200000ull, &m) && m.page_size == 2 * kMiB && m.attributes.cache == CacheMode::Uncached);
    CHECK(pt.is_mapped(0x200000000ull, 4 * kMiB));
    // over firmware's 1 GiB page: nothing changes at all
    const int flushes = cpu.full_flushes;
    CHECK(pt.map_missing(0x40000000, 16 * kMiB, uc) && cpu.full_flushes == flushes);
    CHECK(pt.query(0x40000000, &m) && m.page_size == kGiB && m.attributes.cache == CacheMode::WriteBack);
  }

  // ---- unmap
  {
    FakeCpu cpu;
    TestAllocator alloc;
    firmware_map(cpu, 4);
    PageTables pt(cpu, alloc);
    CHECK(pt.initialize());
    PageMapping m;
    CHECK(pt.unmap(0x10000000, 4096));   // a hole in the first gigabyte
    CHECK(pt.query(0x10000000, &m) && !m.present);
    CHECK(pt.query(0x10001000, &m) && m.present && m.physical == 0x10001000 && pt.query(0x0FFFF000, &m) && m.present);
    CHECK(!pt.is_mapped(0x0FFFF000, 3 * 4096) && pt.is_mapped(0x10001000, 4096));
    CHECK(pt.unmap(2 * kGiB, kGiB) && pt.query(2 * kGiB, &m) && !m.present && pt.query(3 * kGiB, &m) && m.present);
    CHECK(pt.unmap(0x500000000ull, 4096));   // nothing there: fine
  }

  // ---- write-combining needs the PAT set up; the bits go where the PAT keeps them for 4 KiB and for large pages
  {
    FakeCpu cpu;
    TestAllocator alloc;
    firmware_map(cpu, 4);
    PageTables pt(cpu, alloc);
    CHECK(pt.initialize() && !pt.write_combining_enabled());
    PageAttributes wc;
    wc.cache = CacheMode::WriteCombining;
    CHECK(!pt.map(0xD0000000, 4096, wc));
    CHECK(pt.enable_write_combining() && pt.write_combining_enabled() && ((cpu.pat_msr >> 32) & 0xff) == 1 && (cpu.pat_msr & 0xffffffff) == 0x00070406);
    CHECK(pt.map(0xD0001000, 4096, wc));
    Leaf l = walk(cpu.root_table, 0xD0001000);
    CHECK(l.present && l.size == 4096 && (l.entry & (1 << 7)) && !(l.entry & 0x18));   // PAT bit 7, PCD and PWT clear
    CHECK(pt.map(0xD0200000, 2 * kMiB, wc));
    l = walk(cpu.root_table, 0xD0200000);
    CHECK(l.present && l.size == 2 * kMiB && (l.entry & (1 << 12)) && !(l.entry & 0x18));
    PageMapping m;
    CHECK(pt.query(0xD0001000, &m) && m.attributes.cache == CacheMode::WriteCombining && pt.query(0xD0200000, &m) && m.attributes.cache == CacheMode::WriteCombining);
    // splitting a write-combining 2 MiB page keeps it write-combining
    PageAttributes uc;
    uc.cache = CacheMode::Uncached;
    CHECK(pt.map(0xD0203000, 4096, uc));
    CHECK(pt.query(0xD0202000, &m) && m.page_size == 4096 && m.attributes.cache == CacheMode::WriteCombining && pt.query(0xD0203000, &m) && m.attributes.cache == CacheMode::Uncached);
    CHECK(pt.query(0xD0204000, &m) && m.attributes.cache == CacheMode::WriteCombining);
    // and so does splitting a write-combining gigabyte
    PageTables pt2(cpu, alloc);
    CHECK(pt2.initialize() && pt2.write_combining_enabled());   // picked up from the PAT register
    CHECK(pt2.map(0x8000000000ull, kGiB, wc));
    CHECK(pt2.map(0x8000002000ull, 4096, uc));
    CHECK(pt2.query(0x8000000000ull, &m) && m.page_size == 4096 && m.attributes.cache == CacheMode::WriteCombining);
    CHECK(pt2.query(0x8000200000ull, &m) && m.page_size == 2 * kMiB && m.attributes.cache == CacheMode::WriteCombining);
    CHECK(pt2.query(0x8000001000ull, &m) && m.attributes.cache == CacheMode::WriteCombining);
    // a CPU without PAT cannot
    FakeCpu old;
    old.pat = false;
    firmware_map(old, 1);
    PageTables pt3(old, alloc);
    CHECK(pt3.initialize() && !pt3.enable_write_combining() && !pt3.map(0x1000, 4096, wc));
    // another core loads the same PAT
    FakeCpu other;
    PageTables pt4(other, alloc);
    CHECK(pt4.initialize());
    other.pat_msr = cpu.pat_msr;
    pt4.load_pat();
    CHECK(other.writes == 0 || other.pat_msr == cpu.pat_msr);
  }

  // ---- no-execute only when the CPU has it on
  {
    FakeCpu cpu;
    TestAllocator alloc;
    firmware_map(cpu, 1);
    cpu.nx = false;
    PageTables pt(cpu, alloc);
    CHECK(pt.initialize());
    PageAttributes a;
    a.executable = false;
    CHECK(pt.map(0x2000, 4096, a));
    Leaf l = walk(cpu.root_table, 0x2000);
    CHECK(l.present && !(l.entry >> 63));   // a reserved bit would fault
    PageMapping m;
    CHECK(pt.query(0x2000, &m) && m.attributes.executable);
    cpu.nx = true;
    PageTables pt2(cpu, alloc);
    CHECK(pt2.initialize() && pt2.map(0x3000, 4096, a));
    l = walk(cpu.root_table, 0x3000);
    CHECK((l.entry >> 63) == 1 && pt2.query(0x3000, &m) && !m.attributes.executable);
    a.writable = false;
    CHECK(pt2.map(0x4000, 4096, a) && pt2.query(0x4000, &m) && !m.attributes.writable);
  }

  // ---- bad requests and running out of memory
  {
    FakeCpu cpu;
    TestAllocator alloc;
    firmware_map(cpu, 1);
    PageTables pt(cpu, alloc);
    PageAttributes a;
    CHECK(!pt.map(0x1000, 4096, a));   // not initialized
    CHECK(pt.initialize());
    CHECK(!pt.map(0x1000, 0, a) && !pt.map(1ull << 47, 4096, a) && !pt.map((1ull << 47) - 4096, 8192, a) && !pt.map(~0ull - 100, 4096, a));
    cpu.five = true;
    PageTables five(cpu, alloc);
    CHECK(!five.initialize());
    cpu.five = false;
    alloc.fail_after = 1;   // a PDPT entry, a PD and a PT are needed for 4 KiB far away; only one table can be had
    CHECK(!pt.map(0x3000001000ull, 4096, a));
    alloc.fail_after = -1;
    CHECK(pt.map(0x3000001000ull, 4096, a) && pt.is_mapped(0x3000001000ull, 4096));
    alloc.fail_after = 0;   // splitting needs a table
    CHECK(!pt.map(0x12345000, 4096, a));
    PageMapping m;
    CHECK(pt.query(0x12345000, &m) && m.present && m.page_size == kGiB);   // still as it was
  }
  // ---- several cores at once, no lock: they race to split the same large pages and to link in the same new tables
  {
    constexpr int kThreads = 8;
    constexpr uint64_t kSplitBase = 0xC0000000, kFarBase = 0x5000000000ull;   // inside a firmware gigabyte page; where nothing is mapped yet
    constexpr uint64_t kPages = 4 * 512;                                      // 8 MiB: four 2 MiB pages / page tables
    int lost_races = 0;
    for (int round = 0; round < 40; ++round) {
      FakeCpu cpu;
      TestAllocator alloc;
      firmware_map(cpu, 4);
      PageTables pt(cpu, alloc);
      CHECK(pt.initialize());
      PageAttributes uc;
      uc.cache = CacheMode::Uncached;
      std::atomic<int> failures{0};
      std::atomic<int> go{0};
      std::vector<std::thread> threads;
      for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
          ++go;
          while (go.load() < kThreads) {}
          // every thread takes every 8th page, so neighbouring pages (same 2 MiB page, same table) belong to different threads
          for (uint64_t i = t; i < kPages; i += kThreads) {
            if (!pt.map(kSplitBase + i * 4096, 4096, uc)) ++failures;
            if (!pt.map_missing(kFarBase + i * 4096, 4096, PageAttributes())) ++failures;
          }
          for (uint64_t i = t; i < kPages; i += kThreads) {
            PageMapping m;
            if (!pt.query(kFarBase + i * 4096, &m) || !m.present) ++failures;   // a page this thread mapped stays mapped while the others work
            if (i % 3 == 0 && !pt.unmap(kFarBase + i * 4096, 4096)) ++failures;
          }
        });
      for (std::thread& th : threads) th.join();
      CHECK(failures.load() == 0);
      bool ok = true;
      for (uint64_t i = 0; i < kPages; ++i) {
        PageMapping m;
        ok &= pt.query(kSplitBase + i * 4096, &m) && m.present && m.page_size == 4096 && m.physical == kSplitBase + i * 4096 &&
              m.attributes.cache == CacheMode::Uncached;
        ok &= pt.query(kFarBase + i * 4096, &m) && m.present == (i % 3 != 0) && (!m.present || (m.page_size == 4096 && m.physical == kFarBase + i * 4096));
      }
      CHECK(ok);
      PageMapping m;
      CHECK(pt.query(kSplitBase + kPages * 4096, &m) && m.page_size == 2 * kMiB && m.attributes.cache == CacheMode::WriteBack);   // past the range: as split, untouched
      CHECK(pt.query(0x80000000, &m) && m.page_size == kGiB);
      // one table per thing that needed one, whoever won: the split gigabyte (1 PD), its four 2 MiB pages (4 PTs), and far away (in the
      // firmware's PDPT) a PD and four PTs
      CHECK(alloc.blocks.size() == 1 + 4 + 1 + 4 && owned_tables(cpu.root_table, 4) == 10);
      CHECK(cpu.edits_begun.load() == cpu.edits_ended.load());
      lost_races += alloc.returned.load();
    }
    printf("paging: threaded rounds done, %d table(s) built by a core that lost the race\n", lost_races);
  }

  // ---- other cores' TLBs, and freeing the tables that a change unlinks
  {
    FakeCpu cpu;
    TestAllocator alloc;
    firmware_map(cpu, 4);
    uint64_t* pdpt = reinterpret_cast<uint64_t*>(cpu.root_table[0] & 0x000FFFFFFFFFF000ull);
    uint64_t* firmware_pd = static_cast<uint64_t*>(aligned_alloc(4096, 4096));   // gigabyte 5: the firmware mapped it with 2 MiB pages
    cpu.owned.push_back(firmware_pd);
    for (uint64_t i = 0; i < 512; ++i) firmware_pd[i] = (5 * kGiB + i * 2 * kMiB) | kP | kRW | kPS;
    pdpt[5] = reinterpret_cast<uint64_t>(firmware_pd) | kP | kRW;
    RcuDomain rcu;
    PageTables pt(cpu, alloc, &rcu);
    CHECK(pt.initialize());
    PageAttributes a;
    const uint64_t g6 = 6 * kGiB, g7 = 7 * kGiB;

    // without a shootdown: this core is flushed, and a table that is mapped over stays (another core might still use it)
    CHECK(pt.map(g6 + 0x1000, 4096, a) && alloc.blocks.size() == 2);   // a PD and a PT
    int flushes = cpu.full_flushes.load();
    CHECK(pt.map(g6, 2 * kMiB, a));
    rcu.barrier();
    CHECK(cpu.full_flushes.load() > flushes && alloc.returned.load() == 1 && alloc.blocks.size() == 2);   // (1: the node that would have carried it)
    PageMapping m;
    CHECK(pt.query(g6 + 0x1000, &m) && m.present && m.page_size == 2 * kMiB);

    // with one: every core is flushed instead, and the table is freed, but only after the grace period
    FakeShootdown sd;
    pt.set_shootdown(&sd);
    flushes = cpu.full_flushes.load();
    CHECK(pt.map(g6 + 2 * kMiB + 0x1000, 4096, a) && sd.calls.load() == 1 && sd.everything.load() == 0 && alloc.blocks.size() == 3);
    CHECK(pt.map(g6 + 2 * kMiB, 2 * kMiB, a) && sd.calls.load() == 2 && sd.everything.load() == 1);   // (a table went: flush everything)
    CHECK(cpu.full_flushes.load() == flushes);   // (the shootdown did this core, too)
    CHECK(alloc.returned.load() == 1);   // not before the grace period
    {
      RcuReader reader(rcu);              // a walk still under way holds it up
      CHECK(rcu.poll() == 0 && alloc.returned.load() == 1);
    }
    rcu.barrier();
    CHECK(alloc.returned.load() == 3 && alloc.blocks.size() == 2);   // the PT, and the node that carried it through the grace period
    CHECK(owned_tables(cpu.root_table, 4) == 1);   // the PD; the first PT is unreachable, but kept, as it was unlinked without a shootdown

    // a whole tree at once: unmapping the gigabyte frees the PD and the PTs below it
    CHECK(pt.map(g6 + 4 * kMiB, 4096, a) && pt.map(g6 + 6 * kMiB + 0x5000, 4096, a) && owned_tables(cpu.root_table, 4) == 3);
    int returned = alloc.returned.load();
    CHECK(pt.unmap(g6, kGiB));
    rcu.barrier();
    CHECK(alloc.returned.load() == returned + 3 + 1 && owned_tables(cpu.root_table, 4) == 0 && alloc.blocks.size() == 1);   // (+ the node; the old PT is left)
    CHECK(pt.query(g6 + 4 * kMiB, &m) && !m.present);

    // the firmware's tables are never given to the allocator, but tables of ours below them are
    CHECK(pt.map(5 * kGiB + 0x3000, 4096, a) && owned_tables(cpu.root_table, 4) == 1);   // its 2 MiB page split into a PT of ours
    returned = alloc.returned.load();
    CHECK(pt.map(5 * kGiB, kGiB, a));   // one 1 GiB page over the firmware's PD
    rcu.barrier();
    CHECK(alloc.foreign.load() == 0 && alloc.returned.load() == returned + 1 + 1 && owned_tables(cpu.root_table, 4) == 0);
    CHECK(pt.query(5 * kGiB + 0x3000, &m) && m.present && m.page_size == kGiB);

    // a shootdown that fails: map() says so, and nothing is freed
    CHECK(pt.map(g7 + 0x1000, 4096, a));
    const size_t live = alloc.blocks.size();
    sd.fail = true;
    CHECK(!pt.map(g7, 2 * kMiB, a));
    rcu.barrier();
    CHECK(alloc.blocks.size() == live);
    CHECK(pt.query(g7 + 0x1000, &m) && m.present && m.page_size == 2 * kMiB);   // (the change itself is made)
    sd.fail = false;

    // a caller that cannot wait for the other cores (interrupts masked) only flushes its own, and keeps what it unlinked
    CHECK(pt.map(g7 + 2 * kMiB + 0x1000, 4096, a));
    const int calls = sd.calls.load();
    flushes = cpu.full_flushes.load();
    cpu.may_wait = false;
    CHECK(pt.map(g7 + 2 * kMiB, 2 * kMiB, a));
    rcu.barrier();
    CHECK(sd.calls.load() == calls && cpu.full_flushes.load() == flushes + 1 && alloc.blocks.size() == live + 1);
    cpu.may_wait = true;
  }

  // ---- several cores churning: whole 2 MiB pages mapped and unmapped over the tables that others are filling with 4 KiB pages at the same moment.
  // Tables are freed while other walks run; the allocator frees them for real, so a walk that still used one would be caught by the address sanitizer.
  {
    constexpr int kThreads = 8;
    constexpr uint64_t kChurnBase = 0x6000000000ull;   // four 2 MiB pages
    int freed = 0;
    for (int round = 0; round < 20; ++round) {
      FakeCpu cpu;
      TestAllocator alloc;
      firmware_map(cpu, 1);
      RcuDomain rcu;
      FakeShootdown sd;
      PageTables pt(cpu, alloc, &rcu);
      CHECK(pt.initialize());
      pt.set_shootdown(&sd);
      std::atomic<int> failures{0}, go{0};
      std::vector<std::thread> threads;
      for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
          ++go;
          while (go.load() < kThreads) {}
          uint32_t x = 0x9E3779B9u * (t + 1) + round;
          for (int i = 0; i < 400; ++i) {
            x = x * 1664525u + 1013904223u;
            const uint64_t big = kChurnBase + ((x >> 8) & 3) * 2 * kMiB;
            const uint64_t page = big + ((x >> 12) & 511) * 4096;
            bool ok = true;
            switch ((x >> 24) % 8) {
              case 0: ok = pt.map(big, 2 * kMiB, PageAttributes()); break;   // over the PT, if there is one
              case 1: ok = pt.unmap(big, 2 * kMiB); break;
              case 2: ok = pt.unmap(kChurnBase, 8 * kMiB); break;          // (the PD stays: the range is part of its gigabyte)
              default: {
                ok = pt.map(page, 4096, PageAttributes());
                PageMapping m;
                ok &= pt.query(page, &m);
                ok &= !m.present || m.physical == (page & ~(m.page_size - 1));   // whatever is there maps 1:1
              }
            }
            if (!ok) ++failures;
          }
        });
      for (std::thread& th : threads) th.join();
      rcu.barrier();
      CHECK(failures.load() == 0 && alloc.foreign.load() == 0);
      // everything still allocated is a table that can be reached: nothing leaked, nothing freed that is still linked in
      CHECK(static_cast<int>(alloc.blocks.size()) == owned_tables(cpu.root_table, 4));
      freed += alloc.returned.load();
    }
    printf("paging: churn rounds done, %d table(s) and node(s) given back\n", freed);
  }

  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("paging tests passed\n");
  return 0;
}
