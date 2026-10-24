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

// Host-side tests for the Intel CPU generation rules (9th-gen Core and newer) and the Intel GPU architecture table. Includes the
// lookup with every family/model/stepping at the edges of a range, and every possible 16-bit GPU device ID (the table must never
// read out of range, and an ID is either one architecture or Unknown).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Hardware/CentralProcessingUnit/Intel/IntelGenerations.hpp"
#include "Hardware/CentralProcessingUnit/X86_64Cpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaEngineHw.hpp"
#include <vector>

using namespace hardware;

// The CPU classes call these assembly routines; the identity tests never do.
extern "C" {
void tcos_halt() {}
void tcos_pause() {}
void tcos_breakpoint() {}
void tcos_disable_interrupts() {}
void tcos_enable_interrupts() {}
uint64_t tcos_rdtsc() { return 0; }
}

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static const char* cpu_name(uint32_t family, uint32_t model, uint32_t stepping) {
  alignas(X86_64Cpu) unsigned char storage[sizeof(X86_64Cpu)];
  ICentralProcessingUnit* cpu = create_intel_generation(storage, nullptr, {family, model, stepping});
  return cpu ? cpu->name() : nullptr;
}

static bool cpu_is(uint32_t family, uint32_t model, uint32_t stepping, const char* expected) {
  const char* n = cpu_name(family, model, stepping);
  return n && strcmp(n, expected) == 0;
}

static void test_cpu() {
  CHECK(cpu_is(6, 0x9E, 0xC, "Intel Coffee Lake Refresh") && cpu_is(6, 0x9E, 0xD, "Intel Coffee Lake Refresh"));
  CHECK(cpu_name(6, 0x9E, 0xB) == nullptr);  // 8th gen: not a generation this table names
  CHECK(cpu_is(6, 0x8E, 0xC, "Intel Comet Lake") && cpu_is(6, 0xA5, 0x2, "Intel Comet Lake") && cpu_is(6, 0xA6, 0x0, "Intel Comet Lake"));
  CHECK(cpu_is(6, 0x7E, 0x5, "Intel Ice Lake (client)") && cpu_is(6, 0x7D, 0x1, "Intel Ice Lake (client)"));
  CHECK(cpu_is(6, 0x6A, 0x6, "Intel Ice Lake-SP"));  // the Xeon keeps its own model
  CHECK(cpu_is(6, 0x8C, 0x1, "Intel Tiger Lake") && cpu_is(6, 0x8D, 0x1, "Intel Tiger Lake"));
  CHECK(cpu_is(6, 0xA7, 0x1, "Intel Rocket Lake"));
  CHECK(cpu_is(6, 0x97, 0x2, "Intel Alder Lake") && cpu_is(6, 0x9A, 0x3, "Intel Alder Lake"));
  CHECK(cpu_is(6, 0xBE, 0x0, "Intel Alder Lake-N / Twin Lake"));
  CHECK(cpu_is(6, 0xB7, 0x1, "Intel Raptor Lake") && cpu_is(6, 0xBA, 0x2, "Intel Raptor Lake") && cpu_is(6, 0xBF, 0x5, "Intel Raptor Lake"));
  CHECK(cpu_is(6, 0xAA, 0x4, "Intel Meteor Lake") && cpu_is(6, 0xAC, 0x1, "Intel Meteor Lake"));
  CHECK(cpu_is(6, 0xC6, 0x2, "Intel Arrow Lake") && cpu_is(6, 0xC5, 0x1, "Intel Arrow Lake") && cpu_is(6, 0xB5, 0x0, "Intel Arrow Lake"));
  CHECK(cpu_is(6, 0xBD, 0x1, "Intel Lunar Lake"));
  CHECK(cpu_is(6, 0xCC, 0x0, "Intel Panther Lake") && cpu_is(6, 0xD5, 0x0, "Intel Wildcat Lake"));
  CHECK(cpu_is(18, 0x01, 0x0, "Intel Nova Lake") && cpu_is(18, 0x03, 0x0, "Intel Nova Lake"));
  CHECK(cpu_is(6, 0x96, 0x1, "Intel Tremont") && cpu_is(6, 0x9C, 0x0, "Intel Tremont") && cpu_is(6, 0x8A, 0x1, "Intel Lakefield"));
  // Models the table must keep: the older generations and the neighbouring server parts did not move.
  CHECK(cpu_is(6, 0x5E, 0x3, "Intel Skylake (client)") && cpu_is(6, 0xAF, 0x1, "Intel Sierra Forest") && cpu_is(19, 0x01, 0x0, "Intel Diamond Rapids"));
  // Nothing outside the Intel families matches, and every signature is answered without reading out of range.
  CHECK(cpu_name(5, 0xA7, 0x1) == nullptr && cpu_name(0xF, 0xA7, 0x1) == nullptr);
  for (uint32_t family = 0; family < 32; ++family)
    for (uint32_t model = 0; model < 256; ++model)
      for (uint32_t stepping = 0; stepping < 16; ++stepping) (void)cpu_name(family, model, stepping);
}

