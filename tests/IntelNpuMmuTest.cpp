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


// Host test for the Intel NPU MMU: page tables, the address allocator, the SMMU programming and the contexts, against a pretend SMMU that walks the tables the
// host built, caches translations and descriptors the way the real one does, and reports faults in its event queue.
#include <stdio.h>
#include <sys/mman.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuMmu.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// DMA memory below 4 GiB so that a pointer is the bus address, as in the kernel; can be made to run out.
struct TestAllocator final : allocators::IAllocator {
  std::map<void*, size_t> dma;
  int dma_budget = -1;  // allocations left before failing; -1 = unlimited
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void* allocate_dma(uint64_t size, uint64_t) override {
    if (dma_budget == 0) return nullptr;
    if (dma_budget > 0) --dma_budget;
    const size_t rounded = (size + 4095) / 4096 * 4096;
    void* p = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    dma[p] = rounded;
    return p;
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
    auto it = dma.find(ptr);
    if (it != dma.end()) { munmap(ptr, it->second); dma.erase(it); } else free(ptr);
  }
};

struct SimBar final : INpuRegisters {
  std::map<uint64_t, uint32_t> regs;
  std::map<uint64_t, uint64_t> writes64;
  std::set<uint64_t> touched;
  std::function<void(SimBar&, uint64_t, uint32_t)> on_write;
  bool chaos = false;
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  uint32_t write_count = 0;
  uint32_t read32(uint64_t offset) override {
    touched.insert(offset);
    if (chaos) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<uint32_t>(rng); }
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint64_t offset, uint32_t value) override {
    touched.insert(offset);
    ++write_count;
    regs[offset] = value;
    if (on_write) on_write(*this, offset, value);
  }
  void write64(uint64_t offset, uint64_t value) override {
    touched.insert(offset);
    ++write_count;
    writes64[offset] = value;
    regs[offset] = static_cast<uint32_t>(value);
    regs[offset + 4] = static_cast<uint32_t>(value >> 32);
  }
};

constexpr uint64_t kCr0 = IntelNpuMmu::kRegCr0, kCmdqProd = IntelNpuMmu::kRegCmdqProd, kCmdqCons = IntelNpuMmu::kRegCmdqCons;

// The SMMU side of the NPU.
struct SimSmmu {
  SimBar bar;
  bool hang = false;          // commands never complete
  bool no_cr0_ack = false;
  std::vector<uint32_t> opcodes;
  uint32_t cfgi = 0, tlbi_all = 0, tlbi_asid = 0;
  uint32_t evt_prod = 0;
  struct Cd { uint64_t w0, ttb; };
  std::map<uint32_t, Cd> cd_cache;
  struct Tlb { uint64_t pa, entry; };
  std::map<uint64_t, Tlb> tlb;  // (ssid << 48 | page)

  SimSmmu() {
    bar.on_write = [this](SimBar& b, uint64_t off, uint32_t v) {
      if (off == kCr0 && !no_cr0_ack) b.regs[IntelNpuMmu::kRegCr0Ack] = v;
      if (off == IntelNpuMmu::kRegIrqCtrl) b.regs[IntelNpuMmu::kRegIrqCtrlAck] = v;
      if (off == kCmdqProd) process_commands();
    };
  }

  void process_commands() {
    if (hang || !(bar.regs[kCr0] & IntelNpuMmu::kCr0CmdqEn)) return;
    const uint64_t base = bar.writes64[IntelNpuMmu::kRegCmdqBase] & (((1ull << 52) - 1) & ~0x1Full);
    const uint32_t prod = bar.regs[kCmdqProd] & 0x1F;
    uint32_t cons = bar.regs[kCmdqCons] & 0xFFFFFF;
    while (cons != prod) {
      const uint64_t* q = reinterpret_cast<const uint64_t*>(base) + (cons & 15) * 2;
      const uint32_t op = q[0] & 0xFF;
      opcodes.push_back(op);
      if (op == IntelNpuMmu::kCmdSync) {
      } else if (op == IntelNpuMmu::kCmdCfgiAll) { ++cfgi; cd_cache.clear(); }
      else if (op == IntelNpuMmu::kCmdTlbiNsnhAll) { ++tlbi_all; tlb.clear(); }
      else if (op == IntelNpuMmu::kCmdTlbiNhAsid) {
        ++tlbi_asid;
        const uint32_t asid = static_cast<uint32_t>(q[0] >> 48);
        for (auto it = tlb.begin(); it != tlb.end();) it = (it->first >> 48) == asid ? tlb.erase(it) : std::next(it);
      } else {
        bar.regs[kCmdqCons] = cons | (1u << 24);  // illegal command: the consumer stops with an error code
        return;
      }
      cons = (cons + 1) & 0x1F;
    }
    bar.regs[kCmdqCons] = cons;
  }

