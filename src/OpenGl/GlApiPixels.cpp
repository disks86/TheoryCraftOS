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

// Pixels between the program and the GL: every format and type of client pixels to and from four floats per pixel, pixel transfer, glDrawPixels, glBitmap, glCopyPixels, glReadPixels and
// the accumulation buffer.
#include "OpenGl/GlApi.hpp"
#include "OpenGl/GlSurface.hpp"
#include "OpenGl/generated/GlEnums.inc"
#include "WebGl/GlPixels.hpp"

namespace opengl {

namespace {

float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }

uint32_t components_of(uint32_t format) {
  switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: case GL_INTENSITY: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: case GL_COLOR_INDEX: case GL_RED_INTEGER: case GL_GREEN_INTEGER: case GL_BLUE_INTEGER: case GL_ALPHA_INTEGER: return 1;
    case GL_RG: case GL_LUMINANCE_ALPHA: case GL_RG_INTEGER: case GL_DEPTH_STENCIL: return 2;
    case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER: return 3;
    case GL_RGBA: case GL_BGRA: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER: case GL_ABGR_EXT: return 4;
    default: return 0;
  }
}

bool is_packed(uint32_t type) {
  switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_24_8: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: return true;
    default: return false;
  }
}

uint32_t type_bytes(uint32_t type) {
  switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: return 2;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    case GL_BITMAP: return 0;
    default: return 4;
  }
}

uint32_t pixel_bytes(uint32_t format, uint32_t type) {
  if (is_packed(type) || type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) return type_bytes(type);
  return type_bytes(type) * components_of(format);
}

bool valid_combination(uint32_t format, uint32_t type) {
  const uint32_t n = components_of(format);
  if (n == 0) return false;
  switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_UNSIGNED_INT: case GL_INT: case GL_HALF_FLOAT: case GL_FLOAT: return format != GL_DEPTH_STENCIL;
    case GL_BITMAP: return format == GL_COLOR_INDEX || format == GL_STENCIL_INDEX;
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: return n == 3;
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV: return n == 4;
    case GL_UNSIGNED_INT_24_8: case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return format == GL_DEPTH_STENCIL;
    default: return false;
  }
}

float clamp01(float v) { return !(v > 0) ? 0 : (v > 1 ? 1 : v); }

// The components of one packed pixel, in the order the type names them, scaled 0..1.
void packed_decode(uint32_t type, uint32_t raw, float c[4]) {
  const auto bits = [&](uint32_t shift, uint32_t n) { return static_cast<float>((raw >> shift) & ((1u << n) - 1)) / static_cast<float>((1u << n) - 1); };
  c[3] = 1;
  switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: c[0] = bits(5, 3); c[1] = bits(2, 3); c[2] = bits(0, 2); return;
    case GL_UNSIGNED_BYTE_2_3_3_REV: c[0] = bits(0, 3); c[1] = bits(3, 3); c[2] = bits(6, 2); return;
    case GL_UNSIGNED_SHORT_5_6_5: c[0] = bits(11, 5); c[1] = bits(5, 6); c[2] = bits(0, 5); return;
    case GL_UNSIGNED_SHORT_5_6_5_REV: c[0] = bits(0, 5); c[1] = bits(5, 6); c[2] = bits(11, 5); return;
    case GL_UNSIGNED_SHORT_4_4_4_4: c[0] = bits(12, 4); c[1] = bits(8, 4); c[2] = bits(4, 4); c[3] = bits(0, 4); return;
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: c[0] = bits(0, 4); c[1] = bits(4, 4); c[2] = bits(8, 4); c[3] = bits(12, 4); return;
    case GL_UNSIGNED_SHORT_5_5_5_1: c[0] = bits(11, 5); c[1] = bits(6, 5); c[2] = bits(1, 5); c[3] = bits(0, 1); return;
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: c[0] = bits(0, 5); c[1] = bits(5, 5); c[2] = bits(10, 5); c[3] = bits(15, 1); return;
    case GL_UNSIGNED_INT_8_8_8_8: c[0] = bits(24, 8); c[1] = bits(16, 8); c[2] = bits(8, 8); c[3] = bits(0, 8); return;
    case GL_UNSIGNED_INT_8_8_8_8_REV: c[0] = bits(0, 8); c[1] = bits(8, 8); c[2] = bits(16, 8); c[3] = bits(24, 8); return;
    case GL_UNSIGNED_INT_10_10_10_2: c[0] = bits(22, 10); c[1] = bits(12, 10); c[2] = bits(2, 10); c[3] = bits(0, 2); return;
    case GL_UNSIGNED_INT_2_10_10_10_REV: c[0] = bits(0, 10); c[1] = bits(10, 10); c[2] = bits(20, 10); c[3] = bits(30, 2); return;
    case GL_UNSIGNED_INT_10F_11F_11F_REV: {
      const auto f11 = [](uint32_t m) { const uint32_t e = (m >> 6) & 31, f = m & 63; return e == 0 ? (f / 64.0f) * 6.103515625e-05f : (1.0f + f / 64.0f) * shader::fpow(2.0f, static_cast<float>(e) - 15.0f); };
      const auto f10 = [](uint32_t m) { const uint32_t e = (m >> 5) & 31, f = m & 31; return e == 0 ? (f / 32.0f) * 6.103515625e-05f : (1.0f + f / 32.0f) * shader::fpow(2.0f, static_cast<float>(e) - 15.0f); };
      c[0] = f11(raw & 2047); c[1] = f11((raw >> 11) & 2047); c[2] = f10(raw >> 22);
      return;
    }
    case GL_UNSIGNED_INT_5_9_9_9_REV: {
      const float scale = shader::fpow(2.0f, static_cast<float>(static_cast<int32_t>(raw >> 27)) - 24.0f);
      c[0] = static_cast<float>(raw & 511) * scale; c[1] = static_cast<float>((raw >> 9) & 511) * scale; c[2] = static_cast<float>((raw >> 18) & 511) * scale;
      return;
    }
    default: c[0] = c[1] = c[2] = 0; return;
  }
}

uint32_t packed_encode(uint32_t type, const float c[4]) {
  const auto q = [](float v, uint32_t n) { return static_cast<uint32_t>(clamp01(v) * static_cast<float>((1u << n) - 1) + 0.5f); };
  switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: return q(c[0], 3) << 5 | q(c[1], 3) << 2 | q(c[2], 2);
    case GL_UNSIGNED_BYTE_2_3_3_REV: return q(c[0], 3) | q(c[1], 3) << 3 | q(c[2], 2) << 6;
    case GL_UNSIGNED_SHORT_5_6_5: return q(c[0], 5) << 11 | q(c[1], 6) << 5 | q(c[2], 5);
    case GL_UNSIGNED_SHORT_5_6_5_REV: return q(c[0], 5) | q(c[1], 6) << 5 | q(c[2], 5) << 11;
    case GL_UNSIGNED_SHORT_4_4_4_4: return q(c[0], 4) << 12 | q(c[1], 4) << 8 | q(c[2], 4) << 4 | q(c[3], 4);
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: return q(c[0], 4) | q(c[1], 4) << 4 | q(c[2], 4) << 8 | q(c[3], 4) << 12;
    case GL_UNSIGNED_SHORT_5_5_5_1: return q(c[0], 5) << 11 | q(c[1], 5) << 6 | q(c[2], 5) << 1 | q(c[3], 1);
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: return q(c[0], 5) | q(c[1], 5) << 5 | q(c[2], 5) << 10 | q(c[3], 1) << 15;
    case GL_UNSIGNED_INT_8_8_8_8: return q(c[0], 8) << 24 | q(c[1], 8) << 16 | q(c[2], 8) << 8 | q(c[3], 8);
    case GL_UNSIGNED_INT_8_8_8_8_REV: return q(c[0], 8) | q(c[1], 8) << 8 | q(c[2], 8) << 16 | q(c[3], 8) << 24;
    case GL_UNSIGNED_INT_10_10_10_2: return q(c[0], 10) << 22 | q(c[1], 10) << 12 | q(c[2], 10) << 2 | q(c[3], 2);
    case GL_UNSIGNED_INT_2_10_10_10_REV: return q(c[0], 10) | q(c[1], 10) << 10 | q(c[2], 10) << 20 | q(c[3], 2) << 30;
    default: return 0;
  }
}

