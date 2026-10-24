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

// Host-side tests for the video codec layer: frame formats, colour conversion, the baseline JPEG decoder and
// encoder, the software codec sessions (MJPEG and raw) and the device list.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H264Encoder.hpp"
#include "Hardware/VideoCodec/Software/H264Parser.hpp"
#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Hardware/VideoCodec/Software/Vp8.hpp"
#include "Hardware/VideoCodec/Software/Av1.hpp"
#include "Hardware/VideoCodec/Software/Vp9.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Hardware/VideoCodec/AMD/AmdMediaEngineHw.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVideoEngine.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/NVIDIA/NvidiaVideoEngine.hpp"
#include <vector>
#include "Utility/Crc32.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// A real 24 x 16 baseline JPEG (4:4:4, quality 90, the standard Huffman tables) made by another encoder. Its
// decoded pixels, as 24 x 16 RGB triples, have this CRC-32.
static const uint8_t kSmallJpeg[] = {
0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03,
0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05, 0x05, 0x04, 0x04, 0x05, 0x0A, 0x07,
0x07, 0x06, 0x08, 0x0C, 0x0A, 0x0C, 0x0C, 0x0B, 0x0A, 0x0B, 0x0B, 0x0D, 0x0E, 0x12, 0x10, 0x0D,
0x0E, 0x11, 0x0E, 0x0B, 0x0B, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0C, 0x0F,
0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x03, 0x04,
0x04, 0x05, 0x04, 0x05, 0x09, 0x05, 0x05, 0x09, 0x14, 0x0D, 0x0B, 0x0D, 0x14, 0x14, 0x14, 0x14,
0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0xFF, 0xC0,
0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x18, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
0x01, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23,
0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17,
0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5,
0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1,
0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xC4, 0x00, 0x1F, 0x01, 0x00, 0x03,
0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x11, 0x00,
0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15,
0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27,
0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2,
0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9,
0xFA, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xF8,
0xDF, 0x44, 0xF8, 0x55, 0xFD, 0x9D, 0xB7, 0xF7, 0x3E, 0x66, 0xFF, 0x00, 0x6C, 0x63, 0x1F, 0xFE,
0xBA, 0xFA, 0xFC, 0x46, 0x71, 0xFE, 0xAD, 0xF5, 0xF6, 0x9E, 0xD3, 0xFE, 0xDD, 0xB7, 0x2F, 0xDF,
0x7B, 0xDF, 0xCB, 0x63, 0xE7, 0x72, 0x8C, 0xFF, 0x00, 0xDB, 0x5B, 0x5B, 0x58, 0xEF, 0xF4, 0x4F,
0x85, 0x3F, 0xD9, 0xFB, 0x7F, 0x73, 0xE6, 0x6F, 0xF6, 0xC6, 0x31, 0xFF, 0x00, 0xEB, 0xAF, 0x9B,
0xC4, 0x67, 0x1F, 0xEA, 0xDF, 0x5F, 0x69, 0xED, 0x3F, 0xED, 0xDB, 0x72, 0xFD, 0xF7, 0xBD, 0xFC,
0xB6, 0x3F, 0x6B, 0xCA, 0x33, 0xFF, 0x00, 0x6D, 0x6D, 0x6D, 0x63, 0xD0, 0x34, 0x4F, 0x85, 0x3F,
0xD9, 0xFB, 0x7F, 0x73, 0xE6, 0x6F, 0xF6, 0xC6, 0x31, 0xFF, 0x00, 0xEB, 0xAF, 0x9B, 0xC4, 0x67,
0x1F, 0xEA, 0xDF, 0x5F, 0x69, 0xED, 0x3F, 0xED, 0xDB, 0x72, 0xFD, 0xF7, 0xBD, 0xFC, 0xB6, 0x3F,
0x6A, 0xCA, 0x33, 0xFF, 0x00, 0x6D, 0x6D, 0x6D, 0x63, 0xD2, 0x74, 0x4F, 0x85, 0x5F, 0xD9, 0xFB,
0x7F, 0x75, 0xE6, 0x6F, 0xF6, 0xC6, 0x31, 0xFF, 0x00, 0xEB, 0xAF, 0x96, 0xC4, 0x67, 0x1F, 0xEA,
0xDF, 0x5F, 0x69, 0xED, 0x3F, 0xED, 0xDB, 0x72, 0xFD, 0xF7, 0xBD, 0xFC, 0xB6, 0x3F, 0xC9, 0xBC,
0xA3, 0x3E, 0xF6, 0xD6, 0xD6, 0xD6, 0x3D, 0x03, 0x44, 0xF8, 0x55, 0xFD, 0x9F, 0xB7, 0xF7, 0x5E,
0x66, 0xFF, 0x00, 0x6C, 0x63, 0x1F, 0xFE, 0xBA, 0xF9, 0xBC, 0x46, 0x71, 0xFE, 0xAD, 0xF5, 0xF6,
0x9E, 0xD3, 0xFE, 0xDD, 0xB7, 0x2F, 0xDF, 0x7B, 0xDF, 0xCB, 0x63, 0xF6, 0xBC, 0xA3, 0x3E, 0xF6,
0xD6, 0xD6, 0xD6, 0x3D, 0x03, 0x44, 0xF8, 0x55, 0xFD, 0x9F, 0xB7, 0xF7, 0x3E, 0x66, 0xFF, 0x00,
0x6C, 0x63, 0x1F, 0xFE, 0xBA, 0xF9, 0xBC, 0x46, 0x71, 0xFE, 0xAD, 0xF5, 0xF6, 0x9E, 0xD3, 0xFE,
0xDD, 0xB7, 0x2F, 0xDF, 0x7B, 0xDF, 0xCB, 0x63, 0xF6, 0xAC, 0xA3, 0x3E, 0xF6, 0xD6, 0xD6, 0xD6,
0x3F, 0xFF, 0xD9
};
static const uint32_t kSmallJpegCrc = 0x2cff05a9;

// A test picture: smooth colour gradients with a lighter ring and a darker patch (edges in the luma only, as chroma
// edges would be smeared by the 4:2:0 subsampling no matter how well the codec does).
static void make_picture(VideoFrameBuffer& b, uint32_t w, uint32_t h) {
  CHECK(b.allocate({w, h, PixelFormat::Bgra8888}));
  uint32_t* p = reinterpret_cast<uint32_t*>(b.plane(0).data());
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const int dx = (int)x - (int)w / 2, dy = (int)y - (int)h / 2;
      const int d2 = dx * dx + dy * dy;
      int r = 128 + (int)(100 * sin(x / 13.0)), g = 128 + (int)(100 * sin(y / 11.0)), bl = 128 + (int)(100 * sin((x + y) / 17.0));
      int lift = 0;
      if (d2 > (int)(w * h / 20) && d2 < (int)(w * h / 12)) lift = 60;
      if (x > w * 3 / 4 && y < h / 4) lift = -60;
      r += lift; g += lift; bl += lift;
      auto c = [](int v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
      p[y * w + x] = 0xFF000000 | (c(r) << 16) | (c(g) << 8) | c(bl);
    }
}

static double psnr_bgra(const VideoFrame& a, const VideoFrame& b) {
  double sum = 0;
  uint64_t n = 0;
  for (uint32_t y = 0; y < a.format.height; ++y)
    for (uint32_t x = 0; x < a.format.width; ++x)
      for (int c = 0; c < 3; ++c) {
        const int d = (int)a.plane[0][y * a.stride[0] + 4 * x + c] - (int)b.plane[0][y * b.stride[0] + 4 * x + c];
        sum += d * d;
        ++n;
      }
  const double mse = sum / (double)n;
  return mse == 0 ? 99 : 10 * log10(255.0 * 255.0 / mse);
}

static void test_formats() {
  VideoFormat f{5, 3, PixelFormat::I420};
  CHECK(f.valid() && f.plane_count() == 3);
  CHECK(f.plane_width(0) == 5 && f.plane_height(0) == 3 && f.plane_stride(0) == 5);
  CHECK(f.plane_width(1) == 3 && f.plane_height(1) == 2 && f.plane_stride(2) == 3);   // odd sizes round up
  CHECK(f.frame_bytes() == 5 * 3 + 2 * (3 * 2));
  VideoFormat n{5, 3, PixelFormat::Nv12};
  CHECK(n.plane_count() == 2 && n.plane_stride(1) == 6 && n.frame_bytes() == 15 + 12);
  VideoFormat b{5, 3, PixelFormat::Bgra8888};
  CHECK(b.plane_count() == 1 && b.frame_bytes() == 60 && b.plane_stride(0) == 20);
  CHECK(!VideoFormat{0, 3, PixelFormat::I420}.valid() && !VideoFormat{20000, 3, PixelFormat::I420}.valid());
  CHECK(strcmp(codec_name(VideoCodec::H264), "H.264") == 0 && codec_mask(VideoCodec::Mjpeg) == 2);

  TestAllocator a;
  {
    VideoFrameBuffer buf(a);
    CHECK(!buf.valid() && !buf.allocate({0, 0, PixelFormat::I420}));
    CHECK(buf.allocate(f) && buf.valid() && buf.plane(1).size() == 6 && buf.plane(3).empty());
    const VideoFrame frame = buf.frame(1234, false);
    CHECK(frame.pts_us == 1234 && !frame.keyframe && frame.plane[1] == buf.plane(1).data() && frame.stride[1] == 3);
    CHECK(buf.plane(2).data() == buf.plane(1).data() + 6);
  }
  CHECK(a.live == 0);
}

