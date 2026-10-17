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
#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Hardware/VideoCodec/NVIDIA/NvidiaVideoEngine.hpp"
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
    CHECK(device.can_decode(VideoCodec::Mjpeg) && device.can_encode(VideoCodec::Raw) && !device.can_decode(VideoCodec::H264) && !device.can_encode(VideoCodec::Av1));
    DecoderConfig h264;
    h264.codec = VideoCodec::H264;
    CHECK(!device.create_decoder(h264));
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
    CHECK(system.decode_codecs() == (codec_mask(VideoCodec::Raw) | codec_mask(VideoCodec::Mjpeg)));
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

int main() {
  test_formats();
  test_convert();
  test_jpeg_decode_reference();
  test_jpeg_roundtrip();
  test_jpeg_robustness();
  test_sessions();
  test_system_list();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("video codec tests passed\n");
  return 0;
}
