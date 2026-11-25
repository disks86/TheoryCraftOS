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

// Typed views of buffers (Direct3D's Buffer<> and RWBuffer<>, OpenGL's texture buffers and buffer images): a run of elements of one format in a buffer, read and written by shaders
// element by element. The formats are numbered as DXGI_FORMAT numbers them, so that the runtimes hand theirs down as they are; TexelFormat says which of them a view may have.
//
// An element travels as a Vec4, as the pixels of the wide formats do (WidePixels.hpp): floats for the float and normalised formats (what the format keeps of a stored value: clamped and
// rounded to its steps), the bits of the integers for the integer formats. A channel the format does not have reads as 0, the alpha as 1 (the integer 1 for an integer format).
// Conversions follow Direct3D's rules: unorm and snorm round to the nearest step (snorm's two lowest codes both read as -1), floats to half floats round to nearest even, to the
// 11- and 10-bit floats of R11G11B10 round to nearest and saturate to their largest finite value (negative values are 0, NaN stays NaN), and an integer stored into a narrower channel
// is clamped to the channel's range.
#pragma once
#include "Graphics/GraphicsTypes.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/ArrayView.hpp"

namespace graphics {

enum class TexelFormat : uint32_t {
  Undefined = 0,
  R32G32B32A32Float = 2, R32G32B32A32Uint = 3, R32G32B32A32Sint = 4,
  R32G32B32Float = 6, R32G32B32Uint = 7, R32G32B32Sint = 8,
  R16G16B16A16Float = 10, R16G16B16A16Unorm = 11, R16G16B16A16Uint = 12, R16G16B16A16Snorm = 13, R16G16B16A16Sint = 14,
  R32G32Float = 16, R32G32Uint = 17, R32G32Sint = 18,
  R10G10B10A2Unorm = 24, R10G10B10A2Uint = 25, R11G11B10Float = 26,
  R8G8B8A8Unorm = 28, R8G8B8A8Uint = 30, R8G8B8A8Snorm = 31, R8G8B8A8Sint = 32,
  R16G16Float = 34, R16G16Unorm = 35, R16G16Uint = 36, R16G16Snorm = 37, R16G16Sint = 38,
  R32Float = 41, R32Uint = 42, R32Sint = 43,
  R8G8Unorm = 49, R8G8Uint = 50, R8G8Snorm = 51, R8G8Sint = 52,
  R16Float = 54, R16Unorm = 56, R16Uint = 57, R16Snorm = 58, R16Sint = 59,
  R8Unorm = 61, R8Uint = 62, R8Snorm = 63, R8Sint = 64, A8Unorm = 65,
  B5G6R5Unorm = 85, B5G5R5A1Unorm = 86, B8G8R8A8Unorm = 87, B8G8R8X8Unorm = 88, B4G4R4A4Unorm = 115,
};

// What the channels of a format are.
enum class TexelKind : uint8_t { Invalid, Float, Unorm, Snorm, Uint, Sint, Packed };

struct TexelLayout {
  TexelKind kind = TexelKind::Invalid;
  uint8_t channels = 0;   // channels in memory order (R, G, B, A; A8: one, alpha)
  uint8_t bits = 0;       // bits of each channel (Packed: the bytes of an element are worked out per format)
  uint8_t bytes = 0;      // bytes of an element
};

constexpr TexelLayout texel_layout(TexelFormat f) {
  switch (f) {
    case TexelFormat::R32G32B32A32Float: return {TexelKind::Float, 4, 32, 16};
    case TexelFormat::R32G32B32A32Uint: return {TexelKind::Uint, 4, 32, 16};
    case TexelFormat::R32G32B32A32Sint: return {TexelKind::Sint, 4, 32, 16};
    case TexelFormat::R32G32B32Float: return {TexelKind::Float, 3, 32, 12};
    case TexelFormat::R32G32B32Uint: return {TexelKind::Uint, 3, 32, 12};
    case TexelFormat::R32G32B32Sint: return {TexelKind::Sint, 3, 32, 12};
    case TexelFormat::R16G16B16A16Float: return {TexelKind::Float, 4, 16, 8};
    case TexelFormat::R16G16B16A16Unorm: return {TexelKind::Unorm, 4, 16, 8};
    case TexelFormat::R16G16B16A16Uint: return {TexelKind::Uint, 4, 16, 8};
    case TexelFormat::R16G16B16A16Snorm: return {TexelKind::Snorm, 4, 16, 8};
    case TexelFormat::R16G16B16A16Sint: return {TexelKind::Sint, 4, 16, 8};
    case TexelFormat::R32G32Float: return {TexelKind::Float, 2, 32, 8};
    case TexelFormat::R32G32Uint: return {TexelKind::Uint, 2, 32, 8};
    case TexelFormat::R32G32Sint: return {TexelKind::Sint, 2, 32, 8};
    case TexelFormat::R10G10B10A2Unorm: case TexelFormat::R10G10B10A2Uint: case TexelFormat::R11G11B10Float: return {TexelKind::Packed, 4, 0, 4};
    case TexelFormat::R8G8B8A8Unorm: return {TexelKind::Unorm, 4, 8, 4};
    case TexelFormat::R8G8B8A8Uint: return {TexelKind::Uint, 4, 8, 4};
    case TexelFormat::R8G8B8A8Snorm: return {TexelKind::Snorm, 4, 8, 4};
    case TexelFormat::R8G8B8A8Sint: return {TexelKind::Sint, 4, 8, 4};
    case TexelFormat::R16G16Float: return {TexelKind::Float, 2, 16, 4};
    case TexelFormat::R16G16Unorm: return {TexelKind::Unorm, 2, 16, 4};
    case TexelFormat::R16G16Uint: return {TexelKind::Uint, 2, 16, 4};
    case TexelFormat::R16G16Snorm: return {TexelKind::Snorm, 2, 16, 4};
    case TexelFormat::R16G16Sint: return {TexelKind::Sint, 2, 16, 4};
    case TexelFormat::R32Float: return {TexelKind::Float, 1, 32, 4};
    case TexelFormat::R32Uint: return {TexelKind::Uint, 1, 32, 4};
    case TexelFormat::R32Sint: return {TexelKind::Sint, 1, 32, 4};
    case TexelFormat::R8G8Unorm: return {TexelKind::Unorm, 2, 8, 2};
    case TexelFormat::R8G8Uint: return {TexelKind::Uint, 2, 8, 2};
    case TexelFormat::R8G8Snorm: return {TexelKind::Snorm, 2, 8, 2};
    case TexelFormat::R8G8Sint: return {TexelKind::Sint, 2, 8, 2};
    case TexelFormat::R16Float: return {TexelKind::Float, 1, 16, 2};
    case TexelFormat::R16Unorm: return {TexelKind::Unorm, 1, 16, 2};
    case TexelFormat::R16Uint: return {TexelKind::Uint, 1, 16, 2};
    case TexelFormat::R16Snorm: return {TexelKind::Snorm, 1, 16, 2};
    case TexelFormat::R16Sint: return {TexelKind::Sint, 1, 16, 2};
    case TexelFormat::R8Unorm: case TexelFormat::A8Unorm: return {TexelKind::Unorm, 1, 8, 1};
    case TexelFormat::R8Uint: return {TexelKind::Uint, 1, 8, 1};
    case TexelFormat::R8Snorm: return {TexelKind::Snorm, 1, 8, 1};
    case TexelFormat::R8Sint: return {TexelKind::Sint, 1, 8, 1};
    case TexelFormat::B5G6R5Unorm: case TexelFormat::B5G5R5A1Unorm: case TexelFormat::B4G4R4A4Unorm: return {TexelKind::Packed, 4, 0, 2};
    case TexelFormat::B8G8R8A8Unorm: case TexelFormat::B8G8R8X8Unorm: return {TexelKind::Packed, 4, 0, 4};
    default: return {};
  }
}
constexpr bool texel_format_valid(TexelFormat f) { return texel_layout(f).kind != TexelKind::Invalid; }
constexpr uint32_t texel_bytes(TexelFormat f) { return texel_layout(f).bytes; }
constexpr bool texel_is_integer(TexelFormat f) {
  const TexelKind k = texel_layout(f).kind;
  return k == TexelKind::Uint || k == TexelKind::Sint || f == TexelFormat::R10G10B10A2Uint;
}
// The formats an unordered access view of a buffer (RWBuffer<>, a storage image of a buffer) may have: Direct3D's typed UAV formats - not the three-channel 32-bit ones, the B8G8R8 and
// 16-bit packed ones, or A8.
constexpr bool texel_format_storable(TexelFormat f) {
  return texel_format_valid(f) && texel_layout(f).channels != 3 && f != TexelFormat::A8Unorm && f != TexelFormat::B5G6R5Unorm && f != TexelFormat::B5G5R5A1Unorm &&
         f != TexelFormat::B4G4R4A4Unorm && f != TexelFormat::B8G8R8A8Unorm && f != TexelFormat::B8G8R8X8Unorm;
}
// Atomic operations on elements take the 32-bit integer formats.
constexpr bool texel_format_atomic(TexelFormat f) { return f == TexelFormat::R32Uint || f == TexelFormat::R32Sint; }

namespace texel_detail {
inline float bits_float(uint32_t v) { return __builtin_bit_cast(float, v); }
inline uint32_t float_bits(float v) { return __builtin_bit_cast(uint32_t, v); }
// (round to nearest, ties away from zero: Direct3D's float-to-normalised conversion, within its tolerance)
inline uint32_t to_unorm(float v, uint32_t max) { return !(v > 0.0f) ? 0 : v >= 1.0f ? max : static_cast<uint32_t>(v * static_cast<float>(max) + 0.5f); }
inline float from_unorm(uint32_t v, uint32_t max) { return static_cast<float>(v) / static_cast<float>(max); }
inline int32_t to_snorm(float v, int32_t max) {
  if (!(v > -1.0f)) return v == v ? -max : 0;
  if (v >= 1.0f) return max;
  return static_cast<int32_t>(v * static_cast<float>(max) + (v >= 0.0f ? 0.5f : -0.5f));
}
inline float from_snorm(int32_t v, int32_t max) {
  const float f = static_cast<float>(v) / static_cast<float>(max);
  return f < -1.0f ? -1.0f : f;
}
inline uint32_t clamp_uint(uint32_t v, uint32_t max) { return v > max ? max : v; }
inline int32_t clamp_sint(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int32_t sign_extend(uint32_t v, uint32_t bits) { return bits >= 32 ? static_cast<int32_t>(v) : static_cast<int32_t>(v << (32 - bits)) >> (32 - bits); }

// The small floats of R11G11B10_FLOAT: no sign, a 5-bit exponent (bias 15) and 6 or 5 fraction bits.
inline float small_float_to_float(uint32_t v, uint32_t mantissa_bits) {
  const uint32_t exponent = (v >> mantissa_bits) & 0x1F, mantissa = v & ((1u << mantissa_bits) - 1);
  if (exponent == 0x1F) return mantissa ? __builtin_nanf("") : __builtin_inff();
  if (exponent == 0) return static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits) * (1.0f / 16384.0f);
  return bits_float(((exponent - 15 + 127) << 23) | (mantissa << (23 - mantissa_bits)));
}
inline uint32_t float_to_small_float(float f, uint32_t mantissa_bits) {
  const uint32_t max_finite = (0x1Eu << mantissa_bits) | ((1u << mantissa_bits) - 1);
  if (f != f) return (0x1Fu << mantissa_bits) | 1;   // NaN
  if (!(f > 0.0f)) return 0;                          // negative values and zero
  const uint32_t b = float_bits(f);
  if (b >= 0x7F800000u) return max_finite;            // (infinity saturates, as the finite values above the range do)
  const int32_t exponent = static_cast<int32_t>((b >> 23) & 0xFF) - 127 + 15;
  const uint32_t shift = 23 - mantissa_bits;
  if (exponent >= 0x1F) return max_finite;
  if (exponent <= 0) {
    // (a denormal of the small format: the fraction with the implicit one, shifted down, rounded to nearest even)
    const uint32_t full = (b & 0x7FFFFF) | 0x800000;
    const uint32_t down = shift + static_cast<uint32_t>(1 - exponent);
    if (down >= 32) return 0;
    uint32_t m = full >> down;
    const uint32_t rest = full & ((1u << down) - 1), half = 1u << (down - 1);
    if (rest > half || (rest == half && (m & 1))) ++m;
    return m;
  }
  uint32_t v = (static_cast<uint32_t>(exponent) << mantissa_bits) | ((b & 0x7FFFFF) >> shift);
  const uint32_t rest = b & ((1u << shift) - 1), half = 1u << (shift - 1);
  if (rest > half || (rest == half && (v & 1))) ++v;
  return v > max_finite ? max_finite : v;
}
}  // namespace texel_detail

// One element from memory (little endian). `in` has texel_bytes(f) bytes.
inline Vec4 decode_texel(TexelFormat f, ConstByteView in) {
  using namespace texel_detail;
  const TexelLayout l = texel_layout(f);
  const bool integer = texel_is_integer(f);
  float c[4] = {0, 0, 0, integer ? bits_float(1) : 1.0f};
  if (l.kind == TexelKind::Invalid || in.size() < l.bytes) return {c[0], c[1], c[2], c[3]};
  ByteReader r(in.first(l.bytes));
  if (l.kind == TexelKind::Packed) {
    const uint32_t w = l.bytes == 2 ? r.le16() : r.le32();
    switch (f) {
      case TexelFormat::R10G10B10A2Unorm:
        c[0] = from_unorm(w & 0x3FF, 1023), c[1] = from_unorm((w >> 10) & 0x3FF, 1023), c[2] = from_unorm((w >> 20) & 0x3FF, 1023), c[3] = from_unorm(w >> 30, 3);
        break;
      case TexelFormat::R10G10B10A2Uint:
        c[0] = bits_float(w & 0x3FF), c[1] = bits_float((w >> 10) & 0x3FF), c[2] = bits_float((w >> 20) & 0x3FF), c[3] = bits_float(w >> 30);
        break;
      case TexelFormat::R11G11B10Float:
        c[0] = small_float_to_float(w & 0x7FF, 6), c[1] = small_float_to_float((w >> 11) & 0x7FF, 6), c[2] = small_float_to_float(w >> 22, 5);
        break;
      case TexelFormat::B5G6R5Unorm: c[2] = from_unorm(w & 0x1F, 31), c[1] = from_unorm((w >> 5) & 0x3F, 63), c[0] = from_unorm((w >> 11) & 0x1F, 31); break;
      case TexelFormat::B5G5R5A1Unorm:
        c[2] = from_unorm(w & 0x1F, 31), c[1] = from_unorm((w >> 5) & 0x1F, 31), c[0] = from_unorm((w >> 10) & 0x1F, 31), c[3] = static_cast<float>(w >> 15);
        break;
      case TexelFormat::B4G4R4A4Unorm:
        c[2] = from_unorm(w & 0xF, 15), c[1] = from_unorm((w >> 4) & 0xF, 15), c[0] = from_unorm((w >> 8) & 0xF, 15), c[3] = from_unorm(w >> 12, 15);
        break;
      case TexelFormat::B8G8R8A8Unorm: case TexelFormat::B8G8R8X8Unorm:
        c[2] = from_unorm(w & 0xFF, 255), c[1] = from_unorm((w >> 8) & 0xFF, 255), c[0] = from_unorm((w >> 16) & 0xFF, 255);
        c[3] = f == TexelFormat::B8G8R8X8Unorm ? 1.0f : from_unorm(w >> 24, 255);
        break;
      default: break;
    }
    return {c[0], c[1], c[2], c[3]};
  }
  for (uint32_t k = 0; k < l.channels; ++k) {
    const uint32_t v = l.bits == 32 ? r.le32() : l.bits == 16 ? r.le16() : r.u8();
    const uint32_t max = l.bits == 32 ? 0xFFFFFFFFu : (1u << l.bits) - 1;
    const int32_t smax = static_cast<int32_t>(max >> 1);
    float out = 0;
    switch (l.kind) {
      case TexelKind::Float: out = l.bits == 32 ? bits_float(v) : half_to_float(static_cast<uint16_t>(v)); break;
      case TexelKind::Unorm: out = from_unorm(v, max); break;
      case TexelKind::Snorm: out = from_snorm(sign_extend(v, l.bits), smax); break;
      case TexelKind::Uint: out = bits_float(v); break;
      case TexelKind::Sint: out = bits_float(static_cast<uint32_t>(sign_extend(v, l.bits))); break;
      default: break;
    }
    c[f == TexelFormat::A8Unorm ? 3 : k] = out;
  }
  if (f == TexelFormat::A8Unorm) c[0] = 0;
  return {c[0], c[1], c[2], c[3]};
}

// One element into memory: `out` has texel_bytes(f) bytes.
inline void encode_texel(TexelFormat f, const Vec4& value, ByteView out) {
  using namespace texel_detail;
  const TexelLayout l = texel_layout(f);
  if (l.kind == TexelKind::Invalid || out.size() < l.bytes) return;
  const float c[4] = {value.x, value.y, value.z, value.w};
  ByteWriter w(out.first(l.bytes));
  if (l.kind == TexelKind::Packed) {
    uint32_t v = 0;
    switch (f) {
      case TexelFormat::R10G10B10A2Unorm: v = to_unorm(c[0], 1023) | to_unorm(c[1], 1023) << 10 | to_unorm(c[2], 1023) << 20 | to_unorm(c[3], 3) << 30; break;
      case TexelFormat::R10G10B10A2Uint:
        v = clamp_uint(float_bits(c[0]), 1023) | clamp_uint(float_bits(c[1]), 1023) << 10 | clamp_uint(float_bits(c[2]), 1023) << 20 | clamp_uint(float_bits(c[3]), 3) << 30;
        break;
      case TexelFormat::R11G11B10Float: v = float_to_small_float(c[0], 6) | float_to_small_float(c[1], 6) << 11 | float_to_small_float(c[2], 5) << 22; break;
      case TexelFormat::B5G6R5Unorm: v = to_unorm(c[2], 31) | to_unorm(c[1], 63) << 5 | to_unorm(c[0], 31) << 11; break;
      case TexelFormat::B5G5R5A1Unorm: v = to_unorm(c[2], 31) | to_unorm(c[1], 31) << 5 | to_unorm(c[0], 31) << 10 | to_unorm(c[3], 1) << 15; break;
      case TexelFormat::B4G4R4A4Unorm: v = to_unorm(c[2], 15) | to_unorm(c[1], 15) << 4 | to_unorm(c[0], 15) << 8 | to_unorm(c[3], 15) << 12; break;
      case TexelFormat::B8G8R8A8Unorm: case TexelFormat::B8G8R8X8Unorm:
        v = to_unorm(c[2], 255) | to_unorm(c[1], 255) << 8 | to_unorm(c[0], 255) << 16 | (f == TexelFormat::B8G8R8X8Unorm ? 0xFFu : to_unorm(c[3], 255)) << 24;
        break;
      default: break;
    }
    if (l.bytes == 2) w.le16(static_cast<uint16_t>(v));
    else w.le32(v);
    return;
  }
  for (uint32_t k = 0; k < l.channels; ++k) {
    const float x = c[f == TexelFormat::A8Unorm ? 3 : k];
    const uint32_t max = l.bits == 32 ? 0xFFFFFFFFu : (1u << l.bits) - 1;
    const int32_t smax = static_cast<int32_t>(max >> 1);
    uint32_t v = 0;
    switch (l.kind) {
      case TexelKind::Float: v = l.bits == 32 ? float_bits(x) : float_to_half(x); break;
      case TexelKind::Unorm: v = to_unorm(x, max); break;
      case TexelKind::Snorm: v = static_cast<uint32_t>(to_snorm(x, smax)) & max; break;
      case TexelKind::Uint: v = clamp_uint(float_bits(x), max); break;
      case TexelKind::Sint: v = static_cast<uint32_t>(clamp_sint(static_cast<int32_t>(float_bits(x)), -smax - 1, smax)) & max; break;
      default: break;
    }
    if (l.bits == 32) w.le32(v);
    else if (l.bits == 16) w.le16(static_cast<uint16_t>(v));
    else w.u8(static_cast<uint8_t>(v));
  }
}

}  // namespace graphics
