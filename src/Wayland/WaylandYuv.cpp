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

#include "Wayland/WaylandYuv.hpp"

namespace wayland {

namespace {

constexpr uint32_t kCodes[] = {
    0x3231564E,  // NV12
    0x3132564E,  // NV21
    0x3631564E,  // NV16
    0x3136564E,  // NV61
    0x3432564E,  // NV24
    0x3234564E,  // NV42
    0x30313050,  // P010
    0x32315559,  // YUV420 (YU12)
    0x32315659,  // YVU420 (YV12)
    0x36315559,  // YUV422 (YU16)
    0x36315659,  // YVU422 (YV16)
    0x34325559,  // YUV444 (YU24)
    0x34325659,  // YVU444 (YV24)
    0x56595559,  // YUYV
    0x55595659,  // YVYU
    0x59565955,  // UYVY
    0x59555956,  // VYUY
};

YuvFormat make(uint32_t code, uint8_t planes, uint8_t hsub, uint8_t vsub, bool swapped, uint8_t sample = 1, bool luma_first = true) {
  YuvFormat f;
  f.code = code;
  f.planes = planes;
  f.hsub = hsub;
  f.vsub = vsub;
  f.swapped = swapped;
  f.sample = sample;
  f.luma_first = luma_first;
  return f;
}

uint32_t clamp8(int32_t v) { return v < 0 ? 0u : v > 255 ? 255u : uint32_t(v); }

// BT.601, limited range, integer (the usual 8-bit fixed point: 1.164 = 298/256, 1.596 = 409/256, 0.391 = 100/256, 0.813 = 208/256, 2.018 = 516/256).
uint32_t to_xrgb(uint32_t y, uint32_t u, uint32_t v) {
  const int32_t c = int32_t(y) - 16, d = int32_t(u) - 128, e = int32_t(v) - 128;
  const uint32_t r = clamp8((298 * c + 409 * e + 128) >> 8);
  const uint32_t g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
  const uint32_t b = clamp8((298 * c + 516 * d + 128) >> 8);
  return 0xFF000000u | r << 16 | g << 8 | b;
}

// Sample `i` of a row (a 16-bit sample keeps its top eight bits).
uint32_t sample(ConstByteView row, uint64_t i, uint8_t size) {
  if (size == 2) return row[i * 2 + 1];      // (little-endian: the high byte second)
  return row[i];
}

}  // namespace

uint64_t YuvFormat::row_bytes(uint32_t plane, uint32_t width) const {
  if (planes == 1) return (uint64_t{width} + 1) / 2 * 4;
  if (plane == 0) return uint64_t{width} * sample;
  const uint64_t chroma = (uint64_t{width} + hsub - 1) / hsub;
  return planes == 2 ? chroma * 2 * sample : chroma;
}

uint32_t YuvFormat::rows(uint32_t plane, uint32_t height) const {
  if (planes == 1 || plane == 0) return height;
  return (height + vsub - 1) / vsub;
}

uint64_t YuvFormat::plane_bytes(uint32_t plane, uint32_t width, uint32_t height, uint32_t stride) const {
  const uint32_t n = rows(plane, height);
  if (n == 0) return 0;
  return uint64_t{stride} * (n - 1) + row_bytes(plane, width);
}

bool yuv_format(uint32_t code, YuvFormat* out) {
  switch (code) {
    case 0x3231564E: *out = make(code, 2, 2, 2, false); return true;
    case 0x3132564E: *out = make(code, 2, 2, 2, true); return true;
    case 0x3631564E: *out = make(code, 2, 2, 1, false); return true;
    case 0x3136564E: *out = make(code, 2, 2, 1, true); return true;
    case 0x3432564E: *out = make(code, 2, 1, 1, false); return true;
    case 0x3234564E: *out = make(code, 2, 1, 1, true); return true;
    case 0x30313050: *out = make(code, 2, 2, 2, false, 2); return true;
    case 0x32315559: *out = make(code, 3, 2, 2, false); return true;
    case 0x32315659: *out = make(code, 3, 2, 2, true); return true;
    case 0x36315559: *out = make(code, 3, 2, 1, false); return true;
    case 0x36315659: *out = make(code, 3, 2, 1, true); return true;
    case 0x34325559: *out = make(code, 3, 1, 1, false); return true;
    case 0x34325659: *out = make(code, 3, 1, 1, true); return true;
    case 0x56595559: *out = make(code, 1, 2, 1, false, 1, true); return true;
    case 0x55595659: *out = make(code, 1, 2, 1, true, 1, true); return true;
    case 0x59565955: *out = make(code, 1, 2, 1, false, 1, false); return true;
    case 0x59555956: *out = make(code, 1, 2, 1, true, 1, false); return true;
    default: return false;
  }
}

ArrayView<const uint32_t> yuv_formats() { return ArrayView<const uint32_t>(kCodes, sizeof(kCodes) / sizeof(kCodes[0])); }

bool yuv_shm_layout(const YuvFormat& f, uint32_t width, uint32_t height, uint32_t stride, uint64_t offsets[3], uint32_t strides[3], uint64_t* bytes) {
  for (int i = 0; i < 3; ++i) offsets[i] = 0, strides[i] = 0;
  strides[0] = stride;
  if (f.planes == 2) {
    strides[1] = f.hsub == 1 ? stride * 2 : stride;      // (a row of UV pairs: twice the samples of a luma row over hsub)
    if (f.hsub == 1 && stride > 0x7FFFFFFFu) return false;
  } else if (f.planes == 3) {
    if (stride % f.hsub != 0) return false;
    strides[1] = strides[2] = stride / f.hsub;
  }
  uint64_t at = 0;
  for (uint32_t p = 0; p < f.planes; ++p) {
    offsets[p] = at;
    if (p + 1 < f.planes) at += uint64_t{strides[p]} * f.rows(p, height);
  }
  *bytes = offsets[f.planes - 1] + f.plane_bytes(f.planes - 1, width, height, strides[f.planes - 1]);
  return true;
}

bool yuv_to_xrgb(const YuvFormat& f, uint32_t width, uint32_t height, ArrayView<const YuvPlane> planes, ArrayView<uint32_t> out) {
  if (f.planes == 0 || planes.size() < f.planes || width == 0 || height == 0) return false;
  if (out.size() < uint64_t{width} * height) return false;
  for (uint32_t p = 0; p < f.planes; ++p) {
    if (planes[p].stride < f.row_bytes(p, width)) return false;
    if (planes[p].bytes.size() < f.plane_bytes(p, width, height, planes[p].stride)) return false;
  }
  const uint32_t first = f.swapped ? 1 : 0;      // which of a chroma pair is U
  for (uint32_t y = 0; y < height; ++y) {
    ArrayView<uint32_t> dest = out.subview(uint64_t{y} * width, width);
    const ConstByteView luma = planes[0].bytes.subview(uint64_t{y} * planes[0].stride, f.row_bytes(0, width));
    if (f.planes == 1) {      // packed 4:2:2: four bytes for two pixels
      const uint64_t yb = f.luma_first ? 0 : 1, cb = f.luma_first ? 1 : 0;
      for (uint32_t x = 0; x < width; ++x) {
        const uint64_t base = uint64_t{x / 2} * 4;
        const uint32_t yy = luma[base + yb + (x & 1) * 2];
        const uint32_t c0 = luma[base + cb], c1 = luma[base + cb + 2];
        dest[x] = to_xrgb(yy, first ? c1 : c0, first ? c0 : c1);
      }
      continue;
    }
    const uint32_t cy = y / f.vsub;
    if (f.planes == 2) {
      const ConstByteView chroma = planes[1].bytes.subview(uint64_t{cy} * planes[1].stride, f.row_bytes(1, width));
      for (uint32_t x = 0; x < width; ++x) {
        const uint64_t cx = x / f.hsub;
        const uint32_t c0 = sample(chroma, cx * 2, f.sample), c1 = sample(chroma, cx * 2 + 1, f.sample);
        dest[x] = to_xrgb(sample(luma, x, f.sample), first ? c1 : c0, first ? c0 : c1);
      }
      continue;
    }
    const ConstByteView a = planes[1].bytes.subview(uint64_t{cy} * planes[1].stride, f.row_bytes(1, width));
    const ConstByteView b = planes[2].bytes.subview(uint64_t{cy} * planes[2].stride, f.row_bytes(2, width));
    const ConstByteView u = first ? b : a, v = first ? a : b;
    for (uint32_t x = 0; x < width; ++x) {
      const uint64_t cx = x / f.hsub;
      dest[x] = to_xrgb(luma[x], u[cx], v[cx]);
    }
  }
  return true;
}

}  // namespace wayland
