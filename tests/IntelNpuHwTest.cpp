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

// Host test for the Intel NPU hardware layer: a pretend NPU answers the register sequences of the Linux ivpu driver.
#include <stdio.h>
#include <functional>
#include <map>
#include <string>
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// A BAR whose registers are plain values plus a hook that reacts to writes the way the device does.
struct SimBar final : INpuRegisters {
  std::map<uint64_t, uint32_t> regs;
  std::map<uint64_t, uint64_t> writes64;
  std::function<void(SimBar&, uint64_t, uint32_t)> on_write;
  bool chaos = false;
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  uint32_t read32(uint64_t offset) override {
    if (chaos) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<uint32_t>(rng); }
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint64_t offset, uint32_t value) override {
    regs[offset] = value;
    if (on_write) on_write(*this, offset, value);
  }
  void write64(uint64_t offset, uint64_t value) override {
    writes64[offset] = value;
    regs[offset] = static_cast<uint32_t>(value);
    regs[offset + 4] = static_cast<uint32_t>(value >> 32);
  }
};

struct SimNpu {
  SimBar ip, btrs;
  IntelNpuPlatform platform;
  bool wp_stuck = false, island_stuck = false, host_axi_refused = false, top_noc_denied = false, no_clock_ack = false, cpu_noc_refused = false;
  explicit SimNpu(uint16_t device_id) : platform(intel_npu_platform(device_id)) {
    const bool mtl = platform.buttress == IntelNpuButtress::Mtl;
    // Fuses: min ratio 8, power-neutral ratio 0x10, max ratio 0x30; both tiles present.
    btrs.regs[mtl ? 0x24 : 0x174] = 0x1008;
    btrs.regs[mtl ? 0x28 : 0x178] = 0x30;
    if (!mtl) { btrs.regs[0x150] = 0x1; btrs.regs[0x154] = no_clock_ack ? 0 : (1u << 6); }
    ip.regs[0x108] = 0x00010203;
    ip.regs[0x10C] = 0x00000042;
    btrs.on_write = [this, mtl](SimBar& b, uint64_t off, uint32_t v) {
      const uint64_t cmd = mtl ? 0x14 : 0x13C;
      if (off == cmd && (v & 1)) {
        if (wp_stuck) return;
        b.regs[cmd] = v & ~1u;
        const uint32_t p1 = b.regs[mtl ? 0x0C : 0x134];
        const bool enable = (p1 & 0xFFFF) != 0 && (mtl || (b.regs[0x138] >> 16) != 0);
        const uint64_t status = mtl ? 0x44 : 0x154;
        b.regs[status] = (b.regs[status] & ~1u) | (enable ? 1u : 0u);
        if (mtl) b.regs[0x40] = enable ? 2u : 0u;
        else b.regs[0x14C] = b.regs[0x138] >> 16;
      }
      if (off == (mtl ? 0x60u : 0x164u)) b.regs[off] = v & ~1u;  // the D0i3 transition completes at once
    };
    ip.on_write = [this](SimBar& b, uint64_t off, uint32_t v) {
      if (off == 0x154 && !host_axi_refused) b.regs[0x158] = v;       // host NoC QREQN -> QACCEPTN
      if (off == 0x160 && !top_noc_denied) b.regs[0x164] = v;          // top NoC
      if (off == 0x160 && top_noc_denied) b.regs[0x168] = v;           // ... or QDENY
      if (off == 0x30024 && !island_stuck) b.regs[0x3002C] = v;        // power island enable -> status
      if (off == 0x01010030 && !cpu_noc_refused) b.regs[0x01010034] = v;
    };
  }
  IntelNpuHw hw() { return IntelNpuHw(ip, btrs, platform); }
};

static void test_platform() {
  CHECK(intel_npu_platform(0x7D1D).ip == IntelNpuIp::Ip37xx && intel_npu_platform(0x7D1D).buttress == IntelNpuButtress::Mtl);
  CHECK(intel_npu_platform(0xAD1D).ip == IntelNpuIp::Ip37xx);
  CHECK(intel_npu_platform(0x643E).ip == IntelNpuIp::Ip40xx && intel_npu_platform(0x643E).buttress == IntelNpuButtress::Lnl);
  CHECK(intel_npu_platform(0xB03E).ip == IntelNpuIp::Ip50xx && intel_npu_platform(0xFD3E).ip == IntelNpuIp::Ip50xx);
  CHECK(intel_npu_platform(0xD71D).ip == IntelNpuIp::Ip60xx);
  uint32_t known = 0;
  for (uint32_t id = 0; id <= 0xFFFF; ++id) known += intel_npu_platform(static_cast<uint16_t>(id)).valid();
  CHECK(known == 6);
  CHECK(IntelNpuHw::ip_window(IntelNpuIp::Ip37xx) > 0x06010040 && IntelNpuHw::ip_window(IntelNpuIp::Ip40xx) > 0x01010038 && IntelNpuHw::ip_window(IntelNpuIp::Unknown) == 0);
  CHECK(IntelNpuHw::ratio_to_mhz(IntelNpuButtress::Mtl, 0x30) == 1600 && IntelNpuHw::ratio_to_mhz(IntelNpuButtress::Lnl, 0x30) == 1200);
}