  void report(uint32_t op, uint32_t sid, uint32_t ssid, uint64_t in_addr) {
    if (!(bar.regs[kCr0] & IntelNpuMmu::kCr0EvtqEn)) return;
    const uint64_t base = bar.writes64[IntelNpuMmu::kRegEvtqBase] & (((1ull << 52) - 1) & ~0x1Full);
    uint32_t* e = reinterpret_cast<uint32_t*>(base) + (evt_prod & 15) * 8;
    e[0] = op | (ssid << 12);
    e[1] = sid;
    e[2] = e[3] = 0;
    e[4] = static_cast<uint32_t>(in_addr); e[5] = static_cast<uint32_t>(in_addr >> 32);
    e[6] = 0x1000; e[7] = 0;
    evt_prod = (evt_prod + 1) & 0x1F;
    bar.regs[IntelNpuMmu::kRegEvtqProdSec] = evt_prod;
  }

  // One access by the NPU. Returns the bus address it reaches, or false after reporting a fault.
  bool translate(uint32_t sid, uint32_t ssid, uint64_t va, bool write, uint64_t* pa) {
    if (!(bar.regs[kCr0] & IntelNpuMmu::kCr0SmmuEn)) return false;
    const uint64_t key = (uint64_t{ssid} << 48) | (va >> 12);
    auto cached = tlb.find(key);
    if (cached != tlb.end()) return finish(cached->second.entry, cached->second.pa, va, write, sid, ssid, pa);
    if (sid >= IntelNpuMmu::kStreams) { report(IntelNpuMmu::kEvtBadStreamId, sid, ssid, va); return false; }
    const uint64_t strtab = bar.writes64[IntelNpuMmu::kRegStrtabBase] & (((1ull << 52) - 1) & ~0x3Full);
    const uint64_t* ste = reinterpret_cast<const uint64_t*>(strtab) + sid * 8;
    if (!(ste[0] & 1) || ((ste[0] >> 1) & 7) != 5) { report(IntelNpuMmu::kEvtBadSte, sid, ssid, va); return false; }
    if (ssid >= (1u << (ste[0] >> 59))) { report(IntelNpuMmu::kEvtBadSubstreamId, sid, ssid, va); return false; }
    if (!cd_cache.count(ssid)) {
      const uint64_t* cd = reinterpret_cast<const uint64_t*>(ste[0] & (((1ull << 52) - 1) & ~0x3Full)) + ssid * 8;
      cd_cache[ssid] = {cd[0], cd[1] & (((1ull << 52) - 1) & ~0xFull)};
    }
    const Cd cd = cd_cache[ssid];
    if (!((cd.w0 >> 31) & 1)) { report(IntelNpuMmu::kEvtBadCd, sid, ssid, va); return false; }
    uint64_t table = cd.ttb;
    for (int shift : {39, 30, 21}) {
      const uint64_t e = reinterpret_cast<const uint64_t*>(table)[(va >> shift) & 511];
      if ((e & 3) != 3) { report(IntelNpuMmu::kEvtTranslation, sid, ssid, va); return false; }
      table = e & IntelNpuPageTable::kAddressMask;
    }
    const uint64_t leaf = reinterpret_cast<const uint64_t*>(table)[(va >> 12) & 511];
    if ((leaf & 3) != 3) { report(IntelNpuMmu::kEvtTranslation, sid, ssid, va); return false; }
    tlb[key] = {leaf & IntelNpuPageTable::kAddressMask, leaf};
    return finish(leaf, leaf & IntelNpuPageTable::kAddressMask, va, write, sid, ssid, pa);
  }
  bool finish(uint64_t leaf, uint64_t page, uint64_t va, bool write, uint32_t sid, uint32_t ssid, uint64_t* pa) {
    if (write && (leaf & IntelNpuPageTable::kReadOnly)) { report(IntelNpuMmu::kEvtPermission, sid, ssid, va); return false; }
    *pa = page | (va & 0xFFF);
    return true;
  }
};

using Flags = IntelNpuPageTable::MapFlags;
static const uint64_t kMapped = IntelNpuPageTable::kMapped;