// A component of a non-packed type, as stored bytes -> float (colour rules: normalised) or as integer.
float read_component(uint32_t type, const uint8_t* p, bool swap, bool integer) {
  uint8_t b[4] = {p[0], 0, 0, 0};
  const uint32_t n = type_bytes(type);
  for (uint32_t i = 0; i < n && i < 4; ++i) b[i] = swap ? p[n - 1 - i] : p[i];
  const uint32_t u = uint32_t{b[0]} | uint32_t{b[1]} << 8 | uint32_t{b[2]} << 16 | uint32_t{b[3]} << 24;
  switch (type) {
    case GL_UNSIGNED_BYTE: return integer ? static_cast<float>(u) : static_cast<float>(u) / 255.0f;
    case GL_BYTE: { const int8_t v = static_cast<int8_t>(u); return integer ? static_cast<float>(v) : (v <= -127 ? -1.0f : static_cast<float>(v) / 127.0f); }
    case GL_UNSIGNED_SHORT: return integer ? static_cast<float>(u) : static_cast<float>(u) / 65535.0f;
    case GL_SHORT: { const int16_t v = static_cast<int16_t>(u); return integer ? static_cast<float>(v) : (v <= -32767 ? -1.0f : static_cast<float>(v) / 32767.0f); }
    case GL_UNSIGNED_INT: return integer ? static_cast<float>(u) : static_cast<float>(u / 4294967295.0);
    case GL_INT: { const int32_t v = static_cast<int32_t>(u); return integer ? static_cast<float>(v) : static_cast<float>(v / 2147483647.0); }
    case GL_HALF_FLOAT: return shader::half_to_float(static_cast<uint16_t>(u));
    default: return u2f(u);
  }
}

void write_component(uint32_t type, float v, uint8_t* p, bool swap) {
  uint32_t u = 0;
  const auto clamp1 = [](float f) { return f < -1 ? -1.0f : (f > 1 ? 1.0f : f); };
  switch (type) {
    case GL_UNSIGNED_BYTE: u = static_cast<uint32_t>(clamp01(v) * 255.0f + 0.5f); break;
    case GL_BYTE: { const float s = clamp1(v) * 127.0f; u = static_cast<uint32_t>(static_cast<int32_t>(s < 0 ? s - 0.5f : s + 0.5f)); break; }
    case GL_UNSIGNED_SHORT: u = static_cast<uint32_t>(clamp01(v) * 65535.0f + 0.5f); break;
    case GL_SHORT: { const float s = clamp1(v) * 32767.0f; u = static_cast<uint32_t>(static_cast<int32_t>(s < 0 ? s - 0.5f : s + 0.5f)); break; }
    case GL_UNSIGNED_INT: u = static_cast<uint32_t>(static_cast<double>(clamp01(v)) * 4294967295.0 + 0.5); break;
    case GL_INT: { const double s = static_cast<double>(clamp1(v)) * 2147483647.0; u = static_cast<uint32_t>(static_cast<int32_t>(s < 0 ? s - 0.5 : s + 0.5)); break; }
    case GL_HALF_FLOAT: u = shader::float_to_half(v); break;
    default: u = f2u(v); break;
  }
  const uint32_t n = type_bytes(type);
  for (uint32_t i = 0; i < n; ++i) p[swap ? n - 1 - i : i] = static_cast<uint8_t>(u >> (8 * i));
}

}  // namespace

// Row layout by the pixel store state.
void GlApi::store_layout(const PixelStore& s, uint32_t w, uint32_t h, uint32_t bytes_per_pixel, uint64_t* row_bytes, uint64_t* slice_bytes, uint64_t* first_byte) {
  const uint64_t row_length = s.row_length > 0 ? static_cast<uint64_t>(s.row_length) : w;
  const uint64_t rows = s.image_height > 0 ? static_cast<uint64_t>(s.image_height) : h;
  const uint64_t align = s.alignment > 0 ? static_cast<uint64_t>(s.alignment) : 1;
  const uint64_t step = bytes_per_pixel > align ? bytes_per_pixel : align;
  (void)step;
  *row_bytes = (row_length * bytes_per_pixel + align - 1) / align * align;
  *slice_bytes = *row_bytes * rows;
  *first_byte = static_cast<uint64_t>(s.skip_images) * *slice_bytes + static_cast<uint64_t>(s.skip_rows) * *row_bytes + static_cast<uint64_t>(s.skip_pixels) * bytes_per_pixel;
}

