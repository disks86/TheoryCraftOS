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

// Host-side tests for the probe of a discrete Intel GPU (IntelGpuProbe) against a register model: the platform of each PCI ID, the local memory size of DG1
// (GSMBASE) and DG2 / Battlemage (SG_TILE_ADDR_RANGE), the package temperature registers, a GPU that does not answer, and a resume after a power cut. The
// register offsets are typed in from the Linux xe driver's xe_regs.h, xe_mchbar_regs.h and xe_pcode_regs.h.
#include <stdio.h>
#include <string.h>
#include <map>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuProbe.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint64_t kGsmBase = 0x108100, kTileAddrRange0 = 0x1083a0, kPcuPackageTemperature = 0x140000 + 0x5978, kBmgPackageTemperature = 0x138434;
constexpr uint64_t GiB = 1ull << 30, MiB = 1ull << 20;

struct FakeBank final : intel_gt::IRegisterBank {
  std::map<uint64_t, uint32_t> regs;
  uint32_t writes = 0;
  bool dead = false;
  uint32_t read32(uint64_t offset) override {
    if (dead) return 0xFFFFFFFFu;
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint64_t offset, uint32_t value) override { ++writes; regs[offset] = value; }
};

// SG_TILE_ADDR_RANGE: size in GiB in bits 17:8, the tile's offset in GiB in bits 7:1.
static uint32_t tile_range(uint32_t gib, uint32_t offset_gib = 0) { return (gib << 8) | (offset_gib << 1); }

static void test_platforms() {
  using P = IntelGpuProbe::Platform;
  CHECK(IntelGpuProbe::platform_of(0x4905) == P::Dg1);      // Iris Xe MAX
  CHECK(IntelGpuProbe::platform_of(0x4908) == P::Dg1);
  CHECK(IntelGpuProbe::platform_of(0x5690) == P::Dg2);      // Arc A770M
  CHECK(IntelGpuProbe::platform_of(0x5693) == P::Dg2);      // Arc A370M
  CHECK(IntelGpuProbe::platform_of(0x56A0) == P::Dg2);      // Arc A770
  CHECK(IntelGpuProbe::platform_of(0xE20B) == P::Battlemage);   // Arc B580
  CHECK(IntelGpuProbe::platform_of(0xE20C) == P::Battlemage);   // Arc B570
  // The integrated GPUs, Ponte Vecchio and unknown IDs get none.
  CHECK(IntelGpuProbe::platform_of(0x9A49) == P::None);     // Tiger Lake
  CHECK(IntelGpuProbe::platform_of(0x46A6) == P::None);     // Alder Lake
  CHECK(IntelGpuProbe::platform_of(0x7D55) == P::None);     // Meteor Lake
  CHECK(IntelGpuProbe::platform_of(0x64A0) == P::None);     // Lunar Lake
  CHECK(IntelGpuProbe::platform_of(0x0BD5) == P::None);     // Ponte Vecchio
  CHECK(IntelGpuProbe::platform_of(0x591B) == P::None);
  CHECK(IntelGpuProbe::platform_of(0x0000) == P::None && !IntelGpuProbe::supported(0xFFFF));
}

static void test_conversions() {
  CHECK(IntelGpuProbe::tile_bytes(tile_range(16)) == 16 * GiB);
  CHECK(IntelGpuProbe::tile_bytes(tile_range(8, 3)) == 8 * GiB);    // the tile offset is not the size
  CHECK(IntelGpuProbe::tile_bytes(tile_range(4) | 0xFFFC0001u) == 4 * GiB);   // bits outside 17:8 and 7:1 are ignored
  CHECK(IntelGpuProbe::tile_bytes(0) == 0);
  CHECK(IntelGpuProbe::tile_bytes(0xFFFFFFFFu) == 0);
  CHECK(IntelGpuProbe::tile_bytes(tile_range(1023)) == 0);           // more than any card: not believed
  CHECK(IntelGpuProbe::package_temperature(47) == 47000);
  CHECK(IntelGpuProbe::package_temperature(0xFFFFFF00u | 55) == 55000);   // only bits 7:0
  CHECK(IntelGpuProbe::package_temperature(0) == -1);                 // xe: 0 is no reading
  CHECK(IntelGpuProbe::package_temperature(0xFFFFFFFFu) == -1);
}

