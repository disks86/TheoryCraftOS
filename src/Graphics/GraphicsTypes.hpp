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
  B8G8R8A8Srgb,        // like B8G8R8A8Unorm, but the colour channels are sRGB encoded (kFeatureSrgbFormats): a texture is read as linear values, a render target takes linear values and stores them encoded
  R8G8B8A8Srgb,        // the same with R, G, B, A in memory order
  R16G16B16A16Unorm,   // 64-bit, four 16-bit normalised channels in the order R, G, B, A (kFeatureUnorm16Formats)
  R32G32B32A32Uint,    // 128-bit, four unsigned integers R, G, B, A (kFeatureIntegerFormats): values travel as the bits of the Vec4s (read_pixels_float); no blending, no filtering - a texture is read with fetch
  R32G32B32A32Sint,    // the same with signed integers
  R16G16B16A16Uint,    // 64-bit, four unsigned 16-bit integers (kFeatureIntegerFormats): a value above 65535 is stored as 65535
  R16G16B16A16Sint,    // four signed 16-bit integers: stored clamped to -32768 .. 32767
  R8G8B8A8Uint,        // 32-bit, four unsigned bytes in the order R, G, B, A: stored clamped to 255 (still a wide format: the pixels travel as Vec4s of integer bits)
  R16G16B16A16Snorm,   // 64-bit, four 16-bit signed normalised channels (-1 .. 1; kFeatureSnormFormats)
  R10G10B10A2Unorm,    // 32-bit packed: R in bits 0-9, G 10-19, B 20-29, A 30-31, all normalised (kFeatureSnormFormats)
  // Block-compressed (Graphics/BlockCompression.hpp): 4 x 4 pixels a block of 8 (BC1) or 16 bytes; sampled only, filled with IImage::write_compressed (kFeatureCompressedFormats).
  Bc1Unorm, Bc2Unorm, Bc3Unorm, Bc1Srgb, Bc2Srgb, Bc3Srgb,
  // 32-bit, one channel (kFeature2SingleChannelFormats): an unsigned or signed integer, or a float. Wide formats (the pixels travel as Vec4s: (r, 0, 0, 1), the 1 an integer one for the integer
  // formats); the integer ones are what atomic operations on storage images take.
  R32Uint, R32Sint, R32Float,
  // The rest of the Direct3D 11 / 12 colour formats (numbered from 64, so that formats other changes append above stay where they are). DeviceFormatCaps / IGraphicsDevice::format_caps
  // say what a device does with each.
  // Packed into 32-bit pixels like B8G8R8A8Unorm (not wide: mip levels as the 8-bit formats have them): a device keeps them as 0xAARRGGBB with the channels the format does not have
  // at 0 (alpha 255) and the channels it has rounded to its bits (Surface::encode); in memory they are their own size (texel_bytes).
  R8Unorm = 64, R8G8Unorm, A8Unorm, B5G6R5Unorm, B5G5R5A1Unorm, B4G4R4A4Unorm,
  // Wide (the pixels travel as Vec4s: WidePixels.hpp): one- and two-channel 8-, 16- and 32-bit formats and the signed 8-bit four-channel ones, the 96-bit ones, the packed floats and
  // R10G10B10A2Uint. The channels a format does not have read as 0 and alpha as 1 (an integer 1 for the integer formats).
  R8Snorm, R8Uint, R8Sint, R8G8Snorm, R8G8Uint, R8G8Sint, R8G8B8A8Snorm, R8G8B8A8Sint,
  R16Unorm, R16Snorm, R16Uint, R16Sint, R16Float, R16G16Unorm, R16G16Snorm, R16G16Uint, R16G16Sint, R16G16Float,
  R32G32Uint, R32G32Sint, R32G32Float, R32G32B32Uint, R32G32B32Sint, R32G32B32Float,
  R11G11B10Float, R9G9B9E5Float, R10G10B10A2Uint,
  // Block-compressed (Graphics/BlockCompression.hpp): BC4 (one channel, 8 bytes a block), BC5 (two), BC6H (half floats, unsigned and signed), BC7.
  Bc4Unorm, Bc4Snorm, Bc5Unorm, Bc5Snorm, Bc6hUfloat, Bc6hSfloat, Bc7Unorm, Bc7Srgb,
};

