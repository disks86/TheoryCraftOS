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

// Reading a vertex attribute out of a vertex buffer as a vertex shader's input register, for the devices that fetch vertices on the CPU (the software rasterizer; the Vega
// device, which hands the card every attribute as four 32-bit values). VertexFormat (IGraphicsDevice.hpp) says how each kind arrives.
#pragma once

#include <stdint.h>
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Graphics/GraphicsTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace graphics {

// An unsigned little-endian number of `bytes` bytes (1, 2 or 4) at `offset`; 0 when it is not all inside the buffer.
inline uint32_t read_vertex_unsigned(ConstByteView bytes, uint64_t offset, uint32_t count) {
  ConstByteView field;
  if (!bytes.try_subview(offset, count, &field)) return 0;
  uint32_t v = 0;
  for (uint32_t i = 0; i < count; ++i) v |= uint32_t{field[i]} << (8 * i);
  return v;
}

// One vertex attribute as the shader's input register (VertexFormat says how each kind arrives).
inline Vec4 read_vertex_attribute(ConstByteView bytes, uint64_t offset, VertexFormat format) {
  if (format == VertexFormat::Bgra8Unorm) {
    const uint32_t v = read_vertex_unsigned(bytes, offset, 4);
    return {static_cast<float>((v >> 16) & 0xFF) / 255.0f, static_cast<float>((v >> 8) & 0xFF) / 255.0f, static_cast<float>(v & 0xFF) / 255.0f, static_cast<float>(v >> 24) / 255.0f};
  }
  if (format == VertexFormat::Rgb10A2Unorm) {
    const uint32_t v = read_vertex_unsigned(bytes, offset, 4);
    return {static_cast<float>(v & 0x3FF) / 1023.0f, static_cast<float>((v >> 10) & 0x3FF) / 1023.0f, static_cast<float>((v >> 20) & 0x3FF) / 1023.0f, static_cast<float>(v >> 30) / 3.0f};
  }
  const VertexType type = vertex_format_type(format);
  const uint32_t size = vertex_type_bytes(type), components = vertex_format_components(format);
  const bool integer = vertex_format_is_integer(format);
  float v[4] = {0, 0, 0, integer ? __builtin_bit_cast(float, uint32_t{1}) : 1.0f};
  for (uint32_t k = 0; k < components; ++k) {
    const uint32_t raw = read_vertex_unsigned(bytes, offset + uint64_t{size} * k, size);
    const int32_t wide = size == 1 ? static_cast<int8_t>(raw) : size == 2 ? static_cast<int16_t>(raw) : static_cast<int32_t>(raw);   // (the value sign extended)
    switch (type) {
      case VertexType::Float: v[k] = __builtin_bit_cast(float, raw); break;
      case VertexType::Half: v[k] = half_to_float(static_cast<uint16_t>(raw)); break;
      case VertexType::Unorm8: v[k] = static_cast<float>(raw) / 255.0f; break;
      case VertexType::Unorm16: v[k] = static_cast<float>(raw) / 65535.0f; break;
      case VertexType::Snorm8: v[k] = wide < -127 ? -1.0f : static_cast<float>(wide) / 127.0f; break;
      case VertexType::Snorm16: v[k] = wide < -32767 ? -1.0f : static_cast<float>(wide) / 32767.0f; break;
      case VertexType::Uint8: case VertexType::Uint16: case VertexType::Uint32: v[k] = __builtin_bit_cast(float, raw); break;
      case VertexType::Sint8: case VertexType::Sint16: case VertexType::Sint32: v[k] = __builtin_bit_cast(float, static_cast<uint32_t>(wide)); break;
    }
  }
  return {v[0], v[1], v[2], v[3]};
}

}  // namespace graphics
