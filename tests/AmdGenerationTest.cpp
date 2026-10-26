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

// Host-side tests for the AMD CPU generation rules (Ryzen and newer), the AMD GPU architecture table and the NPU generation ids. Every CPU signature and every possible 16-bit GPU device ID is looked up (nothing may read out of range).
#include <stdio.h>
#include <string.h>
#include "Hardware/CentralProcessingUnit/AMD/AmdGenerations.hpp"
#include "Hardware/CentralProcessingUnit/X86_64Cpu.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaNpu.hpp"

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
  ICentralProcessingUnit* cpu = create_amd_generation(storage, nullptr, {family, model, stepping});
  return cpu ? cpu->name() : nullptr;
}

static bool cpu_is(uint32_t family, uint32_t model, uint32_t stepping, const char* expected) {
  const char* n = cpu_name(family, model, stepping);
  return n && strcmp(n, expected) == 0;
}

static void test_cpu() {
  CHECK(cpu_is(0x17, 0x01, 0x1, "AMD Zen") && cpu_is(0x17, 0x11, 0x0, "AMD Zen") && cpu_is(0x17, 0x20, 0x1, "AMD Zen"));
  CHECK(cpu_is(0x17, 0x08, 0x2, "AMD Zen+") && cpu_is(0x17, 0x18, 0x1, "AMD Zen+"));
  CHECK(cpu_is(0x17, 0x31, 0x0, "AMD Zen 2") && cpu_is(0x17, 0x60, 0x1, "AMD Zen 2") && cpu_is(0x17, 0xA0, 0x0, "AMD Zen 2"));
  CHECK(cpu_is(0x19, 0x01, 0x2, "AMD Zen 3") && cpu_is(0x19, 0x21, 0x0, "AMD Zen 3") && cpu_is(0x19, 0x50, 0x0, "AMD Zen 3"));
  CHECK(cpu_is(0x19, 0x11, 0x1, "AMD Zen 4") && cpu_is(0x19, 0x61, 0x2, "AMD Zen 4") && cpu_is(0x19, 0xA0, 0x0, "AMD Zen 4"));
  CHECK(cpu_is(0x1A, 0x02, 0x0, "AMD Zen 5") && cpu_is(0x1A, 0x44, 0x0, "AMD Zen 5") && cpu_is(0x1A, 0x70, 0x0, "AMD Zen 5"));
  // Older families keep their classes; QEMU's qemu64 (family 15, model 0x6B) is not named here.
  CHECK(cpu_is(0x0F, 0x04, 0x0, "AMD K8") && cpu_is(0x10, 0x04, 0x0, "AMD K10") && cpu_is(0x15, 0x01, 0x0, "AMD Bulldozer"));
  CHECK(cpu_name(0x0F, 0x6B, 0x1) == nullptr);
  // Nothing outside the AMD families matches, and every signature is answered without reading out of range.
  CHECK(cpu_name(6, 0x9E, 0xC) == nullptr && cpu_name(0x18, 0x00, 0x0) == nullptr);
  for (uint32_t family = 0; family < 32; ++family)
    for (uint32_t model = 0; model < 256; ++model)
      for (uint32_t stepping = 0; stepping < 16; ++stepping) (void)cpu_name(family, model, stepping);
}

