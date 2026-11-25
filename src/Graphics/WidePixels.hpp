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

// How the pixels of the wide formats (GraphicsTypes.hpp: is_wide_format) are laid out in memory, and what an image of one keeps of a colour. A pixel travels as a Vec4: floats for the float
// and normalised formats, the bits of the integers for the integer formats. One place, so the devices and the memory manager agree.
#pragma once
#include "Graphics/GraphicsTypes.hpp"
#include "Utility/ByteStream.hpp"

namespace graphics {

namespace wide_detail {
inline float bits_float(uint32_t v) { return __builtin_bit_cast(float, v); }
inline uint32_t float_bits(float v) { return __builtin_bit_cast(uint32_t, v); }
inline float unorm(uint32_t v, uint32_t max) { return static_cast<float>(v) * (1.0f / static_cast<float>(max)); }
inline uint32_t to_unorm(float v, uint32_t max) { return !(v > 0.0f) ? 0 : v >= 1.0f ? max : static_cast<uint32_t>(v * static_cast<float>(max) + 0.5f); }
inline float snorm16(int32_t v) {
  const float f = static_cast<float>(v) * (1.0f / 32767.0f);
  return f < -1.0f ? -1.0f : f;
}
inline int32_t to_snorm16(float v) {
  if (!(v > -1.0f)) return v == v ? -32767 : 0;
  if (v >= 1.0f) return 32767;
  return static_cast<int32_t>(v * 32767.0f + (v >= 0.0f ? 0.5f : -0.5f));
}
inline uint32_t clamp_unsigned(uint32_t bits, uint32_t max) { return bits > max ? max : bits; }
inline int32_t clamp_signed(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace wide_detail

namespace wide_detail {
// The formats numbered from 64 that are wide (is_extra_wide_format): `count` channels of `bits` each of one kind, or a packed layout of their own.
enum class Kind : uint8_t { Unorm, Snorm, Uint, Sint, Float, R11G11B10, R9G9B9E5, R10G10B10A2Uint };
struct Layout {
  uint8_t count, bits;
  Kind kind;
};
constexpr Layout layout_of(Format f) {
  switch (f) {
    case Format::R8Snorm: return {1, 8, Kind::Snorm};
    case Format::R8Uint: return {1, 8, Kind::Uint};
    case Format::R8Sint: return {1, 8, Kind::Sint};
    case Format::R8G8Snorm: return {2, 8, Kind::Snorm};
    case Format::R8G8Uint: return {2, 8, Kind::Uint};
    case Format::R8G8Sint: return {2, 8, Kind::Sint};
    case Format::R8G8B8A8Snorm: return {4, 8, Kind::Snorm};
    case Format::R8G8B8A8Sint: return {4, 8, Kind::Sint};
    case Format::R16Unorm: return {1, 16, Kind::Unorm};
    case Format::R16Snorm: return {1, 16, Kind::Snorm};
    case Format::R16Uint: return {1, 16, Kind::Uint};
    case Format::R16Sint: return {1, 16, Kind::Sint};
    case Format::R16Float: return {1, 16, Kind::Float};
    case Format::R16G16Unorm: return {2, 16, Kind::Unorm};
    case Format::R16G16Snorm: return {2, 16, Kind::Snorm};
    case Format::R16G16Uint: return {2, 16, Kind::Uint};
    case Format::R16G16Sint: return {2, 16, Kind::Sint};
    case Format::R16G16Float: return {2, 16, Kind::Float};
    case Format::R32G32Uint: return {2, 32, Kind::Uint};
    case Format::R32G32Sint: return {2, 32, Kind::Sint};
    case Format::R32G32Float: return {2, 32, Kind::Float};
    case Format::R32G32B32Uint: return {3, 32, Kind::Uint};
    case Format::R32G32B32Sint: return {3, 32, Kind::Sint};
    case Format::R32G32B32Float: return {3, 32, Kind::Float};
    case Format::R11G11B10Float: return {3, 0, Kind::R11G11B10};
    case Format::R9G9B9E5Float: return {3, 0, Kind::R9G9B9E5};
    case Format::R10G10B10A2Uint: return {4, 0, Kind::R10G10B10A2Uint};
    default: return {0, 0, Kind::Unorm};
  }
}
// A signed normalised channel of `bits` (Direct3D: v / (2^(bits-1) - 1), the most negative code is -1 as well), and back (round to nearest, clamped; NaN is 0).
inline float snorm(int32_t v, uint32_t bits) {
  const float f = static_cast<float>(v) / static_cast<float>((1u << (bits - 1)) - 1);
  return f < -1.0f ? -1.0f : f;
}
inline int32_t to_snorm(float v, uint32_t bits) {
  const int32_t max = static_cast<int32_t>((1u << (bits - 1)) - 1);
  if (!(v > -1.0f)) return v == v ? -max : 0;
  if (v >= 1.0f) return max;
  return static_cast<int32_t>(v * static_cast<float>(max) + (v >= 0.0f ? 0.5f : -0.5f));
}
// An unsigned float of 5 exponent bits (bias 15) and `mantissa_bits` (R11G11B10's 6 and 5): to float, and from it rounded to nearest even (negative numbers and -0 become 0, NaN stays a
// NaN, what is too large becomes infinity, as the Direct3D conversion rules have it).
inline float small_float(uint32_t v, uint32_t mantissa_bits) {
  const uint32_t exponent = (v >> mantissa_bits) & 31, mantissa = v & ((1u << mantissa_bits) - 1);
  if (exponent == 31) return bits_float(mantissa ? 0x7FC00000u : 0x7F800000u);
  if (exponent == 0) return static_cast<float>(mantissa) * bits_float((127u - 14u - mantissa_bits) << 23);   // (subnormal: mantissa * 2^(-14 - mantissa_bits))
  return bits_float(((exponent + 127u - 15u) << 23) | (mantissa << (23 - mantissa_bits)));
}
inline uint32_t to_small_float(float f, uint32_t mantissa_bits) {
  const uint32_t b = float_bits(f), max_code = (31u << mantissa_bits);
  if ((b & 0x7F800000u) == 0x7F800000u) return (b & 0x7FFFFFu) ? max_code | 1u : (b >> 31) ? 0 : max_code;   // (NaN; +infinity; -infinity is 0)
  if (b >> 31) return 0;
  const int32_t e = static_cast<int32_t>((b >> 23) & 0xFF) - 127 + 15;
  const uint32_t significand = (b & 0x7FFFFFu) | 0x800000u;   // (24 bits)
  uint32_t shift = 23 - mantissa_bits;   // (bits dropped from the significand of a normal result)
  uint32_t base = 0;
  if (e <= 0) {
    if (((b >> 23) & 0xFF) == 0 || e < -static_cast<int32_t>(mantissa_bits)) return 0;   // (zero, float subnormals and what rounds below half the smallest step)
    shift += static_cast<uint32_t>(1 - e);   // (a subnormal result)
  } else {
    base = static_cast<uint32_t>(e) << mantissa_bits;
  }
  const uint32_t kept = significand >> shift, rest = significand & ((1u << shift) - 1), half = 1u << (shift - 1);
  uint32_t out = e <= 0 ? kept : base | (kept & ((1u << mantissa_bits) - 1));
  if (rest > half || (rest == half && (out & 1))) ++out;   // (a carry runs into the exponent: right, up to infinity)
  return out > max_code ? max_code : out;
}
// RGB9E5: three 9-bit mantissas and a shared exponent (bias 15, the mantissas have no leading one).
inline Vec4 decode_rgb9e5(uint32_t v) {
  const float scale = bits_float(((v >> 27) + 127u - 15u - 9u) << 23);   // (2^(e - 24); never below a normal float)
  return {static_cast<float>(v & 511) * scale, static_cast<float>((v >> 9) & 511) * scale, static_cast<float>((v >> 18) & 511) * scale, 1.0f};
}
inline uint32_t encode_rgb9e5(Vec4 c) {
  // (the EXT_texture_shared_exponent / Direct3D conversion: clamp to 0 .. 65408, the largest channel picks the exponent, the mantissas rounded to nearest)
  constexpr float kMax = 65408.0f;   // (511 / 512 * 2^16)
  auto clamp = [](float x) { return !(x > 0.0f) ? 0.0f : x > kMax ? kMax : x; };
  const float r = clamp(c.x), g = clamp(c.y), b = clamp(c.z);
  const float largest = r > g ? (r > b ? r : b) : (g > b ? g : b);
  // floor(log2(largest)), at least -16: the exponent field of the float (a subnormal or zero float is below 2^-16 anyway)
  int32_t log2 = static_cast<int32_t>((float_bits(largest) >> 23) & 0xFF) - 127;
  if (log2 < -16) log2 = -16;
  int32_t exponent = log2 + 1 + 15;   // (the shared exponent, 0 .. 31)
  auto scale_of = [](int32_t e) { return bits_float(static_cast<uint32_t>(127 - (e - 15 - 9)) << 23); };   // (1 / 2^(e - 24))
  if (static_cast<uint32_t>(largest * scale_of(exponent) + 0.5f) == 512) ++exponent;
  const float scale = scale_of(exponent);
  auto mantissa = [&](float x) { const uint32_t m = static_cast<uint32_t>(x * scale + 0.5f); return m > 511 ? 511u : m; };
  return mantissa(r) | mantissa(g) << 9 | mantissa(b) << 18 | static_cast<uint32_t>(exponent) << 27;
}
inline uint32_t read_bits(ByteReader& in, uint32_t bits) { return bits == 8 ? in.u8() : bits == 16 ? in.le16() : in.le32(); }
inline void write_bits(ByteWriter& out, uint32_t bits, uint32_t v) {
  if (bits == 8) out.u8(static_cast<uint8_t>(v));
  else if (bits == 16) out.le16(static_cast<uint16_t>(v));
  else out.le32(v);
}
inline Vec4 decode_extra(Format f, ByteReader& in) {
  const Layout l = layout_of(f);
  if (l.kind == Kind::R11G11B10) {
    const uint32_t v = in.le32();
    return {small_float(v & 0x7FF, 6), small_float((v >> 11) & 0x7FF, 6), small_float(v >> 22, 5), 1.0f};
  }
  if (l.kind == Kind::R9G9B9E5) return decode_rgb9e5(in.le32());
  if (l.kind == Kind::R10G10B10A2Uint) {
    const uint32_t v = in.le32();
    return {bits_float(v & 0x3FF), bits_float((v >> 10) & 0x3FF), bits_float((v >> 20) & 0x3FF), bits_float(v >> 30)};
  }
  const bool integer = l.kind == Kind::Uint || l.kind == Kind::Sint;
  float c[4] = {0.0f, 0.0f, 0.0f, integer ? bits_float(1) : 1.0f};
  for (uint32_t i = 0; i < l.count; ++i) {
    const uint32_t v = read_bits(in, l.bits);
    const int32_t s = l.bits == 8 ? static_cast<int8_t>(v) : l.bits == 16 ? static_cast<int16_t>(v) : static_cast<int32_t>(v);
    switch (l.kind) {
      case Kind::Unorm: c[i] = unorm(v, l.bits == 8 ? 255u : 65535u); break;
      case Kind::Snorm: c[i] = snorm(s, l.bits); break;
      case Kind::Uint: c[i] = bits_float(v); break;
      case Kind::Sint: c[i] = bits_float(static_cast<uint32_t>(s)); break;
      default: c[i] = l.bits == 16 ? half_to_float(static_cast<uint16_t>(v)) : bits_float(v); break;
    }
  }
  return {c[0], c[1], c[2], c[3]};
}
inline void encode_extra(Format f, const Vec4& colour, ByteWriter& out) {
  const Layout l = layout_of(f);
  if (l.kind == Kind::R11G11B10) return out.le32(to_small_float(colour.x, 6) | to_small_float(colour.y, 6) << 11 | to_small_float(colour.z, 5) << 22);
  if (l.kind == Kind::R9G9B9E5) return out.le32(encode_rgb9e5(colour));
  if (l.kind == Kind::R10G10B10A2Uint)
    return out.le32(clamp_unsigned(float_bits(colour.x), 1023) | clamp_unsigned(float_bits(colour.y), 1023) << 10 | clamp_unsigned(float_bits(colour.z), 1023) << 20 | clamp_unsigned(float_bits(colour.w), 3) << 30);
  const float c[4] = {colour.x, colour.y, colour.z, colour.w};
  const uint32_t umax = l.bits == 32 ? 0xFFFFFFFFu : (1u << l.bits) - 1;
  const int32_t smax = l.bits == 32 ? 0x7FFFFFFF : static_cast<int32_t>((1u << (l.bits - 1)) - 1), smin = -smax - 1;
  for (uint32_t i = 0; i < l.count; ++i) {
    uint32_t v = 0;
    switch (l.kind) {
      case Kind::Unorm: v = to_unorm(c[i], umax); break;
      case Kind::Snorm: v = static_cast<uint32_t>(to_snorm(c[i], l.bits)); break;
      case Kind::Uint: v = clamp_unsigned(float_bits(c[i]), umax); break;
      case Kind::Sint: v = static_cast<uint32_t>(clamp_signed(static_cast<int32_t>(float_bits(c[i])), smin, smax)); break;
      default: v = l.bits == 16 ? float_to_half(c[i]) : float_bits(c[i]); break;
    }
    write_bits(out, l.bits, l.bits == 32 ? v : v & umax);
  }
}
inline Vec4 quantize_extra(Format f, Vec4 c) {
  uint8_t bytes[16] = {};
  ByteWriter out{ByteView(bytes, sizeof bytes)};
  encode_extra(f, c, out);
  ByteReader in{ConstByteView(bytes, sizeof bytes)};
  return decode_extra(f, in);
}
}  // namespace wide_detail

// One pixel from memory (little endian).
inline Vec4 decode_wide_pixel(Format f, ByteReader& in) {
  using namespace wide_detail;
  if (is_extra_wide_format(f)) return decode_extra(f, in);
  switch (f) {
    case Format::R16G16B16A16Float: {
      const float r = half_to_float(in.le16()), g = half_to_float(in.le16()), b = half_to_float(in.le16()), a = half_to_float(in.le16());
      return {r, g, b, a};
    }
    case Format::R16G16B16A16Unorm: {
      const float r = unorm(in.le16(), 65535), g = unorm(in.le16(), 65535), b = unorm(in.le16(), 65535), a = unorm(in.le16(), 65535);
      return {r, g, b, a};
    }
    case Format::R16G16B16A16Snorm: {
      const float r = snorm16(static_cast<int16_t>(in.le16())), g = snorm16(static_cast<int16_t>(in.le16())), b = snorm16(static_cast<int16_t>(in.le16())), a = snorm16(static_cast<int16_t>(in.le16()));
      return {r, g, b, a};
    }
    case Format::R16G16B16A16Uint: {
      const float r = bits_float(in.le16()), g = bits_float(in.le16()), b = bits_float(in.le16()), a = bits_float(in.le16());
      return {r, g, b, a};
    }
    case Format::R16G16B16A16Sint: {
      const float r = bits_float(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(in.le16())))), g = bits_float(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(in.le16())))),
                  b = bits_float(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(in.le16())))), a = bits_float(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(in.le16()))));
      return {r, g, b, a};
    }
    case Format::R8G8B8A8Uint: {
      const uint32_t word = in.le32();
      return {bits_float(word & 0xFF), bits_float((word >> 8) & 0xFF), bits_float((word >> 16) & 0xFF), bits_float(word >> 24)};
    }
    case Format::R10G10B10A2Unorm: {
      const uint32_t word = in.le32();
      return {unorm(word & 0x3FF, 1023), unorm((word >> 10) & 0x3FF, 1023), unorm((word >> 20) & 0x3FF, 1023), unorm(word >> 30, 3)};
    }
    case Format::R32Uint: case Format::R32Sint: return {bits_float(in.le32()), 0.0f, 0.0f, bits_float(1)};
    case Format::R32Float: return {bits_float(in.le32()), 0.0f, 0.0f, 1.0f};
    default: {   // (single floats, or 32-bit integers as their bits)
      const float r = bits_float(in.le32()), g = bits_float(in.le32()), b = bits_float(in.le32()), a = bits_float(in.le32());
      return {r, g, b, a};
    }
  }
}

