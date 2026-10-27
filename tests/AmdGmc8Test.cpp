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


// Host-side tests for the Vega M memory controller and GART set-up (AmdGmc8) against a simulated memory controller: where the GART goes in the GPU's address space, the
// register values of the start (worked out by hand from the bit positions in AMD's headers), the order of the writes, the page table entries that map() writes and what the
// simulated hardware does with them (including a TLB that is stale until it is invalidated), and every refused argument.
#include <stdio.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.hpp"
#include "FakeAmdGmc8.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static AmdGmc8MemoryMap make_map(uint64_t base, uint64_t bytes) {
  AmdGmc8MemoryMap map;
  map.read = true;
  map.video_memory_bytes = bytes;
  map.gpu_base = base;
  map.gpu_top = (base + bytes + (16ull << 20) - 1) / (16ull << 20) * (16ull << 20);
  return map;
}
constexpr uint64_t kMiB = 1ull << 20, kGiB = 1ull << 30;

static void test_plan() {
  // The Vega M: 4 GiB at the bottom of the address space; the GART goes to the top (best fit: nothing before video memory), 256 MiB aligned to 4 GiB.
  AmdGmc8GartPlan p = amd_plan_gmc8_gart(make_map(0, 4 * kGiB));
  CHECK(p.ok && p.vram_start == 0 && p.vram_end == 4 * kGiB - 1);
  CHECK(p.gart_size == 256 * kMiB && p.gart_start == 0xFF00000000ull && p.gart_end == 0xFF0FFFFFFFull && p.table_bytes == 65536 * 8);
  // Video memory at the very top of the address space: nothing after it, so the GART goes to 0.
  p = amd_plan_gmc8_gart(make_map(0xFF80000000ull, 2 * kGiB));
  CHECK(p.ok && p.gart_start == 0 && p.gart_size == 256 * kMiB);
  // Room before (4 GiB) and a lot more after: best fit takes the smaller hole that is big enough, at 0.
  p = amd_plan_gmc8_gart(make_map(4 * kGiB, 4 * kGiB));
  CHECK(p.ok && p.gart_start == 0);
  // Video memory high in the space with a lot of room before it and 60 GiB after: the top.
  p = amd_plan_gmc8_gart(make_map(0xF000000000ull, 4 * kGiB));
  CHECK(p.ok && p.gart_start == 0xFF00000000ull);
  // Video memory that ends mid-4 GiB: the "after" space starts at the next 4 GiB boundary.
  p = amd_plan_gmc8_gart(make_map(0xFC00000000ull, 2 * kGiB));   // ends at 0xFC7FFFFFFF; the next boundary is 0xFD00000000: 12 GiB left after, 0xFC00000000 before
  CHECK(p.ok && p.gart_start == 0xFF00000000ull);
  // A GART that does not fit anywhere is reduced to what does; one that fits nowhere is refused.
  p = amd_plan_gmc8_gart(make_map(64 * kMiB, 0x10000000000ull - 64 * kMiB));   // 64 MiB before video memory, none after: the GART shrinks to that, at 0
  CHECK(p.ok && p.gart_size == 64 * kMiB && p.gart_start == 0 && p.table_bytes == 16384 * 8);
  p = amd_plan_gmc8_gart(make_map(0, 0x10000000000ull));   // video memory fills the space: nothing left
  CHECK(!p.ok);
  p = amd_plan_gmc8_gart(make_map(0, 1 * kGiB), 1ull << 41);   // bigger than the whole space: cut to what is free after video memory (from the next 4 GiB boundary)
  CHECK(p.ok && p.gart_size == 0x10000000000ull - 4 * kGiB && p.gart_start == 4 * kGiB && p.gart_end == 0xFFFFFFFFFFull);
  // Refused: nothing read, a size that is not whole pages, nothing to size it by, memory beyond the 40-bit space.
  AmdGmc8MemoryMap unread = make_map(0, 4 * kGiB);
  unread.read = false;
  CHECK(!amd_plan_gmc8_gart(unread).ok);
  CHECK(!amd_plan_gmc8_gart(make_map(0, 4 * kGiB), 256 * kMiB + 1).ok && !amd_plan_gmc8_gart(make_map(0, 4 * kGiB), 0).ok);
  CHECK(!amd_plan_gmc8_gart(make_map(0, 0)).ok);
  CHECK(!amd_plan_gmc8_gart(make_map(0xFFFFFFF000ull, 1 * kGiB)).ok && !amd_plan_gmc8_gart(make_map(0x10000000000ull, 1 * kMiB)).ok);
}

