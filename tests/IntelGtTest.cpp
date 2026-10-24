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

// Host-side tests for the Intel GPU submission building blocks (Gen8 to Gen10 layout): global GTT geometry and writes inside the range the driver owns, the
// 32-bit per-context page tables checked by walking them the way the hardware does, the context register image checked against the register offsets it must contain,
// and the execlist port against a pretend engine that records writes and answers with status entries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGtt.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelLrc.hpp"

using namespace hardware;
using namespace hardware::intel_gt;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// Page memory with made-up device addresses (not the CPU addresses), so a table that holds the wrong kind of address is noticed.
struct FakePageSource final : IGpuPageSource {
  struct Page { std::vector<uint64_t> words; };
  std::map<uint64_t, Page*> by_dma;
  uint64_t next_dma = 0x4000'0000;
  int live = 0;
  int budget = 1 << 30;  // pages that may still be allocated
  bool allocate_page(ArrayView<uint64_t>* entries, uint64_t* dma) override {
    if (budget <= 0) return false;
    --budget;
    Page* p = new Page;
    p->words.assign(512, 0);
    *dma = next_dma;
    next_dma += 0x1000;
    by_dma[*dma] = p;
    ++live;
    *entries = ArrayView<uint64_t>(p->words.data(), p->words.size());
    return true;
  }
  void free_page(ArrayView<uint64_t> entries) override {
    for (auto it = by_dma.begin(); it != by_dma.end(); ++it)
      if (it->second->words.data() == entries.data()) { delete it->second; by_dma.erase(it); --live; return; }
    CHECK(false);  // freed a page that is not ours
  }
  const std::vector<uint64_t>* page_at(uint64_t dma) const {
    auto it = by_dma.find(dma & ~0xFFFull);
    return it == by_dma.end() ? nullptr : &it->second->words;
  }
};

static void test_entries_and_geometry() {
  CHECK(ppgtt_pte(0x1000, Caching::Cached) == 0x1083 && ppgtt_pte(0x1000, Caching::Uncached) == 0x101B);
  CHECK(ppgtt_pde(0x1000, Caching::Cached) == 0x1003 && ppgtt_pde(0x1000, Caching::Uncached) == 0x101B);
  CHECK(ggtt_pte(0x12345000) == 0x12345001 && ggtt_pte(0x12345FFF) == 0x12345001);  // low bits of the address are dropped
  CHECK(gen9_private_pat().low == 0x00030907 && gen9_private_pat().high == 0x3B2B1B0B);

  GgttGeometry g;
  CHECK(ggtt_geometry(0x0080, 16u << 20, &g) && g.table_offset == (8u << 20) && g.table_bytes == (4u << 20) && g.entries == 524288 && g.address_space == (2ull << 30));
  CHECK(ggtt_geometry(0x00C0, 16u << 20, &g) && g.table_bytes == (8u << 20) && g.address_space == (4ull << 30));
  CHECK(ggtt_geometry(0x0040, 16u << 20, &g) && g.table_bytes == (2u << 20) && g.address_space == (1ull << 30));
  CHECK(ggtt_geometry(0xFF40, 16u << 20, &g) && g.table_bytes == (2u << 20));  // other fields of the word are ignored
  CHECK(!ggtt_geometry(0x0000, 16u << 20, &g) && !ggtt_geometry(0x0080, 0, &g) && !ggtt_geometry(0x0080, (16u << 20) + 1, &g));
  CHECK(!ggtt_geometry(0x00C0, 8u << 20, &g));  // an 8 MiB table does not fit after the first half of an 8 MiB BAR
}