static void test_page_table() {
  TestAllocator alloc;
  {
    IntelNpuPageTable t(alloc, true);
    uint64_t e = 0;
    CHECK(t.root_bus_address() == 0 && !t.lookup(0x1000, &e) && t.table_count() == 0);
    CHECK(t.map(0x80002000, 0x12345000, 0x1000, Flags()));
    CHECK(t.lookup(0x80002000, &e) && e == (0x12345000 | kMapped));
    CHECK(t.table_count() == 3 && t.root_bus_address() != 0 && alloc.dma.size() == 4);   // root + PUD + PMD + PTE
    CHECK(!t.lookup(0x80003000, &e) || !(e & IntelNpuPageTable::kValid));
    // Flags.
    Flags ro; ro.read_only = true; ro.llc_coherent = true;
    CHECK(t.map(0x80010000 - 0x2000, 0x2000, 0x1000, ro) && t.lookup(0x8000E000, &e) && e == (0x2000 | kMapped | IntelNpuPageTable::kReadOnly | IntelNpuPageTable::kLlcCoherent));
    // 64 KiB hint only where both addresses are 64 KiB aligned and at least 64 KiB remain.
    CHECK(t.map(0x90000000, 0x40000000, 0x28000, Flags()));  // 160 KiB: two blocks and eight single pages
    for (uint64_t i = 0; i < 0x28000; i += 0x1000) {
      CHECK(t.lookup(0x90000000 + i, &e) && e == ((0x40000000 + i) | kMapped | (i < 0x20000 ? IntelNpuPageTable::kContiguous : 0)));
    }
    CHECK(t.map(0xA0001000, 0x50000000, 0x20000, Flags()) && t.lookup(0xA0001000, &e) && !(e & IntelNpuPageTable::kContiguous));  // misaligned virtual address
    CHECK(t.map(0xB0000000, 0x50001000, 0x20000, Flags()) && t.lookup(0xB0000000, &e) && !(e & IntelNpuPageTable::kContiguous));  // misaligned bus address
    Flags nocont; nocont.allow_contiguous = false;
    CHECK(t.map(0xC0000000, 0x60000000, 0x20000, nocont) && t.lookup(0xC0000000, &e) && !(e & IntelNpuPageTable::kContiguous));
    // Unmapping leaves the recognizable invalid entry.
    t.unmap(0x80002000, 0x1000);
    CHECK(t.lookup(0x80002000, &e) && e == IntelNpuPageTable::kInvalidEntry && !(e & 1));
    t.unmap(0x7000000000ull, 0x1000);   // tables never created: nothing happens
    // Refusals.
    const uint64_t before = alloc.dma.size();
    CHECK(!t.map(0x80000001, 0x1000, 0x1000, Flags()) && !t.map(0x80000000, 0x1001, 0x1000, Flags()) && !t.map(0x80000000, 0x1000, 0x1001, Flags()) && !t.map(0x80000000, 0x1000, 0, Flags()));
    CHECK(!t.map(IntelNpuPageTable::kAddressLimit, 0x1000, 0x1000, Flags()) && !t.map(IntelNpuPageTable::kAddressLimit - 0x1000, 0x1000, 0x2000, Flags()));
    CHECK(!t.map(~0ull & ~0xFFFull, 0x1000, 0x2000, Flags()));   // wraps
    CHECK(!t.map(0x80000000, IntelNpuPageTable::kBusLimit, 0x1000, Flags()) && !t.map(0x80000000, IntelNpuPageTable::kBusLimit - 0x1000, 0x2000, Flags()));
    CHECK(alloc.dma.size() == before);
    // The last page of the address space.
    CHECK(t.map(IntelNpuPageTable::kAddressLimit - 0x1000, 0x3000, 0x1000, Flags()) && t.lookup(IntelNpuPageTable::kAddressLimit - 0x1000, &e));
    CHECK(!t.lookup(IntelNpuPageTable::kAddressLimit, &e));
  }
  CHECK(alloc.dma.empty());  // the destructor freed every table

  // Out of memory in the middle of a mapping: nothing stays mapped, nothing leaks.
  for (int budget = 0; budget < 6; ++budget) {
    TestAllocator a;
    a.dma_budget = budget;
    {
      IntelNpuPageTable t(a, true);
      // Two PTE tables are needed: the range crosses a 2 MiB boundary.
      const bool ok = t.map(0x80000000 + 0x200000 - 0x2000, 0x1000, 0x4000, Flags());
      uint64_t e = 0;
      if (!ok) for (uint64_t i = 0; i < 4; ++i) CHECK(!t.lookup(0x80000000 + 0x200000 - 0x2000 + i * 0x1000, &e) || !(e & 1));
      CHECK(ok == (budget >= 5));  // root, PUD, PMD, and two PTE tables
    }
    CHECK(a.dma.empty());
  }

  // Read-only: splits the 64 KiB hints at the edges.
  {
    TestAllocator a;
    IntelNpuPageTable t(a, true);
    CHECK(t.map(0x90000000, 0x40000000, 0x40000, Flags()));   // four hinted blocks
    CHECK(t.set_read_only(0x90008000, 0x20000));               // starts and ends in the middle of blocks
    uint64_t e = 0;
    for (uint64_t i = 0; i < 0x40000; i += 0x1000) {
      CHECK(t.lookup(0x90000000 + i, &e));
      const bool ro = i >= 0x8000 && i < 0x28000;
      const bool hint = i >= 0x10000 && i < 0x20000 ? true : (i >= 0x30000);
      CHECK(((e & IntelNpuPageTable::kReadOnly) != 0) == ro);
      CHECK(((e & IntelNpuPageTable::kContiguous) != 0) == hint);   // blocks 0 and 2 were split, 1 and 3 untouched
    }
    CHECK(!t.set_read_only(0x90000000, 0x50000) && t.lookup(0x90000000, &e) && !(e & IntelNpuPageTable::kReadOnly));  // runs into unmapped pages: nothing changes
    CHECK(!t.set_read_only(0x90000800, 0x1000) && !t.set_read_only(0x90000000, 0) && !t.set_read_only(0xD0000000, 0x1000));
  }
}