static void test_convert() {
  TestAllocator a;
  {
    VideoFrameBuffer bgra(a), yuv(a), back(a), nv(a);
    make_picture(bgra, 33, 21);
    CHECK(yuv.allocate({33, 21, PixelFormat::I420}) && convert_frame(bgra.frame(), yuv));
    CHECK(back.allocate({33, 21, PixelFormat::Bgra8888}) && convert_frame(yuv.frame(), back));
    CHECK(psnr_bgra(bgra.frame(), back.frame()) > 30);

    // I420 and NV12 hold the same samples.
    CHECK(nv.allocate({33, 21, PixelFormat::Nv12}) && convert_frame(yuv.frame(), nv));
    VideoFrameBuffer yuv2(a);
    CHECK(yuv2.allocate({33, 21, PixelFormat::I420}) && convert_frame(nv.frame(), yuv2));
    CHECK(memcmp(yuv.plane(0).data(), yuv2.plane(0).data(), yuv.format().frame_bytes()) == 0);
    VideoFrameBuffer back2(a);
    CHECK(back2.allocate({33, 21, PixelFormat::Bgra8888}) && convert_frame(nv.frame(), back2));
    CHECK(memcmp(back.plane(0).data(), back2.plane(0).data(), back.format().frame_bytes()) == 0);

    // Wrong size: refused.
    VideoFrameBuffer other(a);
    CHECK(other.allocate({32, 21, PixelFormat::Bgra8888}) && !convert_frame(yuv.frame(), other));

    // Known colours, full range: grey 128/128/128 is mid grey; limited range 16 is black, 235 is white.
    VideoFrameBuffer one(a), px(a);
    CHECK(one.allocate({2, 2, PixelFormat::I420}) && px.allocate({2, 2, PixelFormat::Bgra8888}));
    one.plane(0).fill(128); one.plane(1).fill(128); one.plane(2).fill(128);
    CHECK(convert_frame(one.frame(), px) && reinterpret_cast<uint32_t*>(px.plane(0).data())[0] == 0xFF808080);
    one.plane(0).fill(16);
    VideoFrame limited = one.frame();
    limited.limited_range = true;
    CHECK(convert_frame(limited, px) && reinterpret_cast<uint32_t*>(px.plane(0).data())[0] == 0xFF000000);
    one.plane(0).fill(235);
    limited = one.frame();
    limited.limited_range = true;
    CHECK(convert_frame(limited, px) && reinterpret_cast<uint32_t*>(px.plane(0).data())[0] == 0xFFFFFFFF);
    // A pure red (255, 0, 0) goes to Y 76, Cb 85, Cr 255 and back.
    uint32_t* red = reinterpret_cast<uint32_t*>(px.plane(0).data());
    for (int i = 0; i < 4; ++i) red[i] = 0xFFFF0000;
    CHECK(convert_frame(px.frame(), one));
    CHECK(one.plane(0)[0] == 76 && one.plane(1)[0] == 85 && one.plane(2)[0] == 255);
    CHECK(convert_frame(one.frame(), px) && (red[0] >> 16 & 0xFF) > 250 && (red[0] & 0xFF) < 5);

    // convert_to_bgra into a padded surface.
    uint32_t padded[2 * 8];
    for (uint32_t& v : padded) v = 0x12345678;
    CHECK(convert_to_bgra(one.frame(), padded, 8) && padded[2] == 0x12345678 && padded[8] == red[2]);
    CHECK(!convert_to_bgra(one.frame(), padded, 1));
  }
  CHECK(a.live == 0);
}

static void test_jpeg_decode_reference() {
  TestAllocator a;
  {
    JpegDecoder d(a);
    CHECK(!d.decoded());
    CHECK(d.decode(ConstByteView(kSmallJpeg, sizeof kSmallJpeg)));
    CHECK(d.width() == 24 && d.height() == 16);
    VideoFrameBuffer out(a);
    CHECK(out.allocate({24, 16, PixelFormat::Bgra8888}) && d.output(out));
    uint8_t rgb[24 * 16 * 3];
    const uint32_t* p = reinterpret_cast<const uint32_t*>(out.plane(0).data());
    for (int i = 0; i < 24 * 16; ++i) { rgb[3 * i] = p[i] >> 16; rgb[3 * i + 1] = p[i] >> 8; rgb[3 * i + 2] = p[i]; }
    CHECK(crc32(ConstByteView(rgb, sizeof rgb)) == kSmallJpegCrc);   // bit for bit what the other decoder produced
    VideoFrameBuffer wrong(a);
    CHECK(wrong.allocate({24, 15, PixelFormat::Bgra8888}) && !d.output(wrong));
  }
  CHECK(a.live == 0);
}

static void test_jpeg_roundtrip() {
  TestAllocator a;
  {
    static const uint32_t sizes[][2] = {{1, 1}, {7, 5}, {16, 16}, {17, 33}, {100, 75}, {64, 48}};
    size_t previous_size = 0;
    for (const auto& size : sizes) {
      VideoFrameBuffer picture(a);
      make_picture(picture, size[0], size[1]);
      JpegEncoder enc(a);
      CHECK(enc.encode(picture.frame(), 90));
      const ConstByteView file = enc.output();
      CHECK(file.size() > 100 && file[0] == 0xFF && file[1] == 0xD8 && file[file.size() - 2] == 0xFF && file[file.size() - 1] == 0xD9);
      JpegDecoder dec(a);
      CHECK(dec.decode(file) && dec.width() == size[0] && dec.height() == size[1]);
      VideoFrameBuffer out(a);
      CHECK(out.allocate({size[0], size[1], PixelFormat::Bgra8888}) && dec.output(out));
      const double psnr = psnr_bgra(picture.frame(), out.frame());
      if (size[0] >= 16 && size[1] >= 16) CHECK(psnr > 30);
      (void)previous_size;
    }

    // Quality changes the size and the error.
    VideoFrameBuffer picture(a);
    make_picture(picture, 128, 96);
    size_t sizes_by_quality[3];
    double psnr_by_quality[3];
    const uint32_t qualities[3] = {20, 60, 95};
    for (int i = 0; i < 3; ++i) {
      JpegEncoder enc(a);
      CHECK(enc.encode(picture.frame(), qualities[i]));
      sizes_by_quality[i] = enc.output().size();
      JpegDecoder dec(a);
      VideoFrameBuffer out(a);
      CHECK(dec.decode(enc.output()) && out.allocate({128, 96, PixelFormat::Bgra8888}) && dec.output(out));
      psnr_by_quality[i] = psnr_bgra(picture.frame(), out.frame());
    }
    CHECK(sizes_by_quality[0] < sizes_by_quality[1] && sizes_by_quality[1] < sizes_by_quality[2]);
    CHECK(psnr_by_quality[0] < psnr_by_quality[1] && psnr_by_quality[1] < psnr_by_quality[2] && psnr_by_quality[2] > 38);

    // Any input format; any output format.
    VideoFrameBuffer yuv(a), nv(a);
    CHECK(yuv.allocate({128, 96, PixelFormat::I420}) && convert_frame(picture.frame(), yuv));
    CHECK(nv.allocate({128, 96, PixelFormat::Nv12}) && convert_frame(picture.frame(), nv));
    JpegEncoder from_yuv(a), from_nv(a), from_bgra(a);
    CHECK(from_yuv.encode(yuv.frame(), 90) && from_nv.encode(nv.frame(), 90) && from_bgra.encode(picture.frame(), 90));
    CHECK(from_yuv.output().size() == from_nv.output().size() && memcmp(from_yuv.output().data(), from_nv.output().data(), from_nv.output().size()) == 0);
    JpegDecoder dec(a);
    CHECK(dec.decode(from_yuv.output()));
    VideoFrameBuffer out_yuv(a), out_nv(a), out_bgra(a);
    CHECK(out_yuv.allocate({128, 96, PixelFormat::I420}) && dec.output(out_yuv));
    CHECK(out_nv.allocate({128, 96, PixelFormat::Nv12}) && dec.output(out_nv));
    CHECK(out_bgra.allocate({128, 96, PixelFormat::Bgra8888}) && dec.output(out_bgra));
    VideoFrameBuffer via_yuv(a);
    CHECK(via_yuv.allocate({128, 96, PixelFormat::Bgra8888}) && convert_frame(out_yuv.frame(), via_yuv));
    // The same picture reached two ways: the BGRA path smooths the chroma while upsampling it (as libjpeg does), the other repeats it.
    CHECK(psnr_bgra(out_bgra.frame(), via_yuv.frame()) > 30);

    // A limited-range YUV picture is widened before it is encoded: black (16) comes out black.
    VideoFrameBuffer dark(a);
    CHECK(dark.allocate({16, 16, PixelFormat::I420}));
    dark.plane(0).fill(16); dark.plane(1).fill(128); dark.plane(2).fill(128);
    VideoFrame limited = dark.frame();
    limited.limited_range = true;
    JpegEncoder enc(a);
    CHECK(enc.encode(limited, 95));
    JpegDecoder dec2(a);
    VideoFrameBuffer black(a);
    CHECK(dec2.decode(enc.output()) && black.allocate({16, 16, PixelFormat::Bgra8888}) && dec2.output(black));
    CHECK((reinterpret_cast<uint32_t*>(black.plane(0).data())[40] & 0xFF) < 4);

    // Bad frames.
    VideoFrame empty;
    CHECK(!enc.encode(empty, 50));
  }
  CHECK(a.live == 0);
}

