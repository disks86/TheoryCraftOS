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
#include "Hardware/VideoCodec/NVIDIA/NvidiaMediaEngineHw.hpp"
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
    CHECK(device.can_decode(VideoCodec::Mjpeg) && device.can_encode(VideoCodec::Raw) && device.can_decode(VideoCodec::H264) && device.can_encode(VideoCodec::H264) && device.can_decode(VideoCodec::Hevc) && !device.can_encode(VideoCodec::Hevc) && !device.can_encode(VideoCodec::Av1));
    DecoderConfig h264;
    h264.codec = VideoCodec::H264;
    CHECK(device.create_decoder(h264));  // an Annex B stream: the parameter sets come with the pictures
    h264.codec_config = ConstByteView(reinterpret_cast<const uint8_t*>("junk"), 4);
    CHECK(!device.create_decoder(h264));  // not an avcC record
    DecoderConfig hevc;
    hevc.codec = VideoCodec::Hevc;
    CHECK(device.create_decoder(hevc));
    hevc.codec_config = ConstByteView(reinterpret_cast<const uint8_t*>("junk"), 4);
    CHECK(!device.create_decoder(hevc));  // not an hvcC record
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
    CHECK(system.at(1)->engine_summary()[0] == 0 && strstr(system.at(0)->engine_summary(), "chip ID not read"));   // no mapper: nothing was read
    CHECK(system.at(2) == nullptr);
    CHECK(system.decode_codecs() == (codec_mask(VideoCodec::Raw) | codec_mask(VideoCodec::Mjpeg) | codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Hevc) | codec_mask(VideoCodec::Vp8) | codec_mask(VideoCodec::Vp9) | codec_mask(VideoCodec::Av1) | codec_mask(VideoCodec::Mpeg2) | codec_mask(VideoCodec::Vc1)));
    DecoderConfig dc;
    dc.codec = VideoCodec::Mjpeg;
    CHECK(system.create_decoder(dc));
    dc.codec = VideoCodec::Hevc;
    CHECK(system.create_decoder(dc));
    dc.codec = VideoCodec::Av1;
    CHECK(system.create_decoder(dc));
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
  uint16_t device_id; bool memory_decode; uint64_t bar0 = 0x1000; uint16_t vendor = 0x8086;
  IntelPci(uint16_t id, bool decode) : device_id(id), memory_decode(decode) {}
  const char* name() const override { return "fake intel pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t index, PciDeviceInfo* out) const override {
    if (index != 0) return false;
    *out = {{0, 2, 0}, vendor, device_id, kPciClassDisplay, 0, 0};
    return true;
  }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset == 0x04 && memory_decode ? 2 : 0; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return bar0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

struct FakeRegisterMapper final : IRegisterMapper {
  std::vector<uint32_t> words;
  int calls = 0;
  uint64_t asked = 0;   // the length of the last request
  explicit FakeRegisterMapper(uint64_t bytes = IntelMediaEngineHw::kRegisterWindow) : words(bytes / 4, 0) {}
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    ++calls;
    asked = length;
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
    IntelPci pci(0x0BD5, true);  // Data Center GPU Max (Xe-HPC): no video engine, no register layout
    FakeRegisterMapper mapper;
    SystemVideoCodecs system(pci, a);
    system.set_mmio(&mapper);
    system.initialize();
    CHECK(mapper.calls == 0 && !intel_engine(system)->engine_status().probed);
    system.shutdown();
  }
  {
    // Meteor Lake, Arc Battlemage and Panther Lake: the media tile is far into the BAR, so the request covers it (a mapper that cannot give that much leaves the engine unprobed).
    const uint64_t window = IntelMediaEngineHw::register_window(IntelGpuArchitecture::XeLpg);
    for (uint16_t id : {uint16_t{0x7D55}, uint16_t{0xE20B}, uint16_t{0xB080}}) {
      IntelPci pci(id, true);
      FakeRegisterMapper mapper(window);
      const uint64_t base = IntelMediaEngineHw::kMediaTileBase;
      mapper.words[(base + 0x0D50) / 4] = mapper.words[(base + 0x0D58) / 4] = 1;
      SystemVideoCodecs system(pci, a);
      system.set_mmio(&mapper);
      system.initialize();
      CHECK(mapper.calls == 1 && mapper.asked == window && intel_engine(system)->engine_status().probed && intel_engine(system)->engine_status().vdbox_count() == 2);
      system.shutdown();
      IntelPci cut_pci(id, true);
      FakeRegisterMapper small;
      SystemVideoCodecs cut(cut_pci, a);
      cut.set_mmio(&small);
      cut.initialize();
      CHECK(small.calls == 1 && !intel_engine(cut)->engine_status().probed);
      cut.shutdown();
    }
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
// An Arc Battlemage card: the media tile is 0x380000 into the register BAR, and its local memory has a BAR of its own.
static void test_intel_arc_engine() {
  TestAllocator heap;
  std::vector<uint32_t> bar(IntelMediaEngineHw::register_window(IntelGpuArchitecture::Xe2) / 4, 0);
  const uint64_t base = IntelMediaEngineHw::kMediaTileBase;
  bar[(base + 0x0D50) / 4] = bar[(base + 0x0D58) / 4] = 1;
  const MmioRegion registers(reinterpret_cast<uint64_t>(bar.data()), bar.size() * 4);
  IntelMediaVideoEngine arc(heap, 0xE20B, registers);   // Arc B580
  arc.set_local_memory_bar(16ull << 30);
  CHECK(arc.initialize() && arc.engine_status().probed && arc.engine_status().vdbox_count() == 2 && arc.gpu().discrete);
  CHECK(strstr(arc.engine_summary(), "Xe2") && strstr(arc.engine_summary(), "2 of 2 awake") && strstr(arc.engine_summary(), "local memory aperture 16384 MiB"));
  CHECK(arc.engine_decode_codecs() & codec_mask(VideoCodec::Av1) && arc.engine_encode_codecs() & codec_mask(VideoCodec::Av1));
  // An integrated part never reports local memory; a card with a BAR too short for the tile is not probed.
  IntelMediaVideoEngine lunar(heap, 0x64A0, registers);
  lunar.set_local_memory_bar(1ull << 30);
  CHECK(lunar.initialize() && lunar.engine_status().probed && !strstr(lunar.engine_summary(), "local memory"));
  std::vector<uint32_t> short_bar(IntelMediaEngineHw::kRegisterWindow / 4, 0);
  IntelMediaVideoEngine cut(heap, 0xE20B, MmioRegion(reinterpret_cast<uint64_t>(short_bar.data()), short_bar.size() * 4));
  CHECK(cut.initialize() && !cut.engine_status().probed && strstr(cut.engine_summary(), "not read"));
}

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
  CHECK(strstr(engine.engine_summary(), "Xe-LP") && strstr(engine.engine_summary(), "video boxes:") && strstr(blind.engine_summary(), "not read"));
  IntelMediaVideoEngine meteor(heap, 0x7D55, registers);  // media on a separate tile: not probed
  CHECK(meteor.initialize() && !meteor.engine_status().probed);
}