static void test_address_space() {
  TestAllocator a;
  IntelNpuAddressSpace s(a, 0x1000000, 0x2000000);
  uint64_t x = 0, y = 0, z = 0;
  CHECK(s.free_bytes() == 0x1000000 && s.free_ranges() == 1);
  CHECK(s.allocate(0x1000000, 0x2000000, 0x1000, &x) && x == 0x1000000);
  CHECK(s.allocate(0x1000000, 0x2000000, 0x10000, &y) && y == 0x1010000);   // 64 KiB aligned, leaving a hole before it
  CHECK(s.free_ranges() == 2 && s.free_bytes() == 0x1000000 - 0x11000);
  CHECK(s.allocate(0x1000000, 0x2000000, 0x2000, &z) && z == 0x1001000);   // best fit: the hole, not the big range
  CHECK(!s.allocate(0x1000000, 0x2000000, 0, &z) && !s.allocate(0x1000000, 0x2000000, 0x1800, &z) && !s.allocate(0x1000000, 0x2000000, 0x2000000, &z));
  // Bounds of the request.
  uint64_t w = 0;
  CHECK(s.allocate(0x1800000, 0x1810000, 0x10000, &w) && w == 0x1800000);
  CHECK(!s.allocate(0x1800000, 0x1810000, 0x1000, &w));                      // that window is used up
  CHECK(!s.allocate(0x3000000, 0x4000000, 0x1000, &w) && !s.allocate(0x1900000, 0x1900000, 0x1000, &w));
  CHECK(!s.allocate(~0ull - 0x800, ~0ull, 0x1000, &w));                      // near the top of the address space: no wrap
  // Fixed addresses: carved out of a free range, refused when any of it is taken.
  { IntelNpuAddressSpace f(a, 0x2000000, 0x3000000);
    CHECK(f.reserve(0x2100000, 0x10000) && f.free_ranges() == 2 && f.free_bytes() == 0x1000000 - 0x10000);
    CHECK(!f.reserve(0x2100000, 0x1000) && !f.reserve(0x20FF000, 0x2000) && !f.reserve(0x2000800, 0x1000) && !f.reserve(0x2000000, 0) && !f.reserve(0x2FFF000, 0x2000) && !f.reserve(0x1000000, 0x1000));
    uint64_t g = 0;
    CHECK(f.allocate(0x2100000, 0x2110000, 0x1000, &g) == false);        // that window is taken now
    CHECK(f.reserve(0x2000000, 0x1000) && f.reserve(0x2FFF000, 0x1000) && f.free_ranges() == 2);   // from the start and from the end of a range
    CHECK(f.reserve(0x2001000, 0xFF000) && f.free_ranges() == 1);   // a whole range at once
    CHECK(f.release(0x2100000, 0x10000) && f.free_ranges() == 1 && f.free_bytes() == 0x2FFF000 - 0x2100000);   // joins its neighbour
  }
  // Release and merge.
  CHECK(s.release(x, 0x1000) && s.release(z, 0x2000));
  CHECK(!s.release(z, 0x2000));                                              // twice
  CHECK(!s.release(x + 0x800, 0x1000));                                     // overlaps free space
  CHECK(!s.release(0x500000000ull, 0x1000) && !s.release(0x1000000, 0) && !s.release(0x1FFF000, 0x2000));
  CHECK(s.release(y, 0x10000) && s.release(w, 0x10000));
  CHECK(s.free_bytes() == 0x1000000 && s.free_ranges() == 1);               // everything joined again
  // Out of memory while splitting leaves the space as it was.
  struct Fail final : allocators::IAllocator {
    int budget = 100;
    void* allocate(uint64_t n) override { if (budget-- <= 0) return nullptr; return malloc(n); }
    void* allocate_aligned(uint64_t n, uint64_t al) override { return allocate(n ? (n + al - 1) / al * al : 1); }
    void deallocate(void* p) override { free(p); }
  } f;
  IntelNpuAddressSpace t(f, 0, 0x100000);
  f.budget = 0;
  const uint64_t free_before = t.free_bytes();
  const uint64_t ranges_before = t.free_ranges();
  const bool ok = t.allocate(0x40000, 0x50000, 0x1000, &y);   // from the middle: the free range has to be split in two
  CHECK(t.free_bytes() == free_before - (ok ? 0x1000 : 0) && t.free_ranges() == (ok ? ranges_before + 1 : ranges_before));
}