struct Block {
  std::vector<uint8_t> storage;
  uint8_t* base = nullptr;
  explicit Block(size_t bytes) : storage(bytes + 4096) { base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage.data()) + 4095) & ~uintptr_t{4095}); }
};

struct Rig {
  fakegmc::Card card;
  fakegmc::FakeClock clock;
  Block table, data;
  AmdGmc8GartPlan plan;
  uint64_t table_gpu = 0x100000000ull, data_gpu = 0x120000000ull;
  Rig() : table(512 * 1024), data(64 * 4096) {
    plan = amd_plan_gmc8_gart(make_map(0, 4 * kGiB));
    card.windows.push_back({table_gpu, table.base, 512 * 1024});
    card.windows.push_back({data_gpu, data.base, 64 * 4096});
  }
  AmdGmc8::Memory memory() {
    AmdGmc8::Memory m;
    m.table_gpu_address = table_gpu;
    m.table = MmioRegion(reinterpret_cast<uint64_t>(table.base), 512 * 1024);
    m.dummy_page_gpu_address = 0x7000;
    m.scratch_page_gpu_address = 0x9000;
    return m;
  }
};

static void test_start() {
  Rig rig;
  for (int i = 0; i < 512 * 1024; ++i) rig.table.base[i] = 0xAA;   // stale contents: the start clears the table
  const uint32_t prt[] = {0x52C, 0x52D, 0x52E, 0x52F};
  for (uint32_t r : prt) rig.card.set_reg(r, 0xA5A5A5A5);
  rig.card.set_reg(fakegmc::kHdpMisc, 0x5);
  rig.card.set_reg(fakegmc::kHdpHostPath, 0x1234);
  AmdGmc8 gmc(rig.card, rig.clock);
  CHECK(gmc.start(rig.plan, rig.memory()) == AmdGmc8::Result::Ok);
  const fakegmc::Card& c = rig.card;
  // Golden registers: the owned 28 bits set, the others left.
  for (uint32_t r : prt) CHECK(c.reg(r) == 0xAFFFFFFF);
  // mc_program
  CHECK(c.reg(fakegmc::kApertureLow) == 0 && c.reg(fakegmc::kApertureHigh) == 0xFFFFF && c.reg(fakegmc::kApertureDefault) == 0x9);
  CHECK(c.reg(fakegmc::kAgpBase) == 0 && c.reg(fakegmc::kAgpTop) == 0 && c.reg(fakegmc::kAgpBot) == 0x3FFFFFF);
  CHECK(c.reg(fakegmc::kBifFbEn) == 3 && c.reg(fakegmc::kHdpMisc) == 0x4 && c.reg(fakegmc::kHdpHostPath) == 0x1234 && c.reg(fakegmc::kHdpCoherency) == 0);
  // The 160 writes that zero the HDP registers 0xb05 + 6 * i + (0..4).
  size_t hdp_writes = 0;
  for (const auto& w : c.writes)
    if (w.first >= 0xb05 && w.first <= 0xb05 + 31 * 6 + 4 && (w.first - 0xb05) % 6 <= 4 && w.second == 0) ++hdp_writes;
  CHECK(hdp_writes == 160);
  // gart_enable: the values worked out from the field positions.
  CHECK(c.reg(fakegmc::kL1TlbCntl) == 0x5b && c.reg(fakegmc::kL2Cntl) == 0xb8e03 && c.reg(fakegmc::kL2Cntl2) == 0x3 && c.reg(fakegmc::kL2Cntl3) == 0x148009 && c.reg(fakegmc::kL2Cntl4) == 0);
  CHECK(c.reg(fakegmc::kCtx0Start) == 0xFF00000 && c.reg(fakegmc::kCtx0End) == 0xFF0FFFF && c.reg(fakegmc::kCtx0Base) == 0x100000 && c.reg(fakegmc::kCtx0Fault) == 0x7);
  CHECK(c.reg(fakegmc::kContext0Cntl) == 0x11 && c.reg(fakegmc::kContext1Cntl) == 0x492493);
  CHECK(c.reg(0x50c) == 0 && c.reg(0x50d) == 4 && c.reg(0x547) == 0x7);   // CONTEXT0_CNTL2, CONTEXT1_CNTL2, CONTEXT1 fault default address
  CHECK(c.reg(0x558) == 0 && c.reg(0x560) == (64u << 18) - 1);          // context 1: start, end (64 GiB of 4 KiB pages)
  for (uint32_t i = 1; i < 8; ++i) CHECK(c.reg(0x54F + i) == 0x100000);   // contexts 1..7 point at the same table
  for (uint32_t i = 0; i < 8; ++i) CHECK(c.reg(0x50E + i) == 0x100000);   // contexts 8..15
  CHECK(c.reg(0x575) == 0 && c.reg(0x576) == 0 && c.reg(0x577) == 0);
  // The table was cleared, and the TLBs were invalidated at the end.
  bool clear = true;
  for (uint64_t o = 0; o < 512 * 1024; o += 8) clear = clear && rig.card.get64(rig.table_gpu + o) == 0;
  CHECK(clear && c.invalidations == 1 && c.writes.back() == std::make_pair(fakegmc::kInvalidate, uint32_t{1}));
  // The order: golden (4 writes), the HDP loop, the aperture, the AGP window, BIF, then the VM registers.
  const auto& w = c.writes;
  auto index_of = [&](uint32_t reg) { for (size_t i = 0; i < w.size(); ++i) if (w[i].first == reg) return i; return w.size(); };
  CHECK(w.size() > 4 && w[0] == std::make_pair(fakegmc::kHdpFlush, uint32_t{1}) && w[1].first == 0x52C && w[4].first == 0x52F && w[5].first == 0xb05);
  const size_t coherency = index_of(fakegmc::kHdpCoherency);
  CHECK(coherency == 1 + 4 + 160 && index_of(fakegmc::kApertureLow) == coherency + 1 && index_of(fakegmc::kApertureHigh) == coherency + 2 && index_of(fakegmc::kApertureDefault) == coherency + 3);
  CHECK(index_of(fakegmc::kAgpBase) == coherency + 4 && index_of(fakegmc::kAgpTop) == coherency + 5 && index_of(fakegmc::kAgpBot) == coherency + 6 && index_of(fakegmc::kBifFbEn) == coherency + 7);
  CHECK(index_of(fakegmc::kHdpMisc) == coherency + 8 && index_of(fakegmc::kHdpHostPath) == coherency + 9 && index_of(fakegmc::kL1TlbCntl) == coherency + 10);
  CHECK(index_of(fakegmc::kL1TlbCntl) < index_of(fakegmc::kL2Cntl) && index_of(fakegmc::kL2Cntl) < index_of(fakegmc::kL2Cntl2) && index_of(fakegmc::kL2Cntl2) < index_of(fakegmc::kL2Cntl3) &&
        index_of(fakegmc::kL2Cntl3) < index_of(fakegmc::kL2Cntl4) && index_of(fakegmc::kL2Cntl4) < index_of(fakegmc::kCtx0Start) && index_of(fakegmc::kCtx0Start) < index_of(fakegmc::kCtx0End) &&
        index_of(fakegmc::kCtx0End) < index_of(fakegmc::kCtx0Base) && index_of(fakegmc::kCtx0Base) < index_of(fakegmc::kContext0Cntl) && index_of(fakegmc::kContext0Cntl) < index_of(fakegmc::kContext1Cntl));
  // A card that is still busy is waited for; one that never goes idle is reported, after the set-up is done.
  Rig busy;
  busy.card.busy_reads = 5;
  AmdGmc8 gmc2(busy.card, busy.clock);
  CHECK(gmc2.start(busy.plan, busy.memory()) == AmdGmc8::Result::Ok);
  Rig stuck;
  stuck.card.busy_reads = 1u << 30;
  AmdGmc8 gmc3(stuck.card, stuck.clock);
  CHECK(gmc3.start(stuck.plan, stuck.memory(), 2000) == AmdGmc8::Result::NotIdle && stuck.card.reg(fakegmc::kContext0Cntl) == 0x11);
  Rig late;   // idle at first, busy at the second wait
  late.card.busy_after_agp = true;
  AmdGmc8 gmc4(late.card, late.clock);
  CHECK(gmc4.start(late.plan, late.memory(), 2000) == AmdGmc8::Result::NotIdle);
}