// One pixel to memory.
inline void encode_wide_pixel(Format f, const Vec4& c, ByteWriter& out) {
  using namespace wide_detail;
  if (is_extra_wide_format(f)) return encode_extra(f, c, out);
  switch (f) {
    case Format::R16G16B16A16Float: out.le16(float_to_half(c.x)), out.le16(float_to_half(c.y)), out.le16(float_to_half(c.z)), out.le16(float_to_half(c.w)); return;
    case Format::R16G16B16A16Unorm: out.le16(static_cast<uint16_t>(to_unorm(c.x, 65535))), out.le16(static_cast<uint16_t>(to_unorm(c.y, 65535))), out.le16(static_cast<uint16_t>(to_unorm(c.z, 65535))), out.le16(static_cast<uint16_t>(to_unorm(c.w, 65535))); return;
    case Format::R16G16B16A16Snorm:
      out.le16(static_cast<uint16_t>(to_snorm16(c.x))), out.le16(static_cast<uint16_t>(to_snorm16(c.y))), out.le16(static_cast<uint16_t>(to_snorm16(c.z))), out.le16(static_cast<uint16_t>(to_snorm16(c.w)));
      return;
    case Format::R16G16B16A16Uint:
      out.le16(static_cast<uint16_t>(clamp_unsigned(float_bits(c.x), 65535))), out.le16(static_cast<uint16_t>(clamp_unsigned(float_bits(c.y), 65535))), out.le16(static_cast<uint16_t>(clamp_unsigned(float_bits(c.z), 65535))),
          out.le16(static_cast<uint16_t>(clamp_unsigned(float_bits(c.w), 65535)));
      return;
    case Format::R16G16B16A16Sint:
      out.le16(static_cast<uint16_t>(clamp_signed(static_cast<int32_t>(float_bits(c.x)), -32768, 32767))), out.le16(static_cast<uint16_t>(clamp_signed(static_cast<int32_t>(float_bits(c.y)), -32768, 32767))),
          out.le16(static_cast<uint16_t>(clamp_signed(static_cast<int32_t>(float_bits(c.z)), -32768, 32767))), out.le16(static_cast<uint16_t>(clamp_signed(static_cast<int32_t>(float_bits(c.w)), -32768, 32767)));
      return;
    case Format::R8G8B8A8Uint: out.le32(clamp_unsigned(float_bits(c.x), 255) | clamp_unsigned(float_bits(c.y), 255) << 8 | clamp_unsigned(float_bits(c.z), 255) << 16 | clamp_unsigned(float_bits(c.w), 255) << 24); return;
    case Format::R10G10B10A2Unorm: out.le32(to_unorm(c.x, 1023) | to_unorm(c.y, 1023) << 10 | to_unorm(c.z, 1023) << 20 | to_unorm(c.w, 3) << 30); return;
    case Format::R32Uint: case Format::R32Sint: case Format::R32Float: out.le32(float_bits(c.x)); return;
    default: out.le32(float_bits(c.x)), out.le32(float_bits(c.y)), out.le32(float_bits(c.z)), out.le32(float_bits(c.w)); return;
  }
}