// The sequence the driver programs, checked against the register values the SMMU manual and ivpu give.
static void test_mmu_init() {
  TestAllocator a;
  SimSmmu sim;
  {
    IntelNpuMmu mmu(sim.bar, a, true);
    CHECK(!mmu.on() && !mmu.set_context(0, 0x1000) && !mmu.enable());   // not initialized
    CHECK(mmu.init() && mmu.on());
    CHECK(sim.bar.regs[kCr0] == 0x1D && sim.bar.regs[IntelNpuMmu::kRegIrqCtrl] == 5);           // SMMU, CMDQ, EVTQ and ATS check; event and error interrupts
    CHECK(sim.bar.regs[IntelNpuMmu::kRegCr1] == 0xD75 && sim.bar.regs[IntelNpuMmu::kRegStrtabBaseCfg] == 2);
    const uint64_t cmdq = sim.bar.writes64[IntelNpuMmu::kRegCmdqBase], evtq = sim.bar.writes64[IntelNpuMmu::kRegEvtqBase], str = sim.bar.writes64[IntelNpuMmu::kRegStrtabBase];
    CHECK((cmdq >> 62) == 1 && (cmdq & 0x1F) == 4 && (evtq >> 62) == 1 && (evtq & 0x1F) == 4 && (str >> 62) == 1);
    CHECK((cmdq & 0xFFF) == 4 && (evtq & 0xFFF) == 4 && (str & 0xFFF) == 0);   // page-aligned structures
    // The commands of the reset: CFGI_ALL, TLBI_NSNH_ALL, SYNC, then again after the MMU is on.
    const std::vector<uint32_t> expected = {0x04, 0x30, 0x46, 0x04, 0x46, 0x30, 0x46};
    CHECK(sim.opcodes == expected);
    // Streams 0 and 3 are stage-1 translating, with the same descriptor table; 1 and 2 are not valid.
    const uint64_t* ste = reinterpret_cast<const uint64_t*>(str & ~0x7FFull & ((1ull << 52) - 1) & ~0x3Full);
    CHECK((ste[0] & 1) && ((ste[0] >> 1) & 7) == 5 && (ste[0] >> 59) == 8 && ((ste[0] >> 4) & 3) == 0);
    CHECK((ste[8 * 3] & 1) && (ste[8 * 3] & ~0x3Full & ((1ull << 52) - 1)) == (ste[0] & ~0x3Full & ((1ull << 52) - 1)));
    CHECK(!(ste[8] & 1) && !(ste[16] & 1));
    CHECK(((ste[1] >> 48) & 3) == 2 && ((ste[1] >> 50) & 3) == 2 && ((ste[1] >> 13) & 0xF) == 2 && (ste[1] & (1u << 19)) && (ste[1] & (1u << 27)));
    // Descriptors.
    CHECK(mmu.set_context(5, 0x20000000));
    const uint64_t* cdt = reinterpret_cast<const uint64_t*>(ste[0] & ~0x3Full & ((1ull << 52) - 1));
    const uint64_t* cd = cdt + 5 * 8;
    CHECK((cd[0] & 0x3F) == 16 && ((cd[0] >> 32) & 7) == 5 && (cd[0] >> 48) == 5 && (cd[0] & (1ull << 31)) && (cd[0] & (1ull << 41)) && (cd[0] & (1ull << 45)) &&
          (cd[0] & (1ull << 47)) && (cd[0] & (1ull << 30)) && !(cd[0] & (1ull << 46)) && cd[1] == 0x20000000 && cd[3] == 0x7444);
    CHECK(mmu.set_context(0, 0x20000000) && (cdt[0] & (1ull << 46)) && mmu.set_context(1, 0x20000000) && (cdt[8] & (1ull << 46)));   // A bit: fault loudly
    CHECK(sim.cfgi == 5);   // reset, enable, and one per descriptor written
    CHECK(!mmu.set_context(256, 0x20000000) && !mmu.set_context(1000, 0x20000000));     // one past the table (ivpu allows 256)
    CHECK(!mmu.set_context(7, 0) && !mmu.set_context(7, 0x20000008) && !mmu.set_context(7, 1ull << 52));
    CHECK(mmu.clear_context(5) && !(cd[0] & (1ull << 31)) && !mmu.clear_context(256));
    CHECK(mmu.disable_context_events(0) && !(cdt[0] & (1ull << 45)) && !mmu.disable_context_events(256));
    // TLB invalidation is one TLBI_NH_ASID and a SYNC.
    sim.opcodes.clear();
    CHECK(mmu.invalidate_tlb(9));
    const std::vector<uint32_t> inval = {0x11, 0x46};
    CHECK(sim.opcodes == inval);
    // Off: nothing reaches the hardware.
    mmu.disable();
    const uint32_t writes = sim.bar.write_count;
    CHECK(!mmu.on() && mmu.invalidate_tlb(3) && mmu.set_context(6, 0x30000000) && sim.bar.write_count == writes);
    // Back on (the NPU lost power): the registers are programmed again.
    CHECK(mmu.enable() && mmu.on() && sim.bar.regs[kCr0] == 0x1D);
  }
  CHECK(a.dma.empty());
}

