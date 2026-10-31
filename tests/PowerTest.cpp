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

// The power code against models of the processor's registers: the HWP range and request (package and per-core variants), the SpeedStep fallback, the C-state hint, the thermal manager's
// ceiling (steps, hysteresis, the critical point) and the profile policy that follows the battery.
#include <stdint.h>
#include <stdio.h>
#include <map>
#include <vector>
#include "Hardware/Power/PowerManager.hpp"
#include "Hardware/Power/Thermal.hpp"

using namespace hardware;
using namespace hardware::cpu_msr;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace kernel {
bool run_on_every_core(void (*fn)(void*), void* argument, uint64_t) { fn(argument); return true; }   // (HostMsr is not used here)
}

struct FakeMsr final : IMsrAccess {
  std::map<uint32_t, uint64_t> regs;
  std::vector<std::pair<uint32_t, uint64_t>> all_cores;      // writes to every core
  uint32_t writes = 0;
  bool fail_read = false;
  bool read(uint32_t msr, uint64_t* v) override { if (fail_read) return false; *v = regs[msr]; return true; }
  bool write(uint32_t msr, uint64_t v) override { regs[msr] = v; ++writes; return true; }
  bool write_every_core(uint32_t msr, uint64_t v) override { regs[msr] = v; all_cores.push_back({msr, v}); return true; }
};

static CpuPowerCaps hwp_caps(bool package_request = true) {
  CpuPowerCaps c;
  c.intel = c.hwp = c.hwp_epp = c.eist = true;
  c.hwp_package_request = package_request;
  return c;
}
static uint64_t capabilities(uint32_t highest, uint32_t guaranteed, uint32_t efficient, uint32_t lowest) { return highest | (uint64_t{guaranteed} << 8) | (uint64_t{efficient} << 16) | (uint64_t{lowest} << 24); }
static uint32_t field(uint64_t request, uint32_t shift) { return static_cast<uint32_t>((request >> shift) & 0xFF); }