static void test_gpu() {
  using A = AmdGpuArchitecture;
  CHECK(amd_gpu_identify(0x67DF).architecture == A::Polaris && amd_gpu_identify(0x699F).architecture == A::Polaris);
  // Radeon RX Vega M GH/GL (Kaby Lake-G, Dell XPS 15 9575): a Polaris-family chip (amdgpu: VEGAM) with its own HBM2, UVD 6.3 so HEVC but no VP9.
  CHECK(amd_gpu_identify(0x694C).architecture == A::Polaris && amd_gpu_identify(0x694E).architecture == A::Polaris && amd_gpu_identify(0x694F).discrete);
  CHECK(amd_gpu_identify(0x694C).media_hevc && !amd_gpu_identify(0x694C).media_vp9 && amd_gpu_identify(0x694B).architecture == A::Unknown && amd_gpu_identify(0x6950).architecture == A::Unknown);
  CHECK(amd_gpu_identify(0x687F).architecture == A::Vega && amd_gpu_identify(0x66AF).architecture == A::Vega && amd_gpu_identify(0x687F).discrete);
  CHECK(amd_gpu_identify(0x15DD).architecture == A::Vega && !amd_gpu_identify(0x15DD).discrete);
  CHECK(amd_gpu_identify(0x1636).architecture == A::Vega && amd_gpu_identify(0x1638).architecture == A::Vega);
  CHECK(amd_gpu_identify(0x731F).architecture == A::Rdna1 && amd_gpu_identify(0x7340).architecture == A::Rdna1);
  CHECK(amd_gpu_identify(0x73BF).architecture == A::Rdna2 && amd_gpu_identify(0x73FF).architecture == A::Rdna2 && amd_gpu_identify(0x743F).architecture == A::Rdna2);
  CHECK(amd_gpu_identify(0x1681).architecture == A::Rdna2 && !amd_gpu_identify(0x1681).discrete && amd_gpu_identify(0x164E).architecture == A::Rdna2);
  CHECK(amd_gpu_identify(0x744C).architecture == A::Rdna3 && amd_gpu_identify(0x747E).architecture == A::Rdna3 && amd_gpu_identify(0x15BF).architecture == A::Rdna3);
  CHECK(amd_gpu_identify(0x150E).architecture == A::Rdna3_5 && amd_gpu_identify(0x1586).architecture == A::Rdna3_5);
  CHECK(amd_gpu_identify(0x7550).architecture == A::Rdna4 && amd_gpu_identify(0x7590).architecture == A::Rdna4 && amd_gpu_identify(0x7550).discrete);
  CHECK(amd_gpu_identify(0x74A1).architecture == A::Cdna);
  CHECK(amd_gpu_identify(0x0000).architecture == A::Unknown && amd_gpu_identify(0xFFFF).architecture == A::Unknown);
  // Video engine: AV1 decode arrives with RDNA 2 (not the entry-level Navi 24), AV1 encode with RDNA 3; Polaris has no VP9.
  CHECK(!amd_gpu_identify(0x67DF).media_vp9 && amd_gpu_identify(0x67DF).media_hevc);
  CHECK(!amd_gpu_identify(0x731F).media_av1_decode && amd_gpu_identify(0x73BF).media_av1_decode && !amd_gpu_identify(0x73BF).media_av1_encode);
  CHECK(!amd_gpu_identify(0x743F).media_av1_decode);
  CHECK(amd_gpu_identify(0x744C).media_av1_encode && amd_gpu_identify(0x7550).media_av1_encode);
  CHECK(!amd_gpu_identify(0x74A1).media_hevc && !amd_gpu_has_video_engine(A::Cdna) && !amd_gpu_has_video_engine(A::Unknown) && amd_gpu_has_video_engine(A::Rdna3));
  // Every device ID gives a name, and the table has no overlapping ranges: the flags agree with the architecture.
  for (uint32_t id = 0; id <= 0xFFFF; ++id) {
    const AmdGpuInfo info = amd_gpu_identify(static_cast<uint16_t>(id));
    CHECK(info.name != nullptr && (info.architecture != A::Unknown || (!info.discrete && !info.media_hevc && !info.media_vp9)));
    CHECK(amd_gpu_has_video_engine(info.architecture) || (!info.media_hevc && !info.media_vp9 && !info.media_av1_decode && !info.media_av1_encode));
  }
}

static void test_npu() {
  CHECK(AmdXdnaNpu::xdna_generation(0x1502) == 1 && AmdXdnaNpu::xdna_generation(0x17F0) == 2 && AmdXdnaNpu::xdna_generation(0x1234) == 0);
  CHECK(AmdXdnaNpu::platform(0x1502) && AmdXdnaNpu::platform(0x17F0) && !AmdXdnaNpu::platform(0x1234) && strcmp(AmdXdnaNpu::platform(0x17F0), "XDNA 2, Ryzen AI 300") == 0);
  CHECK(AmdXdnaNpu::matches(0x1502) && AmdXdnaNpu::matches(0x17F0) && !AmdXdnaNpu::matches(0));
}

int main() {
  test_cpu();
  test_gpu();
  test_npu();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdGenerationTest passed\n");
  return 0;
}