static void test_ggtt_writer() {
  std::vector<uint64_t> table(1024, 0x1111111111111111ull);  // 1024 entries: a pretend table
  const MmioRegion region(reinterpret_cast<uint64_t>(table.data()), table.size() * 8);
  Ggtt g(region, 100, 100);  // the driver owns entries 100-199
  CHECK(g.valid() && g.page_count() == 100 && g.address_of(0) == 100 * 4096 && g.address_of(5) == 105 * 4096);
  CHECK(g.map(0, 0x5000) && table[100] == 0x5001);
  CHECK(g.map(99, 0x7000'0000'0000ull & ~0xFFFull) && table[199] == ((0x7000'0000'0000ull & ~0xFFFull) | 1));
  CHECK(!g.map(100, 0x5000) && !g.map(0, 0x5001) && table[200] == 0x1111111111111111ull && table[99] == 0x1111111111111111ull);
  CHECK(g.clear(10, 5, 0x9000) && table[110] == 0x9001 && table[114] == 0x9001 && table[109] == 0x1111111111111111ull && table[115] == 0x1111111111111111ull);
  CHECK(!g.clear(98, 5, 0x9000) && !g.clear(101, 0, 0x9000) && !g.clear(0, 1, 0x9001));
  uint64_t e = 0;
  CHECK(g.read(10, &e) && e == 0x9001 && !g.read(100, &e));
  CHECK(!Ggtt(region, 1000, 100).valid() && !Ggtt(region, 0, 0).valid() && !Ggtt(MmioRegion(), 0, 1).valid() && Ggtt(region, 0, 1024).valid() && !Ggtt(region, 0, 1025).valid());
  CHECK(!Ggtt(region, ~0ull, 2).valid());  // no wraparound

  std::vector<uint32_t> regs(0x102000 / 4, 0);
  const MmioRegion registers(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4);
  ggtt_invalidate(registers);
  CHECK(regs[kGttFlushRegister / 4] == 1);
  program_private_pat(registers, gen9_private_pat());
  CHECK(regs[kPrivatePatLow / 4] == 0x00030907 && regs[kPrivatePatHigh / 4] == 0x3B2B1B0B);
}

// Walks the tables the way the hardware does: directory (from the context image) -> page table -> page.
static bool walk(const FakePageSource& src, const Ppgtt32& vm, uint64_t gpu, uint64_t* dma) {
  const std::vector<uint64_t>* pd = src.page_at(vm.page_directory_dma(static_cast<uint32_t>(gpu >> 30)));
  if (!pd) return false;
  const uint64_t pde = (*pd)[(gpu >> 21) & 511];
  if (!(pde & 1)) return false;
  const std::vector<uint64_t>* pt = src.page_at(pde & ~0xFFFull);
  if (!pt) return false;
  const uint64_t pte = (*pt)[(gpu >> 12) & 511];
  if (!(pte & 1)) return false;
  *dma = (pte & ~0xFFFull) | (gpu & 0xFFF);
  return true;
}

static void test_unused_range() {
  uint64_t first = 0;
  std::vector<uint64_t> t(1024, 0);
  const MmioRegion r(reinterpret_cast<uint64_t>(t.data()), t.size() * 8);
  CHECK(find_unused_ggtt_range(r, 8, 16, &first) && first == 1024 - 16 - 8);  // empty table: the top, below the guard
  for (uint64_t i = 990; i < 1024; ++i) t[i] = (i << 12) | 1;                // mappings at the top
  CHECK(find_unused_ggtt_range(r, 8, 16, &first) && first == 982);           // 982-989 are the highest eight free entries below the mapped ones
  CHECK(first + 8 <= 1024 - 16);
  for (uint64_t i = 0; i < 1024; ++i) t[i] = ((i + 1) << 12) | 1;            // everything mapped to something different
  CHECK(!find_unused_ggtt_range(r, 8, 16, &first));
  for (uint64_t i = 600; i < 640; ++i) t[i] = 0x7000'0001ull;                // a run of the same entry: a scratch page
  CHECK(find_unused_ggtt_range(r, 8, 16, &first) && first >= 600 && first + 8 <= 640);
  for (uint64_t i = 0; i < 1024; ++i) t[i] = 0;
  t[1024 - 16 - 1] = 1;                                                      // the entry just under the guard is in use: the range must still not overlap it
  CHECK(find_unused_ggtt_range(r, 8, 16, &first) && first + 8 <= 1024 - 16 - 1);
  CHECK(!find_unused_ggtt_range(r, 0, 16, &first) && !find_unused_ggtt_range(r, 8, 1020, &first) && !find_unused_ggtt_range(r, 2000, 0, &first));
  CHECK(!find_unused_ggtt_range(MmioRegion(), 8, 0, &first));
}