static void test_info() {
  SimNpu mtl(0x7D1D);
  IntelNpuHw h = mtl.hw();
  const IntelNpuInfo& i = h.read_info();
  CHECK(i.valid && i.min_ratio == 8 && i.pn_ratio == 0x10 && i.max_ratio == 0x30 && i.max_mhz == 1600 && i.hw_version == 0x00010203 && i.sw_version == 0);
  SimNpu lnl(0x643E);
  IntelNpuHw hl = lnl.hw();
  const IntelNpuInfo& l = hl.read_info();
  CHECK(l.valid && l.max_mhz == 1200 && l.pn_mhz == 400 && l.sw_version == 0x42 && l.tile_disable_mask == 0 && !l.ready);
  lnl.btrs.regs[0x150] = 0x1 | (0x2u << 1);  // one tile fused off
  CHECK(hl.read_info().valid && hl.info().tile_disable_mask == 0x2);
  lnl.btrs.regs[0x150] = 0;  // fuses not valid
  CHECK(!hl.read_info().valid);
  lnl.btrs.regs[0x150] = 1;
  lnl.btrs.regs[0x178] = 4;  // max below min
  CHECK(!hl.read_info().valid);
  lnl.btrs.regs[0x178] = 0xFFFFFFFF;  // device off the bus
  CHECK(!hl.read_info().valid);
  lnl.btrs.regs[0x178] = 0x30;
  lnl.btrs.regs[0x174] = 0x5008;  // pn above max is clamped
  CHECK(hl.read_info().valid && hl.info().pn_ratio == 0x30);
  IntelNpuPlatform none;
  SimBar a, b;
  IntelNpuHw nh(a, b, none);
  CHECK(!nh.read_info().valid && !nh.power_up().ok && !nh.boot(0x1000).ok);
}

static void test_power_up() {
  SimNpu mtl(0x7D1D);
  IntelNpuHw h = mtl.hw();
  CHECK(!h.power_up().ok && h.power_up().failed == IntelNpuStep::Info);  // fuses first
  h.read_info();
  IntelNpuResult r = h.power_up();
  CHECK(r.ok && !r.d0i3_warning);
  CHECK(mtl.btrs.regs[0x08] == ((0x30u << 16) | 8u));             // min/max ratio
  CHECK(mtl.btrs.regs[0x0C] == ((0x80u << 16) | 0x10u));          // EPP and target (the power-neutral ratio)
  CHECK((mtl.btrs.regs[0x10] & 0xFFFF) == 0x202);                 // two tiles, 4/3 PLL
  CHECK((mtl.ip.regs[0x84] & 0xC02) == 0xC02 && (mtl.ip.regs[0x94] & 0xC02) == 0xC02);  // MTL: CLK_SET and RST_SET
  CHECK(mtl.ip.regs[0x98] == 0xC02 && (mtl.ip.regs[0x30204] & 1));  // reset released, DPU active
  CHECK((mtl.ip.regs[0x30024] & 8) && (mtl.ip.regs[0x30028] & 8) && !(mtl.ip.regs[0x30020] & 8) && !(mtl.ip.regs[0x30200] & 1));
  CHECK((mtl.ip.regs[0x160] & 3) == 3 && (mtl.ip.regs[0x154] & 1));
  CHECK(mtl.btrs.regs.count(0x54) == 0);                          // no port weights on Meteor Lake

  SimNpu lnl(0x643E);
  IntelNpuHw hl = lnl.hw();
  hl.read_info();
  CHECK(hl.power_up().ok);
  CHECK((lnl.ip.regs[0x80] & 0xC02) == 0xC02 && (lnl.ip.regs[0x90] & 0xC02) == 0xC02 && lnl.ip.regs.count(0x84) == 0);  // 40xx: CLK_EN and RST_EN
  CHECK((lnl.ip.regs[0x160] & 0x5) == 0x5);                                                                          // CPU_CTRL and the 40xx L2 bit
  CHECK(lnl.btrs.regs[0x54] == 0xF711F711u && lnl.btrs.regs[0x24] == 0xF711u);
  CHECK((lnl.btrs.regs[0x138] >> 16) == 0x80 && (lnl.btrs.regs[0x134] >> 16) == 0x80);                                // CDYN and EPP
  SimNpu ptl(0xB03E);
  IntelNpuHw hp = ptl.hw();
  hp.read_info();
  CHECK(hp.power_up().ok && ptl.ip.regs[0x30068] == 0 && ptl.ip.regs[0x3006C] == 3);
  SimNpu nvl(0xD71D);
  IntelNpuHw hn = nvl.hw();
  hn.read_info();
  CHECK(hn.power_up().ok && nvl.ip.regs[0x30068] == (17u | (17u << 16)));
}