// What an image of the format keeps of a colour (half floats round to half precision, the normalised formats to their steps, narrow integers saturate; single floats and 32-bit integers are
// kept as they are).
inline Vec4 quantize_wide_pixel(Format f, Vec4 c) {
  using namespace wide_detail;
  if (is_extra_wide_format(f)) return quantize_extra(f, c);
  switch (f) {
    case Format::R16G16B16A16Float: return {half_to_float(float_to_half(c.x)), half_to_float(float_to_half(c.y)), half_to_float(float_to_half(c.z)), half_to_float(float_to_half(c.w))};
    case Format::R16G16B16A16Unorm: return {unorm(to_unorm(c.x, 65535), 65535), unorm(to_unorm(c.y, 65535), 65535), unorm(to_unorm(c.z, 65535), 65535), unorm(to_unorm(c.w, 65535), 65535)};
    case Format::R16G16B16A16Snorm: return {snorm16(to_snorm16(c.x)), snorm16(to_snorm16(c.y)), snorm16(to_snorm16(c.z)), snorm16(to_snorm16(c.w))};
    case Format::R10G10B10A2Unorm: return {unorm(to_unorm(c.x, 1023), 1023), unorm(to_unorm(c.y, 1023), 1023), unorm(to_unorm(c.z, 1023), 1023), unorm(to_unorm(c.w, 3), 3)};
    case Format::R16G16B16A16Uint: return {bits_float(clamp_unsigned(float_bits(c.x), 65535)), bits_float(clamp_unsigned(float_bits(c.y), 65535)), bits_float(clamp_unsigned(float_bits(c.z), 65535)), bits_float(clamp_unsigned(float_bits(c.w), 65535))};
    case Format::R8G8B8A8Uint: return {bits_float(clamp_unsigned(float_bits(c.x), 255)), bits_float(clamp_unsigned(float_bits(c.y), 255)), bits_float(clamp_unsigned(float_bits(c.z), 255)), bits_float(clamp_unsigned(float_bits(c.w), 255))};
    case Format::R16G16B16A16Sint: {
      auto q = [](float v) { return bits_float(static_cast<uint32_t>(clamp_signed(static_cast<int32_t>(float_bits(v)), -32768, 32767))); };
      return {q(c.x), q(c.y), q(c.z), q(c.w)};
    }
    case Format::R32Uint: case Format::R32Sint: return {c.x, 0.0f, 0.0f, bits_float(1)};
    case Format::R32Float: return {c.x, 0.0f, 0.0f, 1.0f};
    default: return c;
  }
}

}  // namespace graphics