static void test_jpeg_robustness() {
  TestAllocator a;
  {
    VideoFrameBuffer picture(a);
    make_picture(picture, 48, 40);
    JpegEncoder enc(a);
    CHECK(enc.encode(picture.frame(), 80));
    const ConstByteView good = enc.output();
    uint8_t* copy = static_cast<uint8_t*>(malloc(good.size()));
    JpegDecoder dec(a);
    VideoFrameBuffer out(a);
    // Truncation at every length, and single-byte corruption at many places: never a crash or an overrun.
    for (uint64_t n = 0; n < good.size(); n += 7) {
      memcpy(copy, good.data(), n);
      dec.decode(ConstByteView(copy, n));
    }
    uint32_t seed = 12345;
    for (int i = 0; i < 3000; ++i) {
      memcpy(copy, good.data(), good.size());
      for (int k = 0; k < 1 + i % 4; ++k) {
        seed = seed * 1664525u + 1013904223u;
        copy[(seed >> 8) % good.size()] = static_cast<uint8_t>(seed >> 24);
      }
      if (dec.decode(ConstByteView(copy, good.size())) && out.allocate({dec.width(), dec.height(), PixelFormat::I420})) dec.output(out);
    }
    // Not JPEGs at all.
    CHECK(!dec.decode(ConstByteView()));
    uint8_t junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = static_cast<uint8_t>(i * 37);
    CHECK(!dec.decode(ConstByteView(junk, 64)));
    // A progressive JPEG header is refused (SOF2).
    uint8_t progressive[] = {0xFF, 0xD8, 0xFF, 0xC2, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00, 0x08, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xD9};
    CHECK(!dec.decode(ConstByteView(progressive, sizeof progressive)));
    // After a failure the decoder works again.
    CHECK(dec.decode(good) && dec.decoded());
    free(copy);
  }
  CHECK(a.live == 0);
}

static void test_sessions() {
  TestAllocator a;
  {
    SoftwareVideoCodec device(a);
    CHECK(device.initialize() && !device.info().hardware_engine && !device.info().accelerated);
    CHECK(device.can_decode(VideoCodec::Mjpeg) && device.can_encode(VideoCodec::Raw) && device.can_decode(VideoCodec::H264) && device.can_encode(VideoCodec::H264) && !device.can_decode(VideoCodec::Hevc) && !device.can_encode(VideoCodec::Av1));
    DecoderConfig h264;
    h264.codec = VideoCodec::H264;
    CHECK(device.create_decoder(h264));  // an Annex B stream: the parameter sets come with the pictures
    h264.codec_config = ConstByteView(reinterpret_cast<const uint8_t*>("junk"), 4);
    CHECK(!device.create_decoder(h264));  // not an avcC record
    DecoderConfig hevc;
    hevc.codec = VideoCodec::Hevc;
    CHECK(!device.create_decoder(hevc));
    EncoderConfig bad;
    bad.codec = VideoCodec::Mjpeg;
    bad.width = 0; bad.height = 10;
    CHECK(!device.create_encoder(bad));

    VideoFrameBuffer picture(a);
    make_picture(picture, 80, 60);
    EncoderConfig ec;
    ec.codec = VideoCodec::Mjpeg; ec.width = 80; ec.height = 60; ec.input = PixelFormat::Bgra8888; ec.quality = 85;
    UniquePointer<IVideoEncoder> enc = device.create_encoder(ec);
    CHECK(enc && enc->codec() == VideoCodec::Mjpeg);
    VideoPacket packet;
    CHECK(!enc->receive(&packet));
    VideoFrame frame = picture.frame(40000, true);
    CHECK(enc->submit(frame) == SubmitResult::Accepted);
    CHECK(enc->submit(frame) == SubmitResult::Full);   // the packet has not been taken
    CHECK(enc->receive(&packet) && packet.pts_us == 40000 && packet.keyframe && packet.data.size() > 100);
    CHECK(!enc->receive(&packet));
    // A frame of another size or format is refused.
    VideoFrameBuffer other(a);
    make_picture(other, 40, 60);
    CHECK(enc->submit(other.frame()) == SubmitResult::Invalid);
    VideoFrameBuffer yuv(a);
    CHECK(yuv.allocate({80, 60, PixelFormat::I420}));
    CHECK(enc->submit(yuv.frame()) == SubmitResult::Invalid);
    CHECK(enc->statistics().frames == 1 && enc->statistics().packets == 1 && enc->statistics().errors == 2);

    // Decode what it made, as NV12.
    DecoderConfig dc;
    dc.codec = VideoCodec::Mjpeg; dc.output = PixelFormat::Nv12;
    UniquePointer<IVideoDecoder> dec = device.create_decoder(dc);
    CHECK(dec && dec->output_format().width == 0);
    VideoFrame decoded;
    CHECK(!dec->receive(&decoded));
    VideoPacket in = packet;
    CHECK(dec->submit(in) == SubmitResult::Accepted);
    CHECK(dec->submit(in) == SubmitResult::Full);
    CHECK(dec->receive(&decoded) && decoded.format.pixel_format == PixelFormat::Nv12 && decoded.format.width == 80 && decoded.pts_us == 40000);
    CHECK(dec->output_format() == decoded.format);
    VideoFrameBuffer back(a);
    CHECK(back.allocate({80, 60, PixelFormat::Bgra8888}) && convert_frame(decoded, back));
    CHECK(psnr_bgra(picture.frame(), back.frame()) > 30);
    // reset drops what is pending; garbage is refused.
    CHECK(dec->submit(in) == SubmitResult::Accepted);
    dec->reset();
    CHECK(!dec->receive(&decoded));
    uint8_t junk[40] = {1, 2, 3};
    VideoPacket bad_packet;
    bad_packet.data = ConstByteView(junk, 40);
    CHECK(dec->submit(bad_packet) == SubmitResult::Invalid && dec->statistics().errors == 1);
    dec->flush();

    // Raw: encode I420 frames, decode them to Bgra8888.
    EncoderConfig rc;
    rc.codec = VideoCodec::Raw; rc.width = 80; rc.height = 60; rc.input = PixelFormat::I420;
    UniquePointer<IVideoEncoder> raw_enc = device.create_encoder(rc);
    CHECK(raw_enc);
    CHECK(convert_frame(picture.frame(), yuv));
    CHECK(raw_enc->submit(yuv.frame(7, true)) == SubmitResult::Accepted);
    CHECK(raw_enc->receive(&packet) && packet.data.size() == yuv.format().frame_bytes() && memcmp(packet.data.data(), yuv.plane(0).data(), packet.data.size()) == 0);
    DecoderConfig rd;
    rd.codec = VideoCodec::Raw; rd.width = 80; rd.height = 60; rd.raw_format = PixelFormat::I420; rd.output = PixelFormat::Bgra8888;
    UniquePointer<IVideoDecoder> raw_dec = device.create_decoder(rd);
    CHECK(raw_dec && raw_dec->output_format().width == 80);
    CHECK(raw_dec->submit(packet) == SubmitResult::Accepted && raw_dec->receive(&decoded) && decoded.pts_us == 7);
    VideoFrameBuffer expect(a);
    CHECK(expect.allocate({80, 60, PixelFormat::Bgra8888}) && convert_frame(yuv.frame(), expect));
    CHECK(memcmp(decoded.plane[0], expect.plane(0).data(), expect.format().frame_bytes()) == 0);
    VideoPacket short_packet;
    short_packet.data = ConstByteView(packet.data.data(), 100);
    CHECK(raw_dec->submit(short_packet) == SubmitResult::Invalid);
    rd.width = 0;
    CHECK(!device.create_decoder(rd));   // a raw stream needs its size

    // Out of memory: sessions are refused or fail cleanly.
    a.fail = true;
    CHECK(!device.create_decoder(rd));
    a.fail = false;
  }
  CHECK(a.live == 0);
}

// A PCI bus with two devices: an NVIDIA display controller and a network card.
struct FakePci final : IPciBus {
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 2; }
  bool pci_device(uint32_t index, PciDeviceInfo* out) const override {
    if (index == 0) *out = {{0, 1, 0}, 0x10DE, 0x2204, kPciClassDisplay, 0, 0};
    else if (index == 1) *out = {{0, 2, 0}, 0x8086, 0x100E, 0x02, 0, 0};
    else return false;
    return true;
  }
  uint32_t read32(PciAddress, uint16_t) const override { return 0; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

static void test_system_list() {
  TestAllocator a;
  {
    FakePci pci;
    SystemVideoCodecs system(pci, a);
    system.initialize();
    CHECK(system.count() == 2);   // the NVIDIA engine and the software codec
    CHECK(system.at(0)->info().hardware_engine && !system.at(0)->info().accelerated && strstr(system.at(0)->name(), "NVIDIA"));
    CHECK(!system.at(1)->info().hardware_engine && strcmp(system.at(1)->name(), "Software video codec") == 0);
    CHECK(system.at(2) == nullptr);
    CHECK(system.decode_codecs() == (codec_mask(VideoCodec::Raw) | codec_mask(VideoCodec::Mjpeg) | codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Vp8) | codec_mask(VideoCodec::Vp9) | codec_mask(VideoCodec::Av1)));
    DecoderConfig dc;
    dc.codec = VideoCodec::Mjpeg;
    CHECK(system.create_decoder(dc));
    dc.codec = VideoCodec::Hevc;
    CHECK(!system.create_decoder(dc));
    EncoderConfig ec;
    ec.codec = VideoCodec::Mjpeg; ec.width = 16; ec.height = 16;
    CHECK(system.create_encoder(ec));
    ec.codec = VideoCodec::Av1;
    CHECK(!system.create_encoder(ec));
    system.shutdown();
    CHECK(system.count() == 0);
  }
  CHECK(a.live == 0);
}