static void test_gpu() {
  using A = IntelGpuArchitecture;
  CHECK(intel_gpu_identify(0x3E92).architecture == A::Gen9_5 && intel_gpu_identify(0x9BC8).architecture == A::Gen9_5);
  CHECK(intel_gpu_identify(0x8A52).architecture == A::Gen11 && intel_gpu_identify(0x4E55).architecture == A::Gen11);
  CHECK(intel_gpu_identify(0x9A49).architecture == A::XeLp && intel_gpu_identify(0x4C8A).architecture == A::XeLp);
  CHECK(intel_gpu_identify(0x4680).architecture == A::XeLp && intel_gpu_identify(0xA780).architecture == A::XeLp);
  CHECK(intel_gpu_identify(0x56A0).architecture == A::XeHpg && intel_gpu_identify(0x56A0).discrete);
  CHECK(intel_gpu_identify(0x7D55).architecture == A::XeLpg && !intel_gpu_identify(0x7D55).discrete);
  CHECK(intel_gpu_identify(0x64A0).architecture == A::Xe2 && intel_gpu_identify(0xE20B).architecture == A::Xe2 && intel_gpu_identify(0xE20B).discrete);
  CHECK(intel_gpu_identify(0xB080).architecture == A::Xe3);
  CHECK(intel_gpu_identify(0x0BD5).architecture == A::XeHpc);
  CHECK(intel_gpu_identify(0x0000).architecture == A::Unknown && intel_gpu_identify(0xFFFF).architecture == A::Unknown);
  // Media engine: AV1 decode arrives with Xe-LP, AV1 encode with Arc.
  CHECK(!intel_gpu_identify(0x3E92).media_av1_decode && intel_gpu_identify(0x9A49).media_av1_decode && !intel_gpu_identify(0x9A49).media_av1_encode);
  CHECK(intel_gpu_identify(0x56A0).media_av1_encode && intel_gpu_identify(0xE20B).media_av1_encode);
  // Every device ID gives a name and an architecture/name pair that agree.
  for (uint32_t id = 0; id <= 0xFFFF; ++id) {
    const IntelGpuInfo info = intel_gpu_identify(static_cast<uint16_t>(id));
    CHECK(info.name != nullptr && (info.architecture != A::Unknown || (!info.discrete && !info.media_hevc && !info.media_vp9)));
  }
}

// A pretend register BAR: plain memory the test fills with what the hardware would show.
struct FakeRegisters {
  std::vector<uint32_t> words = std::vector<uint32_t>(IntelMediaEngineHw::kRegisterWindow / 4, 0);
  MmioRegion region() { return MmioRegion(reinterpret_cast<uint64_t>(words.data()), words.size() * 4); }
  uint32_t& at(uint32_t offset) { return words[offset / 4]; }
};

