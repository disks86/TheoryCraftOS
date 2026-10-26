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

// Host-side tests for the P010 pixel format (10-bit NV12, each sample the top ten bits of a little-endian 16-bit word): the format's sizes, and the conversions to and
// from the 8-bit formats (an 8-bit picture widened to P010 and brought back is unchanged; a P010 picture loses its two low bits going to 8 bits).
#include <stdio.h>
#include <stdlib.h>
#include "Hardware/VideoCodec/ColorConvert.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static void test_sizes() {
  VideoFormat f{33, 21, PixelFormat::P010};
  CHECK(f.valid() && f.plane_count() == 2);
  CHECK(f.plane_stride(0) == 66 && f.plane_height(0) == 21);
  CHECK(f.plane_stride(1) == 17 * 4 && f.plane_height(1) == 11);   // 17 chroma pairs of two 16-bit samples
  CHECK(f.frame_bytes() == 66u * 21 + 68u * 11);
  VideoFormat n{33, 21, PixelFormat::Nv12};
  CHECK(f.frame_bytes() == 2 * n.frame_bytes());
}

static void test_roundtrip() {
  TestAllocator a;
  {
    VideoFrameBuffer src(a), wide(a), back(a);
    CHECK(src.allocate({33, 21, PixelFormat::Nv12}) && wide.allocate({33, 21, PixelFormat::P010}) && back.allocate({33, 21, PixelFormat::Nv12}));
    for (uint32_t y = 0; y < 21; ++y)
      for (uint32_t x = 0; x < 33; ++x) src.plane(0)[uint64_t{y} * 33 + x] = static_cast<uint8_t>(x * 7 + y * 13);
    for (uint32_t y = 0; y < 11; ++y)
      for (uint32_t x = 0; x < 34; ++x) src.plane(1)[uint64_t{y} * 34 + x] = static_cast<uint8_t>(255 - x * 5 - y * 9);
    CHECK(convert_frame(src.frame(), wide) && convert_frame(wide.frame(), back));
    bool same = true;
    for (uint32_t i = 0; i < 33 * 21; ++i) same = same && back.plane(0)[i] == src.plane(0)[i];
    for (uint32_t i = 0; i < 34 * 11; ++i) same = same && back.plane(1)[i] == src.plane(1)[i];
    CHECK(same);
    // The widened samples are ten-bit values in the top of the word: 8-bit 0 is 0, 255 is 1023 << 6 = 0xFFC0.
    src.plane(0)[0] = 0;
    src.plane(0)[1] = 255;
    src.plane(0)[2] = 128;
    CHECK(convert_frame(src.frame(), wide));
    const uint8_t* y = wide.plane(0).data();
    CHECK(y[0] == 0x00 && y[1] == 0x00 && y[2] == 0xC0 && y[3] == 0xFF);
    CHECK(y[4] == 0x80 && y[5] == 0x80 && (y[4] | (y[5] << 8)) == (((128 << 2) | (128 >> 6)) << 6));   // 128 is 514 << 6 = 0x8080
  }
  {  // A real ten-bit picture to eight bits: the top eight bits are kept.
    VideoFrameBuffer ten(a), eight(a), i420(a);
    CHECK(ten.allocate({8, 4, PixelFormat::P010}) && eight.allocate({8, 4, PixelFormat::Nv12}) && i420.allocate({8, 4, PixelFormat::I420}));
    for (uint32_t i = 0; i < 8 * 4; ++i) {
      const uint16_t v = static_cast<uint16_t>((i * 31 % 1024) << 6);
      ten.plane(0)[i * 2] = static_cast<uint8_t>(v);
      ten.plane(0)[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    for (uint32_t i = 0; i < 8 * 2; ++i) {   // 4 chroma pairs per row, 2 rows: 16 samples
      const uint16_t v = static_cast<uint16_t>(((i * 61 + 3) % 1024) << 6);
      ten.plane(1)[i * 2] = static_cast<uint8_t>(v);
      ten.plane(1)[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    CHECK(convert_frame(ten.frame(), eight) && convert_frame(ten.frame(), i420));
    bool ok = true;
    for (uint32_t i = 0; i < 8 * 4; ++i) ok = ok && eight.plane(0)[i] == ((i * 31 % 1024) >> 2) && i420.plane(0)[i] == eight.plane(0)[i];
    for (uint32_t i = 0; i < 8 * 2; i += 2) {
      const uint32_t cx = (i / 2) % 4, cy = (i / 2) / 4;
      ok = ok && eight.plane(1)[cy * 8 + cx * 2] == (((i * 61 + 3) % 1024) >> 2) && eight.plane(1)[cy * 8 + cx * 2 + 1] == ((((i + 1) * 61 + 3) % 1024) >> 2);
      ok = ok && i420.plane(1)[cy * 4 + cx] == eight.plane(1)[cy * 8 + cx * 2] && i420.plane(2)[cy * 4 + cx] == eight.plane(1)[cy * 8 + cx * 2 + 1];
    }
    CHECK(ok);
    // To pixels: a grey ten-bit picture is the same grey in BGRA as its 8-bit version.
    VideoFrameBuffer grey10(a), grey8(a), bgra10(a), bgra8(a);
    CHECK(grey10.allocate({4, 4, PixelFormat::P010}) && grey8.allocate({4, 4, PixelFormat::Nv12}) && bgra10.allocate({4, 4, PixelFormat::Bgra8888}) && bgra8.allocate({4, 4, PixelFormat::Bgra8888}));
    for (uint32_t i = 0; i < 16; ++i) { grey10.plane(0)[i * 2] = 0x40; grey10.plane(0)[i * 2 + 1] = 0x80; grey8.plane(0)[i] = 0x80; }   // 0x8040 is 513 << 6: 8 bits 0x80
    for (uint32_t i = 0; i < 8; ++i) { grey10.plane(1)[i * 2] = 0x00; grey10.plane(1)[i * 2 + 1] = 0x80; grey8.plane(1)[i] = 0x80; }
    CHECK(convert_frame(grey10.frame(), bgra10) && convert_frame(grey8.frame(), bgra8));
    bool same = true;
    for (uint32_t i = 0; i < 16 * 4; ++i) same = same && bgra10.plane(0)[i] == bgra8.plane(0)[i];
    CHECK(same);
    // Bgra8888 into P010 (through the 8-bit arithmetic) and back.
    VideoFrameBuffer p(a), q(a);
    CHECK(p.allocate({4, 4, PixelFormat::P010}) && q.allocate({4, 4, PixelFormat::Nv12}) && convert_frame(bgra8.frame(), p) && convert_frame(bgra8.frame(), q));
    bool match = true;
    for (uint32_t i = 0; i < 16; ++i) match = match && p.plane(0)[i * 2 + 1] == q.plane(0)[i];
    CHECK(match);
  }
}

static void test_same_format() {
  TestAllocator a;
  VideoFrameBuffer src(a), dst(a);
  CHECK(src.allocate({6, 4, PixelFormat::P010}) && dst.allocate({6, 4, PixelFormat::P010}));
  for (uint32_t p = 0; p < 2; ++p)
    for (uint32_t i = 0; i < src.format().plane_bytes(p); ++i) src.plane(p)[i] = static_cast<uint8_t>(i * 37 + p * 11 + 5);
  CHECK(convert_frame(src.frame(), dst));
  bool same = true;
  for (uint32_t p = 0; p < 2; ++p)
    for (uint32_t i = 0; i < src.format().plane_bytes(p); ++i) same = same && src.plane(p)[i] == dst.plane(p)[i];
  CHECK(same);   // (all ten bits of every sample, not the eight a trip through the 8-bit formats keeps)
}

int main() {
  test_same_format();
  test_sizes();
  test_roundtrip();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("p010 tests passed\n");
  return 0;
}