static void make_moving_i420(VideoFrameBuffer& b, uint32_t w, uint32_t h, int t);

// The H.264 stream parser: the syntax of a stream the encoder made, without decoding a picture.
static void test_h264_parser() {
  TestAllocator a;
  {
    H264Encoder enc(a);
    CHECK(enc.configure(64, 48, 25, 1, 85, 3));  // a keyframe every third picture
    ByteBuffer stream(a);
    VideoFrameBuffer picture(a);
    std::vector<uint64_t> ends;
    for (int t = 0; t < 7; ++t) {
      make_moving_i420(picture, 64, 48, t);
      bool key;
      CHECK(enc.encode(picture.frame(), false, &stream, &key));
      ends.push_back(stream.size());
    }
    H264StreamParser parser(a);
    List<H264Slice> slices(a);
    uint64_t begin = 0;
    for (size_t i = 0; i < ends.size(); ++i) {
      const ConstByteView unit = stream.view().subview(begin, ends[i] - begin);
      begin = ends[i];
      CHECK(parser.parse(unit, &slices));
      CHECK(slices.count() == 1 && slices[0].starts_picture);  // one slice per picture, each a new picture
      const H264Slice& s = slices[0];
      CHECK(s.header.idr() == (i % 3 == 0) && s.header.type == (i % 3 == 0 ? h264::kSliceI : h264::kSliceP));
      const h264::Sps* sps = parser.sps_for(s.header);
      const h264::Pps* pps = parser.pps_for(s.header);
      CHECK(sps && pps && sps->mb_w == 4 && sps->mb_h == 3 && !pps->cabac);
      CHECK(s.nal_offset < unit.size() && s.nal_size <= unit.size() - s.nal_offset);
      CHECK(s.data_bit_offset > 8 && s.data_bit_offset <= s.nal_size * 8);
      CHECK(s.header.slice_qp >= 0 && s.header.slice_qp <= 51);
    }
    // The same slices split across calls, or fed twice, keep the picture boundaries: a second slice of the same picture does not start one.
    parser.reset();
    CHECK(parser.parse(stream.view().subview(0, ends[0]), &slices) && slices[0].starts_picture);
    CHECK(parser.parse(stream.view().subview(0, ends[0]), &slices) && slices.count() == 1 && slices[0].starts_picture == false);
  }
  CHECK(a.live == 0);
  {
    // Emulation prevention: payload bit positions are shifted by the 0x03 bytes stored before them.
    const uint8_t nal[] = {0x65, 0x00, 0x00, 0x03, 0x01, 0xAA, 0x00, 0x00, 0x03, 0x00, 0xBB};
    // payload (header removed): 00 00 01 AA 00 00 00 BB
    CHECK(H264StreamParser::stored_bit_offset(nal, sizeof nal, 0) == 8);
    CHECK(H264StreamParser::stored_bit_offset(nal, sizeof nal, 16) == 8 * 4);              // payload byte 2 (0x01) is stored at index 4
    CHECK(H264StreamParser::stored_bit_offset(nal, sizeof nal, 8 * 3 + 3) == 8 * 5 + 3);  // 0xAA at index 5
    CHECK(H264StreamParser::stored_bit_offset(nal, sizeof nal, 8 * 6) == 8 * 9);              // payload byte 6 (the 00 after the second 0x03) is stored at index 9
    CHECK(H264StreamParser::stored_bit_offset(nal, sizeof nal, 8 * 7) == 8 * 10);             // 0xBB
  }
  {
    // Truncated and bit-flipped streams must come out as a refusal or as slices inside the data, never as a crash.
    H264Encoder enc(a);
    CHECK(enc.configure(48, 32, 25, 1, 80, 2));
    ByteBuffer stream(a);
    VideoFrameBuffer picture(a);
    for (int t = 0; t < 3; ++t) {
      make_moving_i420(picture, 48, 32, t);
      bool key;
      CHECK(enc.encode(picture.frame(), false, &stream, &key));
    }
    const uint64_t size = stream.size();
    for (uint64_t cut = 0; cut <= size; ++cut) {
      H264StreamParser parser(a);
      List<H264Slice> slices(a);
      parser.parse(stream.view().first(cut), &slices);
      for (const H264Slice& s : slices) CHECK(s.nal_offset + s.nal_size <= cut && s.data_bit_offset <= s.nal_size * 8);
    }
    for (uint64_t byte = 0; byte < size; ++byte)
      for (int bit = 0; bit < 8; bit += 3) {
        ByteBuffer copy(a);
        CHECK(copy.append(stream.view()));
        copy.writable_view()[byte] ^= static_cast<uint8_t>(1u << bit);
        H264StreamParser parser(a);
        List<H264Slice> slices(a);
        parser.parse(copy.view(), &slices);
        for (const H264Slice& s : slices) CHECK(s.nal_offset + s.nal_size <= size && s.data_bit_offset <= s.nal_size * 8);
      }
  }
  CHECK(a.live == 0);
}

// An Intel GPU on the bus, and a mapper that hands out plain memory as its registers.
struct IntelPci final : IPciBus {
  uint16_t device_id; bool memory_decode; uint64_t bar0 = 0x1000;
  IntelPci(uint16_t id, bool decode) : device_id(id), memory_decode(decode) {}
  const char* name() const override { return "fake intel pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t index, PciDeviceInfo* out) const override {
    if (index != 0) return false;
    *out = {{0, 2, 0}, 0x8086, device_id, kPciClassDisplay, 0, 0};
    return true;
  }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset == 0x04 && memory_decode ? 2 : 0; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return bar0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

struct FakeRegisterMapper final : IRegisterMapper {
  std::vector<uint32_t> words = std::vector<uint32_t>(IntelMediaEngineHw::kRegisterWindow / 4, 0);
  int calls = 0;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    ++calls;
    if (physical != 0x1000 || length > words.size() * 4) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(words.data()), words.size() * 4);
    return true;
  }
};

static IntelMediaVideoEngine* intel_engine(SystemVideoCodecs& system) { return static_cast<IntelMediaVideoEngine*>(system.at(0)); }

static void test_intel_system() {
  TestAllocator a;
  {
    IntelPci pci(0x9A49, true);  // Tiger Lake, memory decoding on
    FakeRegisterMapper mapper;
    mapper.words[0x0D50 / 4] = 1;
    SystemVideoCodecs system(pci, a);
    system.set_mmio(&mapper);
    system.initialize();
    CHECK(system.count() == 2 && strstr(system.at(0)->name(), "Intel") && mapper.calls == 1);
    CHECK(intel_engine(system)->engine_status().probed && intel_engine(system)->engine_status().vdbox_count() == 1);
    system.shutdown();
  }
  {
    IntelPci pci(0x9A49, false);  // memory decoding off: the registers are not touched
    FakeRegisterMapper mapper;
    SystemVideoCodecs system(pci, a);
    system.set_mmio(&mapper);
    system.initialize();
    CHECK(mapper.calls == 0 && !intel_engine(system)->engine_status().probed);
    system.shutdown();
  }
  {
    IntelPci pci(0x7D55, true);  // Meteor Lake: no known register layout
    FakeRegisterMapper mapper;
    SystemVideoCodecs system(pci, a);
    system.set_mmio(&mapper);
    system.initialize();
    CHECK(mapper.calls == 0 && !intel_engine(system)->engine_status().probed);
    system.shutdown();
  }
  {
    IntelPci pci(0x9A49, true);  // no mapper at all
    SystemVideoCodecs system(pci, a);
    system.initialize();
    CHECK(system.count() == 2 && !intel_engine(system)->engine_status().probed);
    system.shutdown();
  }
  CHECK(a.live == 0);
}

// A moving I420 picture for the H.264 encoder: smooth waves and a square, moved `t` pixels to the right.
static void make_moving_i420(VideoFrameBuffer& b, uint32_t w, uint32_t h, int t) {
  CHECK(b.allocate({w, h, PixelFormat::I420}));
  auto clip = [](double v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const int sx = (int)x - 2 * t;
      double v = 128 + 60 * sin(sx / 13.0) + 40 * sin(y / 11.0);
      if (sx > 20 && sx < 36 && y > 12 && y < 28) v += 70;
      b.plane(0)[y * w + x] = clip(v);
    }
  for (uint32_t y = 0; y < h / 2; ++y)
    for (uint32_t x = 0; x < w / 2; ++x) {
      b.plane(1)[y * (w / 2) + x] = clip(128 + 40 * sin(((int)x - t) / 9.0));
      b.plane(2)[y * (w / 2) + x] = clip(128 + 40 * cos(y / 7.0));
    }
}

static bool same_i420(const VideoFrame& a, const VideoFrame& b) {
  if (a.format.width != b.format.width || a.format.height != b.format.height) return false;
  for (uint32_t p = 0; p < 3; ++p)
    for (uint32_t y = 0; y < a.format.plane_height(p); ++y)
      if (memcmp(a.plane[p] + (size_t)y * a.stride[p], b.plane[p] + (size_t)y * b.stride[p], a.format.plane_width(p)) != 0) return false;
  return true;
}

static double psnr_luma(const VideoFrame& a, const VideoFrame& b) {
  double sum = 0;
  for (uint32_t y = 0; y < a.format.height; ++y)
    for (uint32_t x = 0; x < a.format.width; ++x) {
      const int d = (int)a.plane[0][(size_t)y * a.stride[0] + x] - (int)b.plane[0][(size_t)y * b.stride[0] + x];
      sum += d * d;
    }
  const double mse = sum / ((double)a.format.width * a.format.height);
  return mse == 0 ? 99 : 10 * log10(255.0 * 255.0 / mse);
}