static void test_media_engine() {
  using A = IntelGpuArchitecture;
  CHECK(IntelMediaEngineHw::supported(A::Gen9_5) && IntelMediaEngineHw::supported(A::XeLp));
  CHECK(IntelMediaEngineHw::supported(A::XeHpg) && IntelMediaEngineHw::supported(A::XeLpg) && IntelMediaEngineHw::supported(A::Xe2));
  CHECK(IntelMediaEngineHw::supported(A::Xe3) && !IntelMediaEngineHw::supported(A::XeHpc) && !IntelMediaEngineHw::supported(A::Unknown));
  // The register block starts at the BAR for most generations and 0x380000 into it on a standalone media tile; the window to map covers the block.
  CHECK(IntelMediaEngineHw::register_base(A::XeHpg) == 0 && IntelMediaEngineHw::register_base(A::Gen9_5) == 0);
  CHECK(IntelMediaEngineHw::register_base(A::XeLpg) == 0x380000 && IntelMediaEngineHw::register_base(A::Xe2) == 0x380000 && IntelMediaEngineHw::register_base(A::Xe3) == 0x380000);
  CHECK(IntelMediaEngineHw::register_window(A::XeLp) == 0x20000 && IntelMediaEngineHw::register_window(A::Xe2) == 0x3A0000 && IntelMediaEngineHw::register_window(A::Xe3) == 0x3A0000 && IntelMediaEngineHw::register_window(A::XeHpc) == 0);

  // Gen9.5: one box; it acknowledges when the ack bit is set, and the request is withdrawn afterwards.
  FakeRegisters g9;
  g9.at(0x0D88) = 1;
  IntelMediaEngineStatus s = IntelMediaEngineHw::probe(g9.region(), A::Gen9_5);
  CHECK(s.probed && s.vdbox_present == 1 && s.vdbox_awake == 1 && s.vdbox_count() == 1);
  CHECK(g9.at(0xA270) == 0x00010000);
  g9.at(0x0D88) = 0;  // never acknowledges
  s = IntelMediaEngineHw::probe(g9.region(), A::Gen9_5);
  CHECK(s.probed && s.vdbox_present == 1 && s.vdbox_awake == 0 && g9.at(0xA270) == 0x00010000);

  // Xe-LP: boxes 0 and 2 exist; the fuse can disable one of them; box 1 and 3 are never touched.
  FakeRegisters lp;
  lp.at(0x0D50) = lp.at(0x0D58) = 1;
  s = IntelMediaEngineHw::probe(lp.region(), A::XeLp);
  CHECK(s.vdbox_present == 0x5 && s.vdbox_awake == 0x5 && s.vdbox_count() == 2);
  CHECK(lp.at(0xA540) == 0x00010000 && lp.at(0xA548) == 0x00010000 && lp.at(0xA544) == 0 && lp.at(0xA54C) == 0);
  lp.at(0x9140) = 0x4;  // VCS2 fused off
  s = IntelMediaEngineHw::probe(lp.region(), A::XeLp);
  CHECK(s.vdbox_present == 0x1 && s.vdbox_awake == 0x1);
  lp.at(0x9140) = 0xFFFFFF05;  // bits outside the fuse field are ignored
  s = IntelMediaEngineHw::probe(lp.region(), A::XeLp);
  CHECK(s.vdbox_present == 0 && s.vdbox_count() == 0);

  // Unsupported generations and short windows read nothing.
  // Arc Alchemist (Xe-HPG) reads like Xe-LP. The standalone media tile (Meteor Lake and Arrow Lake graphics, Lunar Lake, Arc Battlemage, Panther Lake) is the same
  // block 0x380000 into the BAR: a BAR that ends before it is refused, and nothing outside the tile is touched.
  FakeRegisters hpg;
  hpg.at(0x0D50) = hpg.at(0x0D58) = 1;
  s = IntelMediaEngineHw::probe(hpg.region(), A::XeHpg);
  CHECK(s.probed && s.vdbox_present == 0x5 && s.vdbox_awake == 0x5);
  CHECK(!IntelMediaEngineHw::probe(lp.region(), A::XeLpg).probed && !IntelMediaEngineHw::probe(lp.region(), A::Xe2).probed && !IntelMediaEngineHw::probe(lp.region(), A::Xe3).probed);   // BAR too short for the tile
  CHECK(!IntelMediaEngineHw::probe(lp.region(), A::XeHpc).probed);
  for (A tile : {A::XeLpg, A::Xe2, A::Xe3}) {
    std::vector<uint32_t> bar(IntelMediaEngineHw::register_window(tile) / 4, 0);
    const uint64_t base = IntelMediaEngineHw::kMediaTileBase;
    bar[(base + 0x0D50) / 4] = bar[(base + 0x0D58) / 4] = 1;   // both boxes acknowledge
    const MmioRegion region(reinterpret_cast<uint64_t>(bar.data()), bar.size() * 4);
    s = IntelMediaEngineHw::probe(region, tile);
    CHECK(s.probed && s.vdbox_present == 0x5 && s.vdbox_awake == 0x5 && s.vdbox_count() == 2);
    CHECK(bar[(base + 0xA540) / 4] == 0x00010000 && bar[(base + 0xA548) / 4] == 0x00010000 && bar[0xA540 / 4] == 0);   // requests withdrawn; the GT half untouched
    bar[(base + 0x9140) / 4] = 0x4;   // VCS2 fused off
    s = IntelMediaEngineHw::probe(region, tile);
    CHECK(s.vdbox_present == 0x1 && s.vdbox_awake == 0x1);
    bar[(base + 0x0D50) / 4] = 0;     // the remaining box never wakes
    s = IntelMediaEngineHw::probe(region, tile);
    CHECK(s.probed && s.vdbox_awake == 0 && bar[(base + 0xA540) / 4] == 0x00010000);
  }
  CHECK(!IntelMediaEngineHw::probe(MmioRegion(), A::XeLp).probed);
  alignas(4) static uint32_t small[16] = {};
  CHECK(!IntelMediaEngineHw::probe(MmioRegion(reinterpret_cast<uint64_t>(small), sizeof small), A::XeLp).probed);
}

int main() {
  test_cpu();
  test_gpu();
  test_media_engine();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("IntelGenerationTest passed\n");
  return 0;
}
