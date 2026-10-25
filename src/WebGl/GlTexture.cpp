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

// Texture and renderbuffer storage for the GL context: the table of formats, pixel upload and download conversions, mipmaps and sampling.
#include "WebGl/GlContext.hpp"
#include "WebGl/GlPixels.hpp"

namespace webgl {

namespace {
constexpr uint32_t RED = 0x1903, RG = 0x8227, RGB = 0x1907, RGBA = 0x1908, LUM = 0x1909, ALPHA = 0x1906, LUMA = 0x190A, DEPTHC = 0x1902, DEPTHSTENCIL = 0x84F9;

const FormatInfo kFormats[] = {
    //            internal  store          base   srgb   rend   filt   float  signed
    {0x8058, Store::U8x4, RGBA, false, true, true, false, false},   // RGBA8
    {0x8051, Store::U8x4, RGB, false, true, true, false, false},    // RGB8
    {0x822B, Store::U8x4, RG, false, true, true, false, false},     // RG8
    {0x8229, Store::U8x4, RED, false, true, true, false, false},    // R8
    {0x8C43, Store::U8x4, RGBA, true, true, true, false, false},    // SRGB8_ALPHA8
    {0x8C41, Store::U8x4, RGB, true, false, true, false, false},    // SRGB8
    {0x8D62, Store::U8x4, RGB, false, true, true, false, false},    // RGB565
    {0x8056, Store::U8x4, RGBA, false, true, true, false, false},   // RGBA4
    {0x8057, Store::U8x4, RGBA, false, true, true, false, false},   // RGB5_A1
    {0x8059, Store::U8x4, RGBA, false, true, true, false, false},   // RGB10_A2
    {0x1908, Store::U8x4, RGBA, false, true, true, false, false},   // RGBA (unsized, ubyte)
    {0x1907, Store::U8x4, RGB, false, true, true, false, false},    // RGB
    {0x1909, Store::U8x4, LUM, false, false, true, false, false},   // LUMINANCE
    {0x190A, Store::U8x4, LUMA, false, false, true, false, false},  // LUMINANCE_ALPHA
    {0x1906, Store::U8x4, ALPHA, false, false, true, false, false}, // ALPHA
    {0x21908, Store::F32x4, RGBA, false, true, true, true, false},  // RGBA float (unsized)
    {0x21907, Store::F32x4, RGB, false, true, true, true, false},
    {0x21909, Store::F32x4, LUM, false, false, true, true, false},
    {0x2190A, Store::F32x4, LUMA, false, false, true, true, false},
    {0x21906, Store::F32x4, ALPHA, false, false, true, true, false},
    {0x822D, Store::F32x4, RED, false, true, true, true, false},     // R16F
    {0x822F, Store::F32x4, RG, false, true, true, true, false},      // RG16F
    {0x881B, Store::F32x4, RGB, false, true, true, true, false},     // RGB16F
    {0x881A, Store::F32x4, RGBA, false, true, true, true, false},    // RGBA16F
    {0x822E, Store::F32x4, RED, false, true, true, true, false},     // R32F
    {0x8230, Store::F32x4, RG, false, true, true, true, false},      // RG32F
    {0x8815, Store::F32x4, RGB, false, true, true, true, false},     // RGB32F
    {0x8814, Store::F32x4, RGBA, false, true, true, true, false},    // RGBA32F
    {0x8C3A, Store::F32x4, RGB, false, true, true, true, false},     // R11F_G11F_B10F
    {0x8C3D, Store::F32x4, RGB, false, false, true, true, false},    // RGB9_E5
    {0x8F94, Store::F32x4, RED, false, false, true, true, true},     // R8_SNORM
    {0x8F95, Store::F32x4, RG, false, false, true, true, true},
    {0x8F96, Store::F32x4, RGB, false, false, true, true, true},
    {0x8F97, Store::F32x4, RGBA, false, false, true, true, true},
    {0x8231, Store::I32x4, RED, false, true, false, false, true}, {0x8233, Store::I32x4, RED, false, true, false, false, true}, {0x8235, Store::I32x4, RED, false, true, false, false, true},
    {0x8237, Store::I32x4, RG, false, true, false, false, true}, {0x8239, Store::I32x4, RG, false, true, false, false, true}, {0x823B, Store::I32x4, RG, false, true, false, false, true},
    {0x8D8F, Store::I32x4, RGB, false, false, false, false, true}, {0x8D89, Store::I32x4, RGB, false, false, false, false, true}, {0x8D83, Store::I32x4, RGB, false, false, false, false, true},
    {0x8D8E, Store::I32x4, RGBA, false, true, false, false, true}, {0x8D88, Store::I32x4, RGBA, false, true, false, false, true}, {0x8D82, Store::I32x4, RGBA, false, true, false, false, true},
    {0x8232, Store::U32x4, RED, false, true, false, false, false}, {0x8234, Store::U32x4, RED, false, true, false, false, false}, {0x8236, Store::U32x4, RED, false, true, false, false, false},
    {0x8238, Store::U32x4, RG, false, true, false, false, false}, {0x823A, Store::U32x4, RG, false, true, false, false, false}, {0x823C, Store::U32x4, RG, false, true, false, false, false},
    {0x8D7D, Store::U32x4, RGB, false, false, false, false, false}, {0x8D77, Store::U32x4, RGB, false, false, false, false, false}, {0x8D71, Store::U32x4, RGB, false, false, false, false, false},
    {0x8D7C, Store::U32x4, RGBA, false, true, false, false, false}, {0x8D76, Store::U32x4, RGBA, false, true, false, false, false}, {0x8D70, Store::U32x4, RGBA, false, true, false, false, false},
    {0x906F, Store::U32x4, RGBA, false, true, false, false, false},
    {0x81A5, Store::Depth, DEPTHC, false, true, true, false, false}, {0x81A6, Store::Depth, DEPTHC, false, true, true, false, false}, {0x81A7, Store::Depth, DEPTHC, false, true, true, false, false},
    {0x8CAC, Store::Depth, DEPTHC, false, true, true, true, false}, {0x1902, Store::Depth, DEPTHC, false, true, true, false, false},
    {0x88F0, Store::DepthStencil, DEPTHSTENCIL, false, true, true, false, false}, {0x8CAD, Store::DepthStencil, DEPTHSTENCIL, false, true, true, true, false},
    {0x84F9, Store::DepthStencil, DEPTHSTENCIL, false, true, true, false, false}, {0x8D48, Store::DepthStencil, DEPTHSTENCIL, false, true, false, false, false},
};
}  // namespace

const FormatInfo* find_format(uint32_t internal) {
  for (const FormatInfo& f : kFormats)
    if (f.internal == internal) return &f;
  return nullptr;
}

uint32_t texel_bytes(Store s) {
  switch (s) {
    case Store::U8x4: return 4;
    case Store::Depth: return 4;
    case Store::DepthStencil: return 8;
    default: return 16;
  }
}

bool Image::allocate(const FormatInfo* fi, uint32_t width, uint32_t height, uint32_t depth) {
  const uint64_t n = uint64_t{width} * height * depth * texel_bytes(fi->store);
  if (n > (256ull << 20)) return false;
  px.clear();
  if (!px.put_zeros(n)) return false;
  info = fi;
  w = width;
  h = height;
  d = depth;
  return true;
}

void read_texel(const Image& img, uint32_t x, uint32_t y, uint32_t z, uint32_t out[4]) {
  const uint8_t* p = img.texel(x, y, z);
  switch (img.info->store) {
    case Store::U8x4: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; return;
    case Store::Depth: __builtin_memcpy(&out[0], p, 4); out[1] = out[2] = 0; out[3] = 0; return;
    case Store::DepthStencil: __builtin_memcpy(&out[0], p, 4); __builtin_memcpy(&out[1], p + 4, 4); out[2] = out[3] = 0; return;
    default: __builtin_memcpy(out, p, 16); return;
  }
}

void write_texel(Image& img, uint32_t x, uint32_t y, uint32_t z, const uint32_t v[4]) {
  uint8_t* p = img.texel(x, y, z);
  switch (img.info->store) {
    case Store::U8x4: p[0] = static_cast<uint8_t>(v[0]); p[1] = static_cast<uint8_t>(v[1]); p[2] = static_cast<uint8_t>(v[2]); p[3] = static_cast<uint8_t>(v[3]); return;
    case Store::Depth: __builtin_memcpy(p, &v[0], 4); return;
    case Store::DepthStencil: __builtin_memcpy(p, &v[0], 4); __builtin_memcpy(p + 4, &v[1], 4); return;
    default: __builtin_memcpy(p, v, 16); return;
  }
}

// --- Pixel conversion --------------------------------------------------------------------------------------------------------------------------------------

namespace {
inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }
}  // namespace

