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

// Types shared by the 2D and 3D renderers: extents, pixel formats, vectors and
// matrices (column-major, Vulkan conventions: +Y is down on screen, clip-space
// depth runs 0..1) and the Surface that software rendering draws into.
#pragma once
#include <stdint.h>
#include "Math/FloatMath.hpp"
#include "Utility/ArrayView.hpp"

namespace graphics {

struct Extent2D {
  uint32_t width = 0, height = 0;
};

struct Rect2D {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};

enum class Format : uint8_t {
  Undefined,
  B8G8R8A8Unorm,  // 32-bit; memory order B, G, R, A (the common display layout)
  R8G8B8A8Unorm,  // 32-bit; memory order R, G, B, A
  D32Float,       // depth only
  R16G16B16A16Float,   // 64-bit half floats, memory order R, G, B, A (kFeatureFloatFormats); values are not clamped
  R32G32B32A32Float,   // 128-bit floats, memory order R, G, B, A (kFeatureFloatFormats)
};

constexpr bool is_float_format(Format f) { return f == Format::R16G16B16A16Float || f == Format::R32G32B32A32Float; }
constexpr uint32_t bytes_per_pixel(Format f) { return f == Format::Undefined ? 0 : f == Format::R16G16B16A16Float ? 8 : f == Format::R32G32B32A32Float ? 16 : 4; }

// IEEE half floats (1 sign, 5 exponent, 10 fraction bits) to and from float; to_half rounds to nearest even, saturates to infinity, and keeps NaN.
inline float half_to_float(uint16_t h) {
  const uint32_t sign = uint32_t{h & 0x8000u} << 16;
  uint32_t exponent = (h >> 10) & 0x1F, fraction = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (fraction == 0) {
      bits = sign;
    } else {   // a subnormal half is a normal float
      exponent = 127 - 15 + 1;
      while (!(fraction & 0x400)) fraction <<= 1, --exponent;
      bits = sign | (exponent << 23) | ((fraction & 0x3FF) << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (fraction << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (fraction << 13);
  }
  return __builtin_bit_cast(float, bits);
}
inline uint16_t float_to_half(float f) {
  const uint32_t bits = __builtin_bit_cast(uint32_t, f);
  const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
  const uint32_t exponent = (bits >> 23) & 0xFF, fraction = bits & 0x7FFFFF;
  if (exponent == 0xFF) return static_cast<uint16_t>(sign | 0x7C00u | (fraction ? 0x200u | (fraction >> 13) : 0u));
  const int32_t e = static_cast<int32_t>(exponent) - 127 + 15;
  if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return sign;
    const uint32_t shift = static_cast<uint32_t>(14 - e);   // bits dropped from the 24-bit significand
    const uint32_t full = fraction | 0x800000u, kept = full >> shift, rest = full & ((1u << shift) - 1), half = 1u << (shift - 1);
    return static_cast<uint16_t>(sign | (kept + ((rest > half || (rest == half && (kept & 1))) ? 1u : 0u)));
  }
  uint32_t out = (static_cast<uint32_t>(e) << 10) | (fraction >> 13);
  const uint32_t rest = fraction & 0x1FFF;
  if (rest > 0x1000 || (rest == 0x1000 && (out & 1))) ++out;   // (a carry out of the fraction bumps the exponent, up to infinity: correct)
  return static_cast<uint16_t>(sign | out);
}

struct Vec2 {
  float x = 0, y = 0;
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

struct Vec4 {
  float x = 0, y = 0, z = 0, w = 0;
};

inline Vec4 operator+(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Vec4 operator-(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline Vec4 operator*(Vec4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }

// Column-major 4x4 matrix: element (row r, column c) is m[c * 4 + r].
struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  static Mat4 identity() { return {}; }
  static Mat4 translation(float x, float y, float z) {
    Mat4 r;
    r.m[12] = x, r.m[13] = y, r.m[14] = z;
    return r;
  }
  static Mat4 scaling(float x, float y, float z) {
    Mat4 r;
    r.m[0] = x, r.m[5] = y, r.m[10] = z;
    return r;
  }
  static Mat4 rotation_x(float radians) {
    const float c = static_cast<float>(fmath::cos(radians)), s = static_cast<float>(fmath::sin(radians));
    Mat4 r;
    r.m[5] = c, r.m[6] = s, r.m[9] = -s, r.m[10] = c;
    return r;
  }
  static Mat4 rotation_y(float radians) {
    const float c = static_cast<float>(fmath::cos(radians)), s = static_cast<float>(fmath::sin(radians));
    Mat4 r;
    r.m[0] = c, r.m[2] = -s, r.m[8] = s, r.m[10] = c;
    return r;
  }
  static Mat4 rotation_z(float radians) {
    const float c = static_cast<float>(fmath::cos(radians)), s = static_cast<float>(fmath::sin(radians));
    Mat4 r;
    r.m[0] = c, r.m[1] = s, r.m[4] = -s, r.m[5] = c;
    return r;
  }
  // Right-handed view space (camera looks down -Z) to Vulkan clip space:
  // Y points down on screen and depth is 0 (near) .. 1 (far).
  static Mat4 perspective(float vertical_fov_radians, float aspect, float near_plane, float far_plane) {
    const float f = static_cast<float>(1.0 / fmath::tan(vertical_fov_radians * 0.5));
    Mat4 r;
    for (float& v : r.m) v = 0;
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = far_plane / (near_plane - far_plane);
    r.m[11] = -1;
    r.m[14] = near_plane * far_plane / (near_plane - far_plane);
    return r;
  }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = sum;
    }
  return r;
}

inline Vec4 operator*(const Mat4& a, Vec4 v) {
  return {a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w,
          a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w,
          a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w,
          a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w};
}

// 0x00RRGGBB / 0xAARRGGBB helpers. Colors in the API are straight (non-premultiplied) alpha.
constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return 0xFF000000u | (uint32_t{r} << 16) | (uint32_t{g} << 8) | b;
}
inline Vec4 unpack_color(uint32_t argb) {
  return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
          ((argb >> 24) & 0xFF) / 255.0f};
}
inline uint8_t to_unorm8(float v) {
  if (!(v > 0)) return 0;  // also catches NaN
  if (v >= 1) return 255;
  return static_cast<uint8_t>(v * 255.0f + 0.5f);
}
inline uint32_t pack_color(Vec4 c) {
  return (uint32_t{to_unorm8(c.w)} << 24) | (uint32_t{to_unorm8(c.x)} << 16) | (uint32_t{to_unorm8(c.y)} << 8) |
         to_unorm8(c.z);
}

// A CPU-visible 32-bit pixel buffer. Callers always hand colors over as
// 0xAARRGGBB; encode()/decode() convert to and from the memory layout the
// format asks for (B8G8R8A8 stores 0xAARRGGBB little-endian as is, R8G8B8A8
// swaps the red and blue bytes).
struct Surface {
  ArrayView<uint32_t> pixels;
  ArrayView<Vec4> floats;   // the pixels of a float format (then `pixels` is empty and `pitch` counts these)
  uint32_t width = 0, height = 0;
  uint32_t pitch = 0;  // in pixels
  Format format = Format::B8G8R8A8Unorm;

  bool valid() const { return width && height && pitch >= width && (pixels.size() >= uint64_t{pitch} * height || floats.size() >= uint64_t{pitch} * height); }
  Vec4* float_row(uint32_t y) const { return floats.data() + uint64_t{y} * pitch; }
  uint32_t* row(uint32_t y) const { return pixels.data() + uint64_t{y} * pitch; }
  // 0xAARRGGBB <-> storage.
  uint32_t encode(uint32_t argb) const {
    if (format != Format::R8G8B8A8Unorm) return argb;
    return (argb & 0xFF00FF00u) | ((argb & 0xFF) << 16) | ((argb >> 16) & 0xFF);
  }
  uint32_t decode(uint32_t stored) const { return encode(stored); }  // the swap is its own inverse
};

}  // namespace graphics