// Pretend first page of an NVIDIA register BAR: BOOT_0 holds the chipset and stepping, BOOT_42 repeats the ID (as Turing and newer do).
struct NvidiaFakeRegs {
  std::vector<uint32_t> words = std::vector<uint32_t>(0x1000 / 4, 0);
  NvidiaFakeRegs(uint32_t chipset, uint32_t stepping) {
    words[0] = (chipset << 20) | stepping;
    if (chipset >= 0x160) words[0xA00 / 4] = (chipset << 20) | stepping;
  }
  MmioRegion region() { return MmioRegion(reinterpret_cast<uint64_t>(words.data()), words.size() * 4); }
};

static void test_nvidia_media_engine() {
  TestAllocator a;
  const uint32_t h264 = codec_mask(VideoCodec::H264), hevc = codec_mask(VideoCodec::Hevc), vp9 = codec_mask(VideoCodec::Vp9), av1 = codec_mask(VideoCodec::Av1);
  NvidiaVideoEngine pascal(a, 0x1B80), ampere(a, 0x2204), hopper(a, 0x2330), ada(a, 0x2684), blackwell(a, 0x2B85), unknown(a);
  CHECK(pascal.engine_decode_codecs() & hevc && pascal.engine_decode_codecs() & vp9 && !(pascal.engine_decode_codecs() & av1) && !(pascal.engine_encode_codecs() & av1));
  CHECK(ampere.engine_decode_codecs() & av1 && !(ampere.engine_encode_codecs() & av1) && ampere.engine_encode_codecs() & hevc);
  CHECK(hopper.engine_decode_codecs() & av1 && hopper.engine_encode_codecs() == 0);
  CHECK(ada.engine_encode_codecs() & av1 && blackwell.engine_encode_codecs() & av1 && ada.engine_encode_codecs() & h264);
  CHECK(unknown.engine_decode_codecs() == 0 && unknown.engine_encode_codecs() == 0);
  // A device ID the table does not know is classified by the chip ID read from the GPU (GB202); the chip outranks a table entry; GA100 has no NVENC.
  NvidiaFakeRegs gb202(0x1B2, 0xA1), gp104(0x134, 0x11), ga100(0x170, 0xA1);
  NvidiaVideoEngine by_chip(a, 0x0001, gb202.region()), by_id(a, 0x1B80, gb202.region()), no_chip(a, 0x0001), a100(a, 0x20B0, ga100.region()), plain(a, 0x1B80, gp104.region());
  CHECK(by_chip.initialize() && by_id.initialize() && no_chip.initialize() && a100.initialize() && plain.initialize());
  CHECK(by_chip.gpu().architecture == NvidiaGpuArchitecture::Blackwell && by_chip.engine_encode_codecs() & av1 && by_chip.chip_status().probed);
  CHECK(by_id.gpu().architecture == NvidiaGpuArchitecture::Blackwell);   // the table said Pascal; the GPU says Blackwell
  CHECK(!no_chip.chip_status().probed && no_chip.engine_decode_codecs() == 0);
  CHECK(a100.gpu().architecture == NvidiaGpuArchitecture::Ampere && a100.engine_encode_codecs() == 0 && (a100.engine_decode_codecs() & av1));
  CHECK(plain.gpu().architecture == NvidiaGpuArchitecture::Pascal && plain.engine_encode_codecs() & hevc && plain.chip_status().chip && plain.chip_status().stepping_major == 1);
  CHECK(pascal.initialize() && pascal.info().hardware_engine && !pascal.info().accelerated);
  CHECK(strstr(plain.engine_summary(), "GP104") && strstr(plain.engine_summary(), "rev 11"));
  CHECK(strstr(a100.engine_summary(), "GA100") && strstr(a100.engine_summary(), "NVDEC only") && strstr(plain.engine_summary(), "NVDEC+NVENC"));
  CHECK(strstr(no_chip.engine_summary(), "chip ID not read") && strstr(by_chip.engine_summary(), "GB202") && strstr(by_chip.engine_summary(), "rev A1"));
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

struct FakeBarSizer final : IBarSizer {
  uint64_t size; int calls = 0;
  explicit FakeBarSizer(uint64_t bytes) : size(bytes) {}
  bool bar_size(PciAddress, uint8_t index, uint64_t* out) override { ++calls; if (index != 0) return false; *out = size; return true; }
};

// SystemVideoCodecs finds the window of an AMD GPU: the end of the memory BAR for a card, the BAR itself (up to the scan limit) for an APU.
// A card read directly (the table at the end of an aperture that covers all of the memory) and one reached only through the register BAR.
static void test_amd_card_memory() {
  TestAllocator a;
  std::vector<uint32_t> vram = amd_fake_vram();
  const MmioRegion window(reinterpret_cast<uint64_t>(vram.data()), vram.size() * 4);
  AmdVcnVideoEngine resizable(a, 0x744C, window);
  resizable.set_memory_bar(16368ull << 20);
  CHECK(resizable.initialize() && resizable.engine_status().probed && resizable.engine_status().video_memory_bytes == (16368ull << 20));
  CHECK(strstr(resizable.engine_summary(), "video memory 16368 MiB (all CPU-visible)"));
  // What the display device is told: the card's memory, matched to the engine by PCI function; nothing when the table was not read.
  PciAddress where{};
  CHECK(resizable.gpu_memory_bytes() == (16368ull << 20) && !resizable.gpu_location(&where));
  resizable.set_pci_address({3, 0, 0});
  CHECK(resizable.gpu_location(&where) && where.bus == 3 && where.device == 0 && where.function == 0 && !resizable.gpu_location(nullptr));
  // Without a size for the aperture nothing is claimed.
  AmdVcnVideoEngine unsized(a, 0x744C, window);
  CHECK(unsized.initialize() && unsized.engine_status().probed && unsized.engine_status().video_memory_bytes == 0 && !strstr(unsized.engine_summary(), "video memory"));
  // A card whose table is out of reach and whose register BAR shows no table either: the aperture is all there is to report, and the index registers are parked.
  std::vector<uint32_t> regs(AmdMmIndexVramReader::kRegisterWindow / 4, 0);
  AmdVcnVideoEngine hidden(a, 0x744C, MmioRegion(reinterpret_cast<uint64_t>(vram.data()) + 4096, 4096));
  hidden.set_registers(MmioRegion(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4));
  hidden.set_memory_bar(256ull << 20);
  CHECK(hidden.initialize() && !hidden.engine_status().probed && strstr(hidden.engine_summary(), "not read") && strstr(hidden.engine_summary(), "memory aperture 256 MiB"));
  CHECK(regs[0] == 0 && regs[AmdMmIndexVramReader::kMmIndexHi / 4] == 0);
  // An APU never goes through the index registers.
  AmdVcnVideoEngine apu(a, 0x15BF, MmioRegion());
  apu.set_registers(MmioRegion(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4));
  CHECK(apu.initialize() && !apu.engine_status().probed && regs[0] == 0);
}

static void test_amd_system_window() {
  TestAllocator a;
  std::vector<uint32_t> table = amd_fake_vram();
  {   // Radeon APU: Ryzen 7040 (Phoenix), 4 MiB BAR with a 3 MiB carve-out
    IntelPci pci(0x15BF, true); pci.vendor = 0x1002;
    FakeRegisterMapper mapper(4u << 20);
    FakeBarSizer sizer(4u << 20);
    const size_t at = (3u << 20) - AmdMediaEngineHw::kDiscoveryWindow;
    for (size_t i = 0; i < table.size(); ++i) mapper.words[at / 4 + i] = table[i];
    SystemVideoCodecs system(pci, a);
    system.set_mmio(&mapper); system.set_bar_sizer(&sizer);
    system.initialize();
    const AmdVcnVideoEngine* amd = static_cast<AmdVcnVideoEngine*>(system.at(0));
    CHECK(mapper.calls == 1 && mapper.asked == (4u << 20) && sizer.calls == 1 && amd->engine_status().probed && amd->engine_status().carve_out_bytes == (3u << 20));
    PciAddress gpu_at{};
    CHECK(amd->gpu_memory_bytes() == (3u << 20) && amd->gpu_location(&gpu_at) && gpu_at.bus == 0 && gpu_at.device == 2 && gpu_at.function == 0);   // the fake bus puts the GPU at 0:2.0
    system.shutdown();
  }
  {   // A card (Navi 31, 16 GiB, Resizable BAR off or on): only the last 64 KiB of the aperture is read directly, and the register BAR (BAR 5) is mapped for the
      // index/data route. The fake bus puts every BAR at 0x1000, so only the register page can be mapped; the table is not reachable there, and the log says
      // what the firmware left of the aperture instead.
    IntelPci pci(0x744C, true); pci.vendor = 0x1002;
    FakeRegisterMapper mapper(AmdMmIndexVramReader::kRegisterWindow);
    FakeBarSizer sizer(16ull << 30);
    SystemVideoCodecs system(pci, a);
    system.set_mmio(&mapper); system.set_bar_sizer(&sizer);
    system.initialize();
    const AmdVcnVideoEngine* amd = static_cast<AmdVcnVideoEngine*>(system.at(0));
    CHECK(mapper.calls == 2 && mapper.asked == AmdMmIndexVramReader::kRegisterWindow && sizer.calls == 1);   // the end of the aperture (refused: not at 0x1000), then BAR 5
    CHECK(!amd->engine_status().probed && amd->memory_bar() == (16ull << 30) && strstr(amd->engine_summary(), "memory aperture 16384 MiB"));
    system.shutdown();
  }
  {   // An APU with a huge BAR is mapped only up to the limit; without a sizer or mapper nothing is touched.
    IntelPci pci(0x1681, true); pci.vendor = 0x1002;
    FakeRegisterMapper mapper(AmdMediaEngineHw::kApertureScanLimit < (64u << 20) ? 0 : 8u << 20);
    FakeBarSizer sizer(16ull << 30);
    SystemVideoCodecs system(pci, a);
    system.set_mmio(&mapper); system.set_bar_sizer(&sizer);
    system.initialize();
    CHECK(mapper.asked == AmdMediaEngineHw::kApertureScanLimit);
    system.shutdown();
    FakeRegisterMapper untouched;
    SystemVideoCodecs bare(pci, a);
    bare.set_mmio(&untouched);
    bare.initialize();
    CHECK(untouched.calls == 0);
    bare.shutdown();
  }
}

// A file system that has only the shipped firmware images: "/firmware/<name>" is read from firmware/<name> of the repository (the tests run from its root).
struct HostFirmwareFile final : filesystem::IFile {
  std::vector<uint8_t> bytes;
  uint64_t size() const override { return bytes.size(); }
  filesystem::Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    const uint64_t n = offset >= bytes.size() ? 0 : (buffer.size() < bytes.size() - offset ? buffer.size() : bytes.size() - offset);
    for (uint64_t i = 0; i < n; ++i) buffer[i] = bytes[offset + i];
    *transferred = n;
    return filesystem::Status::Ok;
  }
  filesystem::Status write(uint64_t, ConstByteView, uint64_t*) override { return filesystem::Status::ReadOnly; }
  filesystem::Status resize(uint64_t) override { return filesystem::Status::ReadOnly; }
  filesystem::Status flush() override { return filesystem::Status::Ok; }
};