float srgb_to_linear(float c) {
  if (c <= 0.04045f) return c / 12.92f;
  return shader::fpow((c + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float c) {
  if (c <= 0) return 0;
  if (c >= 1) return 1;
  if (c <= 0.0031308f) return c * 12.92f;
  return 1.055f * shader::fpow(c, 1.0f / 2.4f) - 0.055f;
}

bool pixel_layout(uint32_t format, uint32_t type, uint32_t* ncomp, uint32_t* bytes, bool* packed) {
  uint32_t n = 0;
  switch (format) {
    case 0x1903: case 0x8D94: case 0x1909: case 0x1906: case 0x1902: n = 1; break;
    case 0x8227: case 0x8228: case 0x190A: n = 2; break;
    case 0x1907: case 0x8D98: n = 3; break;
    case 0x1908: case 0x8D99: n = 4; break;
    case 0x84F9: n = 2; break;
    default: return false;
  }
  *packed = false;
  switch (type) {
    case 0x1401: case 0x1400: *bytes = n; break;
    case 0x1403: case 0x1402: case 0x140B: case 0x8D61: *bytes = n * 2; break;
    case 0x1405: case 0x1404: case 0x1406: *bytes = n * 4; break;
    case 0x8033: case 0x8034: case 0x8363: *bytes = 2; *packed = true; break;
    case 0x8368: case 0x84FA: case 0x8C3B: case 0x8C3E: *bytes = 4; *packed = true; break;
    case 0x8DAD: *bytes = 8; *packed = true; break;
    default: return false;
  }
  *ncomp = n;
  return true;
}

namespace {

bool is_integer_format(uint32_t f) { return f == 0x8D94 || f == 0x8228 || f == 0x8D98 || f == 0x8D99; }

inline float snorm8(int8_t v) { const float f = static_cast<float>(v) / 127.0f; return f < -1 ? -1 : f; }
inline float snorm16(int16_t v) { const float f = static_cast<float>(v) / 32767.0f; return f < -1 ? -1 : f; }

// One source component of a non-packed pixel, as a float (normalised) or raw integer lane.
void read_component(uint32_t type, const uint8_t* p, bool integer, uint32_t* lane) {
  switch (type) {
    case 0x1401: *lane = integer ? p[0] : f2u(p[0] / 255.0f); return;
    case 0x1400: { const int8_t v = static_cast<int8_t>(p[0]); *lane = integer ? static_cast<uint32_t>(static_cast<int32_t>(v)) : f2u(snorm8(v)); return; }
    case 0x1403: { uint16_t v; __builtin_memcpy(&v, p, 2); *lane = integer ? v : f2u(v / 65535.0f); return; }
    case 0x1402: { int16_t v; __builtin_memcpy(&v, p, 2); *lane = integer ? static_cast<uint32_t>(static_cast<int32_t>(v)) : f2u(snorm16(v)); return; }
    case 0x1405: { uint32_t v; __builtin_memcpy(&v, p, 4); *lane = integer ? v : f2u(static_cast<float>(static_cast<double>(v) / 4294967295.0)); return; }
    case 0x1404: { int32_t v; __builtin_memcpy(&v, p, 4); *lane = integer ? static_cast<uint32_t>(v) : f2u(static_cast<float>(static_cast<double>(v) / 2147483647.0)); return; }
    case 0x1406: __builtin_memcpy(lane, p, 4); return;
    case 0x140B: case 0x8D61: { uint16_t v; __builtin_memcpy(&v, p, 2); *lane = f2u(shader::half_to_float(v)); return; }
    default: *lane = 0; return;
  }
}

uint32_t type_size(uint32_t type) {
  switch (type) {
    case 0x1401: case 0x1400: return 1;
    case 0x1403: case 0x1402: case 0x140B: case 0x8D61: return 2;
    default: return 4;
  }
}

// The four lanes (r, g, b, a) of one source pixel, expanded from the pixel format. `integer`: lanes are raw integers; else floats.
void decode_pixel(uint32_t format, uint32_t type, const uint8_t* p, uint32_t ncomp, bool integer, uint32_t lanes[4]) {
  const uint32_t one = integer ? 1 : f2u(1.0f);
  uint32_t c[4] = {0, 0, 0, 0};
  if (type == 0x8033 || type == 0x8034 || type == 0x8363) {
    uint16_t v;
    __builtin_memcpy(&v, p, 2);
    if (type == 0x8033) { c[0] = f2u(((v >> 12) & 15) / 15.0f); c[1] = f2u(((v >> 8) & 15) / 15.0f); c[2] = f2u(((v >> 4) & 15) / 15.0f); c[3] = f2u((v & 15) / 15.0f); }
    else if (type == 0x8034) { c[0] = f2u(((v >> 11) & 31) / 31.0f); c[1] = f2u(((v >> 6) & 31) / 31.0f); c[2] = f2u(((v >> 1) & 31) / 31.0f); c[3] = f2u((v & 1) ? 1.0f : 0.0f); }
    else { c[0] = f2u(((v >> 11) & 31) / 31.0f); c[1] = f2u(((v >> 5) & 63) / 63.0f); c[2] = f2u((v & 31) / 31.0f); c[3] = one; }
    ncomp = format == 0x1907 ? 3 : 4;
  } else if (type == 0x8368) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    if (integer) { c[0] = v & 1023; c[1] = (v >> 10) & 1023; c[2] = (v >> 20) & 1023; c[3] = v >> 30; }
    else { c[0] = f2u((v & 1023) / 1023.0f); c[1] = f2u(((v >> 10) & 1023) / 1023.0f); c[2] = f2u(((v >> 20) & 1023) / 1023.0f); c[3] = f2u((v >> 30) / 3.0f); }
    ncomp = 4;
  } else if (type == 0x8C3B) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    auto f11 = [](uint32_t m) { const uint32_t e = (m >> 6) & 31, f = m & 63; return e == 0 ? (f / 64.0f) * 6.103515625e-05f : (1.0f + f / 64.0f) * shader::fpow(2.0f, static_cast<float>(e) - 15.0f); };
    auto f10 = [](uint32_t m) { const uint32_t e = (m >> 5) & 31, f = m & 31; return e == 0 ? (f / 32.0f) * 6.103515625e-05f : (1.0f + f / 32.0f) * shader::fpow(2.0f, static_cast<float>(e) - 15.0f); };
    c[0] = f2u(f11(v & 2047)); c[1] = f2u(f11((v >> 11) & 2047)); c[2] = f2u(f10(v >> 22)); c[3] = one;
    ncomp = 3;
  } else if (type == 0x8C3E) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    const float scale = shader::fpow(2.0f, static_cast<float>(static_cast<int32_t>(v >> 27)) - 15.0f - 9.0f);
    c[0] = f2u((v & 511) * scale); c[1] = f2u(((v >> 9) & 511) * scale); c[2] = f2u(((v >> 18) & 511) * scale); c[3] = one;
    ncomp = 3;
  } else if (type == 0x84FA) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    c[0] = f2u((v >> 8) / 16777215.0f);
    c[1] = v & 255;
    ncomp = 2;
  } else if (type == 0x8DAD) {
    __builtin_memcpy(&c[0], p, 4);
    __builtin_memcpy(&c[1], p + 4, 4);
    c[1] &= 255;
    ncomp = 2;
  } else {
    const uint32_t ts = type_size(type);
    for (uint32_t i = 0; i < ncomp; ++i) read_component(type, p + i * ts, integer, &c[i]);
  }
  // expand to RGBA by the pixel format
  switch (format) {
    case 0x1903: case 0x8D94: lanes[0] = c[0]; lanes[1] = 0; lanes[2] = 0; lanes[3] = one; break;
    case 0x8227: case 0x8228: lanes[0] = c[0]; lanes[1] = c[1]; lanes[2] = 0; lanes[3] = one; break;
    case 0x1907: case 0x8D98: lanes[0] = c[0]; lanes[1] = c[1]; lanes[2] = c[2]; lanes[3] = one; break;
    case 0x1908: case 0x8D99: lanes[0] = c[0]; lanes[1] = c[1]; lanes[2] = c[2]; lanes[3] = c[3]; break;
    case 0x1909: lanes[0] = lanes[1] = lanes[2] = c[0]; lanes[3] = one; break;
    case 0x190A: lanes[0] = lanes[1] = lanes[2] = c[0]; lanes[3] = c[1]; break;
    case 0x1906: lanes[0] = lanes[1] = lanes[2] = 0; lanes[3] = c[0]; break;
    case 0x1902: case 0x84F9: lanes[0] = c[0]; lanes[1] = c[1]; lanes[2] = 0; lanes[3] = one; break;
    default: lanes[0] = lanes[1] = lanes[2] = lanes[3] = 0; break;
  }
}