// Client pixels (or the pixel unpack buffer) -> width * height * depth * 4 floats: R, G, B, A of a colour, or depth in R, stencil in G for depth and stencil formats.
bool GlApi::pixels_in(uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type, UserPtr at, ByteBuffer* out, const PixelStore& store) {
  if (!valid_combination(format, type)) { error(GL_INVALID_ENUM); return false; }
  const uint64_t count = uint64_t{w} * h * d;
  if (count > (1ull << 30)) { error(GL_OUT_OF_MEMORY); return false; }
  out->clear();
  if (!out->put_zeros(count * 16)) { error(GL_OUT_OF_MEMORY); return false; }
  const uint32_t bpp = pixel_bytes(format, type);
  uint64_t row_bytes, slice_bytes, first;
  store_layout(store, w, h, bpp, &row_bytes, &slice_bytes, &first);
  const uint64_t total = first + (d ? (uint64_t{d} - 1) * slice_bytes : 0) + (h ? (uint64_t{h} - 1) * row_bytes : 0) + uint64_t{w} * bpp;
  ByteBuffer raw(a_);
  ConstByteView bytes;
  const webgl::Buffer* pbo = gl_.buffer_at(gl_.pixel_unpack_);
  if (pbo) {
    if (at.address > pbo->data.size() || total > pbo->data.size() - at.address) { error(GL_INVALID_OPERATION); return false; }
    bytes = pbo->data.view().subview(at.address, total);
  } else {
    if (at.null()) return true;      // nothing: zeros
    if (!raw.put_zeros(total) || !read_client(at, raw.writable_view())) { error(GL_INVALID_OPERATION); return false; }
    bytes = raw.view();
  }
  const bool swap = store.swap_bytes != 0;
  const bool packed = is_packed(type);
  const uint32_t n = components_of(format), comp_bytes = type_bytes(type);
  const bool integer = format == GL_STENCIL_INDEX || format == GL_COLOR_INDEX || format == GL_RED_INTEGER || format == GL_GREEN_INTEGER || format == GL_BLUE_INTEGER || format == GL_ALPHA_INTEGER || format == GL_RG_INTEGER || format == GL_RGB_INTEGER || format == GL_BGR_INTEGER || format == GL_RGBA_INTEGER || format == GL_BGRA_INTEGER;
  ByteView dst_bytes = out->writable_view();
  uint64_t dst = 0;
  for (uint32_t z = 0; z < d; ++z) {
    for (uint32_t y = 0; y < h; ++y) {
      const uint64_t row_at = first + uint64_t{z} * slice_bytes + uint64_t{y} * row_bytes;
      for (uint32_t x = 0; x < w; ++x, dst += 16) {
        const uint8_t* p = bytes.data() + row_at + uint64_t{x} * bpp;
        float c[4] = {0, 0, 0, 1};
        float v[4] = {0, 0, 0, 1};
        if (type == GL_UNSIGNED_INT_24_8) {
          uint32_t raw_value = 0;
          __builtin_memcpy(&raw_value, p, 4);
          v[0] = static_cast<float>(raw_value >> 8) / 16777215.0f;
          v[1] = static_cast<float>(raw_value & 255);
        } else if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) {
          uint32_t depth_raw = 0, stencil_raw = 0;
          __builtin_memcpy(&depth_raw, p, 4);
          __builtin_memcpy(&stencil_raw, p + 4, 4);
          v[0] = u2f(depth_raw);
          v[1] = static_cast<float>(stencil_raw & 255);
        } else if (packed) {
          uint32_t raw_value = 0;
          const uint32_t pb = type_bytes(type);
          for (uint32_t k = 0; k < pb; ++k) raw_value |= uint32_t{p[swap ? pb - 1 - k : k]} << (8 * k);
          packed_decode(type, raw_value, c);
          for (int k = 0; k < 4; ++k) v[k] = c[k];
          if (format == GL_BGRA || format == GL_BGR) { const float t = v[0]; v[0] = v[2]; v[2] = t; }
        } else {
          for (uint32_t k = 0; k < n; ++k) c[k] = read_component(type, p + k * comp_bytes, swap, integer);
          switch (format) {
            case GL_RED: case GL_RED_INTEGER: v[0] = c[0]; v[3] = integer ? 1 : 1; break;
            case GL_GREEN: case GL_GREEN_INTEGER: v[1] = c[0]; break;
            case GL_BLUE: case GL_BLUE_INTEGER: v[2] = c[0]; break;
            case GL_ALPHA: case GL_ALPHA_INTEGER: v[3] = c[0]; break;
            case GL_RG: case GL_RG_INTEGER: v[0] = c[0]; v[1] = c[1]; break;
            case GL_RGB: case GL_RGB_INTEGER: v[0] = c[0]; v[1] = c[1]; v[2] = c[2]; break;
            case GL_BGR: case GL_BGR_INTEGER: v[0] = c[2]; v[1] = c[1]; v[2] = c[0]; break;
            case GL_BGRA: case GL_BGRA_INTEGER: v[0] = c[2]; v[1] = c[1]; v[2] = c[0]; v[3] = c[3]; break;
            case GL_ABGR_EXT: v[0] = c[3]; v[1] = c[2]; v[2] = c[1]; v[3] = c[0]; break;
            case GL_LUMINANCE: v[0] = v[1] = v[2] = c[0]; break;
            case GL_INTENSITY: v[0] = v[1] = v[2] = v[3] = c[0]; break;
            case GL_LUMINANCE_ALPHA: v[0] = v[1] = v[2] = c[0]; v[3] = c[1]; break;
            case GL_DEPTH_COMPONENT: v[0] = type == GL_FLOAT ? c[0] : c[0]; break;
            case GL_STENCIL_INDEX: case GL_COLOR_INDEX: v[0] = c[0]; v[1] = c[0]; break;
            default: v[0] = c[0]; v[1] = c[1]; v[2] = c[2]; v[3] = c[3]; break;
          }
        }
        for (int k = 0; k < 4; ++k) {
          const uint32_t word = f2u(v[k]);
          ByteView cell = dst_bytes.subview(dst + static_cast<uint64_t>(k) * 4, 4);
          cell[0] = static_cast<uint8_t>(word); cell[1] = static_cast<uint8_t>(word >> 8); cell[2] = static_cast<uint8_t>(word >> 16); cell[3] = static_cast<uint8_t>(word >> 24);
        }
      }
    }
  }
  return true;
}

// Scale and bias, and the colour maps, when any of them is not at its default.
void GlApi::pixels_transfer(ArrayView<float> rgba) {
  bool scaled = fx_.map_color;
  for (int i = 0; i < 4; ++i) if (fx_.pixel_scale[i] != 1 || fx_.pixel_bias[i] != 0) scaled = true;
  if (!scaled) return;
  for (uint64_t i = 0; i + 3 < rgba.size(); i += 4) {
    for (uint32_t k = 0; k < 4; ++k) {
      float v = rgba[i + k] * fx_.pixel_scale[k] + fx_.pixel_bias[k];
      if (fx_.map_color) {
        const PixelMap& map = pixel_maps_[kMapRtoR + k];
        if (map.size) {
          const float c = clamp01(v);
          const uint32_t at = static_cast<uint32_t>(c * static_cast<float>(map.size - 1) + 0.5f);
          v = map.values[at];
        }
      }
      rgba[i + k] = v;
    }
  }
}

bool GlApi::pixels_in_bitmap(uint32_t w, uint32_t h, UserPtr at, ByteView out) {
  // One bit a pixel; rows by the unpack alignment; skip_pixels counts bits.
  const uint64_t row_length = unpack_.row_length > 0 ? static_cast<uint64_t>(unpack_.row_length) : w;
  const uint64_t align = unpack_.alignment > 0 ? static_cast<uint64_t>(unpack_.alignment) : 1;
  const uint64_t row_bytes = ((row_length + 7) / 8 + align - 1) / align * align;
  const uint64_t skip_bits = static_cast<uint64_t>(unpack_.skip_pixels);
  const uint64_t first = static_cast<uint64_t>(unpack_.skip_rows) * row_bytes;
  const uint64_t total = first + (h ? (uint64_t{h} - 1) * row_bytes : 0) + (skip_bits + w + 7) / 8;
  ByteBuffer raw(a_);
  ConstByteView bytes;
  const webgl::Buffer* pbo = gl_.buffer_at(gl_.pixel_unpack_);
  if (pbo) {
    ConstByteView all = pbo->data.view();
    if (!all.try_subview(at.address, total, &bytes)) return false;
  } else {
    if (!raw.put_zeros(total) || !read_client(at, raw.writable_view())) return false;
    bytes = raw.view();
  }
  const uint64_t out_row = (uint64_t{w} + 7) / 8;
  for (uint64_t i = 0; i < out.size(); ++i) out[i] = 0;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const uint64_t bit = skip_bits + x;
      const uint8_t byte = bytes[first + uint64_t{y} * row_bytes + bit / 8];
      const uint32_t mask = unpack_.lsb_first ? 1u << (bit % 8) : 0x80u >> (bit % 8);
      if (byte & mask) out[uint64_t{y} * out_row + x / 8] |= static_cast<uint8_t>(0x80u >> (x % 8));
    }
  }
  return true;
}