struct HostFirmwareFileSystem final : filesystem::IFileSystem {
  TestAllocator& allocator;
  int opened = 0;
  bool empty = false;   // a volume with no firmware folder
  explicit HostFirmwareFileSystem(TestAllocator& a) : allocator(a) {}
  filesystem::FileSystemInfo info() const override { return {}; }
  filesystem::Status open_file(StringView path, filesystem::OpenMode, UniquePointer<filesystem::IFile>* file) override {
    constexpr char kPrefix[] = "/firmware/";
    if (empty) return filesystem::Status::NotFound;
    if (path.size() <= sizeof(kPrefix) - 1 || strncmp(path.data(), kPrefix, sizeof(kPrefix) - 1) != 0) return filesystem::Status::NotFound;
    char host[160];
    snprintf(host, sizeof host, "firmware/%.*s", static_cast<int>(path.size() - (sizeof(kPrefix) - 1)), path.data() + sizeof(kPrefix) - 1);
    FILE* f = fopen(host, "rb");
    if (!f) return filesystem::Status::NotFound;
    UniquePointer<HostFirmwareFile> made = UniquePointer<HostFirmwareFile>::make(allocator);
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) made->bytes.insert(made->bytes.end(), chunk, chunk + n);
    fclose(f);
    ++opened;
    *file = UniquePointer<filesystem::IFile>(util::move(made));
    return filesystem::Status::Ok;
  }
  filesystem::Status open_directory(StringView, UniquePointer<filesystem::IDirectory>*) override { return filesystem::Status::Unsupported; }
  filesystem::Status stat(StringView, filesystem::DirectoryEntry*) override { return filesystem::Status::Unsupported; }
  filesystem::Status create_directory(StringView) override { return filesystem::Status::ReadOnly; }
  filesystem::Status remove(StringView) override { return filesystem::Status::ReadOnly; }
  filesystem::Status rename(StringView, StringView) override { return filesystem::Status::ReadOnly; }
  filesystem::Status flush() override { return filesystem::Status::Ok; }
};