uint8_t to_u8(float v) {
  if (!(v > 0)) return 0;
  if (v >= 1) return 255;
  return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

}  // namespace

// Converts the lanes of a decoded pixel to the storage of the destination (and its base format).
void store_lanes(const FormatInfo* fi, uint32_t lanes_in[4], bool integer_src, bool premultiply, uint32_t out[4]) {
  uint32_t l[4] = {lanes_in[0], lanes_in[1], lanes_in[2], lanes_in[3]};
  const bool int_dst = fi->store == Store::I32x4 || fi->store == Store::U32x4;
  if (int_dst) {
    // integer destinations take integer lanes (floats are rounded)
    for (int i = 0; i < 4; ++i) out[i] = integer_src ? l[i] : static_cast<uint32_t>(static_cast<int32_t>(u2f(l[i])));
    // components outside the base format
    const uint32_t b = fi->base;
    if (b == RED) { out[1] = out[2] = 0; out[3] = 1; }
    else if (b == RG) { out[2] = 0; out[3] = 1; }
    else if (b == RGB) out[3] = 1;
    return;
  }
  float f[4];
  for (int i = 0; i < 4; ++i) f[i] = integer_src ? static_cast<float>(static_cast<int32_t>(l[i])) : u2f(l[i]);
  const uint32_t b = fi->base;
  if (b == RED) { f[1] = f[2] = 0; f[3] = 1; }
  else if (b == RG) { f[2] = 0; f[3] = 1; }
  else if (b == RGB) f[3] = 1;
  if (premultiply) { f[0] *= f[3]; f[1] *= f[3]; f[2] *= f[3]; }
  switch (fi->store) {
    case Store::U8x4: {
      uint8_t q[4];
      for (int i = 0; i < 4; ++i) {
        float v = f[i];
        if (fi->srgb && i < 3) v = linear_to_srgb(v);
        q[i] = to_u8(v);
      }
      // reduced-precision formats lose the extra bits
      if (fi->internal == 0x8056) for (int i = 0; i < 4; ++i) q[i] = static_cast<uint8_t>(((q[i] >> 4) * 17));
      else if (fi->internal == 0x8D62) { q[0] = static_cast<uint8_t>((q[0] >> 3) * 255 / 31); q[1] = static_cast<uint8_t>((q[1] >> 2) * 255 / 63); q[2] = static_cast<uint8_t>((q[2] >> 3) * 255 / 31); }
      else if (fi->internal == 0x8057) { q[0] = static_cast<uint8_t>((q[0] >> 3) * 255 / 31); q[1] = static_cast<uint8_t>((q[1] >> 3) * 255 / 31); q[2] = static_cast<uint8_t>((q[2] >> 3) * 255 / 31); q[3] = q[3] >= 128 ? 255 : 0; }
      out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; out[3] = q[3];
      return;
    }
    case Store::F32x4:
      if (fi->is_signed) for (int i = 0; i < 4; ++i) f[i] = f[i] < -1 ? -1 : (f[i] > 1 ? 1 : f[i]);
      for (int i = 0; i < 4; ++i) out[i] = f2u(f[i]);
      return;
    case Store::Depth: out[0] = f2u(f[0] < 0 ? 0 : (f[0] > 1 ? 1 : f[0])); out[1] = out[2] = out[3] = 0; return;
    case Store::DepthStencil: out[0] = f2u(f[0] < 0 ? 0 : (f[0] > 1 ? 1 : f[0])); out[1] = integer_src ? l[1] : static_cast<uint32_t>(static_cast<int32_t>(u2f(l[1]))); out[2] = out[3] = 0; return;
    default: return;
  }
}