static void test_hwp() {
  FakeMsr msr;
  msr.regs[kHwpCapabilities] = capabilities(40, 22, 8, 4);
  CpuPower cpu(msr);
  CHECK(cpu.initialize(hwp_caps()) && cpu.uses_hwp() && cpu.speed_control());
  CHECK(cpu.lowest_ratio() == 4 && cpu.efficient_ratio() == 8 && cpu.guaranteed_ratio() == 22 && cpu.highest_ratio() == 40);
  CHECK(msr.regs[kPmEnable] == 1);
  CHECK(!msr.all_cores.empty() && msr.all_cores[0].first == kHwpRequest && (msr.all_cores[0].second & kHwpPackageControl));      // every core follows the package request
  uint64_t r = msr.regs[kHwpRequestPkg];                                                                                            // the starting profile is Balanced
  CHECK(field(r, 0) == 4 && field(r, 8) == 40 && field(r, 16) == 0 && field(r, 24) == 0x80);
  CHECK(cpu.set_profile(PowerProfile::Performance));
  r = msr.regs[kHwpRequestPkg];
  CHECK(field(r, 0) == 22 && field(r, 8) == 40 && field(r, 24) == 0);
  CHECK(cpu.set_profile(PowerProfile::PowerSaver));
  r = msr.regs[kHwpRequestPkg];
  CHECK(field(r, 0) == 4 && field(r, 8) == 22 && field(r, 24) == 0xFF);
  // the ceiling cuts the range of the profile
  cpu.set_profile(PowerProfile::Balanced);
  CHECK(cpu.set_ceiling_percent(50));
  r = msr.regs[kHwpRequestPkg];
  CHECK(field(r, 8) == 4 + 36 * 50 / 100 && field(r, 0) == 4);
  CHECK(cpu.set_ceiling_percent(0) && cpu.ceiling_percent() == 1 && field(msr.regs[kHwpRequestPkg], 8) >= 4);
  CHECK(cpu.set_ceiling_percent(500) && cpu.ceiling_percent() == 100 && field(msr.regs[kHwpRequestPkg], 8) == 40);
  // the minimum never exceeds the maximum when the ceiling is low under the Performance profile
  cpu.set_profile(PowerProfile::Performance);
  cpu.set_ceiling_percent(1);
  r = msr.regs[kHwpRequestPkg];
  CHECK(field(r, 0) <= field(r, 8) && field(r, 8) >= 22);
  // no preference field without EPP support
  FakeMsr plain;
  plain.regs[kHwpCapabilities] = capabilities(40, 22, 8, 4);
  CpuPower no_epp(plain);
  CpuPowerCaps caps = hwp_caps();
  caps.hwp_epp = false;
  CHECK(no_epp.initialize(caps) && field(plain.regs[kHwpRequestPkg], 24) == 0);
  // without the package request register the request is written on every core
  FakeMsr per_core;
  per_core.regs[kHwpCapabilities] = capabilities(40, 22, 8, 4);
  CpuPower pc(per_core);
  CHECK(pc.initialize(hwp_caps(false)));
  per_core.all_cores.clear();
  CHECK(pc.set_profile(PowerProfile::PowerSaver) && per_core.all_cores.size() == 1 && per_core.all_cores[0].first == kHwpRequest && field(per_core.all_cores[0].second, 8) == 22);
  // garbage capabilities, an unreadable register, a virtual machine, another vendor
  FakeMsr bad;
  bad.regs[kHwpCapabilities] = capabilities(0, 0, 0, 0);
  CpuPower b1(bad);
  CHECK(!b1.initialize(hwp_caps()) && !b1.speed_control() && !b1.set_profile(PowerProfile::Balanced));
  bad.regs[kHwpCapabilities] = capabilities(10, 20, 30, 40);                     // lowest above highest
  CHECK(!b1.initialize(hwp_caps()));
  FakeMsr unreadable;
  unreadable.fail_read = true;
  CpuPower b2(unreadable);
  CHECK(!b2.initialize(hwp_caps()));
  CpuPowerCaps vm = hwp_caps();
  vm.virtual_machine = true;
  FakeMsr m3;
  m3.regs[kHwpCapabilities] = capabilities(40, 22, 8, 4);
  CpuPower b3(m3);
  CHECK(!b3.initialize(vm) && m3.writes == 0);
  CpuPowerCaps other = hwp_caps();
  other.intel = false;
  CHECK(!b3.initialize(other) && m3.writes == 0);
  // odd guaranteed / efficient values are pulled into the range
  FakeMsr odd;
  odd.regs[kHwpCapabilities] = capabilities(40, 2, 90, 4);
  CpuPower o(odd);
  CHECK(o.initialize(hwp_caps()) && o.guaranteed_ratio() == 40 && o.efficient_ratio() == 4);
}

static void test_speedstep() {
  FakeMsr msr;
  msr.regs[kPlatformInfo] = (uint64_t{28} << 8) | (uint64_t{8} << 40);
  msr.regs[kTurboRatioLimit] = 38;
  CpuPowerCaps caps;
  caps.intel = caps.eist = caps.energy_bias = true;
  CpuPower cpu(msr);
  CHECK(cpu.initialize(caps) && !cpu.uses_hwp() && cpu.speed_control() && cpu.lowest_ratio() == 8 && cpu.highest_ratio() == 38);
  CHECK(msr.regs[kPerfCtl] == (uint64_t{28} << 8) && msr.regs[kEnergyPerfBias] == 6);              // Balanced: the top of the normal range
  CHECK(cpu.set_profile(PowerProfile::Performance) && msr.regs[kPerfCtl] == (uint64_t{38} << 8) && msr.regs[kEnergyPerfBias] == 0);
  CHECK(cpu.set_profile(PowerProfile::PowerSaver) && msr.regs[kPerfCtl] == (uint64_t{8} << 8) && msr.regs[kEnergyPerfBias] == 15);
  cpu.set_profile(PowerProfile::Balanced);
  CHECK(cpu.set_ceiling_percent(1) && ((msr.regs[kPerfCtl] >> 8) & 0xFF) >= 8);
  msr.regs[kPerfStatus] = uint64_t{20} << 8;
  CHECK(cpu.current_ratio() == 20);
  FakeMsr broken;
  broken.regs[kPlatformInfo] = (uint64_t{4} << 8) | (uint64_t{8} << 40);                     // minimum above maximum
  CpuPower b(broken);
  CHECK(!b.initialize(caps) && b.current_ratio() == 0);
}