constexpr bool is_float_format(Format f) { return f == Format::R16G16B16A16Float || f == Format::R32G32B32A32Float; }
constexpr bool is_srgb_format(Format f) { return f == Format::B8G8R8A8Srgb || f == Format::R8G8B8A8Srgb; }
// The formats numbered from 64 (the packed 32-bit ones and the wide ones after them; not the block-compressed ones).
constexpr bool is_packed32_format(Format f) { return f >= Format::R8Unorm && f <= Format::B4G4R4A4Unorm; }
constexpr bool is_extra_wide_format(Format f) { return f >= Format::R8Snorm && f <= Format::R10G10B10A2Uint; }
constexpr bool is_integer_format(Format f) {
  switch (f) {
    case Format::R32G32B32A32Uint: case Format::R32G32B32A32Sint: case Format::R16G16B16A16Uint: case Format::R16G16B16A16Sint: case Format::R8G8B8A8Uint: case Format::R32Uint: case Format::R32Sint:
    case Format::R8Uint: case Format::R8Sint: case Format::R8G8Uint: case Format::R8G8Sint: case Format::R8G8B8A8Sint: case Format::R16Uint: case Format::R16Sint: case Format::R16G16Uint:
    case Format::R16G16Sint: case Format::R32G32Uint: case Format::R32G32Sint: case Format::R32G32B32Uint: case Format::R32G32B32Sint: case Format::R10G10B10A2Uint: return true;
    default: return false;
  }
}
constexpr bool is_signed_integer_format(Format f) {
  return f == Format::R32G32B32A32Sint || f == Format::R16G16B16A16Sint || f == Format::R32Sint || f == Format::R8Sint || f == Format::R8G8Sint || f == Format::R8G8B8A8Sint || f == Format::R16Sint ||
         f == Format::R16G16Sint || f == Format::R32G32Sint || f == Format::R32G32B32Sint;
}
constexpr bool is_single_channel_format(Format f) { return f == Format::R32Uint || f == Format::R32Sint || f == Format::R32Float; }
// Formats whose pixels do not fit a 32-bit word of 0xAARRGGBB: they are read and written as Vec4s (read_pixels_float / write_pixels_float) and have one mip level.
constexpr bool is_wide_format(Format f) {
  return is_float_format(f) || f == Format::R16G16B16A16Unorm || f == Format::R16G16B16A16Snorm || f == Format::R10G10B10A2Unorm || is_integer_format(f) || f == Format::R32Float || is_extra_wide_format(f);
}
// The bytes of one pixel in memory (of the packed 32-bit formats: their own size, not the 32-bit word a device may keep them in; 0 for the block-compressed formats and Undefined).
constexpr uint32_t bytes_per_pixel(Format f) {
  switch (f) {
    case Format::Undefined: return 0;
    case Format::R16G16B16A16Float: case Format::R16G16B16A16Unorm: case Format::R16G16B16A16Snorm: case Format::R16G16B16A16Uint: case Format::R16G16B16A16Sint: return 8;
    case Format::R32G32B32A32Float: case Format::R32G32B32A32Uint: case Format::R32G32B32A32Sint: return 16;
    case Format::R8Unorm: case Format::A8Unorm: case Format::R8Snorm: case Format::R8Uint: case Format::R8Sint: return 1;
    case Format::R8G8Unorm: case Format::B5G6R5Unorm: case Format::B5G5R5A1Unorm: case Format::B4G4R4A4Unorm: case Format::R8G8Snorm: case Format::R8G8Uint: case Format::R8G8Sint:
    case Format::R16Unorm: case Format::R16Snorm: case Format::R16Uint: case Format::R16Sint: case Format::R16Float: return 2;
    case Format::R32G32Uint: case Format::R32G32Sint: case Format::R32G32Float: return 8;
    case Format::R32G32B32Uint: case Format::R32G32B32Sint: case Format::R32G32B32Float: return 12;
    case Format::Bc1Unorm: case Format::Bc2Unorm: case Format::Bc3Unorm: case Format::Bc1Srgb: case Format::Bc2Srgb: case Format::Bc3Srgb: case Format::Bc4Unorm: case Format::Bc4Snorm:
    case Format::Bc5Unorm: case Format::Bc5Snorm: case Format::Bc6hUfloat: case Format::Bc6hSfloat: case Format::Bc7Unorm: case Format::Bc7Srgb: return 4;   // (as before: callers ask the block size)
    default: return 4;
  }
}

// What a device does with images of a format (IGraphicsDevice::format_caps). The kernel's adapters hand the same bits to user mode for each of their allocation formats.
enum FormatCap : uint32_t {
  kFormatSampled = 1u << 0,       // a texture shaders read (sample or load)
  kFormatFiltered = 1u << 1,      // ... with linear filtering
  kFormatRenderTarget = 1u << 2,  // a colour attachment
  kFormatBlendable = 1u << 3,     // ... that blends
  kFormatStorage = 1u << 4,       // a storage image (typed loads and stores)
  kFormatMipmaps = 1u << 5,       // images of more than one level
  kFormatMultisample = 1u << 6,   // multisampled attachments
  kFormatNative = 1u << 7,        // (block-compressed formats) sampled as blocks: IImage::write_compressed; without it the memory manager decodes them (BlockCompression.hpp decoded_format)
};

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