bool write_region(Image& img, uint32_t xo, uint32_t yo, uint32_t zo, uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t type, ConstByteView data,
                  const GlContext::Unpack& up, uint32_t* error) {
  uint32_t ncomp, bpp;
  bool packed;
  if (!pixel_layout(format, type, &ncomp, &bpp, &packed)) {
    *error = 0x500;
    return false;
  }
  const bool integer = is_integer_format(format);
  const uint64_t row_len = up.row_length ? up.row_length : w;
  const uint64_t row_bytes = (row_len * bpp + up.alignment - 1) / up.alignment * up.alignment;
  const uint64_t img_h = up.image_height ? up.image_height : h;
  const uint64_t slice_bytes = row_bytes * img_h;
  const uint64_t base = up.skip_images * slice_bytes + up.skip_rows * row_bytes + uint64_t{up.skip_pixels} * bpp;
  if (w && h && d) {
    const uint64_t last = base + (d - 1) * slice_bytes + (h - 1) * row_bytes + uint64_t{w} * bpp;
    if (last > data.size()) {
      *error = 0x502;
      return false;
    }
  }
  const FormatInfo* fi = img.info;
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t y = 0; y < h; ++y) {
      const uint32_t dy = up.flip_y ? yo + (h - 1 - y) : yo + y;
      const uint8_t* src = data.data() + base + z * slice_bytes + y * row_bytes;
      for (uint32_t x = 0; x < w; ++x) {
        uint32_t lanes[4], out[4];
        decode_pixel(format, type, src + uint64_t{x} * bpp, ncomp, integer, lanes);
        store_lanes(fi, lanes, integer, up.premultiply, out);
        write_texel(img, xo + x, dy, zo + z, out);
      }
    }
  return true;
}