static void test_idle_hint() {
  CpuPowerCaps c;
  c.intel = c.mwait = c.mwait_interrupt_break = c.always_running_timer = true;
  c.mwait_substates[0] = 2; c.mwait_substates[1] = 1; c.mwait_substates[2] = 1; c.mwait_substates[3] = 1;      // C1 C1E, C3, C6, C7
  CHECK(CpuPower::idle_hint(c, PowerProfile::Performance) == 0x00 && CpuPower::idle_hint(c, PowerProfile::Balanced) == 0x20 && CpuPower::idle_hint(c, PowerProfile::PowerSaver) == 0x30);
  c.mwait_substates[3] = 0;
  CHECK(CpuPower::idle_hint(c, PowerProfile::PowerSaver) == 0x20);
  c.mwait_substates[2] = c.mwait_substates[1] = 0;
  CHECK(CpuPower::idle_hint(c, PowerProfile::Balanced) == 0x00);
  c.mwait_substates[3] = c.mwait_substates[2] = c.mwait_substates[1] = 1;
  c.always_running_timer = false;                                                              // the timer would stop in a deep state: only the first
  CHECK(CpuPower::idle_hint(c, PowerProfile::PowerSaver) == 0x00);
  c.always_running_timer = true;
  c.mwait_interrupt_break = false;
  CHECK(CpuPower::idle_hint(c, PowerProfile::Balanced) == 0xFFFFFFFFu);
  c.mwait_interrupt_break = true;
  c.virtual_machine = true;
  CHECK(CpuPower::idle_hint(c, PowerProfile::Balanced) == 0xFFFFFFFFu);
  c.virtual_machine = false;
  c.mwait = false;
  CHECK(CpuPower::idle_hint(c, PowerProfile::Balanced) == 0xFFFFFFFFu);
  CHECK(power_profile_name(PowerProfile::Balanced)[0] == 'B');
}

struct FakeSource final : IThermalSource {
  int32_t temperature = 50000;
  bool readable = true;
  ThermalLimits lim{85000, 100000};
  const char* label = "fake";
  const char* name() const override { return label; }
  bool read(int32_t* t) override { if (!readable) return false; *t = temperature; return true; }
  ThermalLimits limits() override { return lim; }
};
struct FakeActuator final : IThermalActuator {
  std::vector<uint32_t> set;
  bool set_ceiling_percent(uint32_t p) override { set.push_back(p); return true; }
};