static void test_mmu_failures() {
  TestAllocator a;
  {
    SimSmmu sim;
    sim.no_cr0_ack = true;
    IntelNpuMmu mmu(sim.bar, a, true);
    CHECK(!mmu.init() && !mmu.on());      // CR0 never acknowledged
  }
  {
    SimSmmu sim;
    sim.hang = true;
    IntelNpuMmu mmu(sim.bar, a, true);
    CHECK(!mmu.init() && !mmu.on() && mmu.last_command_error() == 0);
  }
  {
    SimSmmu sim;
    IntelNpuMmu mmu(sim.bar, a, true);
    CHECK(mmu.init());
    // A command the MMU rejects reports its error code in the consumer register.
    sim.bar.regs[kCmdqCons] = 0;
    sim.hang = true;
    CHECK(!mmu.invalidate_tlb(1));
    sim.hang = false;
    // A descriptor the MMU cannot be told about is withdrawn again.
    sim.hang = true;
    CHECK(!mmu.set_context(4, 0x20000000));
    sim.hang = false;
  }
  CHECK(a.dma.empty());
  // Out of memory at each allocation of init().
  for (int budget = 0; budget < 4; ++budget) {
    TestAllocator limited;
    limited.dma_budget = budget;
    SimSmmu sim;
    {
      IntelNpuMmu mmu(sim.bar, limited, true);
      CHECK(!mmu.init());
    }
    CHECK(limited.dma.empty());
  }
}