// The packed 32-bit formats (is_packed32_format) as 0xAARRGGBB words, and in memory (little endian, their own size). A channel of n bits is expanded to 8 by rounding (v * 255 / max) and
// narrowed by rounding back, which gives the same n-bit value again; the channels a format does not have are 0 (alpha 255; A8Unorm has alpha alone).
namespace packed_detail {
constexpr uint32_t expand(uint32_t v, uint32_t max) { return (v * 255u + max / 2) / max; }
constexpr uint32_t narrow(uint32_t c, uint32_t max) { return (c * max + 127u) / 255u; }
constexpr uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }
}  // namespace packed_detail
// The texel bits of a packed 32-bit format of a colour (0xAARRGGBB), and back.
constexpr uint32_t packed32_from_argb(Format f, uint32_t c) {
  using namespace packed_detail;
  const uint32_t a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  switch (f) {
    case Format::R8Unorm: return r;
    case Format::R8G8Unorm: return r | g << 8;
    case Format::A8Unorm: return a;
    case Format::B5G6R5Unorm: return narrow(b, 31) | narrow(g, 63) << 5 | narrow(r, 31) << 11;
    case Format::B5G5R5A1Unorm: return narrow(b, 31) | narrow(g, 31) << 5 | narrow(r, 31) << 10 | narrow(a, 1) << 15;
    case Format::B4G4R4A4Unorm: return narrow(b, 15) | narrow(g, 15) << 4 | narrow(r, 15) << 8 | narrow(a, 15) << 12;
    default: return c;
  }
}
constexpr uint32_t packed32_to_argb(Format f, uint32_t v) {
  using namespace packed_detail;
  switch (f) {
    case Format::R8Unorm: return argb(255, v & 0xFF, 0, 0);
    case Format::R8G8Unorm: return argb(255, v & 0xFF, (v >> 8) & 0xFF, 0);
    case Format::A8Unorm: return argb(v & 0xFF, 0, 0, 0);
    case Format::B5G6R5Unorm: return argb(255, expand((v >> 11) & 31, 31), expand((v >> 5) & 63, 63), expand(v & 31, 31));
    case Format::B5G5R5A1Unorm: return argb(((v >> 15) & 1) ? 255 : 0, expand((v >> 10) & 31, 31), expand((v >> 5) & 31, 31), expand(v & 31, 31));
    case Format::B4G4R4A4Unorm: return argb(expand((v >> 12) & 15, 15), expand((v >> 8) & 15, 15), expand((v >> 4) & 15, 15), expand(v & 15, 15));
    default: return v;
  }
}
// What an image of the format keeps of a colour.
constexpr uint32_t quantize_packed32(Format f, uint32_t argb) { return is_packed32_format(f) ? packed32_to_argb(f, packed32_from_argb(f, argb)) : argb; }
// The colour a pixel of a packed 32-bit format kept as 0xAARRGGBB (quantize_packed32) means, exactly: its n-bit codes over 2^n - 1 (the 8-bit word is only near that for 5, 6 and 4 bits).
inline void packed32_to_floats(Format f, uint32_t argb, float out[4]) {
  const uint32_t v = packed32_from_argb(f, argb);
  auto ch = [](uint32_t code, uint32_t max) { return static_cast<float>(code) / static_cast<float>(max); };
  switch (f) {
    case Format::B5G6R5Unorm: out[0] = ch((v >> 11) & 31, 31), out[1] = ch((v >> 5) & 63, 63), out[2] = ch(v & 31, 31), out[3] = 1.0f; return;
    case Format::B5G5R5A1Unorm: out[0] = ch((v >> 10) & 31, 31), out[1] = ch((v >> 5) & 31, 31), out[2] = ch(v & 31, 31), out[3] = static_cast<float>((v >> 15) & 1); return;
    case Format::B4G4R4A4Unorm: out[0] = ch((v >> 8) & 15, 15), out[1] = ch((v >> 4) & 15, 15), out[2] = ch(v & 15, 15), out[3] = ch((v >> 12) & 15, 15); return;
    default: out[0] = ch((argb >> 16) & 0xFF, 255), out[1] = ch((argb >> 8) & 0xFF, 255), out[2] = ch(argb & 0xFF, 255), out[3] = ch(argb >> 24, 255); return;
  }
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
  // 0xAARRGGBB <-> storage. The packed 32-bit formats are kept as 0xAARRGGBB of what the format holds of the colour (quantize_packed32).
  uint32_t encode(uint32_t argb) const {
    if (is_packed32_format(format)) return quantize_packed32(format, argb);
    if (format != Format::R8G8B8A8Unorm && format != Format::R8G8B8A8Srgb) return argb;
    return (argb & 0xFF00FF00u) | ((argb & 0xFF) << 16) | ((argb >> 16) & 0xFF);
  }
  uint32_t decode(uint32_t stored) const { return is_packed32_format(format) ? stored : encode(stored); }  // the swap is its own inverse
};

}  // namespace graphics