// width * height * 4 floats -> client pixels by the pack state (or the pixel pack buffer).
bool GlApi::pixels_out(uint32_t w, uint32_t h, uint32_t format, uint32_t type, ConstByteView rgba, UserPtr at) {
  if (!valid_combination(format, type)) { error(GL_INVALID_ENUM); return false; }
  const uint32_t bpp = pixel_bytes(format, type);
  uint64_t row_bytes, slice_bytes, first;
  store_layout(pack_, w, h, bpp, &row_bytes, &slice_bytes, &first);
  const uint64_t total = first + (h ? (uint64_t{h} - 1) * row_bytes : 0) + uint64_t{w} * bpp;
  webgl::Buffer* pbo = gl_.buffer_at(gl_.pixel_pack_);
  ByteBuffer raw(a_);
  ByteView dst;
  if (pbo) {
    if (at.address > pbo->data.size() || total > pbo->data.size() - at.address) { error(GL_INVALID_OPERATION); return false; }
    dst = pbo->data.writable_view().subview(at.address, total);
  } else {
    // Bytes between the rows are left as the program has them.
    if (!raw.put_zeros(total)) { error(GL_OUT_OF_MEMORY); return false; }
    if (!read_client(at, raw.writable_view())) { error(GL_INVALID_OPERATION); return false; }
    dst = raw.writable_view();
  }
  const bool swap = pack_.swap_bytes != 0;
  const bool packed = is_packed(type);
  const uint32_t n = components_of(format), comp_bytes = type_bytes(type);
  uint64_t src = 0;
  for (uint32_t y = 0; y < h; ++y) {
    const uint64_t row_at = first + uint64_t{y} * row_bytes;
    for (uint32_t x = 0; x < w; ++x, src += 16) {
      float v[4];
      __builtin_memcpy(v, rgba.data() + src, 16);
      uint8_t* p = dst.data() + row_at + uint64_t{x} * bpp;
      if (type == GL_UNSIGNED_INT_24_8) {
        const uint32_t word = static_cast<uint32_t>(clamp01(v[0]) * 16777215.0f + 0.5f) << 8 | (static_cast<uint32_t>(v[1]) & 255);
        __builtin_memcpy(p, &word, 4);
      } else if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) {
        const uint32_t depth = f2u(v[0]), stencil = static_cast<uint32_t>(v[1]) & 255;
        __builtin_memcpy(p, &depth, 4);
        __builtin_memcpy(p + 4, &stencil, 4);
      } else if (packed) {
        float c[4] = {v[0], v[1], v[2], v[3]};
        if (format == GL_BGRA || format == GL_BGR) { c[0] = v[2]; c[2] = v[0]; }
        const uint32_t word = packed_encode(type, c);
        const uint32_t pb = type_bytes(type);
        for (uint32_t k = 0; k < pb; ++k) p[swap ? pb - 1 - k : k] = static_cast<uint8_t>(word >> (8 * k));
      } else {
        float c[4] = {v[0], v[1], v[2], v[3]};
        switch (format) {
          case GL_GREEN: case GL_GREEN_INTEGER: c[0] = v[1]; break;
          case GL_BLUE: case GL_BLUE_INTEGER: c[0] = v[2]; break;
          case GL_ALPHA: case GL_ALPHA_INTEGER: c[0] = v[3]; break;
          case GL_BGR: case GL_BGR_INTEGER: c[0] = v[2]; c[2] = v[0]; break;
          case GL_BGRA: case GL_BGRA_INTEGER: c[0] = v[2]; c[2] = v[0]; break;
          case GL_ABGR_EXT: c[0] = v[3]; c[1] = v[2]; c[2] = v[1]; c[3] = v[0]; break;
          case GL_LUMINANCE: c[0] = v[0] + v[1] + v[2]; break;
          case GL_LUMINANCE_ALPHA: c[0] = v[0] + v[1] + v[2]; c[1] = v[3]; break;
          case GL_STENCIL_INDEX: c[0] = v[1]; break;
          default: break;
        }
        const bool integer_target = format == GL_STENCIL_INDEX;
        for (uint32_t k = 0; k < n; ++k) {
          if (integer_target || format == GL_RED_INTEGER || format == GL_RG_INTEGER || format == GL_RGB_INTEGER || format == GL_RGBA_INTEGER || format == GL_BGR_INTEGER || format == GL_BGRA_INTEGER || format == GL_GREEN_INTEGER || format == GL_BLUE_INTEGER || format == GL_ALPHA_INTEGER) {
            const int64_t iv = static_cast<int64_t>(c[k]);
            for (uint32_t b = 0; b < comp_bytes; ++b) p[k * comp_bytes + (swap ? comp_bytes - 1 - b : b)] = static_cast<uint8_t>(iv >> (8 * b));
          } else {
            write_component(type, c[k], p + k * comp_bytes, swap);
          }
        }
      }
    }
  }
  if (!pbo && !write_client(at, raw.view())) { error(GL_INVALID_OPERATION); return false; }
  return true;
}

// --- Store state and maps -----------------------------------------------------------------------------------------------------------------------------------

void GlApi::glPixelStorei(GLenum pname, GLint param) {
  const auto positive = [&](int32_t* out) { if (param < 0) { error(GL_INVALID_VALUE); return; } *out = param; };
  switch (pname) {
    case GL_UNPACK_SWAP_BYTES: unpack_.swap_bytes = param != 0; break;
    case GL_UNPACK_LSB_FIRST: unpack_.lsb_first = param != 0; break;
    case GL_UNPACK_ROW_LENGTH: positive(&unpack_.row_length); break;
    case GL_UNPACK_IMAGE_HEIGHT: positive(&unpack_.image_height); break;
    case GL_UNPACK_SKIP_PIXELS: positive(&unpack_.skip_pixels); break;
    case GL_UNPACK_SKIP_ROWS: positive(&unpack_.skip_rows); break;
    case GL_UNPACK_SKIP_IMAGES: positive(&unpack_.skip_images); break;
    case GL_UNPACK_ALIGNMENT: if (param != 1 && param != 2 && param != 4 && param != 8) { error(GL_INVALID_VALUE); return; } unpack_.alignment = param; break;
    case GL_PACK_SWAP_BYTES: pack_.swap_bytes = param != 0; break;
    case GL_PACK_LSB_FIRST: pack_.lsb_first = param != 0; break;
    case GL_PACK_ROW_LENGTH: positive(&pack_.row_length); break;
    case GL_PACK_IMAGE_HEIGHT: positive(&pack_.image_height); break;
    case GL_PACK_SKIP_PIXELS: positive(&pack_.skip_pixels); break;
    case GL_PACK_SKIP_ROWS: positive(&pack_.skip_rows); break;
    case GL_PACK_SKIP_IMAGES: positive(&pack_.skip_images); break;
    case GL_PACK_ALIGNMENT: if (param != 1 && param != 2 && param != 4 && param != 8) { error(GL_INVALID_VALUE); return; } pack_.alignment = param; break;
    default: error(GL_INVALID_ENUM); break;
  }
}
void GlApi::glPixelStoref(GLenum pname, GLfloat param) { glPixelStorei(pname, static_cast<int32_t>(param)); }

void GlApi::glPixelZoom(GLfloat xfactor, GLfloat yfactor) { fx_.pixel_zoom[0] = xfactor; fx_.pixel_zoom[1] = yfactor; }