static IntelNpuStep fail_step(uint16_t id, void (*sabotage)(SimNpu&)) {
  SimNpu n(id);
  sabotage(n);
  IntelNpuHw h = n.hw();
  h.read_info();
  return h.power_up().failed;
}

static void test_failures() {
  CHECK(fail_step(0x7D1D, [](SimNpu& n) { n.wp_stuck = true; }) == IntelNpuStep::WorkPoint);
  CHECK(fail_step(0x7D1D, [](SimNpu& n) { n.ip.regs[0x154] = 1; }) == IntelNpuStep::HostSs);   // the NoC queue is already requested
  CHECK(fail_step(0x7D1D, [](SimNpu& n) { n.ip.regs[0x98] = 1; }) == IntelNpuStep::HostSs);    // AON never leaves reset
  CHECK(fail_step(0x643E, [](SimNpu& n) { n.btrs.regs[0x154] &= ~(1u << 6); }) == IntelNpuStep::ClockAck);  CHECK(std::string(intel_npu_step_name(IntelNpuStep::TopNoc)) == "top NoC");
}

static void test_boot() {
  SimNpu mtl(0x7D1D);
  IntelNpuHw h = mtl.hw();
  h.read_info();
  CHECK(h.boot(0x80000000ull).ok);
  CHECK(mtl.ip.regs[0x41040] == ((0x80000000u >> 9) | 1u));
  CHECK((mtl.ip.regs[0x06010040] & 1) && !(mtl.ip.regs[0x06010040] & 2));  // run vector set, resume pulsed and released
  CHECK(!h.boot(0xFFFFFFFFFFull << 4).ok);
  SimNpu lnl(0x643E);
  IntelNpuHw hl = lnl.hw();
  hl.read_info();
  CHECK(hl.boot(0x1234000ull).ok);
  CHECK(lnl.ip.writes64[0x40040] == (0x1234000ull << 3));
  CHECK(lnl.ip.regs[0x40040] == (static_cast<uint32_t>(0x1234000ull << 3) | 1u));
  CHECK((lnl.ip.regs[0x01010030] & 1) != 0);
  CHECK(!hl.boot(~0ull).ok);
  SimNpu refused(0xB03E);
  refused.cpu_noc_refused = true;
  IntelNpuHw hr = refused.hw();
  hr.read_info();
  CHECK(!hr.boot(0x1000).ok && hr.boot(0x1000).failed == IntelNpuStep::CpuNoc && refused.ip.writes64.empty());
  SimNpu nvl(0xD71D);
  nvl.cpu_noc_refused = true;  // Nova Lake never asks the CPU NoC
  IntelNpuHw hn = nvl.hw();
  hn.read_info();
  CHECK(hn.boot(0x1000).ok && nvl.ip.regs.count(0x01010030) == 0);
}

// Whatever the registers read, every sequence ends (with a result or a refusal) and never touches an offset outside the windows.
static void test_chaos() {
  const uint16_t ids[] = {0x7D1D, 0x643E, 0xB03E, 0xD71D};
  for (uint16_t id : ids)
    for (uint32_t seed = 1; seed <= 3; ++seed) {
      SimNpu n(id);
      n.ip.chaos = n.btrs.chaos = true;
      n.ip.rng = n.btrs.rng = 0x1234567ull * seed;
      IntelNpuHw h = n.hw();
      h.read_info();
      (void)h.power_up();
      (void)h.boot(0x10000ull * seed);
      for (const auto& w : n.ip.regs) CHECK(w.first < IntelNpuHw::ip_window(n.platform.ip));
      for (const auto& w : n.btrs.regs) CHECK(w.first < IntelNpuHw::kButtressWindow);
    }
}

int main() {
  test_platform();
  test_info();
  test_power_up();
  test_failures();
  test_boot();
  test_chaos();
  if (g_failed) { printf("IntelNpuHwTest: %d failed\n", g_failed); return 1; }
  printf("IntelNpuHwTest: ok\n");
  return 0;
}