static void test_map() {
  Rig rig;
  AmdGmc8 gmc(rig.card, rig.clock);
  CHECK(gmc.start(rig.plan, rig.memory()) == AmdGmc8::Result::Ok);
  const uint64_t gart = rig.plan.gart_start;
  CHECK(gmc.gart_address(0) == gart && gmc.gart_address(3) == gart + 3 * 4096);
  // Nothing mapped yet: the hardware sends GART addresses to the dummy page.
  CHECK(!rig.card.translate(gart).ok);
  // Three pages of system memory at 0x1234000, 0x56789000 and 0xABCDEF000, then a page of video memory.
  const uint64_t system_pages[] = {0x1234000, 0x56789000, 0xABCDEF000ull};
  CHECK(gmc.map(10, ArrayView<const uint64_t>(system_pages, 3), kAmdGmc8PteSystemPage));
  CHECK(rig.card.get64(rig.table_gpu + 10 * 8) == (0x1234000 | 0x77) && rig.card.get64(rig.table_gpu + 11 * 8) == (0x56789000 | 0x77) && rig.card.get64(rig.table_gpu + 12 * 8) == (0xABCDEF000ull | 0x77));
  for (int i = 0; i < 3; ++i) {
    const fakegmc::Card::Translation t = rig.card.translate(gart + (10 + i) * 4096 + 0x123);
    CHECK(t.ok && t.system && t.address == system_pages[i] + 0x123);
  }
  const uint64_t video[] = {0x40000000};
  CHECK(gmc.map(13, ArrayView<const uint64_t>(video, 1), kAmdGmc8PteVideoPage));
  CHECK(rig.card.get64(rig.table_gpu + 13 * 8) == (0x40000000 | 0x71));   // valid | exec | read | write, no system, no snoop
  const fakegmc::Card::Translation tv = rig.card.translate(gart + 13 * 4096);
  CHECK(tv.ok && !tv.system && tv.address == 0x40000000);
  // The neighbours stay invalid.
  CHECK(!rig.card.translate(gart + 9 * 4096).ok && !rig.card.translate(gart + 14 * 4096).ok);
  // The TLB: map() flushes the host data path and invalidates, so a remap is seen at once.
  const uint32_t flushes = rig.card.hdp_flushes, inval = rig.card.invalidations;
  const uint64_t other[] = {0x2000000};
  CHECK(gmc.map(10, ArrayView<const uint64_t>(other, 1), kAmdGmc8PteSystemPage));
  CHECK(rig.card.hdp_flushes == flushes + 1 && rig.card.invalidations == inval + 1);
  CHECK(rig.card.translate(gart + 10 * 4096).address == 0x2000000);
  // Unmapping makes the pages invalid and invalidates the TLB again.
  CHECK(gmc.unmap(10, 4));
  CHECK(!rig.card.translate(gart + 10 * 4096).ok && !rig.card.translate(gart + 13 * 4096).ok && rig.card.invalidations == inval + 2);
  // The last page of the GART.
  const uint64_t last = rig.plan.gart_size / 4096 - 1;
  CHECK(gmc.map(last, ArrayView<const uint64_t>(other, 1), kAmdGmc8PteSystemPage) && rig.card.translate(rig.plan.gart_end).ok);
  // Refused, with nothing written.
  const size_t written = rig.card.writes.size();
  CHECK(!gmc.map(last, ArrayView<const uint64_t>(system_pages, 2), kAmdGmc8PteSystemPage));   // runs off the end
  CHECK(!gmc.map(last + 1, ArrayView<const uint64_t>(other, 1), kAmdGmc8PteSystemPage) && !gmc.map(0xFFFFFFFFFFFFFFFFull, ArrayView<const uint64_t>(other, 1), kAmdGmc8PteSystemPage));
  const uint64_t unaligned[] = {0x1000, 0x2008};
  CHECK(!gmc.map(0, ArrayView<const uint64_t>(unaligned, 2), kAmdGmc8PteSystemPage));
  const uint64_t beyond[] = {0x10000000000ull};
  CHECK(!gmc.map(0, ArrayView<const uint64_t>(beyond, 1), kAmdGmc8PteSystemPage));
  CHECK(!gmc.map(0, ArrayView<const uint64_t>(other, 1), kAmdGmc8PteSystemPage & ~kAmdGmc8PteValid) && !gmc.map(0, ArrayView<const uint64_t>(other, 1), kAmdGmc8PteSystemPage | 0x1000));
  CHECK(!gmc.unmap(last, 2) && !gmc.unmap(last + 1, 0));
  CHECK(rig.card.writes.size() == written && rig.card.get64(rig.table_gpu) == 0);
  // Stopping turns the contexts, the L1 TLB and the L2 cache off.
  gmc.stop();
  CHECK(rig.card.reg(fakegmc::kContext0Cntl) == 0 && rig.card.reg(fakegmc::kContext1Cntl) == 0 && rig.card.reg(fakegmc::kL1TlbCntl) == 0x18 && rig.card.reg(fakegmc::kL2Cntl) == 0xb8e02 && rig.card.reg(fakegmc::kL2Cntl2) == 0);
  CHECK(!gmc.map(0, ArrayView<const uint64_t>(other, 1), kAmdGmc8PteSystemPage));   // not started
}