void GlApi::glPixelTransferf(GLenum pname, GLfloat param) {
  switch (pname) {
    case GL_MAP_COLOR: fx_.map_color = param != 0; break;
    case GL_MAP_STENCIL: fx_.map_stencil = param != 0; break;
    case GL_INDEX_SHIFT: fx_.index_shift = static_cast<int32_t>(param); break;
    case GL_INDEX_OFFSET: fx_.index_offset = static_cast<int32_t>(param); break;
    case GL_RED_SCALE: fx_.pixel_scale[0] = param; break;
    case GL_GREEN_SCALE: fx_.pixel_scale[1] = param; break;
    case GL_BLUE_SCALE: fx_.pixel_scale[2] = param; break;
    case GL_ALPHA_SCALE: fx_.pixel_scale[3] = param; break;
    case GL_DEPTH_SCALE: fx_.pixel_scale[4] = param; break;
    case GL_RED_BIAS: fx_.pixel_bias[0] = param; break;
    case GL_GREEN_BIAS: fx_.pixel_bias[1] = param; break;
    case GL_BLUE_BIAS: fx_.pixel_bias[2] = param; break;
    case GL_ALPHA_BIAS: fx_.pixel_bias[3] = param; break;
    case GL_DEPTH_BIAS: fx_.pixel_bias[4] = param; break;
    default: error(GL_INVALID_ENUM); break;
  }
}
void GlApi::glPixelTransferi(GLenum pname, GLint param) { glPixelTransferf(pname, static_cast<float>(param)); }

namespace {
int32_t map_index(uint32_t map) { return map >= GL_PIXEL_MAP_I_TO_I && map <= GL_PIXEL_MAP_A_TO_A ? static_cast<int32_t>(map - GL_PIXEL_MAP_I_TO_I) : -1; }
}

void GlApi::pixel_map_set(uint32_t map, int32_t size, const float* values) {
  const int32_t at = map_index(map);
  if (at < 0) { error(GL_INVALID_ENUM); return; }
  if (size < 1 || size > static_cast<int32_t>(PixelMap::kMaxSize)) { error(GL_INVALID_VALUE); return; }
  if (map <= GL_PIXEL_MAP_I_TO_A && (size & (size - 1))) { error(GL_INVALID_VALUE); return; }
  PixelMap& m = pixel_maps_[at];
  m.size = static_cast<uint32_t>(size);
  for (int32_t i = 0; i < size; ++i) m.values[i] = values[i];
}

void GlApi::glPixelMapfv(GLenum map, GLsizei mapsize, UserPtr values) {
  if (mapsize < 0 || mapsize > static_cast<int32_t>(PixelMap::kMaxSize)) { error(GL_INVALID_VALUE); return; }
  float v[PixelMap::kMaxSize];
  if (!read_values(values, v, static_cast<uint64_t>(mapsize))) { error(GL_INVALID_OPERATION); return; }
  pixel_map_set(map, mapsize, v);
}
void GlApi::glPixelMapuiv(GLenum map, GLsizei mapsize, UserPtr values) {
  if (mapsize < 0 || mapsize > static_cast<int32_t>(PixelMap::kMaxSize)) { error(GL_INVALID_VALUE); return; }
  uint32_t iv[PixelMap::kMaxSize];
  if (!read_values(values, iv, static_cast<uint64_t>(mapsize))) { error(GL_INVALID_OPERATION); return; }
  float v[PixelMap::kMaxSize];
  for (int32_t i = 0; i < mapsize; ++i) v[i] = map <= GL_PIXEL_MAP_S_TO_S ? static_cast<float>(iv[i]) : static_cast<float>(iv[i] / 4294967295.0);
  pixel_map_set(map, mapsize, v);
}
void GlApi::glPixelMapusv(GLenum map, GLsizei mapsize, UserPtr values) {
  if (mapsize < 0 || mapsize > static_cast<int32_t>(PixelMap::kMaxSize)) { error(GL_INVALID_VALUE); return; }
  uint16_t iv[PixelMap::kMaxSize];
  if (!read_values(values, iv, static_cast<uint64_t>(mapsize))) { error(GL_INVALID_OPERATION); return; }
  float v[PixelMap::kMaxSize];
  for (int32_t i = 0; i < mapsize; ++i) v[i] = map <= GL_PIXEL_MAP_S_TO_S ? static_cast<float>(iv[i]) : static_cast<float>(iv[i]) / 65535.0f;
  pixel_map_set(map, mapsize, v);
}
void GlApi::glGetPixelMapfv(GLenum map, UserPtr values) {
  const int32_t at = map_index(map);
  if (at < 0) { error(GL_INVALID_ENUM); return; }
  write_values(values, pixel_maps_[at].values, pixel_maps_[at].size);
}
void GlApi::glGetPixelMapuiv(GLenum map, UserPtr values) {
  const int32_t at = map_index(map);
  if (at < 0) { error(GL_INVALID_ENUM); return; }
  for (uint32_t i = 0; i < pixel_maps_[at].size; ++i) {
    const float v = pixel_maps_[at].values[i];
    const uint32_t out = map <= GL_PIXEL_MAP_S_TO_S ? static_cast<uint32_t>(v) : static_cast<uint32_t>(static_cast<double>(v) * 4294967295.0);
    write_values(values.plus(uint64_t{i} * 4), &out, 1);
  }
}
void GlApi::glGetPixelMapusv(GLenum map, UserPtr values) {
  const int32_t at = map_index(map);
  if (at < 0) { error(GL_INVALID_ENUM); return; }
  for (uint32_t i = 0; i < pixel_maps_[at].size; ++i) {
    const float v = pixel_maps_[at].values[i];
    const uint16_t out = map <= GL_PIXEL_MAP_S_TO_S ? static_cast<uint16_t>(v) : static_cast<uint16_t>(v * 65535.0f + 0.5f);
    write_values(values.plus(uint64_t{i} * 2), &out, 1);
  }
}

// --- Rectangles of pixels through the fragment stage ------------------------------------------------------------------------------------------------------------