// Raven-family APUs read their VCN 1.0 microcode from /firmware; nothing else does yet.
static void test_amd_vcn_firmware() {
  TestAllocator a;
  {
    HostFirmwareFileSystem fs(a);
    AmdVcnVideoEngine raven(a, 0x15DD), picasso(a, 0x15D8), raven2(a, 0x15DD), renoir(a, 0x1636), navi(a, 0x744C);
    raven.set_pci_revision(0x00); picasso.set_pci_revision(0x00); raven2.set_pci_revision(0xC8);
    CHECK(raven.initialize() && picasso.initialize() && raven2.initialize() && renoir.initialize() && navi.initialize());
    CHECK(raven.firmware_status()[0] == 0);   // nothing to say before the file system exists
    CHECK(raven.load_firmware(fs) && picasso.load_firmware(fs) && raven2.load_firmware(fs));
    CHECK(raven.firmware_state() == AmdFirmwareStatus::Ok && raven.firmware().loaded() && raven.firmware().header().ip_version_major == 1);
    CHECK(strstr(raven.firmware_status(), "raven_vcn.bin") && strstr(raven.firmware_status(), "ENC 1.15 DEC 3 revision 9") && strstr(raven.firmware_status(), "engine not started"));
    CHECK(strstr(picasso.firmware_status(), "picasso_vcn.bin") && strstr(raven2.firmware_status(), "raven2_vcn.bin"));
    CHECK(strstr(raven.engine_summary(), "firmware raven_vcn.bin") && !strstr(raven.engine_summary(), "not read"));   // the boot-log line carries it
    CHECK(raven.firmware().ucode().size() == raven.firmware().header().ucode_size_bytes && raven.firmware().ucode().size() > 300000);
    // Generations with no VCN 1.0 file say nothing and are not asked to load anything.
    const int before = fs.opened;
    CHECK(!renoir.load_firmware(fs) && !navi.load_firmware(fs) && renoir.firmware_status()[0] == 0 && navi.firmware_status()[0] == 0 && fs.opened == before);
    CHECK(renoir.firmware_state() == AmdFirmwareStatus::NotApplicable);
    // The software codecs and their reported hardware do not change.
    CHECK(raven.info().hardware_engine && !raven.info().accelerated);
  }
  {
    // A volume without the folder: the engine says what it expected, loads nothing, and stays a software codec.
    HostFirmwareFileSystem none(a);
    none.empty = true;
    AmdVcnVideoEngine raven(a, 0x15DD);
    CHECK(raven.initialize() && !raven.load_firmware(none) && !raven.firmware().loaded() && raven.firmware_state() == AmdFirmwareStatus::NotFound);
    CHECK(strstr(raven.firmware_status(), "not found") && strstr(raven.firmware_status(), "/firmware/raven_vcn.bin") && raven.firmware().ucode().empty());
    CHECK(strstr(raven.engine_summary(), "firmware raven_vcn.bin: not found") && raven.info().hardware_engine && !raven.info().accelerated);
    // Asking again after the file appears works (the volume was mounted late).
    HostFirmwareFileSystem later(a);
    CHECK(raven.load_firmware(later) && raven.firmware().loaded() && strstr(raven.engine_summary(), "microcode loaded"));
  }
  CHECK(a.live == 0);
}