static void test_contexts_and_faults() {
  TestAllocator a;
  SimSmmu sim;
  const IntelNpuMemoryMap map = IntelNpuMemoryMap::for_ip(IntelNpuIp::Ip40xx);
  IntelNpuMmu mmu(sim.bar, a, true);
  CHECK(mmu.init());
  {
    IntelNpuMmuContext user(a, mmu, map, 2, true), global(a, mmu, map, 0, true);
    CHECK(!user.descriptor_valid());
    // A buffer made of two separate pieces of host memory.
    uint64_t va = 0;
    CHECK(user.address_space().allocate(map.user.start, map.user.end, 0x6000, &va) && va >= map.user.start);
    void* p1 = a.allocate_dma(0x2000, 4096);
    void* p2 = a.allocate_dma(0x4000, 4096);
    const IntelNpuDmaExtent ex[2] = {{reinterpret_cast<uint64_t>(p1), 0x2000}, {reinterpret_cast<uint64_t>(p2), 0x4000}};
    CHECK(user.map(va, ArrayView<const IntelNpuDmaExtent>(ex), 0x6000, Flags()) && user.descriptor_valid());
    uint64_t pa = 0;
    CHECK(sim.translate(0, 2, va + 0x10, false, &pa) && pa == ex[0].bus_addr + 0x10);
    CHECK(sim.translate(3, 2, va + 0x2000, true, &pa) && pa == ex[1].bus_addr);
    CHECK(sim.translate(0, 2, va + 0x5FFF, false, &pa) && pa == ex[1].bus_addr + 0x3FFF);
    // The NPU touches what was never mapped: a translation fault is queued with the context, the address and the type.
    CHECK(!sim.translate(0, 2, va + 0x6000, false, &pa));
    IntelNpuMmuEvent ev;
    CHECK(mmu.next_event(&ev) && ev.op == IntelNpuMmu::kEvtTranslation && ev.ssid == 2 && ev.sid == 0 && ev.in_address == va + 0x6000 && ev.fetch_address == 0x1000);
    CHECK(std::string(intel_npu_mmu_event_name(ev.op)) == "Translation fault" && !mmu.next_event(&ev));
    CHECK(sim.bar.regs[IntelNpuMmu::kRegEvtqConsSec] == 1);
    // A context that was never set up, an unknown stream.
    CHECK(!sim.translate(0, 9, va, false, &pa) && mmu.next_event(&ev) && ev.op == IntelNpuMmu::kEvtBadCd && ev.ssid == 9);
    CHECK(!sim.translate(7, 2, 0x1000, false, &pa) && mmu.next_event(&ev) && ev.op == IntelNpuMmu::kEvtBadStreamId);
    // Unmapping invalidates: the cached translation must be gone.
    const uint32_t invalidations = sim.tlbi_asid;
    user.unmap(va, ArrayView<const IntelNpuDmaExtent>(ex));
    CHECK(sim.tlbi_asid == invalidations + 1 && !sim.translate(0, 2, va + 0x10, false, &pa) && mmu.next_event(&ev) && ev.op == IntelNpuMmu::kEvtTranslation);
    CHECK(user.address_space().release(va, 0x6000));

    // Bad maps leave nothing behind: extents shorter or longer than the buffer, an extent that is not page aligned, a hung MMU.
    const IntelNpuDmaExtent shorter[1] = {{ex[0].bus_addr, 0x2000}}, odd[1] = {{ex[0].bus_addr + 0x100, 0x2000}};
    CHECK(!user.map(va, ArrayView<const IntelNpuDmaExtent>(shorter), 0x6000, Flags()) && !user.map(va, ArrayView<const IntelNpuDmaExtent>(ex), 0x5000, Flags()));
    CHECK(!user.map(va, ArrayView<const IntelNpuDmaExtent>(odd), 0x2000, Flags()) && !user.map(va + 1, ArrayView<const IntelNpuDmaExtent>(ex), 0x6000, Flags()));
    uint64_t e = 0;
    for (uint64_t i = 0; i < 0x6000; i += 0x1000) CHECK(!user.page_table().lookup(va + i, &e) || !(e & 1));
    sim.hang = true;
    CHECK(!user.map(va, ArrayView<const IntelNpuDmaExtent>(ex), 0x6000, Flags()));     // the TLB invalidation never completes
    sim.hang = false;
    for (uint64_t i = 0; i < 0x6000; i += 0x1000) CHECK(!user.page_table().lookup(va + i, &e) || !(e & 1));

    // The firmware's memory in the global context, read-only section included.
    IntelNpuFirmware fw;
    fw.error = IntelNpuFirmwareError::None;
    fw.boot_params_addr = map.runtime.start; fw.boot_params_size = 4096;
    fw.fw_version_addr = map.runtime.start + 0x1000; fw.fw_version_size = 4096;
    fw.runtime_addr = map.runtime.start + 0x2000; fw.runtime_size = 0x20000;
    fw.read_only_addr = fw.runtime_addr + 0x4000; fw.read_only_size = 0x2000;
    void* bp = a.allocate_dma(0x1000, 4096); void* ver = a.allocate_dma(0x1000, 4096); void* rt = a.allocate_dma(0x20000, 4096);
    IntelNpuFirmwareMemory mem{{reinterpret_cast<uint64_t>(bp), 0x1000}, {reinterpret_cast<uint64_t>(ver), 0x1000}, {reinterpret_cast<uint64_t>(rt), 0x20000}};
    CHECK(intel_npu_map_firmware(global, fw, mem));
    CHECK(sim.translate(0, 0, fw.boot_params_addr + 8, true, &pa) && pa == mem.boot_params.bus_addr + 8);
    CHECK(sim.translate(0, 0, fw.runtime_addr + 0x1234, false, &pa) && pa == mem.runtime.bus_addr + 0x1234);
    CHECK(sim.translate(0, 0, fw.read_only_addr, false, &pa) && !sim.translate(0, 0, fw.read_only_addr + 0x10, true, &pa));   // reads work, writes fault
    CHECK(mmu.next_event(&ev) && ev.op == IntelNpuMmu::kEvtPermission && ev.ssid == 0 && ev.in_address == fw.read_only_addr + 0x10);
    CHECK(sim.translate(0, 0, fw.read_only_addr + 0x2000, true, &pa));                                                     // just past the section
    // Wrong sizes, and a read-only section outside the runtime, are refused with nothing left mapped.
    IntelNpuFirmwareMemory wrong = mem; wrong.runtime.length = 0x10000;
    CHECK(!intel_npu_map_firmware(global, fw, wrong) && !intel_npu_map_firmware(user, fw, mem) && !intel_npu_map_firmware(global, IntelNpuFirmware(), mem));
    IntelNpuFirmware bad = fw; bad.read_only_addr = fw.runtime_addr + 0x30000;
    IntelNpuMmuContext global2(a, mmu, map, 0, true);
    CHECK(!intel_npu_map_firmware(global2, bad, mem) && (!global2.page_table().lookup(fw.runtime_addr, &e) || !(e & 1)));
    a.deallocate(bp); a.deallocate(ver); a.deallocate(rt); a.deallocate(p1); a.deallocate(p2);
  }
  // Contexts clear their descriptors when destroyed.
  // The reserved context faults on everything.
  {
    IntelNpuMmuContext reserved(a, mmu, map, 1, true);
    CHECK(reserved.init_reserved() && reserved.descriptor_valid());
    uint64_t pa = 0;
    IntelNpuMmuEvent ev;
    CHECK(!sim.translate(0, 1, map.user.start, false, &pa) && mmu.next_event(&ev) && ev.op == IntelNpuMmu::kEvtTranslation && ev.ssid == 1);
    const IntelNpuDmaExtent one[1] = {{0x1000, 0x1000}};
    CHECK(!reserved.map(map.user.start, ArrayView<const IntelNpuDmaExtent>(one), 0x1000, Flags()));
  }
  // Events and errors.
  IntelNpuMmuEvent ev;
  for (uint32_t i = 0; i < 5; ++i) sim.report(IntelNpuMmu::kEvtAccess, 0, 3, 0x1000 * i);
  CHECK(mmu.discard_events() && !mmu.next_event(&ev) && sim.bar.regs[IntelNpuMmu::kRegEvtqConsSec] == sim.evt_prod);
  for (uint32_t i = 0; i < 20; ++i) { sim.report(IntelNpuMmu::kEvtAccess, 0, 3, 0x1000 * i); CHECK(mmu.next_event(&ev) && ev.in_address == 0x1000ull * i); }   // wraps the 16-entry queue
  sim.bar.regs[IntelNpuMmu::kRegGerror] = 0x5; sim.bar.regs[IntelNpuMmu::kRegGerrorn] = 0x1;
  CHECK(mmu.acknowledge_global_errors() == 0x4 && sim.bar.regs[IntelNpuMmu::kRegGerrorn] == 0x5 && mmu.acknowledge_global_errors() == 0);
}

// Whatever the registers read, the code ends and touches only the MMU's registers; memory and descriptors stay inside their tables.
static void test_chaos() {
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    TestAllocator a;
    SimSmmu sim;
    sim.bar.chaos = true;
    sim.bar.rng = 0x1234567ull * seed;
    {
      IntelNpuMmu mmu(sim.bar, a, seed & 1);
      (void)mmu.init();
      (void)mmu.enable();
      (void)mmu.set_context(seed, 0x20000000);
      (void)mmu.invalidate_tlb(seed);
      IntelNpuMmuEvent ev;
      for (int i = 0; i < 40; ++i) (void)mmu.next_event(&ev);
      (void)mmu.discard_events();
      (void)mmu.acknowledge_global_errors();
    }
    for (uint64_t off : sim.bar.touched) CHECK(off >= 0x200000 && off < IntelNpuMmu::kRegisterEnd);
    CHECK(a.dma.empty());
  }
}

int main() {
  test_page_table();
  test_address_space();
  test_mmu_init();
  test_mmu_failures();
  test_contexts_and_faults();
  test_chaos();
  if (g_failed) { printf("IntelNpuMmuTest: %d failed\n", g_failed); return 1; }
  printf("IntelNpuMmuTest: ok\n");
  return 0;
}