static void test_ppgtt() {
  {
    FakePageSource src;
    {
      Ppgtt32 vm(src);
      CHECK(!vm.map(0x1000, 0x8000));  // not initialized
      CHECK(vm.initialize());
      CHECK(src.live == 2 + 4);  // scratch page and table, four directories
      uint64_t d = 0;
      CHECK(walk(src, vm, 0x1234, &d) && d == vm.scratch_dma() + 0x234);  // unmapped reads the scratch page
      CHECK(walk(src, vm, 0xFFFF'F000, &d) && d == vm.scratch_dma());
      CHECK(vm.map(0x1000, 0x8000'0000) && walk(src, vm, 0x1000, &d) && d == 0x8000'0000);
      CHECK(walk(src, vm, 0x1abc, &d) && d == 0x8000'0abc);
      CHECK(walk(src, vm, 0x2000, &d) && d == vm.scratch_dma());
      uint64_t t = 0;
      CHECK(vm.translate(0x1abc, &t) && t == 0x8000'0abc && vm.translate(0x2000, &t) && t == vm.scratch_dma());
      CHECK(src.live == 2 + 4 + 1);  // one page table was added
      CHECK(vm.map(0x3000, 0x8000'1000) && src.live == 2 + 4 + 1);  // the same table serves the next page
      CHECK(vm.map(0x4000'0000, 0x9000'0000) && walk(src, vm, 0x4000'0000, &d) && d == 0x9000'0000);
      CHECK(vm.map(0xFFFF'F000, 0xA000'0000) && walk(src, vm, 0xFFFF'F000, &d) && d == 0xA000'0000);
      CHECK(vm.map(0x20'0000, 0xB000'0000) && walk(src, vm, 0x20'0000, &d) && d == 0xB000'0000);  // the next 2 MiB: its own table
      CHECK(!vm.map(0x1'0000'0000ull, 0x8000) && !vm.map(0x1001, 0x8000) && !vm.map(0x5000, 0x8001));
      CHECK(!vm.translate(0x1'0000'0000ull, &t));
      CHECK(vm.page_directory_dma(0) != vm.page_directory_dma(1) && vm.page_directory_dma(4) == 0);
    }
    CHECK(src.live == 0);  // everything given back
  }
  // Running out of pages anywhere leaves nothing behind and nothing half-mapped.
  for (int budget = 0; budget < 8; ++budget) {
    FakePageSource src;
    src.budget = budget;
    {
      Ppgtt32 vm(src);
      const bool ok = vm.initialize();
      CHECK(ok == (budget >= 6));
      if (ok) {
        const int before = src.live;
        const bool mapped = vm.map(0x1000, 0x8000);
        CHECK(mapped == (budget >= 7));
        CHECK(src.live == before + (mapped ? 1 : 0));
        uint64_t d = 0;
        CHECK(walk(src, vm, 0x1000, &d) && d == (mapped ? 0x8000 : vm.scratch_dma()));
      }
    }
    CHECK(src.live == 0);
  }
}

static void test_lrc() {
  std::vector<uint32_t> page(kLrcStateDwords, 0xAAAAAAAA);
  const ArrayView<uint32_t> state(page.data(), page.size());
  const uint32_t base = kVcs0MmioBase;
  CHECK(!lrc_init_gen9_xcs(ArrayView<uint32_t>(page.data(), 100), base, true));
  CHECK(lrc_init_gen9_xcs(state, base, true));
  CHECK(page[0] == 0 && page[1] == 0x1100101B && page[2] == base + 0x244);
  CHECK(page[kCtxContextControl] == 0x000F0009);
  CHECK(page[33] == 0x11001011 && page[34] == base + 0x3a8);
  CHECK(page[65] == 0x11001001 && page[66] == base + 0x200);
  CHECK(page[81] == 0x11001057 && page[82] == base + 0x028);
  CHECK(page[kCtxRingMiMode] == 0x01000000);
  // Every named value slot sits right after the register it belongs to.
  CHECK(page[kCtxContextControl - 1] == base + 0x244 && page[kCtxRingHead - 1] == base + 0x034 && page[kCtxRingTail - 1] == base + 0x030);
  CHECK(page[kCtxRingStart - 1] == base + 0x038 && page[kCtxRingCtl - 1] == base + 0x03c && page[kCtxRingMiMode - 1] == base + 0x09c);
  CHECK(page[kCtxPdp3Udw - 1] == base + 0x28c && page[kCtxPdp3Ldw - 1] == base + 0x288 && page[kCtxPdp2Udw - 1] == base + 0x284 && page[kCtxPdp2Ldw - 1] == base + 0x280);
  CHECK(page[kCtxPdp1Udw - 1] == base + 0x27c && page[kCtxPdp1Ldw - 1] == base + 0x278 && page[kCtxPdp0Udw - 1] == base + 0x274 && page[kCtxPdp0Ldw - 1] == base + 0x270);
  // Every load header is followed by exactly its count of register offsets inside this engine's register range, and no stray value is set.
  uint32_t headers = 0;
  for (uint32_t i = 0; i < page.size(); ++i)
    if ((page[i] >> 23) == 0x22) {
      ++headers;
      const uint32_t regs = ((page[i] & 0xFFF) + 1) / 2;
      for (uint32_t r = 0; r < regs; ++r) CHECK(page[i + 1 + 2 * r] >= base && page[i + 1 + 2 * r] < base + 0x1000);
    }
  CHECK(headers == 4);
  CHECK(lrc_init_gen9_xcs(state, base, false) && page[kCtxContextControl] == 0x000F0008);

  CHECK(lrc_set_ring(state, 0x00200000, 4096 * 4, 0, 0) && page[kCtxRingStart] == 0x00200000 && page[kCtxRingCtl] == ((4096 * 4 - 4096) | 1) && page[kCtxRingHead] == 0 && page[kCtxRingTail] == 0);
  CHECK(lrc_set_tail(state, 0x40) && page[kCtxRingTail] == 0x40);
  CHECK(!lrc_set_ring(state, 0x00200000, 100, 0, 0) && !lrc_set_ring(state, 0x00200001, 4096, 0, 0) && !lrc_set_ring(state, 0x00200000, 4096, 4096, 0));
  CHECK(!lrc_set_ring(state, 0x00200000, 4096, 0, 4) && !lrc_set_ring(state, 0x00200000, 4096, 0, 8192) && !lrc_set_tail(state, 4));
  const uint64_t pds[4] = {0x1'1111'1000ull, 0x2000, 0x3000, 0x4000};
  CHECK(lrc_set_page_directories(state, pds) && page[kCtxPdp0Udw] == 1 && page[kCtxPdp0Ldw] == 0x11111000 && page[kCtxPdp3Ldw] == 0x4000 && page[kCtxPdp1Udw] == 0);
  CHECK(lrc_descriptor(0x123000, 5) == 0x0000000500123109ull);
  CHECK(lrc_descriptor(0x123FFF, 0) == 0x123109ull);  // the page offset bits are not part of the address
}

// A pretend engine: records every write and, when the context is started through the ELSP, answers with a "started" and a "completed, idle" status entry.
struct FakeEngine final : IRegisterBank {
  std::vector<std::pair<uint64_t, uint32_t>> writes;
  std::map<uint64_t, uint32_t> regs;
  std::vector<uint32_t>* hwsp = nullptr;
  uint32_t base = 0;
  uint32_t write_pointer = ExeclistPort::kCsbEntries - 1;
  int elsp_writes = 0;
  bool answer = true;
  uint32_t read32(uint64_t offset) override { return regs[offset]; }
  void write32(uint64_t offset, uint32_t value) override {
    writes.push_back({offset, value});
    regs[offset] = value;
    if (offset == base + ExeclistPort::kRingContextStatusPtr) write_pointer = ExeclistPort::kCsbEntries - 1;
    if (offset == base + ExeclistPort::kRingElsp && ++elsp_writes % 4 == 0 && answer) {
      for (uint32_t status : {ExeclistPort::kStatusIdleToActive, ExeclistPort::kStatusComplete | ExeclistPort::kStatusActiveToIdle}) {
        write_pointer = (write_pointer + 1) % ExeclistPort::kCsbEntries;
        (*hwsp)[ExeclistPort::kCsbFirstDword + write_pointer * 2] = status;
        (*hwsp)[ExeclistPort::kCsbFirstDword + write_pointer * 2 + 1] = 7;
        (*hwsp)[ExeclistPort::kCsbWriteDword] = 0x00000100 | write_pointer | 0x20;  // other bits of the word must be ignored
      }
    }
  }
};

static void test_execlist() {
  std::vector<uint32_t> hwsp(ExeclistPort::kStatusPageDwords, 0);
  FakeEngine engine;
  engine.hwsp = &hwsp;
  engine.base = kVcs0MmioBase;
  const uint32_t b = kVcs0MmioBase;
  ExeclistPort port(engine, b, ArrayView<uint32_t>(hwsp.data(), hwsp.size()), 0x00300000);
  CHECK(engine.writes.empty());  // nothing touched before enable()
  CHECK(port.enable());
  const std::vector<std::pair<uint64_t, uint32_t>> expect_enable = {
      {b + 0x3a0, 0xFFFF0505}, {b + 0x98, 0xFFFFFFFF}, {b + 0x29c, 0x80008000}, {b + 0x9c, 0x01000000}, {b + 0x80, 0x00300000}};
  // The status pointer is written twice (before and after the buffer is prepared); the rest once, in this order.
  CHECK(engine.writes.size() == 6);
  CHECK(engine.writes[0] == expect_enable[0] && engine.writes[1] == expect_enable[0]);
  for (size_t i = 1; i < expect_enable.size(); ++i) CHECK(engine.writes[i + 1] == expect_enable[i]);
  CHECK(hwsp[ExeclistPort::kCsbWriteDword] == 5);
  for (uint32_t i = 0; i < ExeclistPort::kCsbEntries * 2; ++i) CHECK(hwsp[ExeclistPort::kCsbFirstDword + i] == 0xFFFFFFFFu);
  ExeclistPort::Events ev = port.poll();
  CHECK(ev.started == 0 && ev.completed == 0);  // nothing yet: the write pointer is still at its reset value

  // A refusal: a status page of the wrong size, or at an unaligned address.
  FakeEngine other;
  other.hwsp = &hwsp;
  std::vector<uint32_t> small(16);
  CHECK(!ExeclistPort(other, b, ArrayView<uint32_t>(small.data(), small.size()), 0x1000).enable());
  CHECK(!ExeclistPort(other, b, ArrayView<uint32_t>(hwsp.data(), hwsp.size()), 0x1004).enable() && other.writes.empty());

  // One submission: element 1 (empty) goes in before element 0, each upper half before its lower half.
  engine.writes.clear();
  const uint64_t desc = lrc_descriptor(0x00400000, 3);
  port.submit(desc);
  CHECK(engine.writes.size() == 4);
  const uint64_t elsp = b + 0x230;
  CHECK(engine.writes[0] == std::make_pair(elsp, 0u) && engine.writes[1] == std::make_pair(elsp, 0u));
  CHECK(engine.writes[2] == std::make_pair(elsp, static_cast<uint32_t>(desc >> 32)) && engine.writes[3] == std::make_pair(elsp, static_cast<uint32_t>(desc)));
  ev = port.poll();
  CHECK(ev.started == 1 && ev.completed == 1 && ev.idle == 1 && (ev.status_or & ExeclistPort::kStatusComplete));
  ev = port.poll();
  CHECK(ev.started == 0 && ev.completed == 0);  // consumed

  // Many submissions wrap the six-entry buffer.
  for (int i = 0; i < 7; ++i) {
    port.submit(desc);
    ev = port.poll();
    CHECK(ev.started == 1 && ev.completed == 1 && ev.idle == 1);
  }
  // No answer: nothing is reported. An impossible write pointer (6, 7) is not believed.
  engine.answer = false;
  port.submit(desc);
  CHECK(port.poll().started == 0);
  hwsp[ExeclistPort::kCsbWriteDword] = 7;
  CHECK(port.poll().started == 0);
}

int main() {
  test_entries_and_geometry();
  test_ggtt_writer();
  test_unused_range();
  test_ppgtt();
  test_lrc();
  test_execlist();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("intel gt tests passed\n");
  return 0;
}