// Registers and a clock for the engine-level start test: UVD_STATUS (dword 0x7E00 + 0x5AF) reads IDLE when the fake VCPU is "running", everything else reads zero.
struct VcnStatusRegisters final : IVcnRegisters {
  bool answers = true;
  int writes = 0;
  uint32_t read(uint32_t dword) override { return dword == 0x7E00 + 0x5AF && answers ? 2u : 0u; }
  void write(uint32_t, uint32_t) override { ++writes; }
};
struct SteppingClock final : IClock {
  mutable uint64_t t = 0;
  uint64_t now_us() const override { t += 50; return t; }
  const char* name() const override { return "stepping clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

// The engine starts VCN 1.0 on request, says so in the boot log, and goes back to sleep when stopped.
static void test_amd_vcn_start() {
  TestAllocator a;
  HostFirmwareFileSystem fs(a);
  std::vector<uint32_t> block, rings;
  AmdVcn1::Memory memory;
  auto prepare = [&](const AmdVcnVideoEngine& engine) {
    block.assign(AmdVcn1::block_bytes(engine.firmware()) / 4, 0xFFFFFFFFu);
    rings.assign(AmdVcn1::kRingsTotalBytes / 4, 0xFFFFFFFFu);
    memory.block_gpu_address = 0x280000000ull; memory.block = MmioRegion(reinterpret_cast<uint64_t>(block.data()), block.size() * 4);
    memory.rings_gpu_address = 0x270000000ull; memory.rings = MmioRegion(reinterpret_cast<uint64_t>(rings.data()), rings.size() * 4);
  };
  SteppingClock clock;
  AmdVcn1::Config config;
  config.gb_addr_config = 0x26013041;
  {
    AmdVcnVideoEngine raven(a, 0x15DD);
    VcnStatusRegisters regs;
    CHECK(raven.initialize() && !raven.engine_running());
    CHECK(raven.start_engine(regs, clock, memory, config) == AmdVcn1::Status::BadFirmware && regs.writes == 0);   // no firmware loaded yet
    CHECK(raven.load_firmware(fs) && strstr(raven.firmware_status(), "engine not started"));
    prepare(raven);
    CHECK(raven.start_engine(regs, clock, memory, config) == AmdVcn1::Status::Ok && raven.engine_running() && regs.writes == 69);   // the driver's start sequence in this configuration: 69 register writes (AmdVcn1Test checks them one by one)
    CHECK(strstr(raven.firmware_status(), "engine started, idle; nothing decodes on it yet") && strstr(raven.engine_summary(), "engine started"));
    CHECK(raven.info().hardware_engine && !raven.info().accelerated && raven.info().accelerated_decode_codecs == 0);   // nothing is decoded on it
    const int before = regs.writes;
    raven.stop_engine();
    CHECK(!raven.engine_running() && regs.writes > before && strstr(raven.firmware_status(), "engine stopped"));
    const int after = regs.writes;
    raven.stop_engine();   // nothing to stop
    CHECK(regs.writes == after);
  }
  {
    // A VCPU that never answers: the start fails, the engine is put back to sleep, and the log says so.
    AmdVcnVideoEngine raven(a, 0x15DD);
    VcnStatusRegisters regs;
    regs.answers = false;
    CHECK(raven.initialize() && raven.load_firmware(fs));
    prepare(raven);
    CHECK(raven.start_engine(regs, clock, memory, config) == AmdVcn1::Status::TimedOut && !raven.engine_running());
    CHECK(strstr(raven.firmware_status(), "engine start failed: the VCPU did not answer") && !raven.info().accelerated);
    // A block that is too small is refused before anything is written to the card.
    VcnStatusRegisters other;
    memory.block = MmioRegion(memory.block.address(), 4096);
    CHECK(raven.start_engine(other, clock, memory, config) == AmdVcn1::Status::BadMemory && other.writes == 0 && strstr(raven.firmware_status(), "bad memory block"));
  }
  CHECK(a.live == 0);
}

// A mapper that hands out plain memory for any physical range, remembers what was asked, and refuses what lies at or above a limit (memory the OS owns).
struct AnyRangeMapper final : IRegisterMapper {
  std::vector<std::vector<uint32_t>> windows;
  std::vector<std::pair<uint64_t, uint64_t>> asked;
  uint64_t refuse_from = UINT64_MAX;
  AnyRangeMapper() { windows.reserve(8); }   // the windows must not move while regions point at them
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    asked.push_back({physical, length});
    if (physical >= refuse_from) return false;
    windows.emplace_back(length / 4, 0);
    *region = MmioRegion(reinterpret_cast<uint64_t>(windows.back().data()), length);
    return true;
  }
};

// BAR 5 of a Raven APU as plain memory, with the memory controller's registers holding a 512 MiB carve-out (GPU 0xF400000000, CPU 0xE0000000).
struct FakeRavenBar {
  std::vector<uint32_t> words = std::vector<uint32_t>(0x80000 / 4, 0);
  explicit FakeRavenBar(bool alive = true, uint32_t gb = 0x24000042) {
    if (alive) {
      words[0x0D20 + 0x00C3] = 512;                                // RCC_CONFIG_MEMSIZE, MiB
      words[0x1A000 + 0x082C] = static_cast<uint32_t>(0xF400000000ull >> 24);   // MC_VM_FB_LOCATION_BASE
      words[0x1A000 + 0x082D] = static_cast<uint32_t>((0xF400000000ull + (512ull << 20) - (1u << 24)) >> 24);   // MC_VM_FB_LOCATION_TOP
      words[0x2000 + 0x096B] = static_cast<uint32_t>(0xE0000000ull >> 24);       // MC_VM_FB_OFFSET
      words[0x2000 + 0x063E] = gb;                                  // GB_ADDR_CONFIG
    } else {
      for (uint32_t& w : words) w = 0xFFFFFFFFu;                    // a block that is off
    }
  }
  MmioRegion region() { return MmioRegion(reinterpret_cast<uint64_t>(words.data()), words.size() * 4); }
};

// Finding the memory block and the address configuration of a VCN 1.0 engine: planned from reads, mapped, reported - and what happens when the machine does not
// look as it should.
static void test_amd_vcn_plan() {
  TestAllocator a;
  HostFirmwareFileSystem fs(a);
  const uint64_t cpu = 0xE0000000ull, gpu = 0xF400000000ull;
  {
    FakeRavenBar bar;
    AnyRangeMapper mapper;
    AmdVcnVideoEngine raven(a, 0x15DD);
    raven.set_registers(bar.region());
    VideoEngineEnvironment env;
    env.mapper = &mapper; env.framebuffer_base = cpu; env.framebuffer_bytes = 8ull << 20;
    CHECK(raven.initialize() && !raven.plan_engine(env) && strstr(raven.plan_status(), "no firmware") && mapper.asked.empty());   // the firmware comes first
    CHECK(raven.load_firmware(fs) && raven.plan_engine(env) && raven.engine_planned());
    CHECK(strstr(raven.plan_status(), "block at video memory +25 MiB") && strstr(raven.plan_status(), "gb_addr_config 0x24000042") && strstr(raven.plan_status(), "the hardware agrees"));
    CHECK(strstr(raven.plan_status(), "GPU 0xF401900000") && strstr(raven.plan_status(), "CPU 0xE1900000") && strstr(raven.plan_status(), "1008 KiB"));
    CHECK(raven.plan().block_gpu_address == gpu + (25ull << 20) && raven.plan().block_physical == cpu + (25ull << 20));
    CHECK(mapper.asked.size() == 2 && mapper.asked[0].first == cpu + (25ull << 20) && mapper.asked[1].first == cpu + (26ull << 20));
    // Planning wrote nothing to the card: the register block is exactly as it was.
    FakeRavenBar pristine;
    CHECK(bar.words == pristine.words);
    CHECK(!raven.engine_running() && !raven.info().accelerated);
    // Starting with the plan: the status register reads IDLE, so the fake VCPU "boots"; the microcode lands in the mapped block and the engine is up.
    bar.words[0x7E00 + 0x5AF] = 2;   // UVD_STATUS
    SteppingClock clock;
    CHECK(raven.start_planned_engine(clock) == AmdVcn1::Status::Ok && raven.engine_running());
    CHECK(mapper.windows[0][0] == (static_cast<uint32_t>(raven.firmware().ucode()[0]) | raven.firmware().ucode()[1] << 8 | raven.firmware().ucode()[2] << 16 | static_cast<uint32_t>(raven.firmware().ucode()[3]) << 24));
    // The engine was told where its block is, as the GPU addresses it (0xF401900000): the cache-window registers hold the low and high halves.
    CHECK(bar.words[0x7E00 + 0x045F] == 0x01900000u && bar.words[0x7E00 + 0x045E] == 0xF4u);   // UVD_LMI_VCPU_CACHE_64BIT_BAR_LOW / _HIGH
    raven.stop_engine();
    CHECK(!raven.engine_running());
  }
  {   // The registers answer all ones (the block is powered down): no plan, and the log says why.
    FakeRavenBar off(false);
    AnyRangeMapper mapper;
    AmdVcnVideoEngine raven(a, 0x15DD);
    raven.set_registers(off.region());
    VideoEngineEnvironment env;
    env.mapper = &mapper; env.framebuffer_base = cpu; env.framebuffer_bytes = 8ull << 20;
    CHECK(raven.initialize() && raven.load_firmware(fs) && !raven.plan_engine(env) && strstr(raven.plan_status(), "did not answer") && mapper.asked.empty() && !raven.engine_planned());
    CHECK(raven.start_planned_engine(SteppingClock()) == AmdVcn1::Status::BadMemory);   // nothing planned, nothing started
  }
  {   // A framebuffer that is not in the carve-out the registers describe: the registers cannot be about this machine.
    FakeRavenBar bar;
    AnyRangeMapper mapper;
    AmdVcnVideoEngine raven(a, 0x15DD);
    raven.set_registers(bar.region());
    VideoEngineEnvironment env;
    env.mapper = &mapper; env.framebuffer_base = 0x80000000ull; env.framebuffer_bytes = 8ull << 20;
    CHECK(raven.initialize() && raven.load_firmware(fs) && !raven.plan_engine(env) && strstr(raven.plan_status(), "not inside the video memory") && mapper.asked.empty());
    env.framebuffer_base = 0; env.framebuffer_bytes = 0;
    CHECK(!raven.plan_engine(env) && strstr(raven.plan_status(), "framebuffer is unknown") && mapper.asked.empty());
  }
  {   // A chip whose graphics block reports another address configuration than the variant we took it for.
    FakeRavenBar bar(true, 0x26013041);
    AnyRangeMapper mapper;
    AmdVcnVideoEngine raven(a, 0x15DD);
    raven.set_registers(bar.region());
    VideoEngineEnvironment env;
    env.mapper = &mapper; env.framebuffer_base = cpu; env.framebuffer_bytes = 8ull << 20;
    CHECK(raven.initialize() && raven.load_firmware(fs) && !raven.plan_engine(env) && strstr(raven.plan_status(), "differs") && mapper.asked.empty());
    // The same hardware on a revision-8 chip (Raven2) is the match.
    AmdVcnVideoEngine raven2(a, 0x15DD);
    raven2.set_pci_revision(0x81);
    raven2.set_registers(bar.region());
    CHECK(raven2.initialize() && raven2.load_firmware(fs) && raven2.plan_engine(env) && strstr(raven2.plan_status(), "gb_addr_config 0x26013041"));
  }
  {   // The mapper refuses the block (it is memory the OS owns): not planned, said so, nothing started.
    FakeRavenBar bar;
    AnyRangeMapper mapper;
    mapper.refuse_from = cpu;
    AmdVcnVideoEngine raven(a, 0x15DD);
    raven.set_registers(bar.region());
    VideoEngineEnvironment env;
    env.mapper = &mapper; env.framebuffer_base = cpu; env.framebuffer_bytes = 8ull << 20;
    CHECK(raven.initialize() && raven.load_firmware(fs) && !raven.plan_engine(env) && strstr(raven.plan_status(), "memory the OS owns") && !raven.engine_planned());
    CHECK(raven.start_planned_engine(SteppingClock()) == AmdVcn1::Status::BadMemory);
    // No mapper at all, and no register access: said so, nothing mapped.
    VideoEngineEnvironment none;
    none.framebuffer_base = cpu; none.framebuffer_bytes = 8ull << 20;
    CHECK(!raven.plan_engine(none) && strstr(raven.plan_status(), "no way to map memory"));
    AmdVcnVideoEngine blind(a, 0x15DD);
    CHECK(blind.initialize() && blind.load_firmware(fs) && !blind.plan_engine(env) && strstr(blind.plan_status(), "no access to the registers"));
  }
  {   // Not a VCN 1.0 part: nothing to plan, nothing said.
    FakeRavenBar bar;
    AnyRangeMapper mapper;
    AmdVcnVideoEngine renoir(a, 0x1636);
    renoir.set_registers(bar.region());
    VideoEngineEnvironment env;
    env.mapper = &mapper; env.framebuffer_base = cpu; env.framebuffer_bytes = 8ull << 20;
    CHECK(renoir.initialize() && !renoir.plan_engine(env) && renoir.plan_status()[0] == 0 && mapper.asked.empty());
  }
  CHECK(a.live == 0);
}

// Starting the engine on its own, as the boot code does: only when the plan is whole and the graphics block confirmed the address configuration.
static void test_amd_vcn_start_hardware() {
  TestAllocator a;
  HostFirmwareFileSystem fs(a);
  const uint64_t cpu = 0xE0000000ull;
  SteppingClock clock;
  auto engine_for = [&](FakeRavenBar& bar, AnyRangeMapper& mapper, AmdVcnVideoEngine& e) {
    e.set_registers(bar.region());
    VideoEngineEnvironment env;
    env.mapper = &mapper; env.framebuffer_base = cpu; env.framebuffer_bytes = 8ull << 20;
    CHECK(e.initialize() && e.load_firmware(fs));
    return e.plan_engine(env);
  };
  {   // Everything agrees and the VCPU answers, but the ring is not served (the registers are plain memory): the engine starts, then its H.264 self-test cannot finish a
      // picture, so it is stopped again and nothing is reported as accelerated. (The path where the self-test passes is in AmdVcnAvcDecoderTest, against a simulated ring.)
    FakeRavenBar bar;
    bar.words[0x7E00 + 0x5AF] = 2;   // UVD_STATUS reads IDLE: the fake VCPU answers
    AnyRangeMapper mapper;
    AmdVcnVideoEngine raven(a, 0x15DD);
    CHECK(engine_for(bar, mapper, raven));
    CHECK(raven.plan().work_bytes > 0 && raven.plan().work_offset == 27ull << 20);
    CHECK(!raven.start_hardware(clock) && !raven.engine_running() && !raven.decode_ready());
    CHECK(strstr(raven.firmware_status(), "H.264 self-test failed") && strstr(raven.firmware_status(), "engine stopped"));
    CHECK(!raven.info().accelerated && raven.info().accelerated_decode_codecs == 0);
    raven.shutdown();   // twice is harmless
  }
  {   // The work area cannot be mapped (the mapper says it is memory the OS owns): the engine is stopped and the log says decoding is not enabled.
    FakeRavenBar bar;
    bar.words[0x7E00 + 0x5AF] = 2;
    AnyRangeMapper mapper;
    mapper.refuse_from = cpu + (27ull << 20);
    AmdVcnVideoEngine raven(a, 0x15DD);
    CHECK(engine_for(bar, mapper, raven));
    CHECK(!raven.start_hardware(clock) && !raven.engine_running() && strstr(raven.firmware_status(), "decoding not enabled") && raven.info().accelerated_decode_codecs == 0);
  }
  {   // The graphics block does not report the address configuration (GB_ADDR_CONFIG reads zero): planned, but not started.
    FakeRavenBar bar(true, 0);
    bar.words[0x7E00 + 0x5AF] = 2;
    AnyRangeMapper mapper;
    AmdVcnVideoEngine raven(a, 0x15DD);
    CHECK(engine_for(bar, mapper, raven) && raven.plan().gb_check == AmdGbAddrConfigCheck::HardwareUnreadable);
    FakeRavenBar untouched(true, 0);
    untouched.words[0x7E00 + 0x5AF] = 2;
    CHECK(!raven.start_hardware(clock) && !raven.engine_running() && strstr(raven.firmware_status(), "did not confirm the address configuration"));
    CHECK(bar.words == untouched.words);   // and nothing was written to the card
  }
  {   // No plan (the registers do not answer): nothing is started.
    FakeRavenBar off(false);
    AnyRangeMapper mapper;
    AmdVcnVideoEngine raven(a, 0x15DD);
    CHECK(!engine_for(off, mapper, raven) && !raven.start_hardware(clock) && !raven.engine_running());
    FakeRavenBar pristine(false);
    CHECK(off.words == pristine.words);
  }
  {   // A VCPU that never answers: a failed start, three tries only, engine left stopped, the log says so.
    FakeRavenBar bar;
    AnyRangeMapper mapper;
    AmdVcnVideoEngine raven(a, 0x15DD);
    CHECK(engine_for(bar, mapper, raven));
    SteppingClock before;
    const uint64_t start = clock.now_us();
    CHECK(!raven.start_hardware(clock) && !raven.engine_running() && strstr(raven.firmware_status(), "the VCPU did not answer"));
    const uint64_t spent_ms = (clock.now_us() - start) / 1000;
    CHECK(spent_ms < 4000 && spent_ms > 2500);   // three seconds of waiting, not the driver's ten
    raven.shutdown();   // nothing to stop
  }
  {   // A device that needs no engine start does not claim to have started one.
    AmdVcnVideoEngine renoir(a, 0x1636);
    CHECK(renoir.initialize() && !renoir.start_hardware(clock) && !renoir.engine_running() && renoir.firmware_status()[0] == 0);
  }
  CHECK(a.live == 0);
}

static void test_amd_discovery_engine() {
  TestAllocator a;
  std::vector<uint32_t> vram = amd_fake_vram();
  const MmioRegion window(reinterpret_cast<uint64_t>(vram.data()), vram.size() * 4);
  AmdVcnVideoEngine navi31(a, 0x744C, window), no_window(a, 0x744C), vega(a, 0x15DD, window);
  CHECK(navi31.initialize() && navi31.engine_status().probed && navi31.engine_status().vcn_count() == 1 && navi31.engine_status().discovery.vcn.major == 4);
  CHECK(navi31.engine_status().discovery.gc.major == 11 && navi31.engine_status().discovery.checksum_ok && navi31.engine_status().carve_out_bytes == 0);
  // A Radeon APU (Ryzen 7040, Phoenix): the window is the memory BAR, and the table is at the end of a carve-out inside it.
  std::vector<uint32_t> bar(4u << 18, 0);   // 4 MiB of words
  const size_t at = (3u << 20) - AmdMediaEngineHw::kDiscoveryWindow;
  for (size_t i = 0; i < vram.size() && i < AmdMediaEngineHw::kDiscoveryWindow / 4; ++i) bar[at / 4 + i] = vram[i];
  AmdVcnVideoEngine phoenix(a, 0x15BF, MmioRegion(reinterpret_cast<uint64_t>(bar.data()), bar.size() * 4));
  CHECK(phoenix.initialize() && phoenix.engine_status().probed && phoenix.engine_status().carve_out_bytes == (3u << 20) && !phoenix.gpu().discrete);
  CHECK(strstr(phoenix.engine_summary(), "Radeon 780M") && strstr(phoenix.engine_summary(), "VCN 4.0.0") && strstr(phoenix.engine_summary(), "shared memory 3 MiB"));
  AmdVcnVideoEngine phoenix_blind(a, 0x15BF);
  CHECK(phoenix_blind.initialize() && !phoenix_blind.engine_status().probed && strstr(phoenix_blind.engine_summary(), "not read"));
  CHECK(phoenix.gpu_memory_bytes() == (3u << 20) && phoenix_blind.gpu_memory_bytes() == 0);   // an APU reports its carve-out
  CHECK(!strstr(navi31.engine_summary(), "shared memory"));
  CHECK(no_window.initialize() && !no_window.engine_status().probed && no_window.engine_status().vcn_count() == 0);
  CHECK(vega.initialize() && !vega.engine_status().probed);   // Vega has no table: nothing is read
  CHECK(navi31.info().hardware_engine && !navi31.info().accelerated && a.live == 0);
  // The boot-log line: the generation, the video engine the table lists, and what to say when nothing was read.
  CHECK(strstr(navi31.engine_summary(), "RDNA 3") && strstr(navi31.engine_summary(), "VCN 4.0.0 x1") && strstr(navi31.engine_summary(), "graphics 11.0"));
  CHECK(strstr(no_window.engine_summary(), "not read") && strstr(vega.engine_summary(), "Vega") && !strstr(vega.engine_summary(), "not read"));   // (no table to be missing)
}

static void test_amd_media_engine() {
  TestAllocator a;
  const uint32_t h264 = codec_mask(VideoCodec::H264), hevc = codec_mask(VideoCodec::Hevc), vp9 = codec_mask(VideoCodec::Vp9), av1 = codec_mask(VideoCodec::Av1);
  AmdVcnVideoEngine polaris(a, 0x67DF), raven(a, 0x15DD), navi21(a, 0x73BF), navi31(a, 0x744C), cdna(a, 0x74A1), unknown(a);
  CHECK(polaris.engine_decode_codecs() & hevc && !(polaris.engine_decode_codecs() & vp9) && !(polaris.engine_decode_codecs() & av1));
  CHECK(raven.engine_decode_codecs() & vp9 && !(raven.engine_decode_codecs() & av1));
  CHECK(raven.engine_decode_codecs() & codec_mask(VideoCodec::Mpeg2) && polaris.engine_decode_codecs() & codec_mask(VideoCodec::Mpeg2) && !(raven.engine_encode_codecs() & codec_mask(VideoCodec::Mpeg2)));
  CHECK(!(unknown.engine_decode_codecs() & codec_mask(VideoCodec::Mpeg2)));
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
  test_intel_arc_engine();
  test_amd_media_engine();
  test_amd_vcn_firmware();
  test_amd_vcn_start();
  test_amd_vcn_plan();
  test_amd_vcn_start_hardware();
  test_amd_discovery_engine();
  test_amd_system_window();
  test_amd_card_memory();
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