// Reads one texel of a stored image as lanes in the form of a pixel format (floats unless integer).
void load_for_readback(const Image& img, uint32_t x, uint32_t y, uint32_t z, uint32_t lanes[4]) {
  uint32_t raw[4];
  read_texel(img, x, y, z, raw);
  switch (img.info->store) {
    case Store::U8x4:
      for (int i = 0; i < 4; ++i) {
        float v = raw[i] / 255.0f;
        if (img.info->srgb && i < 3) v = v;   // (read back as stored)
        lanes[i] = f2u(v);
      }
      break;
    default:
      for (int i = 0; i < 4; ++i) lanes[i] = raw[i];
      break;
  }
}

bool pack_pixels(const Image& img, int32_t x0, int32_t y0, uint32_t w, uint32_t h, uint32_t z, uint32_t format, uint32_t type, ByteView out, const GlContext::Pack& pk, uint32_t* error) {
  uint32_t ncomp, bpp;
  bool packed;
  if (!pixel_layout(format, type, &ncomp, &bpp, &packed)) {
    *error = 0x500;
    return false;
  }
  const bool int_fmt = is_integer_format(format);
  const uint64_t row_len = pk.row_length ? pk.row_length : w;
  const uint64_t row_bytes = (row_len * bpp + pk.alignment - 1) / pk.alignment * pk.alignment;
  const uint64_t base = pk.skip_rows * row_bytes + uint64_t{pk.skip_pixels} * bpp;
  if (w && h && base + (h - 1) * row_bytes + uint64_t{w} * bpp > out.size()) {
    *error = 0x502;
    return false;
  }
  const bool is_int_store = img.info->store == Store::I32x4 || img.info->store == Store::U32x4;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* dst = out.data() + base + y * row_bytes + uint64_t{x} * bpp;
      const int32_t sx = x0 + static_cast<int32_t>(x), sy = y0 + static_cast<int32_t>(y);
      uint32_t lanes[4] = {0, 0, 0, 0};
      if (sx >= 0 && sy >= 0 && sx < static_cast<int32_t>(img.w) && sy < static_cast<int32_t>(img.h)) load_for_readback(img, static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), z, lanes);
      float f[4];
      int64_t iv[4];
      for (int i = 0; i < 4; ++i) {
        if (is_int_store) {
          iv[i] = img.info->store == Store::I32x4 ? static_cast<int64_t>(static_cast<int32_t>(lanes[i])) : static_cast<int64_t>(lanes[i]);
          f[i] = static_cast<float>(iv[i]);
        } else {
          f[i] = u2f(lanes[i]);
          iv[i] = static_cast<int64_t>(f[i]);
        }
      }
      // pick components by the output format
      float c[4];
      int64_t ci[4];
      uint32_t n = ncomp;
      switch (format) {
        case 0x1903: case 0x8D94: case 0x1902: c[0] = f[0]; ci[0] = iv[0]; break;
        case 0x8227: case 0x8228: c[0] = f[0]; c[1] = f[1]; ci[0] = iv[0]; ci[1] = iv[1]; break;
        case 0x1907: case 0x8D98: for (int i = 0; i < 3; ++i) { c[i] = f[i]; ci[i] = iv[i]; } break;
        case 0x1909: c[0] = f[0]; ci[0] = iv[0]; break;
        case 0x190A: c[0] = f[0]; c[1] = f[3]; ci[0] = iv[0]; ci[1] = iv[3]; break;
        case 0x1906: c[0] = f[3]; ci[0] = iv[3]; break;
        default: for (int i = 0; i < 4; ++i) { c[i] = f[i]; ci[i] = iv[i]; } break;
      }
      if (type == 0x1401) {
        for (uint32_t i = 0; i < n; ++i) dst[i] = int_fmt ? static_cast<uint8_t>(ci[i] < 0 ? 0 : (ci[i] > 255 ? 255 : ci[i])) : to_u8(c[i]);
      } else if (type == 0x1406) {
        for (uint32_t i = 0; i < n; ++i) { const float v = int_fmt ? static_cast<float>(ci[i]) : c[i]; __builtin_memcpy(dst + 4 * i, &v, 4); }
      } else if (type == 0x1405 || type == 0x1404) {
        for (uint32_t i = 0; i < n; ++i) { const uint32_t v = int_fmt ? static_cast<uint32_t>(ci[i]) : (c[i] < 0 ? 0u : (c[i] >= 1 ? 0xFFFFFFFFu : static_cast<uint32_t>(c[i] * 4294967295.0))); __builtin_memcpy(dst + 4 * i, &v, 4); }
      } else if (type == 0x1403 || type == 0x1402) {
        for (uint32_t i = 0; i < n; ++i) { const uint16_t v = int_fmt ? static_cast<uint16_t>(ci[i]) : static_cast<uint16_t>(c[i] < 0 ? 0 : (c[i] >= 1 ? 65535 : c[i] * 65535.0f + 0.5f)); __builtin_memcpy(dst + 2 * i, &v, 2); }
      } else if (type == 0x1400) {
        for (uint32_t i = 0; i < n; ++i) dst[i] = static_cast<uint8_t>(int_fmt ? ci[i] : static_cast<int64_t>(c[i] * 127.0f));
      } else if (type == 0x140B || type == 0x8D61) {
        for (uint32_t i = 0; i < n; ++i) { const uint16_t v = shader::float_to_half(c[i]); __builtin_memcpy(dst + 2 * i, &v, 2); }
      } else if (type == 0x8033) {
        const uint16_t v = static_cast<uint16_t>((to_u8(f[0]) >> 4 << 12) | (to_u8(f[1]) >> 4 << 8) | (to_u8(f[2]) >> 4 << 4) | (to_u8(f[3]) >> 4));
        __builtin_memcpy(dst, &v, 2);
      } else if (type == 0x8034) {
        const uint16_t v = static_cast<uint16_t>((to_u8(f[0]) >> 3 << 11) | (to_u8(f[1]) >> 3 << 6) | (to_u8(f[2]) >> 3 << 1) | (f[3] >= 0.5f ? 1 : 0));
        __builtin_memcpy(dst, &v, 2);
      } else if (type == 0x8363) {
        const uint16_t v = static_cast<uint16_t>((to_u8(f[0]) >> 3 << 11) | (to_u8(f[1]) >> 2 << 5) | (to_u8(f[2]) >> 3));
        __builtin_memcpy(dst, &v, 2);
      } else if (type == 0x8368) {
        const uint32_t v = static_cast<uint32_t>(f[0] * 1023.0f + 0.5f) | (static_cast<uint32_t>(f[1] * 1023.0f + 0.5f) << 10) | (static_cast<uint32_t>(f[2] * 1023.0f + 0.5f) << 20) | (static_cast<uint32_t>(f[3] * 3.0f + 0.5f) << 30);
        __builtin_memcpy(dst, &v, 4);
      } else if (type == 0x84FA) {
        const uint32_t v = (static_cast<uint32_t>(f[0] * 16777215.0f) << 8) | static_cast<uint32_t>(lanes[1] & 255);
        __builtin_memcpy(dst, &v, 4);
      } else {
        *error = 0x500;
        return false;
      }
    }
  return true;
}