// A textured rectangle in window coordinates: the fragment tests, blending and masks apply as for any fragment. `rgba`: w * h * 4 floats in window order, a negative alpha means no
// fragment there (glBitmap). `depth` non-empty: w * h floats that replace the rectangle's depth.
void GlApi::fragment_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, ConstByteView rgba, ConstByteView depth, float zoom_x, float zoom_y, float z) {
  if (w == 0 || h == 0) return;
  if (pixel_program_ == 0) {
    pixel_program_ = compile_program(
        "#version 300 es\nprecision highp float;\nin vec4 ff_Vertex;\nout vec2 v_t;\nvoid main(){ gl_Position=vec4(ff_Vertex.xy,ff_Vertex.z,1.0); v_t=ff_Vertex.xy*0.0; v_t=vec2(ff_Vertex.w,0.0); }\n",
        "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\nuniform sampler2D u_i;\nuniform vec2 u_org;\nuniform vec2 u_zoom;\nuniform vec4 u_size;\nuniform float u_has_depth;\nuniform sampler2D u_d;\nout vec4 ff_out;\n"
        "void main(){ ivec2 c=ivec2(floor((gl_FragCoord.xy-u_org)/u_zoom)); c=clamp(c,ivec2(0),ivec2(u_size.xy)-1); vec4 t=texelFetch(u_i,c,0); if(t.a<0.0) discard; ff_out=t; gl_FragDepth=u_has_depth>0.5?texelFetch(u_d,c,0).r:gl_FragCoord.z; }\n");
    if (pixel_program_ == 0) return;
    pixel_texture_ = gl_.create_texture();
    pixel_depth_texture_ = gl_.create_texture();
    pixel_buffer_ = gl_.create_buffer();
  }
  // The picture goes in two hidden textures on the two last units.
  constexpr uint32_t kUnit = webgl::kMaxTextureUnits - 1, kDepthUnit = webgl::kMaxTextureUnits - 2;
  webgl::TextureUnit& unit = gl_.units_[kUnit];
  webgl::TextureUnit& depth_unit = gl_.units_[kDepthUnit];
  const uint32_t saved_tex = unit.tex2d, saved_depth_tex = depth_unit.tex2d, saved_active = gl_.active_unit_, saved_sampler = unit.sampler, saved_depth_sampler = depth_unit.sampler;
  unit.sampler = 0;
  depth_unit.sampler = 0;
  gl_.active_unit_ = kUnit;
  gl_.bind_texture(GL_TEXTURE_2D, pixel_texture_);
  const webgl::GlContext::Unpack saved_unpack = gl_.unpack;
  gl_.unpack = webgl::GlContext::Unpack();
  gl_.unpack.alignment = 1;
  const uint32_t saved_pbo = gl_.pixel_unpack_;
  gl_.pixel_unpack_ = 0;
  gl_.tex_image(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 1, 0, GL_RGBA, GL_FLOAT, rgba, true, false);
  gl_.tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl_.tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (!depth.empty()) {
    gl_.active_unit_ = kDepthUnit;
    gl_.bind_texture(GL_TEXTURE_2D, pixel_depth_texture_);
    gl_.tex_image(GL_TEXTURE_2D, 0, GL_R32F, w, h, 1, 0, GL_RED, GL_FLOAT, depth, true, false);
    gl_.tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_.tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
  gl_.active_unit_ = saved_active;
  gl_.unpack = saved_unpack;
  gl_.pixel_unpack_ = saved_pbo;

  // Draw state of its own for this quad.
  webgl::DesktopState saved_ds = gl_.desktop;
  webgl::DesktopState& ds = gl_.desktop;
  ds.polygon_stipple = false;
  ds.flat_count = 0;
  ds.clip_mask = 0;
  ds.polygon_mode_front = ds.polygon_mode_back = GL_FILL;
  ds.triangle_edges = ConstByteView();
  ds.primitive_sink = nullptr;
  int32_t saved_vp[4] = {gl_.vp_[0], gl_.vp_[1], gl_.vp_[2], gl_.vp_[3]};
  const bool saved_cull = gl_.cull_;
  const uint32_t tw = gl_.width_, th = gl_.height_;
  webgl::GlContext::Target target;
  gl_.resolve_target(gl_.draw_fb_, true, &target);
  const float vw = static_cast<float>(target.w ? target.w : tw), vh = static_cast<float>(target.h ? target.h : th);
  gl_.vp_[0] = 0; gl_.vp_[1] = 0; gl_.vp_[2] = static_cast<int32_t>(vw); gl_.vp_[3] = static_cast<int32_t>(vh);
  gl_.cull_ = false;
  const float x0 = static_cast<float>(x), y0 = static_cast<float>(y), x1 = x0 + static_cast<float>(w) * zoom_x, y1 = y0 + static_cast<float>(h) * zoom_y;
  const float ndc_z = 2 * z - 1;
  const auto cx = [&](float v) { return 2 * v / vw - 1; };
  const auto cy = [&](float v) { return 2 * v / vh - 1; };
  const float quad[24] = {cx(x0), cy(y0), ndc_z, 0, cx(x1), cy(y0), ndc_z, 0, cx(x1), cy(y1), ndc_z, 0, cx(x0), cy(y0), ndc_z, 0, cx(x1), cy(y1), ndc_z, 0, cx(x0), cy(y1), ndc_z, 0};
  webgl::Buffer* qb = gl_.buffer_at(pixel_buffer_);
  qb->data.clear();
  qb->data.append(ConstByteView(reinterpret_cast<const uint8_t*>(quad), sizeof(quad)));
  webgl::Vao* vao = gl_.current_vao_ptr();
  const webgl::Vao saved_vao = *vao;
  for (auto& at : vao->attribs) at.enabled = false;
  webgl::VertexAttrib& pos = vao->attribs[kAttrVertex];
  pos.enabled = true;
  pos.from_client = false;
  pos.buffer = pixel_buffer_;
  pos.size = 4;
  pos.type = GL_FLOAT;
  pos.normalized = false;
  pos.integer = false;
  pos.divisor = 0;
  pos.stride = 16;
  pos.offset = 0;

  gl_.program_ = pixel_program_;
  const auto loc = [&](const char* name) { return gl_.get_uniform_location(pixel_program_, name); };
  const uint32_t unit_lane = kUnit, depth_lane = kDepthUnit;
  gl_.uniform(loc("u_i"), 'i', 1, 1, 1, &unit_lane);
  gl_.uniform(loc("u_d"), 'i', 1, 1, 1, &depth_lane);
  const uint32_t org[2] = {f2u(zoom_x < 0 ? x1 : x0), f2u(zoom_y < 0 ? y1 : y0)};
  gl_.uniform(loc("u_org"), 'f', 1, 2, 1, org);
  // With a negative zoom the rows run the other way: the fetch below flips through a zoom whose sign is kept.
  const uint32_t zoom[2] = {f2u(zoom_x), f2u(zoom_y)};
  gl_.uniform(loc("u_zoom"), 'f', 1, 2, 1, zoom);
  const uint32_t size[4] = {f2u(static_cast<float>(w)), f2u(static_cast<float>(h)), 0, 0};
  gl_.uniform(loc("u_size"), 'f', 1, 4, 1, size);
  const uint32_t has_depth = f2u(depth.empty() ? 0.0f : 1.0f);
  gl_.uniform(loc("u_has_depth"), 'f', 1, 1, 1, &has_depth);
  gl_.draw_arrays(GL_TRIANGLES, 0, 6, 1);

  *vao = saved_vao;
  gl_.desktop = saved_ds;
  gl_.vp_[0] = saved_vp[0]; gl_.vp_[1] = saved_vp[1]; gl_.vp_[2] = saved_vp[2]; gl_.vp_[3] = saved_vp[3];
  gl_.cull_ = saved_cull;
  gl_.program_ = user_program_;
  unit.tex2d = saved_tex;
  depth_unit.tex2d = saved_depth_tex;
  unit.sampler = saved_sampler;
  depth_unit.sampler = saved_depth_sampler;
}

void GlApi::glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, UserPtr pixels) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  if (!fx_.raster_valid) return;
  if (render_mode_ != GL_RENDER) return;
  const uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
  ByteBuffer data(a_);
  if (!pixels_in(w, h, 1, format, type, pixels, &data, unpack_)) return;
  const int32_t x = static_cast<int32_t>(fx_.raster_pos[0]), y = static_cast<int32_t>(fx_.raster_pos[1]);
  const float z = fx_.raster_pos[2];
  ArrayView<float> floats(reinterpret_cast<float*>(data.writable_view().data()), data.size() / 4);
  if (format == GL_DEPTH_COMPONENT) {
    // Depth from the data, colour from the raster position.
    ByteBuffer depth(a_), colors(a_);
    if (!depth.put_zeros(uint64_t{w} * h * 4) || !colors.put_zeros(uint64_t{w} * h * 16)) { error(GL_OUT_OF_MEMORY); return; }
    ArrayView<float> dv(reinterpret_cast<float*>(depth.writable_view().data()), uint64_t{w} * h);
    ArrayView<float> cv(reinterpret_cast<float*>(colors.writable_view().data()), uint64_t{w} * h * 4);
    for (uint64_t i = 0; i < dv.size(); ++i) {
      dv[i] = floats[i * 4] * fx_.pixel_scale[4] + fx_.pixel_bias[4];
      for (uint64_t k = 0; k < 4; ++k) cv[i * 4 + k] = fx_.raster_color[k];
    }
    fragment_rect(x, y, w, h, colors.view(), depth.view(), fx_.pixel_zoom[0], fx_.pixel_zoom[1], z);
    return;
  }
  if (format == GL_STENCIL_INDEX) {
    // Straight to the stencil buffer, under the stencil write mask, ownership and scissor rules of a fragment.
    webgl::GlContext::Target target;
    gl_.resolve_target(gl_.draw_fb_, true, &target);
    if (!target.stencil) return;
    const uint32_t mask = gl_.swmask_[0] & 255;
    for (uint32_t row = 0; row < h; ++row) {
      for (uint32_t col = 0; col < w; ++col) {
        const int64_t px = int64_t{x} + col, py = int64_t{y} + row;
        if (px < 0 || py < 0 || px >= int64_t{target.w} || py >= int64_t{target.h}) continue;
        if (gl_.scissor_test_ && (px < gl_.sc_[0] || py < gl_.sc_[1] || px >= int64_t{gl_.sc_[0]} + gl_.sc_[2] || py >= int64_t{gl_.sc_[1]} + gl_.sc_[3])) continue;
        uint32_t raw[4];
        webgl::read_texel(*target.stencil, static_cast<uint32_t>(px), static_cast<uint32_t>(py), target.depth_layer, raw);
        const uint32_t value = (static_cast<uint32_t>(static_cast<int64_t>(floats[(uint64_t{row} * w + col) * 4 + 1]) + fx_.index_offset)) & 255;
        raw[1] = (raw[1] & ~mask) | (value & mask);
        webgl::write_texel(*target.stencil, static_cast<uint32_t>(px), static_cast<uint32_t>(py), target.depth_layer, raw);
      }
    }
    return;
  }
  pixels_transfer(floats);
  fragment_rect(x, y, w, h, data.view(), ConstByteView(), fx_.pixel_zoom[0], fx_.pixel_zoom[1], z);
}