static void test_refusals() {
  Rig rig;
  AmdGmc8 gmc(rig.card, rig.clock);
  AmdGmc8::Memory m = rig.memory();
  AmdGmc8GartPlan bad = rig.plan;
  bad.ok = false;
  CHECK(gmc.start(bad, m) == AmdGmc8::Result::BadArgument);
  AmdGmc8::Memory small = m;
  small.table = MmioRegion(m.table.address(), rig.plan.table_bytes - 8);
  CHECK(gmc.start(rig.plan, small) == AmdGmc8::Result::BadArgument);
  AmdGmc8::Memory skew = m;
  skew.table_gpu_address += 8;
  CHECK(gmc.start(rig.plan, skew) == AmdGmc8::Result::BadArgument);
  skew = m;
  skew.dummy_page_gpu_address += 8;
  CHECK(gmc.start(rig.plan, skew) == AmdGmc8::Result::BadArgument);
  skew = m;
  skew.scratch_page_gpu_address = 0x10000000000ull;
  CHECK(gmc.start(rig.plan, skew) == AmdGmc8::Result::BadArgument);
  skew = m;
  skew.table_gpu_address = 0x10000000000ull;
  CHECK(gmc.start(rig.plan, skew) == AmdGmc8::Result::BadArgument);
  CHECK(gmc.start(rig.plan, AmdGmc8::Memory()) == AmdGmc8::Result::BadArgument);
  AmdGmc8::Config config;
  config.block_size = 8;
  AmdGmc8 bad_config(rig.card, rig.clock, config);
  CHECK(bad_config.start(rig.plan, m) == AmdGmc8::Result::BadArgument);
  CHECK(rig.card.writes.empty());
  // A block size above 9 shows in the page table block size field of context 1; a bigger fragment size in the L2 cache.
  AmdGmc8::Config custom;
  custom.block_size = 10;
  custom.fragment_size = 4;
  custom.vm_size_gb = 128;
  AmdGmc8 tuned(rig.card, rig.clock, custom);
  CHECK(tuned.start(rig.plan, m) == AmdGmc8::Result::Ok);
  CHECK(rig.card.reg(fakegmc::kContext1Cntl) == 0x1492493 && rig.card.reg(0x560) == (128u << 18) - 1 && rig.card.reg(fakegmc::kL2Cntl3) == 0x120004);
}