// --- Mipmaps -----------------------------------------------------------------------------------------------------------------------------------------------

void downsample(const Image& src, Image* dst) {
  const uint32_t w = dst->w, h = dst->h, d = dst->d;
  const bool three_d = src.d > 1 && dst->d != src.d;
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        const uint32_t sx0 = x * 2 < src.w ? x * 2 : src.w - 1, sx1 = x * 2 + 1 < src.w ? x * 2 + 1 : src.w - 1;
        const uint32_t sy0 = y * 2 < src.h ? y * 2 : src.h - 1, sy1 = y * 2 + 1 < src.h ? y * 2 + 1 : src.h - 1;
        const uint32_t sz0 = three_d ? (z * 2 < src.d ? z * 2 : src.d - 1) : z, sz1 = three_d ? (z * 2 + 1 < src.d ? z * 2 + 1 : src.d - 1) : z;
        const uint32_t xs[2] = {sx0, sx1}, ys[2] = {sy0, sy1}, zs[2] = {sz0, sz1};
        double acc[4] = {0, 0, 0, 0};
        uint32_t cnt = 0;
        uint32_t raw[4];
        for (uint32_t k = 0; k < (three_d ? 2u : 1u); ++k)
          for (uint32_t j = 0; j < 2; ++j)
            for (uint32_t i = 0; i < 2; ++i) {
              read_texel(src, xs[i], ys[j], zs[k], raw);
              switch (src.info->store) {
                case Store::U8x4: for (int c = 0; c < 4; ++c) acc[c] += raw[c]; break;
                case Store::F32x4: case Store::Depth: for (int c = 0; c < 4; ++c) acc[c] += u2f(raw[c]); break;
                case Store::DepthStencil: acc[0] += u2f(raw[0]); break;
                default: for (int c = 0; c < 4; ++c) acc[c] += static_cast<double>(raw[c]); break;
              }
              ++cnt;
            }
        uint32_t out[4] = {0, 0, 0, 0};
        switch (src.info->store) {
          case Store::U8x4: for (int c = 0; c < 4; ++c) out[c] = static_cast<uint32_t>(acc[c] / cnt + 0.5); break;
          case Store::F32x4: for (int c = 0; c < 4; ++c) out[c] = f2u(static_cast<float>(acc[c] / cnt)); break;
          case Store::Depth: out[0] = f2u(static_cast<float>(acc[0] / cnt)); break;
          case Store::DepthStencil: read_texel(src, sx0, sy0, sz0, raw); out[0] = f2u(static_cast<float>(acc[0] / cnt)); out[1] = raw[1]; break;
          default: read_texel(src, sx0, sy0, sz0, out); break;
        }
        write_texel(*dst, x, y, z, out);
      }
}