static void test_dg2() {
  FakeBank bank;
  bank.regs[kTileAddrRange0] = tile_range(16);
  bank.regs[kGsmBase] = 0xF8000000u;   // (the stolen memory near the top: DG2 does not use it for the size)
  bank.regs[kGsmBase + 4] = 3;
  bank.regs[kPcuPackageTemperature] = 52;
  bank.regs[kBmgPackageTemperature] = 99;   // the other platform's register
  IntelGpuProbe probe(bank, 0x56A0);
  CHECK(probe.temperature_millicelsius() == -1 && !probe.resume());   // not started
  CHECK(probe.start());
  CHECK(strcmp(probe.status(), "identified") == 0 && strstr(probe.name(), "Alchemist") != nullptr);
  CHECK(probe.vram_bytes() == 16 * GiB && probe.temperature_millicelsius() == 52000);
  // A power cut: the card's firmware has not set the range up yet, then has.
  bank.regs.clear();
  CHECK(probe.resume() && probe.vram_bytes() == 16 * GiB && strstr(probe.status(), "power cut") != nullptr);
  CHECK(probe.temperature_millicelsius() == -1);
  bank.regs[kTileAddrRange0] = tile_range(16);
  bank.regs[kPcuPackageTemperature] = 41;
  CHECK(probe.resume() && strcmp(probe.status(), "identified") == 0 && probe.temperature_millicelsius() == 41000);
  bank.dead = true;
  CHECK(!probe.resume() && probe.temperature_millicelsius() == -1);
  CHECK(bank.writes == 0);
}

static void test_battlemage_and_dg1() {
  {
    FakeBank bank;
    bank.regs[kTileAddrRange0] = tile_range(12);
    bank.regs[kBmgPackageTemperature] = 0xA00 | 61;
    bank.regs[kPcuPackageTemperature] = 99;
    IntelGpuProbe probe(bank, 0xE20B);
    CHECK(probe.start() && probe.vram_bytes() == 12 * GiB && probe.temperature_millicelsius() == 61000);
    CHECK(strstr(probe.name(), "Battlemage") != nullptr && bank.writes == 0);
  }
  {
    FakeBank bank;
    // DG1: 4 GiB, the stolen memory starts 64 MiB below the top; bit 0 is the lock bit.
    bank.regs[kGsmBase] = static_cast<uint32_t>(4 * GiB - 64 * MiB) | 1;
    bank.regs[kGsmBase + 4] = 0;
    bank.regs[kTileAddrRange0] = tile_range(32);   // (no such register on DG1: not read)
    bank.regs[kPcuPackageTemperature] = 45;
    IntelGpuProbe probe(bank, 0x4905);
    CHECK(probe.start() && probe.vram_bytes() == 4 * GiB - 64 * MiB && probe.temperature_millicelsius() == 45000);
    CHECK(probe.platform() == IntelGpuProbe::Platform::Dg1 && strstr(probe.name(), "DG1") != nullptr);
    // Above 4 GiB through the high dword.
    bank.regs[kGsmBase] = 0xFC000000u;
    bank.regs[kGsmBase + 4] = 1;
    CHECK(probe.resume() && probe.vram_bytes() == 8 * GiB - 64 * MiB);
  }
}

static void test_refused() {
  FakeBank dead;
  dead.dead = true;
  IntelGpuProbe a(dead, 0x56A0);
  CHECK(!a.start() && strstr(a.status(), "does not answer") != nullptr);
  IntelGpuProbe b(dead, 0x4905);
  CHECK(!b.start());
  FakeBank zero;   // answers, but with no size: identified, size unknown
  IntelGpuProbe c(zero, 0xE20C);
  CHECK(c.start() && c.vram_bytes() == 0 && strstr(c.status(), "reads 0") != nullptr);
  IntelGpuProbe igpu(zero, 0x9A49);
  CHECK(!igpu.start());
  GpuContext context{};
  context.device.vendor_id = 0x8086;
  context.device.device_id = 0x56A0;
  IntelGpuProbe machine(context);
  CHECK(!machine.start() && strstr(machine.status(), "mapper") != nullptr);
  CHECK(!machine.resume() && machine.temperature_millicelsius() == -1);
}

int main() {
  test_platforms();
  test_conversions();
  test_dg2();
  test_battlemage_and_dg1();
  test_refused();
  if (g_failed) {
    printf("IntelGpuProbeTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("IntelGpuProbeTest: all passed\n");
  return 0;
}
