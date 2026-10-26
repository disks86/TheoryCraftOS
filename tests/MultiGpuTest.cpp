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

// Host-side tests for machines with more than one GPU: classifying integrated / discrete / virtual adapters, finding the boot display, the
// power/performance policy, and putting the discrete GPU to sleep (PCI D3hot) until something leases it.
#include <stdio.h>
#include "FakeGpuPci.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
int g_delays = 0;
void count_delay(uint32_t ms) { if (ms >= 10) ++g_delays; }
}  // namespace
using namespace fakegpu;
static void test_classify() {
  CHECK(gpu_classify(kIntel, kIntelTigerLake) == GpuKind::Integrated);
  CHECK(gpu_classify(kIntel, 0x1234) == GpuKind::Integrated);  // unknown Intel ID: still a CPU's graphics
  CHECK(gpu_classify(kNvidia, kRtx4060Laptop) == GpuKind::Discrete);
  CHECK(gpu_classify(0x1234, 0x1111) == GpuKind::Virtual);
  CHECK(gpu_classify(0x15AD, 0x0405) == GpuKind::Virtual);
  CHECK(gpu_classify(0x1AF4, 0x1050) == GpuKind::Virtual);
  CHECK(gpu_classify(0x102B, 0x0522) == GpuKind::Unknown);
  // Every ID of the three vendors classifies without reading out of range, and Intel/NVIDIA never give Unknown.
  for (uint32_t id = 0; id <= 0xFFFF; ++id) {
    CHECK(gpu_classify(kIntel, static_cast<uint16_t>(id)) != GpuKind::Unknown);
    CHECK(gpu_classify(kNvidia, static_cast<uint16_t>(id)) == GpuKind::Discrete);
    (void)gpu_classify(kAmd, static_cast<uint16_t>(id));
  }
  CHECK(gpu_classify(kIntel, 0x56A0) == GpuKind::Discrete);  // Arc A770
}

static void test_topology() {
  FakePci pci;
  pci.add(0, 0x8086, 0x1234, 0x06, 0x7, false);  // a bridge, not a display
  pci.add(1, kNvidia, kRtx4060Laptop, kPciClassDisplay, 0, true);       // dGPU, firmware left it off
  pci.add(2, kIntel, kIntelTigerLake, kPciClassDisplay, 0x7, false);    // iGPU, decoding on: the boot display
  GpuTopology t;
  t.enumerate(pci);
  CHECK(t.count() == 2);
  CHECK(t.is_hybrid());
  CHECK(t.boot_index() == 1 && t.adapter(1).kind == GpuKind::Integrated && t.adapter(1).boot_display);
  CHECK(t.find_first(GpuKind::Discrete) == 0);
  PciDeviceInfo boot{};
  CHECK(gpu_boot_display(pci, &boot) && boot.address.bus == 2);

  FakePci none;
  none.add(0, 0x8086, 0x1234, 0x06, 0, false);
  t.enumerate(none);
  CHECK(t.count() == 0 && t.boot_index() == -1 && !t.is_hybrid());
  CHECK(!gpu_boot_display(none, &boot));

  // Nothing decoding: the first adapter is the boot display.
  FakePci quiet;
  quiet.add(1, kNvidia, kRtx4060Laptop, kPciClassDisplay, 0, true);
  quiet.add(2, kIntel, kIntelTigerLake, kPciClassDisplay, 0, false);
  t.enumerate(quiet);
  CHECK(t.boot_index() == 0);

  // More display controllers than the table holds: the extra ones are ignored.
  FakePci many;
  GpuTopology t2;
  for (int i = 0; i < 4; ++i) many.add(static_cast<uint8_t>(i), kNvidia, kRtx4060Laptop, kPciClassDisplay, 0, false);
  t2.enumerate(many);
  CHECK(t2.count() == 4);
}