bool resolve_samples(const Image& src, uint32_t layer, Image* dst) {
  const uint32_t k = src.ss ? src.ss : 1, w = src.w / k, h = src.h / k;
  if (!dst->allocate(src.info, w, h, 1)) return false;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      double acc[4] = {0, 0, 0, 0};
      uint32_t raw[4], first[4];
      for (uint32_t j = 0; j < k; ++j)
        for (uint32_t i = 0; i < k; ++i) {
          read_texel(src, x * k + i, y * k + j, layer, raw);
          if (i == 0 && j == 0) for (int c = 0; c < 4; ++c) first[c] = raw[c];
          for (int c = 0; c < 4; ++c) acc[c] += src.info->store == Store::U8x4 ? static_cast<double>(raw[c]) : (src.info->store == Store::F32x4 || src.info->store == Store::Depth ? static_cast<double>(u2f(raw[c])) : 0.0);
        }
      uint32_t out[4];
      const double n = static_cast<double>(k) * k;
      switch (src.info->store) {
        case Store::U8x4: for (int c = 0; c < 4; ++c) out[c] = static_cast<uint32_t>(acc[c] / n + 0.5); break;
        case Store::F32x4: case Store::Depth: for (int c = 0; c < 4; ++c) out[c] = f2u(static_cast<float>(acc[c] / n)); break;
        default: for (int c = 0; c < 4; ++c) out[c] = first[c]; break;   // integers and stencil: the first sample
      }
      if (src.info->store == Store::Depth || src.info->store == Store::DepthStencil) out[0] = first[0];
      if (src.info->store == Store::DepthStencil) { out[1] = first[1]; out[2] = out[3] = 0; }
      write_texel(*dst, x, y, 0, out);
    }
  return true;
}

}  // namespace webgl
