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
#include <vector>
#include "Kernel/Paging.hpp"

using namespace kernel;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  long fail_after = -1;   // allocations that still succeed (-1: all)
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (fail_after == 0) return nullptr;
    if (fail_after > 0) --fail_after;
    void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
    blocks.push_back(p);
    return p;
  }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

struct FakeCpu final : IPagingHost {
  uint64_t* root_table;
  bool five = false, nx = true, gb = true, pat = true;
  uint64_t pat_msr = 0x0007040600070406ull;
  int full_flushes = 0, writes = 0;
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
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("paging tests passed\n");
  return 0;
}
