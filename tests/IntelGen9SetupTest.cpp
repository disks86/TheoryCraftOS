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

// Host-side tests for the Gen9.5 setup (MOCS tables and workarounds), the shared forcewake, and the render driver applying them at start-up.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGen9Setup.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"

using namespace hardware;
using namespace hardware::intel_gt;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"

// A bank of plain registers.
struct PlainBank final : IRegisterBank {
  std::map<uint64_t, uint32_t> regs;
  std::vector<std::pair<uint64_t, uint32_t>> writes;
  uint32_t read32(uint64_t offset) override { auto it = regs.find(offset); return it == regs.end() ? 0 : it->second; }
  void write32(uint64_t offset, uint32_t value) override { regs[offset] = value; writes.push_back({offset, value}); }
};

static void test_mocs() {
  using namespace gen9;
  CHECK(mocs_control(kMocsUncached) == 0x09 && mocs_control(kMocsPageTable) == 0x30 && mocs_control(kMocsCached) == 0x3B && mocs_control(kMocsL3Eviction) == 0x37);
  CHECK(mocs_control(3) == 0x30 && mocs_control(40) == 0x30 && mocs_control(62) == 0x30);   // undefined entries follow the page table entry
  CHECK(mocs_l3cc(kMocsUncached) == 0x10 && mocs_l3cc(kMocsPageTable) == 0x30 && mocs_l3cc(kMocsCached) == 0x30 && mocs_l3cc(kMocsL3Eviction) == 0x10 && mocs_l3cc(9) == 0x30);
  PlainBank bank;
  program_mocs(bank, kRenderMocsBase);
  CHECK(bank.writes.size() == 64 && bank.regs[0xC800] == 0x09 && bank.regs[0xC804] == 0x30 && bank.regs[0xC808] == 0x3B && bank.regs[0xC800 + 63 * 4] == 0x37 && !bank.regs.count(0xC900));
  bank.writes.clear();
  program_l3cc(bank);
  CHECK(bank.writes.size() == 32 && bank.regs[0xB020] == 0x00300010 && bank.regs[0xB024] == 0x00300030 && bank.regs[0xB020 + 31 * 4] == 0x00100030);
}

static void test_workarounds() {
  using namespace gen9;
  CHECK(platform_for_device(0x591B) == Platform::KabyLake && platform_for_device(0x5900) == Platform::KabyLake && platform_for_device(0x593F) == Platform::KabyLake);
  CHECK(platform_for_device(0x3E92) == Platform::CoffeeLake && platform_for_device(0x9BC8) == Platform::CoffeeLake && platform_for_device(0x5940) == Platform::CoffeeLake);

  // GT workarounds set bits and keep the others.
  PlainBank kbl;
  kbl.regs[0x4090] = 0x1;
  apply_gt_workarounds(kbl, identity_for(0x591B, 4));
  CHECK(kbl.regs[0x4090] == (0x1u | (1u << 8) | (1u << 25)) && kbl.regs[0x4DDC] == ((1u << 31) | (1u << 27)) && kbl.regs[0x940C] == (1u << 14) && kbl.regs[0x4AB0] == (1u << 18));
  CHECK(kbl.regs.count(0x4AB8) == 0);                      // (stepping C0 or later: no dynamic credit sharing workaround)
  PlainBank early;
  apply_gt_workarounds(early, identity_for(0x591B, 1));
  CHECK(early.regs[0x4AB8] == (1u << 28));
  PlainBank cfl;
  apply_gt_workarounds(cfl, identity_for(0x3E92, 0));
  CHECK(cfl.regs[0x4090] == (1u << 25) && cfl.regs.count(0x4AB8) == 0 && cfl.regs[0x940C] == (1u << 14));

  // The context workarounds: masked writes, in i915's order.
  MaskedWrite list[32];
  const uint32_t n = render_workaround_list(identity_for(0x591B, 4), list, 32);
  CHECK(n == 14);
  CHECK(list[0].reg == 0x7014 && list[0].value == masked_enable(1u << 13));
  bool found_hdc = false, found_preempt = false, found_chicken5 = false;
  for (uint32_t i = 0; i < n; ++i) {
    found_hdc = found_hdc || (list[i].reg == 0x7300 && list[i].value == 0x80208020u);
    found_preempt = found_preempt || (list[i].reg == 0x2580 && list[i].value == 0x00060004u);
    found_chicken5 = found_chicken5 || (list[i].reg == 0xE188 && list[i].value == 0x00080000u);   // a masked clear: only the mask is set
  }
  CHECK(found_hdc && found_preempt && found_chicken5);
  CHECK(render_workaround_list(identity_for(0x591B, 1), list, 32) == 13);   // Kaby Lake before C0 has no push constant fix
  CHECK(render_workaround_list(identity_for(0x3E92, 0), list, 32) == 14);   // Coffee Lake always does
  CHECK(render_workaround_list(identity_for(0x591B, 4), list, 4) == 4 && render_workaround_list(identity_for(0x591B, 4), nullptr, 0) == 0);
  // Every masked register's value has its bits inside its mask (a write that sets a bit it does not unmask would do nothing).
  for (uint32_t i = 0; i < n; ++i) CHECK((list[i].value & 0xFFFF & ~(list[i].value >> 16)) == 0);
  PlainBank bank;
  apply_render_workarounds(bank, identity_for(0x591B, 4));
  CHECK(bank.writes.size() == n);
  for (uint32_t i = 0; i < n && i < bank.writes.size(); ++i) CHECK(bank.writes[i].first == list[i].reg && bank.writes[i].second == list[i].value);
}

