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
// Host-side tests for the kernel's DXVA decoders (src/Hardware/VideoCodec/Dxva): the test plays the program that drives an ID3D11VideoDecoder. It parses real streams of
// tests/data, fills the DXVA structures the way ffmpeg's libavcodec/dxva2_*.c does (that is what real programs send), decodes each picture through create_dxva_decoder()
// and compares it with a reference decode. Damaged buffers (truncated, bits flipped, indices out of range, buffers missing) must give Invalid or a picture, never a fault.
// Each codec's cases are in their own file (DxvaDecoderTest<Codec>.inc), included below.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Hardware/VideoCodec/Dxva/DxvaDecoder.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al + al);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

[[maybe_unused]] static std::vector<uint8_t> read_file(const std::string& path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return bytes;
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

// Codecs whose creators live in files another build may not have yet: weak stand-ins that make no decoder.
namespace hardware {
__attribute__((weak)) UniquePointer<IDxvaDecoder> create_dxva_hevc(allocators::IAllocator&, const DxvaDecoderConfig&) { return {}; }
__attribute__((weak)) UniquePointer<IDxvaDecoder> create_dxva_av1(allocators::IAllocator&, const DxvaDecoderConfig&) { return {}; }
}  // namespace hardware

// (Building with -DDXVA_TEST_ONLY_<CODEC> compiles that codec's cases alone, for a quick loop on one decoder.)
#if !defined(DXVA_TEST_ONLY) || defined(DXVA_TEST_ONLY_H264)
#include "DxvaDecoderTestH264.inc"
#else
static void dxva_test_h264() {}
#endif
#if !defined(DXVA_TEST_ONLY) || defined(DXVA_TEST_ONLY_MPEG2)
#include "DxvaDecoderTestMpeg2.inc"
#else
static void dxva_test_mpeg2() {}
#endif
#if !defined(DXVA_TEST_ONLY) || defined(DXVA_TEST_ONLY_VP9)
#include "DxvaDecoderTestVp9.inc"
#else
static void dxva_test_vp9() {}
#endif
#if !defined(DXVA_TEST_ONLY) || defined(DXVA_TEST_ONLY_HEVC)
#include "DxvaDecoderTestHevc.inc"
#else
static void dxva_test_hevc() {}
#endif
#if !defined(DXVA_TEST_ONLY) || defined(DXVA_TEST_ONLY_AV1)
#include "DxvaDecoderTestAv1.inc"
#else
static void dxva_test_av1() {}
#endif
#if !defined(DXVA_TEST_ONLY) || defined(DXVA_TEST_ONLY_VC1)
#include "DxvaDecoderTestVc1.inc"
#else
static void dxva_test_vc1() {}
#endif
#if !defined(DXVA_TEST_ONLY) || defined(DXVA_TEST_ONLY_MPEG4)
#include "DxvaDecoderTestMpeg4.inc"
#else
static void dxva_test_mpeg4() {}
#endif

int main() {
  dxva_test_h264();
  dxva_test_mpeg2();
  dxva_test_vp9();
  dxva_test_hevc();
  dxva_test_av1();
  dxva_test_vc1();
  dxva_test_mpeg4();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("DxvaDecoderTest passed\n");
  return 0;
}