void GlApi::glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove, UserPtr bitmap) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  if (!fx_.raster_valid) return;
  if (render_mode_ == GL_RENDER && width > 0 && height > 0 && (!bitmap.null() || gl_.pixel_unpack_ != 0)) {
    const uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
    const uint64_t row = (uint64_t{w} + 7) / 8;
    ByteBuffer bits(a_), colors(a_);
    if (!bits.put_zeros(row * h) || !colors.put_zeros(uint64_t{w} * h * 16)) { error(GL_OUT_OF_MEMORY); return; }
    if (!pixels_in_bitmap(w, h, bitmap, bits.writable_view())) { error(GL_INVALID_OPERATION); return; }
    ArrayView<float> cv(reinterpret_cast<float*>(colors.writable_view().data()), uint64_t{w} * h * 4);
    const ConstByteView bv = bits.view();
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const uint64_t at = (uint64_t{y} * w + x) * 4;
        const bool on = (bv[uint64_t{y} * row + x / 8] & (0x80u >> (x % 8))) != 0;
        cv[at] = fx_.raster_color[0]; cv[at + 1] = fx_.raster_color[1]; cv[at + 2] = fx_.raster_color[2]; cv[at + 3] = on ? fx_.raster_color[3] : -1.0f;
      }
    }
    const float wx = fx_.raster_pos[0] - xorig, wy = fx_.raster_pos[1] - yorig;
    const float rx = wx < 0 ? static_cast<float>(static_cast<int32_t>(wx) - 1) : static_cast<float>(static_cast<int32_t>(wx));
    const float ry = wy < 0 ? static_cast<float>(static_cast<int32_t>(wy) - 1) : static_cast<float>(static_cast<int32_t>(wy));
    fragment_rect(static_cast<int32_t>(rx), static_cast<int32_t>(ry), w, h, colors.view(), ConstByteView(), 1, 1, fx_.raster_pos[2]);
  }
  fx_.raster_pos[0] += xmove;
  fx_.raster_pos[1] += ymove;
}

// The colour buffer reads take: image, its layer.
const webgl::Image* GlApi::read_source(uint32_t* layer) {
  *layer = 0;
  if (gl_.read_fb_ == 0) return gl_.dread_;
  webgl::GlContext::Target t;
  gl_.resolve_target(gl_.read_fb_, false, &t);
  for (uint32_t i = 0; i < webgl::kMaxDrawBuffers; ++i) if (t.color[i]) { *layer = t.layer[i]; return t.color[i]; }
  return nullptr;
}

namespace {
void texel_floats(const webgl::Image& im, uint32_t x, uint32_t y, uint32_t z, float out[4]) {
  uint32_t raw[4];
  webgl::read_texel(im, x, y, z, raw);
  switch (im.info->store) {
    case webgl::Store::U8x4:
      for (int i = 0; i < 4; ++i) {
        float v = static_cast<float>(raw[i]) / 255.0f;
        if (im.info->srgb && i < 3) v = webgl::srgb_to_linear(v);
        out[i] = v;
      }
      break;
    case webgl::Store::F32x4: for (int i = 0; i < 4; ++i) out[i] = u2f(raw[i]); break;
    case webgl::Store::Depth: out[0] = u2f(raw[0]); out[1] = 0; out[2] = 0; out[3] = 1; break;
    case webgl::Store::DepthStencil: out[0] = u2f(raw[0]); out[1] = static_cast<float>(raw[1]); out[2] = 0; out[3] = 1; break;
    case webgl::Store::I32x4: for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(static_cast<int32_t>(raw[i])); break;
    case webgl::Store::U32x4: for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(raw[i]); break;
  }
}
}  // namespace

// Copies a rectangle of the read buffer (colours) or the depth and stencil buffer into floats; pixels outside it read as 0.
bool GlApi::read_rectangle(int32_t x, int32_t y, uint32_t w, uint32_t h, bool depth_stencil, ByteBuffer* out) {
  out->clear();
  const uint64_t bytes = uint64_t{w} * h * 16;
  if (bytes > (4ull << 30) || !out->put_zeros(bytes)) { error(GL_OUT_OF_MEMORY); return false; }
  const webgl::Image* src;
  uint32_t layer = 0;
  if (depth_stencil) {
    webgl::GlContext::Target t;
    gl_.resolve_target(gl_.read_fb_, false, &t);
    src = t.depth ? t.depth : t.stencil;
    layer = t.depth_layer;
  } else {
    src = read_source(&layer);
  }
  if (!src || !src->defined()) return false;
  ByteView dst = out->writable_view();
  uint64_t at = 0;
  for (uint32_t row = 0; row < h; ++row) {
    for (uint32_t col = 0; col < w; ++col, at += 16) {
      const int64_t px = int64_t{x} + col, py = int64_t{y} + row;
      if (px < 0 || py < 0 || px >= int64_t{src->w} || py >= int64_t{src->h}) continue;
      float v[4];
      texel_floats(*src, static_cast<uint32_t>(px), static_cast<uint32_t>(py), layer, v);
      __builtin_memcpy(dst.data() + at, v, 16);
    }
  }
  return true;
}

