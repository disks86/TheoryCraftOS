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

// Host-side tests for the NVIDIA GPU architecture table (PCI device IDs, and the chip ID in NV_PMC_BOOT_0) and the codec sets of each
// generation. Every 16-bit device ID and a sweep of boot0 values are classified (nothing may read out of range).
#include <stdio.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

using A = NvidiaGpuArchitecture;

static uint32_t boot0_of(uint32_t chipset) { return (chipset << 20) | 0xA1; }  // chipset in bits 28:20, some revision bits below

static void test_device_ids() {
  CHECK(nvidia_gpu_identify(0x1B80).architecture == A::Pascal && nvidia_gpu_identify(0x1C03).architecture == A::Pascal && nvidia_gpu_identify(0x1D01).architecture == A::Pascal);
  CHECK(nvidia_gpu_identify(0x1D81).architecture == A::Volta && nvidia_gpu_identify(0x1DB4).architecture == A::Volta);
  CHECK(nvidia_gpu_identify(0x1E04).architecture == A::Turing && nvidia_gpu_identify(0x1F82).architecture == A::Turing && nvidia_gpu_identify(0x2182).architecture == A::Turing);
  CHECK(nvidia_gpu_identify(0x2204).architecture == A::Ampere && nvidia_gpu_identify(0x2484).architecture == A::Ampere && nvidia_gpu_identify(0x2503).architecture == A::Ampere);
  CHECK(nvidia_gpu_identify(0x20B0).architecture == A::Ampere);
  CHECK(nvidia_gpu_identify(0x2330).architecture == A::Hopper);
  CHECK(nvidia_gpu_identify(0x2684).architecture == A::AdaLovelace && nvidia_gpu_identify(0x2704).architecture == A::AdaLovelace && nvidia_gpu_identify(0x2882).architecture == A::AdaLovelace);
  CHECK(nvidia_gpu_identify(0x2B85).architecture == A::Blackwell && nvidia_gpu_identify(0x2C02).architecture == A::Blackwell && nvidia_gpu_identify(0x2901).architecture == A::Blackwell);
  CHECK(nvidia_gpu_identify(0x0000).architecture == A::Unknown && nvidia_gpu_identify(0xFFFF).architecture == A::Unknown && nvidia_gpu_identify(0x1000).architecture == A::Unknown);
}

static void test_boot0() {
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x132)).architecture == A::Pascal);   // GP102
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x140)).architecture == A::Volta);    // GV100
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x162)).architecture == A::Turing);   // TU102
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x172)).architecture == A::Ampere);   // GA102
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x180)).architecture == A::Hopper);   // GH100
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x192)).architecture == A::AdaLovelace);  // AD102
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x1A0)).architecture == A::Blackwell);    // GB100
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x1B2)).architecture == A::Blackwell);    // GB202
  // Older chips, a powered-down GPU (all ones) and empty registers are not named.
  CHECK(nvidia_gpu_identify_boot0(boot0_of(0x117)).architecture == A::Unknown && nvidia_gpu_identify_boot0(0xFFFFFFFFu).architecture == A::Unknown);
  CHECK(nvidia_gpu_identify_boot0(0).architecture == A::Unknown);
  // The device-ID table and the chip-ID decoder agree on the architecture of a representative card of each generation.
  CHECK(nvidia_gpu_identify(0x1B80).architecture == nvidia_gpu_identify_boot0(boot0_of(0x134)).architecture);
  CHECK(nvidia_gpu_identify(0x2684).architecture == nvidia_gpu_identify_boot0(boot0_of(0x192)).architecture);
  CHECK(nvidia_gpu_identify(0x2B85).architecture == nvidia_gpu_identify_boot0(boot0_of(0x1B2)).architecture);
  for (uint64_t v = 0; v <= 0xFFFFFFFFull; v += 0x10001) (void)nvidia_gpu_identify_boot0(static_cast<uint32_t>(v));
}

static void test_codecs() {
  const NvidiaGpuInfo pascal = nvidia_gpu_info_for(A::Pascal), turing = nvidia_gpu_info_for(A::Turing), ampere = nvidia_gpu_info_for(A::Ampere);
  const NvidiaGpuInfo hopper = nvidia_gpu_info_for(A::Hopper), ada = nvidia_gpu_info_for(A::AdaLovelace), blackwell = nvidia_gpu_info_for(A::Blackwell);
  CHECK(pascal.media_hevc && pascal.media_vp9 && pascal.has_nvenc && !pascal.media_av1_decode && !pascal.media_av1_encode);
  CHECK(turing.media_hevc && turing.has_nvenc && !turing.media_av1_decode);
  CHECK(ampere.media_av1_decode && ampere.has_nvenc && !ampere.media_av1_encode);   // AV1 decode arrives with Ampere, AV1 encode with Ada
  CHECK(hopper.media_av1_decode && !hopper.has_nvenc && !hopper.media_av1_encode);
  CHECK(ada.media_av1_encode && blackwell.media_av1_encode && ada.has_nvenc);
  const NvidiaGpuInfo none = nvidia_gpu_info_for(A::Unknown);
  CHECK(!none.media_hevc && !none.media_vp9 && !none.media_av1_decode && !none.has_nvenc && !nvidia_gpu_has_video_engine(A::Unknown) && nvidia_gpu_has_video_engine(A::Pascal));
  // Every device ID gives a name; the flags agree with the architecture; AV1 encode implies NVENC.
  for (uint32_t id = 0; id <= 0xFFFF; ++id) {
    const NvidiaGpuInfo info = nvidia_gpu_identify(static_cast<uint16_t>(id));
    CHECK(info.name != nullptr && (info.architecture != A::Unknown || (!info.media_hevc && !info.has_nvenc)));
    CHECK(!info.media_av1_encode || info.has_nvenc);
  }
}

int main() {
  test_device_ids();
  test_boot0();
  test_codecs();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("NvidiaGenerationTest passed\n");
  return 0;
}