// Encodes `frames` moving pictures, decodes the stream with the decoder and checks every decoded picture against
// the encoder's own reconstruction, bit for bit. Returns the stream size.
static uint64_t h264_roundtrip(TestAllocator& a, uint32_t w, uint32_t h, uint32_t quality, uint32_t key_interval, int frames, double min_psnr, bool avcc_mode = false) {
  H264Encoder enc(a);
  CHECK(enc.configure(w, h, 25, 1, quality, key_interval));
  H264Decoder dec(a);
  ByteBuffer avcc(a);
  if (avcc_mode) {
    CHECK(enc.avcc(&avcc) && dec.configure(avcc.view()));
  }
  uint64_t total = 0;
  VideoFrameBuffer src(a);
  for (int t = 0; t < frames; ++t) {
    make_moving_i420(src, w, h, t);
    ByteBuffer out(a);
    bool key = false;
    CHECK(enc.encode(src.frame(t * 40000, true), false, &out, &key));
    CHECK(key == (t % (key_interval ? key_interval : 1) == 0));
    total += out.size();
    ConstByteView au = out.view();
    ByteBuffer prefixed(a);
    if (avcc_mode) {  // the same NAL units, length-prefixed instead of start-code separated, and without the parameter sets
      uint64_t i = 0;
      while (i + 4 <= au.size()) {
        uint64_t j = i + 4;
        while (j + 4 <= au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 0 && au[j + 3] == 1)) ++j;
        if (j + 4 > au.size()) j = au.size();
        const uint8_t type = au[i + 4] & 31;
        if (type != 7 && type != 8) {
          const uint64_t len = j - i - 4;
          prefixed.put8((uint8_t)(len >> 24)); prefixed.put8((uint8_t)(len >> 16)); prefixed.put8((uint8_t)(len >> 8)); prefixed.put8((uint8_t)len);
          prefixed.append(au.subview(i + 4, len));
        }
        i = j;
      }
      au = prefixed.view();
    }
    CHECK(dec.decode(au, t * 40000));
    VideoFrame got, rec;
    CHECK(dec.receive(&got));
    CHECK(enc.reconstruction(&rec));
    CHECK(got.format.width == w && got.format.height == h);
    CHECK(same_i420(got, rec));
    CHECK(got.limited_range == false);
    VideoFrame in = src.frame();
    const double p = psnr_luma(in, got);
    if (p < min_psnr) printf("  frame %d: luma PSNR %.2f dB (< %.1f)\n", t, p, min_psnr);
    CHECK(p >= min_psnr);
    if (const char* dump = getenv("H264_DUMP")) {
      FILE* f = fopen(dump, t == 0 ? "wb" : "ab");
      if (f) { fwrite(out.view().data(), 1, out.size(), f); fclose(f); }
    }
  }
  return total;
}

static void test_h264_encode() {
  TestAllocator a;
  {
    // Sizes that are and are not a multiple of the macroblock size, I-only and with P pictures.
    const uint64_t small = h264_roundtrip(a, 64, 48, 85, 1, 6, 30);
    const uint64_t p_frames = h264_roundtrip(a, 64, 48, 85, 6, 6, 30);
    CHECK(p_frames < small);  // P pictures are smaller than keyframes
    h264_roundtrip(a, 50, 34, 85, 4, 8, 28);
    h264_roundtrip(a, 176, 144, 60, 10, 12, 27);
    h264_roundtrip(a, 176, 144, 100, 10, 4, 38);
    h264_roundtrip(a, 16, 16, 75, 3, 5, 25);
    h264_roundtrip(a, 2, 2, 75, 1, 2, 0);
    h264_roundtrip(a, 96, 64, 80, 5, 7, 28, true);  // length-prefixed NAL units and an avcC record
    CHECK(h264_roundtrip(a, 176, 144, 90, 30, 10, 30) > 0);
  }
  CHECK(a.live == 0);
  {
    H264Encoder enc(a);
    CHECK(!enc.configure(0, 16, 25, 1, 75, 1));
    CHECK(!enc.configure(17, 16, 25, 1, 75, 1));  // odd sizes cannot be cropped exactly
    CHECK(!enc.configure(16, 16, 0, 1, 75, 1));
    CHECK(!enc.configure(32768, 16, 25, 1, 75, 1));
    ByteBuffer out(a);
    bool key;
    VideoFrameBuffer wrong(a);
    make_moving_i420(wrong, 32, 32, 0);
    CHECK(enc.configure(16, 16, 25, 1, 75, 1));
    CHECK(!enc.encode(wrong.frame(), false, &out, &key));  // wrong size
  }
  CHECK(a.live == 0);
  {
    // Through a device session: Bgra8888 pictures in, packets out, decoded again.
    SoftwareVideoCodec device(a);
    CHECK(device.initialize());
    EncoderConfig ec;
    ec.codec = VideoCodec::H264; ec.width = 80; ec.height = 60; ec.input = PixelFormat::Bgra8888; ec.quality = 85; ec.keyframe_interval = 4;
    UniquePointer<IVideoEncoder> enc = device.create_encoder(ec);
    CHECK(enc && enc->codec() == VideoCodec::H264);
    DecoderConfig dc;
    dc.codec = VideoCodec::H264; dc.output = PixelFormat::Bgra8888;
    UniquePointer<IVideoDecoder> dec = device.create_decoder(dc);
    CHECK(dec);
    VideoFrameBuffer picture(a);
    make_picture(picture, 80, 60);
    for (int i = 0; i < 6; ++i) {
      VideoPacket packet;
      CHECK(!enc->receive(&packet));
      CHECK(enc->submit(picture.frame(i * 40000, true)) == SubmitResult::Accepted);
      CHECK(enc->submit(picture.frame(i * 40000, true)) == SubmitResult::Full);
      CHECK(enc->receive(&packet) && packet.data.size() > 4 && packet.pts_us == i * 40000 && packet.keyframe == (i % 4 == 0));
      CHECK(dec->submit(packet) == SubmitResult::Accepted);
      VideoFrame out;
      CHECK(dec->receive(&out));
      CHECK(out.format.pixel_format == PixelFormat::Bgra8888 && out.pts_us == i * 40000);
      const double p = psnr_bgra(picture.frame(), out);
      CHECK(p > 28);
    }
    CHECK(enc->statistics().packets == 6 && enc->statistics().frames == 6);
    VideoFrameBuffer wrong(a);
    CHECK(wrong.allocate({40, 30, PixelFormat::Bgra8888}));
    CHECK(enc->submit(wrong.frame()) == SubmitResult::Invalid);
    EncoderConfig odd = ec;
    odd.width = 81;
    CHECK(!device.create_encoder(odd));
  }
  CHECK(a.live == 0);
}

// Corrupt streams from the encoder must not upset the decoder: truncations and bit flips of every packet.
static void test_h264_encode_robustness() {
  TestAllocator a;
  H264Encoder enc(a);
  CHECK(enc.configure(48, 32, 25, 1, 70, 3));
  VideoFrameBuffer src(a);
  ByteBuffer stream(a);
  uint64_t starts[6];
  for (int t = 0; t < 6; ++t) {
    make_moving_i420(src, 48, 32, t);
    bool key;
    starts[t] = stream.size();
    CHECK(enc.encode(src.frame(), false, &stream, &key));
  }
  const uint64_t n = stream.size();
  uint32_t rng = 12345;
  for (int round = 0; round < 120; ++round) {
    ByteBuffer copy(a);
    copy.append(stream.view());
    ByteView w = copy.writable_view();
    rng = rng * 1664525u + 1013904223u;
    if (round % 3 == 0) {
      copy.truncate(rng % n);
    } else {
      for (int k = 0; k < 1 + round % 4; ++k) {
        rng = rng * 1664525u + 1013904223u;
        w[(rng >> 8) % n] ^= (uint8_t)(1u << (rng & 7));
      }
    }
    H264Decoder dec(a);
    VideoFrame f;
    for (int t = 0; t < 6; ++t) {
      const uint64_t from = starts[t], to = t + 1 < 6 ? starts[t + 1] : copy.size();
      if (from >= copy.size()) break;
      dec.decode(copy.view().subview(from, (to < copy.size() ? to : copy.size()) - from), t);
      while (dec.receive(&f)) {}
    }
    dec.flush();
    while (dec.receive(&f)) {}
  }
  (void)starts;
}

// ---- VP9 ---------------------------------------------------------------------------------------------------------------------------

// The packets of an IVF file.
struct IvfFile {
  unsigned char* data = nullptr;
  long size = 0;
  long first = 32;
  explicit IvfFile(const char* name) {
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", name);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = static_cast<unsigned char*>(malloc(size));
    if (fread(data, 1, size, f) != static_cast<size_t>(size)) { free(data); data = nullptr; }
    fclose(f);
  }
  ~IvfFile() { free(data); }
  // The next packet at *pos (start with 32).
  static bool next(const unsigned char* d, long n, long* pos, ConstByteView* packet) {
    if (*pos + 12 > n) return false;
    const unsigned long sz = d[*pos] | d[*pos + 1] << 8 | d[*pos + 2] << 16 | static_cast<unsigned long>(d[*pos + 3]) << 24;
    if (static_cast<long>(sz) > n - *pos - 12) return false;
    *packet = ConstByteView(d + *pos + 12, sz);
    *pos += 12 + sz;
    return true;
  }
};