void GlApi::glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, UserPtr pixels) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  if (!valid_combination(format, type)) { error(GL_INVALID_ENUM); return; }
  uint32_t status;
  if (!gl_.framebuffer_complete(gl_.read_fb_, &status)) { error(GL_INVALID_FRAMEBUFFER_OPERATION); return; }
  if (width == 0 || height == 0) return;
  gl_.hw_sync();
  const bool ds = format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL;
  const uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
  ByteBuffer data(a_);
  if (!read_rectangle(x, y, w, h, ds, &data)) {
    if (!ds) error(GL_INVALID_OPERATION);
    else error(GL_INVALID_OPERATION);
    return;
  }
  if (!ds) {
    ArrayView<float> floats(reinterpret_cast<float*>(data.writable_view().data()), data.size() / 4);
    pixels_transfer(floats);
  }
  pixels_out(w, h, format, type, data.view(), pixels);
}

void GlApi::glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type) {
  if (width < 0 || height < 0) { error(GL_INVALID_VALUE); return; }
  if (type != GL_COLOR && type != GL_DEPTH && type != GL_STENCIL && type != GL_DEPTH_STENCIL) { error(GL_INVALID_ENUM); return; }
  if (!fx_.raster_valid || render_mode_ != GL_RENDER) return;
  gl_.hw_sync();
  const uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
  ByteBuffer data(a_);
  if (!read_rectangle(x, y, w, h, type != GL_COLOR, &data)) { error(GL_INVALID_OPERATION); return; }
  const int32_t dx = static_cast<int32_t>(fx_.raster_pos[0]), dy = static_cast<int32_t>(fx_.raster_pos[1]);
  ArrayView<float> floats(reinterpret_cast<float*>(data.writable_view().data()), data.size() / 4);
  if (type == GL_COLOR) {
    pixels_transfer(floats);
    fragment_rect(dx, dy, w, h, data.view(), ConstByteView(), fx_.pixel_zoom[0], fx_.pixel_zoom[1], fx_.raster_pos[2]);
  } else if (type == GL_DEPTH) {
    ByteBuffer depth(a_), colors(a_);
    if (!depth.put_zeros(uint64_t{w} * h * 4) || !colors.put_zeros(uint64_t{w} * h * 16)) { error(GL_OUT_OF_MEMORY); return; }
    ArrayView<float> dv(reinterpret_cast<float*>(depth.writable_view().data()), uint64_t{w} * h);
    ArrayView<float> cv(reinterpret_cast<float*>(colors.writable_view().data()), uint64_t{w} * h * 4);
    for (uint64_t i = 0; i < dv.size(); ++i) {
      dv[i] = floats[i * 4];
      for (uint64_t k = 0; k < 4; ++k) cv[i * 4 + k] = fx_.raster_color[k];
    }
    fragment_rect(dx, dy, w, h, colors.view(), depth.view(), fx_.pixel_zoom[0], fx_.pixel_zoom[1], fx_.raster_pos[2]);
  } else {
    webgl::GlContext::Target target;
    gl_.resolve_target(gl_.draw_fb_, true, &target);
    if (!target.stencil) return;
    const uint32_t mask = gl_.swmask_[0] & 255;
    for (uint32_t row = 0; row < h; ++row) {
      for (uint32_t col = 0; col < w; ++col) {
        const int64_t px = int64_t{dx} + col, py = int64_t{dy} + row;
        if (px < 0 || py < 0 || px >= int64_t{target.w} || py >= int64_t{target.h}) continue;
        uint32_t raw[4];
        webgl::read_texel(*target.stencil, static_cast<uint32_t>(px), static_cast<uint32_t>(py), target.depth_layer, raw);
        const uint32_t value = static_cast<uint32_t>(floats[(uint64_t{row} * w + col) * 4 + 1]) & 255;
        raw[1] = (raw[1] & ~mask) | (value & mask);
        webgl::write_texel(*target.stencil, static_cast<uint32_t>(px), static_cast<uint32_t>(py), target.depth_layer, raw);
      }
    }
  }
}

// --- Accumulation -------------------------------------------------------------------------------------------------------------------------------------------

void GlApi::glClearAccum(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  fx_.accum_clear[0] = r; fx_.accum_clear[1] = g; fx_.accum_clear[2] = b; fx_.accum_clear[3] = a;
}

void GlApi::glAccum(GLenum op, GLfloat value) {
  if (op != GL_ACCUM && op != GL_LOAD && op != GL_RETURN && op != GL_ADD && op != GL_MULT) { error(GL_INVALID_ENUM); return; }
  if (!surface_ || gl_.draw_fb_ != 0 || gl_.read_fb_ != 0) { error(GL_INVALID_OPERATION); return; }
  ArrayView<float> accum = surface_->accum();
  if (accum.empty()) { error(GL_INVALID_OPERATION); return; }
  const uint32_t w = surface_->width(), h = surface_->height();
  gl_.hw_sync();
  // The scissor limits it, as it does clears.
  int32_t x0 = 0, y0 = 0, x1 = static_cast<int32_t>(w), y1 = static_cast<int32_t>(h);
  if (gl_.scissor_test_) {
    if (gl_.sc_[0] > x0) x0 = gl_.sc_[0];
    if (gl_.sc_[1] > y0) y0 = gl_.sc_[1];
    if (gl_.sc_[0] + gl_.sc_[2] < x1) x1 = gl_.sc_[0] + gl_.sc_[2];
    if (gl_.sc_[1] + gl_.sc_[3] < y1) y1 = gl_.sc_[1] + gl_.sc_[3];
  }
  uint32_t layer = 0;
  const webgl::Image* src = read_source(&layer);
  webgl::GlContext::Target target;
  gl_.resolve_target(0, true, &target);
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      const uint64_t at = (uint64_t(y) * w + uint64_t(x)) * 4;
      float c[4] = {0, 0, 0, 0};
      if ((op == GL_ACCUM || op == GL_LOAD) && src) texel_floats(*src, static_cast<uint32_t>(x), static_cast<uint32_t>(y), layer, c);
      switch (op) {
        case GL_ACCUM: for (uint64_t k = 0; k < 4; ++k) accum[at + k] += c[k] * value; break;
        case GL_LOAD: for (uint64_t k = 0; k < 4; ++k) accum[at + k] = c[k] * value; break;
        case GL_ADD: for (uint64_t k = 0; k < 4; ++k) accum[at + k] += value; break;
        case GL_MULT: for (uint64_t k = 0; k < 4; ++k) accum[at + k] *= value; break;
        default: {
          if (!target.color[0]) break;
          uint32_t raw[4];
          webgl::read_texel(*target.color[0], static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0, raw);
          for (uint64_t k = 0; k < 4; ++k) if (gl_.cmask_[k]) raw[k] = static_cast<uint32_t>(clamp01(accum[at + k] * value) * 255.0f + 0.5f);
          webgl::write_texel(*target.color[0], static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0, raw);
          break;
        }
      }
    }
  }
}

}  // namespace opengl