static void test_policy() {
  FakePci pci;
  pci.add(1, kNvidia, kRtx4060Laptop, kPciClassDisplay, 0, true);
  pci.add(2, kIntel, kIntelTigerLake, kPciClassDisplay, 0x7, false);
  GpuTopology t;
  t.enumerate(pci);
  const int32_t dgpu = 0, igpu = 1;
  GpuSelectionInput in;

  in.power = PowerSource::Ac;
  CHECK(gpu_select(t, in) == igpu);  // desktop work stays on the efficient GPU
  in.workload = GpuWorkload::Video; CHECK(gpu_select(t, in) == igpu);
  in.workload = GpuWorkload::Web; CHECK(gpu_select(t, in) == igpu);
  in.workload = GpuWorkload::Game; CHECK(gpu_select(t, in) == dgpu);
  in.workload = GpuWorkload::Compute; CHECK(gpu_select(t, in) == dgpu);

  in.power = PowerSource::Battery;
  in.battery_percent = 80; CHECK(gpu_select(t, in) == dgpu);
  in.battery_percent = kDiscreteMinBatteryPercent; CHECK(gpu_select(t, in) == dgpu);
  in.battery_percent = kDiscreteMinBatteryPercent - 1; CHECK(gpu_select(t, in) == igpu);
  in.battery_percent = 90; in.battery_saver = true; CHECK(gpu_select(t, in) == igpu);

  // Explicit preferences win over the workload and the battery.
  in.preference = GpuPreference::HighPerformance; in.workload = GpuWorkload::Desktop; CHECK(gpu_select(t, in) == dgpu);
  in.preference = GpuPreference::LowPower; in.workload = GpuWorkload::Game; in.battery_saver = false; in.power = PowerSource::Ac; CHECK(gpu_select(t, in) == igpu);

  // A single GPU is always the answer; no GPU gives -1.
  FakePci one;
  one.add(1, kIntel, kIntelTigerLake, kPciClassDisplay, 0x7, false);
  t.enumerate(one);
  in = GpuSelectionInput{}; in.preference = GpuPreference::HighPerformance;
  CHECK(gpu_select(t, in) == 0);
  FakePci empty;
  t.enumerate(empty);
  CHECK(gpu_select(t, in) == -1);

  // iGPU + virtual adapter (no discrete): high performance falls back to the integrated GPU.
  FakePci vm;
  vm.add(1, 0x1234, 0x1111, kPciClassDisplay, 0, false);
  vm.add(2, kIntel, kIntelTigerLake, kPciClassDisplay, 0x7, false);
  t.enumerate(vm);
  CHECK(gpu_select(t, in) == 1);
}

static void test_power() {
  FakePci pci;
  pci.add(1, kNvidia, kRtx4060Laptop, kPciClassDisplay, 0, true);
  pci.add(2, kIntel, kIntelTigerLake, kPciClassDisplay, 0x7, true);
  g_delays = 0;
  GpuManager mgr(pci, count_delay);
  mgr.initialize();
  CHECK(mgr.is_hybrid() && mgr.count() == 2 && mgr.display_index() == 1);
  CHECK(mgr.asleep(0) && !mgr.asleep(1));
  CHECK((pci.devices[0].pmcsr & 3) == 3);  // D3hot
  CHECK((pci.devices[1].pmcsr & 3) == 0);  // the display GPU is never touched

  GpuSelectionInput in;
  in.workload = GpuWorkload::Game; in.power = PowerSource::Ac;
  const int32_t pick = mgr.select(in);
  CHECK(pick == 0);
  CHECK(mgr.acquire(static_cast<uint32_t>(pick)) && !mgr.asleep(0) && mgr.leases(0) == 1 && g_delays == 1);
  CHECK((pci.devices[0].pmcsr & 3) == 0);
  CHECK(mgr.acquire(0) && mgr.leases(0) == 2 && g_delays == 1);  // already awake: no second wake
  mgr.release(0);
  CHECK(!mgr.asleep(0) && mgr.leases(0) == 1);
  mgr.release(0);
  CHECK(mgr.asleep(0) && (pci.devices[0].pmcsr & 3) == 3);
  mgr.release(0);  // extra release is harmless
  CHECK(mgr.leases(0) == 0);

  // The display GPU can be leased and released and stays awake.
  CHECK(mgr.acquire(1)); mgr.release(1);
  CHECK(!mgr.asleep(1) && (pci.devices[1].pmcsr & 3) == 0);
  CHECK(!mgr.acquire(7));

  // Upper bits of PMCSR are preserved.
  pci.devices[0].pmcsr = 0x8100 | 3;
  CHECK(mgr.acquire(0));
  CHECK(pci.devices[0].pmcsr == 0x8100);

  // A device that will not wake is not leased.
  FakePci stuck;
  stuck.add(1, kNvidia, kRtx4060Laptop, kPciClassDisplay, 0, true);
  stuck.add(2, kIntel, kIntelTigerLake, kPciClassDisplay, 0x7, true);
  GpuManager m2(stuck);
  m2.initialize();
  stuck.devices[0].stuck = true;
  CHECK(m2.asleep(0) && !m2.acquire(0) && m2.leases(0) == 0);

  // No PM capability: nothing to suspend, the adapter is simply left running.
  FakePci nopm;
  nopm.add(1, kNvidia, kRtx4060Laptop, kPciClassDisplay, 0, false);
  nopm.add(2, kIntel, kIntelTigerLake, kPciClassDisplay, 0x7, false);
  GpuManager m3(nopm);
  m3.initialize();
  CHECK(!m3.asleep(0) && m3.acquire(0) && m3.leases(0) == 1);

  // A machine with one GPU is left alone.
  FakePci single;
  single.add(1, kNvidia, kRtx4060Laptop, kPciClassDisplay, 0, true);
  GpuManager m4(single);
  m4.initialize();
  CHECK(!m4.is_hybrid() && !m4.asleep(0) && single.devices[0].pmcsr == 0);

  PciPowerState s;
  CHECK(!pci_get_power_state(nopm, nopm.devices[0].info.address, &s));
}

int main() {
  test_classify();
  test_topology();
  test_policy();
  test_power();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("MultiGpuTest: ok\n");
  return 0;
}