static void test_thermal() {
  FakeActuator act;
  ThermalManager m(&act);
  FakeSource cpu, zone;
  zone.label = "zone";
  zone.lim = {-1, -1};
  CHECK(m.add(&cpu) && m.add(&zone) && !m.add(nullptr));
  CHECK(!m.poll(0) && m.ceiling_percent() == 100 && m.hottest_millicelsius() == 50000 && act.set.empty());
  cpu.temperature = 90000;                                                  // above the passive point: a step down every second, not faster
  CHECK(!m.poll(500));
  CHECK(m.poll(1000) && m.ceiling_percent() == 90 && act.set.back() == 90);
  CHECK(m.poll(2000) && m.poll(3000) && m.ceiling_percent() == 70);
  for (uint64_t t = 4000; t < 20000; t += 1000) m.poll(t);
  CHECK(m.ceiling_percent() == ThermalManager::kMinCeiling);                // never below the floor
  CHECK(!m.critical());
  cpu.temperature = 83000;                                                  // under passive but inside the margin: held
  CHECK(!m.poll(21000) && m.ceiling_percent() == ThermalManager::kMinCeiling);
  cpu.temperature = 70000;                                                  // cool: speed comes back, a step a second
  CHECK(m.poll(22000) && m.ceiling_percent() == 40);
  for (uint64_t t = 23000; t < 40000; t += 1000) m.poll(t);
  CHECK(m.ceiling_percent() == 100);
  // the hottest source and the critical point
  zone.temperature = 95000;
  m.poll(41000);
  CHECK(m.hottest_millicelsius() == 95000 && m.hottest_source()[0] == 'z' && !m.critical() && m.ceiling_percent() == 100);   // (no limits on this one)
  cpu.temperature = 100000;
  m.poll(42000);
  CHECK(m.critical());
  // a source that cannot be read is skipped
  FakeActuator act2;
  ThermalManager m2(&act2);
  FakeSource dead;
  dead.readable = false;
  m2.add(&dead);
  CHECK(!m2.poll(0) && m2.hottest_millicelsius() == -1);
  ThermalManager m3(nullptr);
  FakeSource hot;
  hot.temperature = 90000;
  m3.add(&hot);
  CHECK(m3.poll(0) && m3.ceiling_percent() == 90);                          // (no actuator: still counted)
}

static void test_policy() {
  FakeMsr msr;
  msr.regs[kHwpCapabilities] = capabilities(40, 22, 8, 4);
  CpuPowerCaps caps = hwp_caps();
  caps.mwait = caps.mwait_interrupt_break = caps.always_running_timer = true;
  caps.mwait_substates[0] = 1; caps.mwait_substates[1] = 1; caps.mwait_substates[2] = 1; caps.mwait_substates[3] = 1;
  CpuPower cpu(msr);
  CHECK(cpu.initialize(caps));
  static uint32_t hint;
  hint = 0;
  PowerManager pm(cpu, caps, [](uint32_t h) { hint = h; });
  pm.start();
  CHECK(pm.profile() == PowerProfile::Balanced && pm.automatic() && hint == 0x20);
  BatteryStatus b;
  b.present = true;
  b.percent = 80;
  b.state = BatteryState::Charging;
  CHECK(!pm.update(b, 0) && !pm.on_battery());
  b.state = BatteryState::Discharging;
  CHECK(!pm.update(b, 1000) && pm.on_battery() && pm.profile() == PowerProfile::Balanced);          // plenty left
  b.percent = 20;
  CHECK(pm.update(b, 2000) && pm.profile() == PowerProfile::PowerSaver && hint == 0x30 && field(msr.regs[kHwpRequestPkg], 24) == 0xFF);
  b.state = BatteryState::Charging;
  CHECK(!pm.update(b, 5000) && pm.profile() == PowerProfile::PowerSaver);                           // too soon after the last change
  CHECK(pm.update(b, 13000) && pm.profile() == PowerProfile::Balanced && hint == 0x20);
  b.present = false;
  CHECK(!pm.update(b, 30000) && !pm.on_battery());                                                  // no battery: mains
  pm.set_profile(PowerProfile::Performance);
  CHECK(!pm.automatic() && hint == 0x00 && field(msr.regs[kHwpRequestPkg], 0) == 22);
  b.present = true; b.state = BatteryState::Discharging; b.percent = 5;
  CHECK(!pm.update(b, 60000) && pm.profile() == PowerProfile::Performance);                         // the user's choice holds
  pm.set_automatic();
  CHECK(pm.update(b, 61000) && pm.profile() == PowerProfile::PowerSaver);
  b.percent = -1;
  b.state = BatteryState::Unknown;
  PowerManager none(cpu, caps, nullptr);
  none.start();
  CHECK(!none.update(b, 0));
}

int main() {
  test_hwp();
  test_speedstep();
  test_idle_hint();
  test_thermal();
  test_policy();
  printf(g_failed ? "PowerTest: %d FAILED\n" : "PowerTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