// gmc_v8_0_vm_fault_interrupt_state: the seven fault interrupt enables of both contexts' control registers, nothing else changed.
static void test_fault_interrupts() {
  Rig rig;
  AmdGmc8 gmc(rig.card, rig.clock);
  CHECK(!gmc.set_fault_interrupts(true));   // (not started)
  CHECK(gmc.start(rig.plan, rig.memory()) == AmdGmc8::Result::Ok);
  const uint32_t ctx0 = rig.card.reg(fakegmc::kContext0Cntl), ctx1 = rig.card.reg(fakegmc::kContext1Cntl);
  // RANGE 0x8, DUMMY_PAGE 0x40, PDE0 0x200, VALID 0x1000, READ 0x8000, WRITE 0x40000, EXECUTE 0x200000
  const uint32_t bits = 0x8 | 0x40 | 0x200 | 0x1000 | 0x8000 | 0x40000 | 0x200000;
  CHECK(bits == 0x249248);
  const size_t before = rig.card.writes.size();
  CHECK(gmc.set_fault_interrupts(true));
  CHECK(rig.card.reg(fakegmc::kContext0Cntl) == (ctx0 | bits) && rig.card.reg(fakegmc::kContext1Cntl) == (ctx1 | bits));
  CHECK(rig.card.writes.size() == before + 2 && rig.card.writes[before].first == fakegmc::kContext0Cntl && rig.card.writes[before + 1].first == fakegmc::kContext1Cntl);
  CHECK(gmc.set_fault_interrupts(false));
  CHECK(rig.card.reg(fakegmc::kContext0Cntl) == (ctx0 & ~bits) && rig.card.reg(fakegmc::kContext1Cntl) == (ctx1 & ~bits));
  // Other bits survive, whatever the register held.
  rig.card.set_reg(fakegmc::kContext0Cntl, 0xFFFFFFFF);
  CHECK(gmc.set_fault_interrupts(false) && rig.card.reg(fakegmc::kContext0Cntl) == (0xFFFFFFFFu & ~bits));
  gmc.stop();
  CHECK(!gmc.set_fault_interrupts(true));
}

int main() {
  test_plan();
  test_start();
  test_map();
  test_refusals();
  test_fault_interrupts();
  printf(g_failed ? "AmdGmc8Test: %d FAILED\n" : "AmdGmc8Test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