// --- Shared power wells -----------------------------------------------------------------------------------------------------------------------

struct WellBank final : IRegisterBank {
  ForcewakeState* shared;
  std::atomic<int> enables{0}, disables{0};
  std::atomic<uint32_t> ack{0};
  bool grant = true;
  explicit WellBank(ForcewakeState* s) : shared(s) {}
  ForcewakeState& forcewake_state() override { return *shared; }
  std::atomic<uint32_t> gt_ack{0};
  uint32_t read32(uint64_t offset) override {
    if (offset == Forcewake::ack_register(ForcewakeDomain::Render)) return ack.load();
    return offset == Forcewake::ack_register(ForcewakeDomain::Gt) ? gt_ack.load() : 0;
  }
  void write32(uint64_t offset, uint32_t value) override {
    const bool on = (value & 1) && (value >> 16);
    if (offset == Forcewake::request_register(ForcewakeDomain::Gt)) { gt_ack = on ? 1 : 0; return; }
    if (offset != Forcewake::request_register(ForcewakeDomain::Render)) return;
    if (on) { ++enables; if (grant) ack = 1; }
    else { ++disables; ack = 0; }
  }
};

static void test_shared_forcewake() {
  ForcewakeState state;
  WellBank a(&state), b(&state);
  // Two users of one GPU: the well is woken once and let go once.
  CHECK(Forcewake::acquire(a, ForcewakeDomain::Render) && Forcewake::acquire(b, ForcewakeDomain::Render));
  CHECK(a.enables + b.enables == 1 && state.counts[1] == 2);
  Forcewake::release(a, ForcewakeDomain::Render);
  CHECK(a.disables + b.disables == 0);
  Forcewake::release(b, ForcewakeDomain::Render);
  CHECK(a.disables + b.disables == 1 && state.counts[1] == 0);
  Forcewake::release(a, ForcewakeDomain::Render);   // (not held: nothing happens)
  CHECK(a.disables + b.disables == 1 && state.counts[1] == 0);
  // A well that does not wake is withdrawn and counts for nothing.
  WellBank dead(&state);
  dead.grant = false;
  CHECK(!Forcewake::acquire(dead, ForcewakeDomain::Render, 10) && state.counts[1] == 0 && dead.disables == 1);
  // Other wells are counted apart.
  CHECK(Forcewake::acquire(a, ForcewakeDomain::Gt) && state.counts[0] == 1 && state.counts[1] == 0);
  Forcewake::release(a, ForcewakeDomain::Gt);
  // Banks with their own state do not share.
  ForcewakeState other;
  WellBank c(&other);
  CHECK(Forcewake::acquire(a, ForcewakeDomain::Render) && Forcewake::acquire(c, ForcewakeDomain::Render));
  CHECK(a.enables == 2 && c.enables == 1);
  Forcewake::release(a, ForcewakeDomain::Render);
  Forcewake::release(c, ForcewakeDomain::Render);
  // Windows onto the same registers are one GPU: banks over the same address share, others do not.
  uint32_t window_a[4] = {}, window_b[4] = {};
  MmioRegisterBank x(MmioRegion(reinterpret_cast<uint64_t>(window_a), sizeof window_a)), y(MmioRegisterBank(MmioRegion(reinterpret_cast<uint64_t>(window_a), sizeof window_a)));
  MmioRegisterBank z(MmioRegion(reinterpret_cast<uint64_t>(window_b), sizeof window_b));
  CHECK(&x.forcewake_state() == &y.forcewake_state() && &x.forcewake_state() != &z.forcewake_state());
  // Many threads taking and giving back the same well: the well is awake whenever anyone holds it, and the count ends at zero.
  ForcewakeState busy;
  WellBank bank(&busy);
  std::atomic<int> violations{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&] {
      for (int i = 0; i < 2000; ++i) {
        if (!Forcewake::acquire(bank, ForcewakeDomain::Render)) { ++violations; continue; }
        if (bank.ack.load() != 1) ++violations;
        Forcewake::release(bank, ForcewakeDomain::Render);
      }
    });
  for (std::thread& th : threads) th.join();
  CHECK(violations == 0 && busy.counts[1] == 0 && bank.enables == bank.disables);
}

// --- The driver applies it all -------------------------------------------------------------------------------------------------------------------

static void test_driver_start() {
  Rig rig(Generation::Gen9, VcsDriver::Engine::Render);
  VcsDriver::Config cfg = rig.config();
  cfg.device_id = 0x591B;
  cfg.revision = 4;
  VcsDriver drv(rig.mem);
  CHECK(drv.initialize(cfg));
  FakeGpu& g = rig.gpu;
  CHECK(g.regs[0xC800] == 0x09 && g.regs[0xC804] == 0x30 && g.regs[0xB020] == 0x00300010);
  CHECK(g.regs[0x4090] == ((1u << 8) | (1u << 25)) && g.regs[0x7300] == 0x00100010u && g.regs[0x2580] == 0x00060004u && g.regs[0xE100] == 0x00100010u);
  CHECK(!g.regs.count(0xC900));   // (the video engine's table is not touched)
  drv.shutdown();
  // The video engine driver leaves all of this alone.
  Rig video;
  VcsDriver vdrv(video.mem);
  CHECK(vdrv.initialize(video.config()));
  CHECK(!video.gpu.regs.count(0xC800) && !video.gpu.regs.count(0x4090) && !video.gpu.regs.count(0x7300));
  vdrv.shutdown();
}

int main() {
  test_mocs();
  test_workarounds();
  test_shared_forcewake();
  test_driver_start();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("intel gen9 setup tests passed\n");
  return 0;
}
