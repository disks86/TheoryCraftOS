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

#include "Hardware/VideoCodec/ColorConvert.hpp"

namespace hardware {

namespace {
uint8_t clamp8(int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// One YUV sample set to a 0xAARRGGBB pixel. 16.16 fixed point; the limited-range scale is 255/219 and 255/224.
uint32_t yuv_to_bgra(int32_t y, int32_t u, int32_t v, bool limited) {
  if (limited) {
    y = ((y - 16) * 76309 + 32768) >> 16;                    // 1.1644
    u = ((u - 128) * 74605 + 32768) >> 16;                   // 255/224 = 1.1384
    v = ((v - 128) * 74605 + 32768) >> 16;
  } else {
    u -= 128;
    v -= 128;
  }
  const int32_t r = y + ((91881 * v + 32768) >> 16);                      // 1.402
  const int32_t g = y - ((22554 * u + 46802 * v + 32768) >> 16);          // 0.34414, 0.71414
  const int32_t b = y + ((116130 * u + 32768) >> 16);                     // 1.772
  return 0xFF000000u | (uint32_t{clamp8(r)} << 16) | (uint32_t{clamp8(g)} << 8) | clamp8(b);
}

// Reads the luma and chroma of pixel (x, y) from an I420 or NV12 picture.
struct YuvReader {
  const VideoFrame& f;
  void sample(uint32_t x, uint32_t y, int32_t* yy, int32_t* u, int32_t* v) const {
    *yy = f.plane[0][uint64_t{y} * f.stride[0] + x];
    const uint32_t cx = x / 2, cy = y / 2;
    if (f.format.pixel_format == PixelFormat::I420) {
      *u = f.plane[1][uint64_t{cy} * f.stride[1] + cx];
      *v = f.plane[2][uint64_t{cy} * f.stride[2] + cx];
    } else {
      const uint8_t* uv = f.plane[1] + uint64_t{cy} * f.stride[1] + 2 * cx;
      *u = uv[0];
      *v = uv[1];
    }
  }
};
}  // namespace

bool convert_to_bgra(const VideoFrame& in, uint32_t* dest, uint32_t pitch) {
  const VideoFormat& fmt = in.format;
  if (!fmt.valid() || !dest || pitch < fmt.width || !in.plane[0]) return false;
  if (fmt.pixel_format == PixelFormat::Bgra8888) {
    for (uint32_t y = 0; y < fmt.height; ++y)
      __builtin_memcpy(dest + uint64_t{y} * pitch, in.plane[0] + uint64_t{y} * in.stride[0], uint64_t{fmt.width} * 4);
    return true;
  }
  if (!in.plane[1] || (fmt.pixel_format == PixelFormat::I420 && !in.plane[2])) return false;
  const YuvReader reader{in};
  for (uint32_t y = 0; y < fmt.height; ++y) {
    uint32_t* row = dest + uint64_t{y} * pitch;
    for (uint32_t x = 0; x < fmt.width; ++x) {
      int32_t yy, u, v;
      reader.sample(x, y, &yy, &u, &v);
      row[x] = yuv_to_bgra(yy, u, v, in.limited_range);
    }
  }
  return true;
}

namespace {
// Pixel (x, y) of a Bgra8888 picture, with the coordinates held inside it (for the odd last row or column).
void read_bgra(const VideoFrame& f, uint32_t x, uint32_t y, int32_t* r, int32_t* g, int32_t* b) {
  if (x >= f.format.width) x = f.format.width - 1;
  if (y >= f.format.height) y = f.format.height - 1;
  const uint8_t* p = f.plane[0] + uint64_t{y} * f.stride[0] + 4 * uint64_t{x};
  *b = p[0];
  *g = p[1];
  *r = p[2];
}

int32_t luma(int32_t r, int32_t g, int32_t b) { return (19595 * r + 38470 * g + 7471 * b + 32768) >> 16; }
int32_t cb(int32_t r, int32_t g, int32_t b) { return ((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16) + 128; }
int32_t cr(int32_t r, int32_t g, int32_t b) { return ((32768 * r - 27439 * g - 5329 * b + 32768) >> 16) + 128; }
}  // namespace

bool convert_frame(const VideoFrame& in, const VideoFrameBuffer& out) {
  const VideoFormat& src = in.format;
  const VideoFormat& dst = out.format();
  if (!out.valid() || !src.valid() || src.width != dst.width || src.height != dst.height || !in.plane[0]) return false;

  if (dst.pixel_format == PixelFormat::Bgra8888) {
    return convert_to_bgra(in, reinterpret_cast<uint32_t*>(out.plane(0).data()), dst.width);
  }

  // Into I420 or NV12.
  const ByteView luma_plane = out.plane(0);
  const uint32_t luma_stride = dst.plane_stride(0);
  if (src.pixel_format == PixelFormat::Bgra8888) {
    for (uint32_t y = 0; y < dst.height; ++y)
      for (uint32_t x = 0; x < dst.width; ++x) {
        int32_t r, g, b;
        read_bgra(in, x, y, &r, &g, &b);
        luma_plane[uint64_t{y} * luma_stride + x] = clamp8(luma(r, g, b));
      }
    for (uint32_t cy = 0; cy < dst.chroma_height(); ++cy)
      for (uint32_t cx = 0; cx < dst.chroma_width(); ++cx) {
        int32_t sr = 0, sg = 0, sb = 0;
        for (uint32_t dy = 0; dy < 2; ++dy)
          for (uint32_t dx = 0; dx < 2; ++dx) {
            int32_t r, g, b;
            read_bgra(in, 2 * cx + dx, 2 * cy + dy, &r, &g, &b);
            sr += r; sg += g; sb += b;
          }
        sr = (sr + 2) / 4; sg = (sg + 2) / 4; sb = (sb + 2) / 4;
        const uint8_t u = clamp8(cb(sr, sg, sb)), v = clamp8(cr(sr, sg, sb));
        if (dst.pixel_format == PixelFormat::I420) {
          out.plane(1)[uint64_t{cy} * dst.plane_stride(1) + cx] = u;
          out.plane(2)[uint64_t{cy} * dst.plane_stride(2) + cx] = v;
        } else {
          const ByteView uv = out.plane(1);
          uv[uint64_t{cy} * dst.plane_stride(1) + 2 * cx] = u;
          uv[uint64_t{cy} * dst.plane_stride(1) + 2 * cx + 1] = v;
        }
      }
    return true;
  }

  // YUV to YUV: the same samples laid out differently (the range is not changed, so a limited-range source stays so).
  if (!in.plane[1] || (src.pixel_format == PixelFormat::I420 && !in.plane[2])) return false;
  for (uint32_t y = 0; y < dst.height; ++y)
    __builtin_memcpy(luma_plane.data() + uint64_t{y} * luma_stride, in.plane[0] + uint64_t{y} * in.stride[0], dst.width);
  for (uint32_t cy = 0; cy < dst.chroma_height(); ++cy)
    for (uint32_t cx = 0; cx < dst.chroma_width(); ++cx) {
      uint8_t u, v;
      if (src.pixel_format == PixelFormat::I420) {
        u = in.plane[1][uint64_t{cy} * in.stride[1] + cx];
        v = in.plane[2][uint64_t{cy} * in.stride[2] + cx];
      } else {
        u = in.plane[1][uint64_t{cy} * in.stride[1] + 2 * cx];
        v = in.plane[1][uint64_t{cy} * in.stride[1] + 2 * cx + 1];
      }
      if (dst.pixel_format == PixelFormat::I420) {
        out.plane(1)[uint64_t{cy} * dst.plane_stride(1) + cx] = u;
        out.plane(2)[uint64_t{cy} * dst.plane_stride(2) + cx] = v;
      } else {
        out.plane(1)[uint64_t{cy} * dst.plane_stride(1) + 2 * cx] = u;
        out.plane(1)[uint64_t{cy} * dst.plane_stride(1) + 2 * cx + 1] = v;
      }
    }
  return true;
}

}  // namespace hardware