static unsigned long long fnv64(unsigned long long h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// Decodes every packet of an IVF image with a Vp9Decoder; the FNV-64 of the pictures (8-bit I420, tight rows).
static bool vp9_decode(allocators::IAllocator& a, const unsigned char* d, long n, unsigned* frames, unsigned* width, unsigned* height, unsigned long long* hash, bool keep_going) {
  hardware::Vp9Decoder dec(a);
  *frames = 0;
  *hash = 0xcbf29ce484222325ull;
  long pos = 32;
  ConstByteView pk;
  bool ok = true;
  while (IvfFile::next(d, n, &pos, &pk)) {
    if (!dec.decode(pk, *frames)) {
      ok = false;
      if (!keep_going) break;
    }
    hardware::VideoFrame f;
    if (dec.receive(&f)) {
      if (*frames == 0) { *width = f.format.width; *height = f.format.height; }
      for (uint32_t p = 0; p < 3; ++p)
        for (uint32_t y = 0; y < f.format.plane_height(p); ++y) *hash = fnv64(*hash, f.plane[p] + static_cast<size_t>(y) * f.stride[p], f.format.plane_width(p));
      ++*frames;
    }
  }
  return ok;
}

// The streams made by libvpx (see tests/data/make_vp9_fixtures.py) decode to the pictures libvpx and ffmpeg make from them: every profile, tiles,
// lossless, segmentation, hidden frames and references of another size.
static void test_vp9() {
  TestAllocator a;
  FILE* m = fopen("tests/data/manifest_vp9.txt", "r");
  CHECK(m != nullptr);
  if (!m) return;
  char kind[16], name[96];
  unsigned w, h, frames;
  unsigned long long hash;
  int count = 0;
  while (fscanf(m, "%15s %95s %u %u %u %llx", kind, name, &w, &h, &frames, &hash) == 6) {
    IvfFile file(name);
    if (!file.data) { printf("missing fixture %s\n", name); ++g_failed; continue; }
    unsigned got_frames = 0, got_w = 0, got_h = 0;
    unsigned long long got_hash = 0;
    CHECK(vp9_decode(a, file.data, file.size, &got_frames, &got_w, &got_h, &got_hash, false));
    if (got_frames != frames || got_w != w || got_h != h || got_hash != hash) {
      printf("VP9 fixture %s: got %u frames %ux%u hash %016llx, wanted %u frames %ux%u hash %016llx\n", name, got_frames, got_w, got_h, got_hash, frames, w, h, hash);
      ++g_failed;
    }
    ++count;
  }
  fclose(m);
  CHECK(count >= 11);
  CHECK(a.live == 0);

  // Through a codec session: pictures come out in the format asked for, one per packet.
  {
    hardware::SoftwareVideoCodec device(a);
    CHECK(device.can_decode(hardware::VideoCodec::Vp9) && !device.can_encode(hardware::VideoCodec::Vp9));
    hardware::DecoderConfig config;
    config.codec = hardware::VideoCodec::Vp9;
    config.output = hardware::PixelFormat::Bgra8888;
    UniquePointer<hardware::IVideoDecoder> dec = device.create_decoder(config);
    CHECK(dec != nullptr);
    IvfFile file("vp9_inter.ivf");
    CHECK(file.data != nullptr);
    if (dec && file.data) {
      long pos = 32;
      ConstByteView pk;
      uint32_t n = 0;
      while (IvfFile::next(file.data, file.size, &pos, &pk)) {
        hardware::VideoPacket packet;
        packet.data = pk;
        packet.pts_us = n * 100000;
        CHECK(dec->submit(packet) == hardware::SubmitResult::Accepted);
        packet.data = pk;
        CHECK(dec->submit(packet) == hardware::SubmitResult::Full);  // the picture has not been taken yet
        hardware::VideoFrame f;
        CHECK(dec->receive(&f) && f.format.width == 95 && f.format.height == 63 && f.format.pixel_format == hardware::PixelFormat::Bgra8888 && f.pts_us == n * 100000);
        CHECK(f.keyframe == (n == 0));
        ++n;
      }
      CHECK(n == 14);
      CHECK(dec->output_format().width == 95 && dec->statistics().frames == 14 && dec->statistics().errors == 0);
      // Garbage is refused, and after a reset the decoder waits for a key frame.
      const uint8_t noise[16] = {0x91, 2, 3, 4, 5, 6, 7, 8};
      hardware::VideoPacket junk;
      junk.data = ConstByteView(noise, sizeof noise);
      CHECK(dec->submit(junk) == hardware::SubmitResult::Invalid);
      dec->reset();
      pos = 32;
      IvfFile::next(file.data, file.size, &pos, &pk);
      IvfFile::next(file.data, file.size, &pos, &pk);  // the second packet is an inter frame
      hardware::VideoPacket second;
      second.data = pk;
      hardware::VideoFrame f;
      CHECK(dec->submit(second) == hardware::SubmitResult::Invalid);
      CHECK(!dec->receive(&f));
    }
  }
  CHECK(a.live == 0);
}

// Truncated and damaged copies of the VP9 fixtures must neither crash nor leak.
static void test_vp9_robustness() {
  TestAllocator a;
  const char* names[] = {"vp9_intra.ivf", "vp9_inter.ivf", "vp9_tiles.ivf", "vp9_lossless.ivf", "vp9_segments.ivf", "vp9_444.ivf", "vp9_10bit.ivf", "vp9_422.ivf"};
  uint32_t seed = 99;
  auto rnd = [&seed]() { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (const char* name : names) {
    IvfFile file(name);
    CHECK(file.data != nullptr);
    if (!file.data) continue;
    for (int round = 0; round < 24; ++round) {
      unsigned char* copy = static_cast<unsigned char*>(malloc(file.size));
      memcpy(copy, file.data, file.size);
      long len = file.size;
      switch (round % 4) {
        case 0: len = 32 + rnd() % (file.size - 32); break;                                                                  // truncated
        case 1: for (int k = 0; k < 4; ++k) copy[32 + rnd() % (file.size - 32)] ^= 1 << (rnd() % 8); break;                 // bit flips
        case 2: for (int k = 0; k < 12; ++k) copy[32 + rnd() % (file.size - 32)] = static_cast<uint8_t>(rnd()); break;      // noise
        default: for (int k = 0; k < 3; ++k) copy[32 + rnd() % (file.size < 400 ? file.size - 32 : 400)] = static_cast<uint8_t>(rnd()); break;  // headers
      }
      unsigned frames, w, h;
      unsigned long long hash;
      vp9_decode(a, copy, len, &frames, &w, &h, &hash, true);
      free(copy);
    }
  }
  CHECK(a.live == 0);
}

// ---- AV1 ---------------------------------------------------------------------------------------------------------------------------

// Decodes every temporal unit of an IVF image with an Av1Decoder; the FNV-64 of the pictures (8-bit I420, tight rows, each at its own size).
static bool av1_decode(allocators::IAllocator& a, const unsigned char* d, long n, unsigned* frames, unsigned* width, unsigned* height, unsigned long long* hash, bool keep_going) {
  hardware::Av1Decoder dec(a);
  *frames = 0;
  *hash = 0xcbf29ce484222325ull;
  long pos = 32;
  ConstByteView pk;
  bool ok = true;
  while (IvfFile::next(d, n, &pos, &pk)) {
    if (!dec.decode(pk, *frames)) {
      ok = false;
      if (!keep_going) break;
    }
    hardware::VideoFrame f;
    if (dec.receive(&f)) {
      if (*frames == 0) { *width = f.format.width; *height = f.format.height; }
      for (uint32_t p = 0; p < 3; ++p)
        for (uint32_t y = 0; y < f.format.plane_height(p); ++y) *hash = fnv64(*hash, f.plane[p] + static_cast<size_t>(y) * f.stride[p], f.format.plane_width(p));
      ++*frames;
    }
  }
  return ok;
}

// The streams made by libaom, SVT-AV1 and rav1e (see tests/data/make_av1_fixtures.py) decode to the pictures dav1d makes from them: every profile and
// bit depth, tiles, 128x128 superblocks, lossless, quantiser matrices, segmentation, palette and intra block copy, super-resolution, scaled references,
// film grain and hidden frames.
static void test_av1() {
  TestAllocator a;
  FILE* m = fopen("tests/data/manifest_av1.txt", "r");
  CHECK(m != nullptr);
  if (!m) return;
  char kind[16], name[96];
  unsigned w, h, frames;
  unsigned long long hash;
  int count = 0;
  while (fscanf(m, "%15s %95s %u %u %u %llx", kind, name, &w, &h, &frames, &hash) == 6) {
    IvfFile file(name);
    if (!file.data) { printf("missing fixture %s\n", name); ++g_failed; continue; }
    unsigned got_frames = 0, got_w = 0, got_h = 0;
    unsigned long long got_hash = 0;
    CHECK(av1_decode(a, file.data, file.size, &got_frames, &got_w, &got_h, &got_hash, false));
    if (got_frames != frames || got_w != w || got_h != h || got_hash != hash) {
      printf("AV1 fixture %s: got %u frames %ux%u hash %016llx, wanted %u frames %ux%u hash %016llx\n", name, got_frames, got_w, got_h, got_hash, frames, w, h, hash);
      ++g_failed;
    }
    ++count;
  }
  fclose(m);
  CHECK(count >= 18);
  CHECK(a.live == 0);

  // Through a codec session: pictures come out in the format asked for, one per packet.
  {
    hardware::SoftwareVideoCodec device(a);
    CHECK(device.can_decode(hardware::VideoCodec::Av1) && !device.can_encode(hardware::VideoCodec::Av1));
    hardware::DecoderConfig config;
    config.codec = hardware::VideoCodec::Av1;
    config.output = hardware::PixelFormat::Bgra8888;
    UniquePointer<hardware::IVideoDecoder> dec = device.create_decoder(config);
    CHECK(dec != nullptr);
    IvfFile file("av1_inter.ivf");
    CHECK(file.data != nullptr);
    if (dec && file.data) {
      long pos = 32;
      ConstByteView pk;
      uint32_t n = 0;
      while (IvfFile::next(file.data, file.size, &pos, &pk)) {
        hardware::VideoPacket packet;
        packet.data = pk;
        packet.pts_us = n * 100000;
        CHECK(dec->submit(packet) == hardware::SubmitResult::Accepted);
        hardware::VideoFrame f;
        if (dec->receive(&f)) {
          CHECK(f.format.width == 95 && f.format.height == 63 && f.format.pixel_format == hardware::PixelFormat::Bgra8888);
          ++n;
        }
      }
      CHECK(n == 14 && dec->statistics().errors == 0);
      // Garbage is refused.
      const uint8_t noise[16] = {0x12, 0x00, 0x0a, 0x0b, 5, 6, 7, 8};
      hardware::VideoPacket junk;
      junk.data = ConstByteView(noise, sizeof noise);
      dec->reset();
      hardware::VideoFrame f;
      dec->submit(junk);
      CHECK(!dec->receive(&f));
    }
  }
  CHECK(a.live == 0);
}

// Truncated and damaged copies of the AV1 fixtures must neither crash nor leak.
static void test_av1_robustness() {
  TestAllocator a;
  const char* names[] = {"av1_intra.ivf", "av1_inter.ivf", "av1_tiles.ivf", "av1_lossless.ivf", "av1_segments.ivf", "av1_444.ivf", "av1_10bit.ivf", "av1_screen.ivf", "av1_superres.ivf", "av1_resize.ivf", "av1_grain.ivf", "av1_svt.ivf", "av1_rav1e.ivf"};
  uint32_t seed = 99;
  auto rnd = [&seed]() { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (const char* name : names) {
    IvfFile file(name);
    CHECK(file.data != nullptr);
    if (!file.data) continue;
    for (int round = 0; round < 24; ++round) {
      unsigned char* copy = static_cast<unsigned char*>(malloc(file.size));
      memcpy(copy, file.data, file.size);
      long len = file.size;
      switch (round % 4) {
        case 0: len = 32 + rnd() % (file.size - 32); break;                                                                  // truncated
        case 1: for (int k = 0; k < 4; ++k) copy[32 + rnd() % (file.size - 32)] ^= 1 << (rnd() % 8); break;                 // bit flips
        case 2: for (int k = 0; k < 12; ++k) copy[32 + rnd() % (file.size - 32)] = static_cast<uint8_t>(rnd()); break;      // noise
        default: for (int k = 0; k < 3; ++k) copy[32 + rnd() % (file.size < 400 ? file.size - 32 : 400)] = static_cast<uint8_t>(rnd()); break;  // headers
      }
      unsigned frames, w, h;
      unsigned long long hash;
      av1_decode(a, copy, len, &frames, &w, &h, &hash, true);
      free(copy);
    }
  }
  CHECK(a.live == 0);
}

// Decodes every packet of an IVF image with a Vp8Decoder; the FNV-64 of the pictures (8-bit I420, tight rows). Pictures of any size are hashed as they are.
static bool vp8_decode(allocators::IAllocator& a, const unsigned char* d, long n, unsigned* frames, unsigned* width, unsigned* height, unsigned long long* hash, bool keep_going) {
  hardware::Vp8Decoder dec(a);
  *frames = 0;
  *hash = 0xcbf29ce484222325ull;
  long pos = 32;
  ConstByteView pk;
  bool ok = true;
  while (IvfFile::next(d, n, &pos, &pk)) {
    if (!dec.decode(pk, *frames)) {
      ok = false;
      if (!keep_going) break;
    }
    hardware::VideoFrame f;
    if (dec.receive(&f)) {
      if (*frames == 0) { *width = f.format.width; *height = f.format.height; }
      for (uint32_t p = 0; p < 3; ++p)
        for (uint32_t y = 0; y < f.format.plane_height(p); ++y) *hash = fnv64(*hash, f.plane[p] + static_cast<size_t>(y) * f.stride[p], f.format.plane_width(p));
      ++*frames;
    }
  }
  return ok;
}

// The streams made by libvpx (see tests/data/make_vp8_fixtures.py) decode to the pictures libvpx's own decoder makes from them, bit for bit: key frames
// only, inter frames, odd sizes, token partitions, loop filter sharpness, the simple filter and bilinear prediction (versions 1 to 3), hidden alternate
// references, segmentation, error resilience, several key frames and a change of the picture size.
static void test_vp8() {
  TestAllocator a;
  FILE* m = fopen("tests/data/manifest_vp8.txt", "r");
  CHECK(m != nullptr);
  if (!m) return;
  char kind[16], name[96];
  unsigned w, h, frames;
  unsigned long long hash;
  int count = 0;
  while (fscanf(m, "%15s %95s %u %u %u %llx", kind, name, &w, &h, &frames, &hash) == 6) {
    IvfFile file(name);
    if (!file.data) { printf("missing fixture %s\n", name); ++g_failed; continue; }
    unsigned got_frames = 0, got_w = 0, got_h = 0;
    unsigned long long got_hash = 0;
    CHECK(vp8_decode(a, file.data, file.size, &got_frames, &got_w, &got_h, &got_hash, false));
    if (got_frames != frames || got_w != w || got_h != h || got_hash != hash) {
      printf("VP8 fixture %s: got %u frames %ux%u hash %016llx, wanted %u frames %ux%u hash %016llx\n", name, got_frames, got_w, got_h, got_hash, frames, w, h, hash);
      ++g_failed;
    }
    ++count;
  }
  fclose(m);
  CHECK(count >= 19);
  CHECK(a.live == 0);

  // Through a codec session: pictures come out in the format asked for, one per packet.
  {
    hardware::SoftwareVideoCodec device(a);
    CHECK(device.can_decode(hardware::VideoCodec::Vp8) && !device.can_encode(hardware::VideoCodec::Vp8));
    hardware::DecoderConfig config;
    config.codec = hardware::VideoCodec::Vp8;
    config.output = hardware::PixelFormat::Bgra8888;
    UniquePointer<hardware::IVideoDecoder> dec = device.create_decoder(config);
    CHECK(dec != nullptr);
    IvfFile file("vp8_inter.ivf");
    CHECK(file.data != nullptr);
    if (dec && file.data) {
      long pos = 32;
      ConstByteView pk;
      uint32_t n = 0;
      while (IvfFile::next(file.data, file.size, &pos, &pk)) {
        hardware::VideoPacket packet;
        packet.data = pk;
        packet.pts_us = n * 100000;
        CHECK(dec->submit(packet) == hardware::SubmitResult::Accepted);
        packet.data = pk;
        CHECK(dec->submit(packet) == hardware::SubmitResult::Full);  // the picture has not been taken yet
        hardware::VideoFrame f;
        CHECK(dec->receive(&f) && f.format.width == 80 && f.format.height == 64 && f.format.pixel_format == hardware::PixelFormat::Bgra8888 && f.pts_us == n * 100000);
        CHECK(f.keyframe == (n == 0));
        ++n;
      }
      CHECK(n == 12);
      CHECK(dec->output_format().width == 80 && dec->statistics().frames == 12 && dec->statistics().errors == 0);
      // Garbage is refused, and after a reset the decoder waits for a key frame.
      const uint8_t noise[16] = {0x91, 2, 3, 4, 5, 6, 7, 8};
      hardware::VideoPacket junk;
      junk.data = ConstByteView(noise, sizeof noise);
      CHECK(dec->submit(junk) == hardware::SubmitResult::Invalid);
      dec->reset();
      pos = 32;
      IvfFile::next(file.data, file.size, &pos, &pk);
      IvfFile::next(file.data, file.size, &pos, &pk);  // the second packet is an inter frame
      hardware::VideoPacket second;
      second.data = pk;
      hardware::VideoFrame f;
      CHECK(dec->submit(second) == hardware::SubmitResult::Invalid);
      CHECK(!dec->receive(&f));
    }
  }
  // A dropped frame (an empty packet) shows the last picture again; with nothing decoded yet it is refused.
  {
    hardware::Vp8Decoder dec(a);
    hardware::VideoFrame f;
    CHECK(!dec.decode(ConstByteView(), 0));
    IvfFile file("vp8_inter.ivf");
    long pos = 32;
    ConstByteView pk;
    CHECK(file.data && IvfFile::next(file.data, file.size, &pos, &pk));
    if (file.data) {
      CHECK(dec.decode(pk, 0) && dec.receive(&f));
      const uint8_t first = f.plane[0][0];
      CHECK(dec.decode(ConstByteView(), 5) && dec.receive(&f) && f.pts_us == 5 && f.plane[0][0] == first && !f.keyframe);
    }
  }
  CHECK(a.live == 0);
}

// Truncated and damaged copies of the VP8 fixtures must neither crash nor leak.
static void test_vp8_robustness() {
  TestAllocator a;
  const char* names[] = {"vp8_intra.ivf", "vp8_inter.ivf", "vp8_parts.ivf", "vp8_segments.ivf", "vp8_arf.ivf", "vp8_profile1.ivf", "vp8_profile3_rt.ivf", "vp8_resize.ivf", "vp8_odd.ivf"};
  uint32_t seed = 77;
  auto rnd = [&seed]() { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (const char* name : names) {
    IvfFile file(name);
    CHECK(file.data != nullptr);
    if (!file.data) continue;
    for (int round = 0; round < 24; ++round) {
      unsigned char* copy = static_cast<unsigned char*>(malloc(file.size));
      memcpy(copy, file.data, file.size);
      long len = file.size;
      switch (round % 4) {
        case 0: len = 32 + rnd() % (file.size - 32); break;                                                                  // truncated
        case 1: for (int k = 0; k < 4; ++k) copy[32 + rnd() % (file.size - 32)] ^= 1 << (rnd() % 8); break;                 // bit flips
        case 2: for (int k = 0; k < 12; ++k) copy[32 + rnd() % (file.size - 32)] = static_cast<uint8_t>(rnd()); break;      // noise
        default: for (int k = 0; k < 3; ++k) copy[32 + rnd() % (file.size < 400 ? file.size - 32 : 400)] = static_cast<uint8_t>(rnd()); break;  // headers
      }
      unsigned frames, w, h;
      unsigned long long hash;
      vp8_decode(a, copy, len, &frames, &w, &h, &hash, true);
      free(copy);
    }
    // Every prefix of the first packets, byte by byte: a frame cut anywhere is refused or decoded, never read beyond.
    {
      long pos = 32;
      ConstByteView pk;
      for (int packet = 0; packet < 2 && IvfFile::next(file.data, file.size, &pos, &pk); ++packet) {
        for (uint64_t cut = 0; cut < pk.size() && cut < 300; ++cut) {
          hardware::Vp8Decoder dec(a);
          dec.decode(pk.first(cut), 0);
        }
      }
    }
  }
  CHECK(a.live == 0);
}

// The Intel media engine device: a software codec that has looked at (pretend) hardware registers and says what it found.
static void test_intel_media_engine() {
  TestAllocator heap;
  std::vector<uint32_t> words(IntelMediaEngineHw::kRegisterWindow / 4, 0);
  const MmioRegion registers(reinterpret_cast<uint64_t>(words.data()), words.size() * 4);
  words[0x0D50 / 4] = 1;  // box 0 acknowledges its power request
  IntelMediaVideoEngine engine(heap, 0x9A49, registers);  // Tiger Lake
  CHECK(engine.initialize() && engine.info().hardware_engine && !engine.info().accelerated);
  CHECK(engine.engine_status().probed && engine.engine_status().vdbox_count() == 1);
  CHECK(engine.engine_decode_codecs() & codec_mask(VideoCodec::Av1));
  CHECK(engine.can_decode(VideoCodec::H264));  // sessions still run in software
  IntelMediaVideoEngine blind(heap, 0x9A49);   // no registers: nothing is read
  CHECK(blind.initialize() && !blind.engine_status().probed);
  IntelMediaVideoEngine meteor(heap, 0x7D55, registers);  // media on a separate tile: not probed
  CHECK(meteor.initialize() && !meteor.engine_status().probed);
}

static void test_nvidia_media_engine() {
  TestAllocator a;
  const uint32_t h264 = codec_mask(VideoCodec::H264), hevc = codec_mask(VideoCodec::Hevc), vp9 = codec_mask(VideoCodec::Vp9), av1 = codec_mask(VideoCodec::Av1);
  NvidiaVideoEngine pascal(a, 0x1B80), ampere(a, 0x2204), hopper(a, 0x2330), ada(a, 0x2684), blackwell(a, 0x2B85), unknown(a);
  CHECK(pascal.engine_decode_codecs() & hevc && pascal.engine_decode_codecs() & vp9 && !(pascal.engine_decode_codecs() & av1) && !(pascal.engine_encode_codecs() & av1));
  CHECK(ampere.engine_decode_codecs() & av1 && !(ampere.engine_encode_codecs() & av1) && ampere.engine_encode_codecs() & hevc);
  CHECK(hopper.engine_decode_codecs() & av1 && hopper.engine_encode_codecs() == 0);
  CHECK(ada.engine_encode_codecs() & av1 && blackwell.engine_encode_codecs() & av1 && ada.engine_encode_codecs() & h264);
  CHECK(unknown.engine_decode_codecs() == 0 && unknown.engine_encode_codecs() == 0);
  // A device ID the table does not know is classified by the chip ID read from the GPU (GB202); a known ID is not overruled by it.
  NvidiaVideoEngine by_chip(a, 0x0001, 0x1B200000u), by_id(a, 0x1B80, 0x1B200000u), no_chip(a, 0x0001, 0);
  CHECK(by_chip.gpu().architecture == NvidiaGpuArchitecture::Blackwell && by_chip.engine_encode_codecs() & av1);
  CHECK(by_id.gpu().architecture == NvidiaGpuArchitecture::Pascal && no_chip.engine_decode_codecs() == 0);
  CHECK(pascal.initialize() && pascal.info().hardware_engine && !pascal.info().accelerated);
}

// A pretend end of video memory holding an IP discovery table (RDNA 3: graphics 11.0.0 and one VCN 4.0.0), the way the amdgpu driver documents it.
static std::vector<uint32_t> amd_fake_vram() {
  std::vector<uint8_t> t;
  auto u8 = [&](uint32_t v) { t.push_back(static_cast<uint8_t>(v)); };
  auto u16 = [&](uint32_t v) { u8(v); u8(v >> 8); };
  auto u32 = [&](uint32_t v) { u16(v); u16(v >> 16); };
  u32(0x28211407); u16(2); u16(0); u16(0); u16(0); u16(60);
  while (t.size() < 60) u8(0);
  u32(0x53445049); u16(2); u16(0); u32(1); u16(1);
  const size_t die_table = t.size();
  for (int i = 0; i < 16; ++i) { u16(0); u16(0); }
  u8(0); u8(0);
  t[die_table + 2] = static_cast<uint8_t>(t.size()); t[die_table + 3] = static_cast<uint8_t>(t.size() >> 8);
  u16(0); u16(2);
  u16(11); u8(0); u8(0); u8(11); u8(0); u8(0); u8(0);   // graphics
  u16(12); u8(0); u8(0); u8(4); u8(0); u8(0); u8(0);    // video engine
  t[10] = static_cast<uint8_t>(t.size()); t[11] = static_cast<uint8_t>(t.size() >> 8);
  uint16_t sum = 0;
  for (size_t i = 10; i < t.size(); ++i) sum = static_cast<uint16_t>(sum + t[i]);
  t[8] = static_cast<uint8_t>(sum); t[9] = static_cast<uint8_t>(sum >> 8);
  std::vector<uint32_t> words(AmdMediaEngineHw::kDiscoveryWindow / 4, 0);
  for (size_t i = 0; i < t.size(); ++i) words[i / 4] |= static_cast<uint32_t>(t[i]) << (8 * (i % 4));
  return words;
}

static void test_amd_discovery_engine() {
  TestAllocator a;
  std::vector<uint32_t> vram = amd_fake_vram();
  const MmioRegion window(reinterpret_cast<uint64_t>(vram.data()), vram.size() * 4);
  AmdVcnVideoEngine navi31(a, 0x744C, window), no_window(a, 0x744C), vega(a, 0x15DD, window);
  CHECK(navi31.initialize() && navi31.engine_status().probed && navi31.engine_status().vcn_count() == 1 && navi31.engine_status().discovery.vcn.major == 4);
  CHECK(navi31.engine_status().discovery.gc.major == 11 && navi31.engine_status().discovery.checksum_ok);
  CHECK(no_window.initialize() && !no_window.engine_status().probed && no_window.engine_status().vcn_count() == 0);
  CHECK(vega.initialize() && !vega.engine_status().probed);   // Vega has no table: nothing is read
  CHECK(navi31.info().hardware_engine && !navi31.info().accelerated && a.live == 0);
}

static void test_amd_media_engine() {
  TestAllocator a;
  const uint32_t h264 = codec_mask(VideoCodec::H264), hevc = codec_mask(VideoCodec::Hevc), vp9 = codec_mask(VideoCodec::Vp9), av1 = codec_mask(VideoCodec::Av1);
  AmdVcnVideoEngine polaris(a, 0x67DF), raven(a, 0x15DD), navi21(a, 0x73BF), navi31(a, 0x744C), cdna(a, 0x74A1), unknown(a);
  CHECK(polaris.engine_decode_codecs() & hevc && !(polaris.engine_decode_codecs() & vp9) && !(polaris.engine_decode_codecs() & av1));
  CHECK(raven.engine_decode_codecs() & vp9 && !(raven.engine_decode_codecs() & av1));
  CHECK(navi21.engine_decode_codecs() & av1 && !(navi21.engine_encode_codecs() & av1));
  CHECK(navi31.engine_decode_codecs() & av1 && navi31.engine_encode_codecs() & av1 && navi31.engine_encode_codecs() & h264);
  CHECK(cdna.engine_decode_codecs() == 0 && cdna.engine_encode_codecs() == 0 && unknown.engine_decode_codecs() == 0);
  // The sessions still run in software: the engine is there but not used.
  CHECK(navi31.initialize() && navi31.info().hardware_engine && !navi31.info().accelerated);
}

int main() {
  test_formats();
  test_convert();
  test_jpeg_decode_reference();
  test_jpeg_roundtrip();
  test_jpeg_robustness();
  test_sessions();
  test_system_list();
  test_intel_media_engine();
  test_amd_media_engine();
  test_amd_discovery_engine();
  test_nvidia_media_engine();
  test_intel_system();
  test_h264_parser();
  test_h264_encode();
  test_h264_encode_robustness();
  test_vp9();
  test_vp9_robustness();
  test_av1();
  test_av1_robustness();
  test_vp8();
  test_vp8_robustness();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("video codec tests passed\n");
  return 0;
}
