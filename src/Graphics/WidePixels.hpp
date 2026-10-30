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

// One pixel from memory (little endian).
inline Vec4 decode_wide_pixel(Format f, ByteReader& in) {
  using namespace wide_detail;
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
    default: {   // (single floats, or 32-bit integers as their bits)
      const float r = bits_float(in.le32()), g = bits_float(in.le32()), b = bits_float(in.le32()), a = bits_float(in.le32());
      return {r, g, b, a};
    }
  }
}

// One pixel to memory.
inline void encode_wide_pixel(Format f, const Vec4& c, ByteWriter& out) {
  using namespace wide_detail;
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
    default: out.le32(float_bits(c.x)), out.le32(float_bits(c.y)), out.le32(float_bits(c.z)), out.le32(float_bits(c.w)); return;
  }
}

// What an image of the format keeps of a colour (half floats round to half precision, the normalised formats to their steps, narrow integers saturate; single floats and 32-bit integers are
// kept as they are).
inline Vec4 quantize_wide_pixel(Format f, Vec4 c) {
  using namespace wide_detail;
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
    default: return c;
  }
}

}  // namespace graphics
