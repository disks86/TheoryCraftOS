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

// The WebGPU device (see GpuDevice.hpp): resources, copies, pipelines, bindings and compute. Drawing is in GpuRender.cpp.
#include "WebGl/GpuDevice.hpp"
#include "WebGl/GlCompressed.hpp"
#include "WebGl/GlPixels.hpp"

namespace wgpu {

using shader::Base;
using shader::GlobalKind;

namespace {

inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }

// kind: 0 unorm8, 1 snorm8, 2 uint8, 3 sint8, 4 unorm16, 5 uint16, 6 sint16, 7 float16, 8 uint32, 9 sint32, 10 float32, 11 depth/stencil, 12 rgb10a2unorm
const FormatDesc kFormats[] = {
    {"r8unorm", 0x8229, false, 1, 0, 1, false, false}, {"r8snorm", 0x8F94, false, 1, 1, 1, false, false}, {"r8uint", 0x8232, false, 1, 2, 1, false, false},
    {"r8sint", 0x8231, false, 1, 3, 1, false, false}, {"rg8unorm", 0x822B, false, 2, 0, 2, false, false}, {"rg8snorm", 0x8F95, false, 2, 1, 2, false, false},
    {"rg8uint", 0x8238, false, 2, 2, 2, false, false}, {"rg8sint", 0x8237, false, 2, 3, 2, false, false}, {"rgba8unorm", 0x8058, false, 4, 0, 4, false, false},
    {"rgba8unorm-srgb", 0x8C43, false, 4, 0, 4, false, false}, {"rgba8snorm", 0x8F97, false, 4, 1, 4, false, false}, {"rgba8uint", 0x8D7C, false, 4, 2, 4, false, false},
    {"rgba8sint", 0x8D8E, false, 4, 3, 4, false, false}, {"bgra8unorm", 0x8058, true, 4, 0, 4, false, false}, {"bgra8unorm-srgb", 0x8C43, true, 4, 0, 4, false, false},
    {"rgb10a2unorm", 0x8059, false, 4, 12, 4, false, false}, {"rg11b10ufloat", 0x8C3A, false, 3, 13, 4, false, false}, {"r16uint", 0x8234, false, 1, 5, 2, false, false}, {"r16sint", 0x8233, false, 1, 6, 2, false, false},
    {"r16float", 0x822D, false, 1, 7, 2, false, false}, {"rg16uint", 0x823A, false, 2, 5, 4, false, false}, {"rg16sint", 0x8239, false, 2, 6, 4, false, false},
    {"rg16float", 0x822F, false, 2, 7, 4, false, false}, {"rgba16uint", 0x8D76, false, 4, 5, 8, false, false}, {"rgba16sint", 0x8D88, false, 4, 6, 8, false, false},
    {"rgba16float", 0x881A, false, 4, 7, 8, false, false}, {"r32uint", 0x8236, false, 1, 8, 4, false, false}, {"r32sint", 0x8235, false, 1, 9, 4, false, false},
    {"r32float", 0x822E, false, 1, 10, 4, false, false}, {"rg32uint", 0x823C, false, 2, 8, 8, false, false}, {"rg32sint", 0x823B, false, 2, 9, 8, false, false},
    {"rg32float", 0x8230, false, 2, 10, 8, false, false}, {"rgba32uint", 0x8D70, false, 4, 8, 16, false, false}, {"rgba32sint", 0x8D82, false, 4, 9, 16, false, false},
    {"rgba32float", 0x8814, false, 4, 10, 16, false, false}, {"stencil8", 0x8D48, false, 1, 11, 1, false, true}, {"depth16unorm", 0x81A5, false, 1, 4, 2, true, false},
    {"depth24plus", 0x81A6, false, 1, 4, 0, true, false}, {"depth24plus-stencil8", 0x88F0, false, 1, 11, 0, true, true}, {"depth32float", 0x8CAC, false, 1, 10, 4, true, false},
    {"depth32float-stencil8", 0x8CAD, false, 1, 10, 4, true, true},
    // block compressed (the texture-compression-bc and -astc features)
    {"bc1-rgba-unorm", 0x8058, false, 4, 0, 0, false, false, 4, 4, 8, 0x83F1},
    {"bc1-rgba-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 4, 4, 8, 0x8C4D},
    {"bc2-rgba-unorm", 0x8058, false, 4, 0, 0, false, false, 4, 4, 16, 0x83F2},
    {"bc2-rgba-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 4, 4, 16, 0x8C4E},
    {"bc3-rgba-unorm", 0x8058, false, 4, 0, 0, false, false, 4, 4, 16, 0x83F3},
    {"bc3-rgba-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 4, 4, 16, 0x8C4F},
    {"bc4-r-unorm", 0x8229, false, 4, 0, 0, false, false, 4, 4, 8, 0x8DBB},
    {"bc4-r-snorm", 0x8F94, false, 4, 0, 0, false, false, 4, 4, 8, 0x8DBC},
    {"bc5-rg-unorm", 0x822B, false, 4, 0, 0, false, false, 4, 4, 16, 0x8DBD},
    {"bc5-rg-snorm", 0x8F95, false, 4, 0, 0, false, false, 4, 4, 16, 0x8DBE},
    {"bc6h-rgb-ufloat", 0x8814, false, 4, 0, 0, false, false, 4, 4, 16, 0x8E8F},
    {"bc6h-rgb-float", 0x8814, false, 4, 0, 0, false, false, 4, 4, 16, 0x8E8E},
    {"bc7-rgba-unorm", 0x8058, false, 4, 0, 0, false, false, 4, 4, 16, 0x8E8C},
    {"bc7-rgba-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 4, 4, 16, 0x8E8D},
    {"etc2-rgb8unorm", 0x8058, false, 4, 0, 0, false, false, 4, 4, 8, 0x9274},
    {"etc2-rgb8unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 4, 4, 8, 0x9275},
    {"etc2-rgb8a1unorm", 0x8058, false, 4, 0, 0, false, false, 4, 4, 8, 0x9276},
    {"etc2-rgb8a1unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 4, 4, 8, 0x9277},
    {"etc2-rgba8unorm", 0x8058, false, 4, 0, 0, false, false, 4, 4, 16, 0x9278},
    {"etc2-rgba8unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 4, 4, 16, 0x9279},
    {"eac-r11unorm", 0x822E, false, 1, 10, 0, false, false, 4, 4, 8, 0x9270},
    {"eac-r11snorm", 0x822E, false, 1, 10, 0, false, false, 4, 4, 8, 0x9271},
    {"eac-rg11unorm", 0x8230, false, 2, 10, 0, false, false, 4, 4, 16, 0x9272},
    {"eac-rg11snorm", 0x8230, false, 2, 10, 0, false, false, 4, 4, 16, 0x9273},
    {"astc-4x4-unorm", 0x8814, false, 4, 0, 0, false, false, 4, 4, 16, 0x93B0},
    {"astc-4x4-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 4, 4, 16, 0x93D0},
    {"astc-5x4-unorm", 0x8814, false, 4, 0, 0, false, false, 5, 4, 16, 0x93B1},
    {"astc-5x4-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 5, 4, 16, 0x93D1},
    {"astc-5x5-unorm", 0x8814, false, 4, 0, 0, false, false, 5, 5, 16, 0x93B2},
    {"astc-5x5-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 5, 5, 16, 0x93D2},
    {"astc-6x5-unorm", 0x8814, false, 4, 0, 0, false, false, 6, 5, 16, 0x93B3},
    {"astc-6x5-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 6, 5, 16, 0x93D3},
    {"astc-6x6-unorm", 0x8814, false, 4, 0, 0, false, false, 6, 6, 16, 0x93B4},
    {"astc-6x6-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 6, 6, 16, 0x93D4},
    {"astc-8x5-unorm", 0x8814, false, 4, 0, 0, false, false, 8, 5, 16, 0x93B5},
    {"astc-8x5-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 8, 5, 16, 0x93D5},
    {"astc-8x6-unorm", 0x8814, false, 4, 0, 0, false, false, 8, 6, 16, 0x93B6},
    {"astc-8x6-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 8, 6, 16, 0x93D6},
    {"astc-8x8-unorm", 0x8814, false, 4, 0, 0, false, false, 8, 8, 16, 0x93B7},
    {"astc-8x8-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 8, 8, 16, 0x93D7},
    {"astc-10x5-unorm", 0x8814, false, 4, 0, 0, false, false, 10, 5, 16, 0x93B8},
    {"astc-10x5-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 10, 5, 16, 0x93D8},
    {"astc-10x6-unorm", 0x8814, false, 4, 0, 0, false, false, 10, 6, 16, 0x93B9},
    {"astc-10x6-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 10, 6, 16, 0x93D9},
    {"astc-10x8-unorm", 0x8814, false, 4, 0, 0, false, false, 10, 8, 16, 0x93BA},
    {"astc-10x8-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 10, 8, 16, 0x93DA},
    {"astc-10x10-unorm", 0x8814, false, 4, 0, 0, false, false, 10, 10, 16, 0x93BB},
    {"astc-10x10-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 10, 10, 16, 0x93DB},
    {"astc-12x10-unorm", 0x8814, false, 4, 0, 0, false, false, 12, 10, 16, 0x93BC},
    {"astc-12x10-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 12, 10, 16, 0x93DC},
    {"astc-12x12-unorm", 0x8814, false, 4, 0, 0, false, false, 12, 12, 16, 0x93BD},
    {"astc-12x12-unorm-srgb", 0x8C43, false, 4, 0, 0, false, false, 12, 12, 16, 0x93DD},
};

const char* const kVertexNames[] = {"uint8x2", "uint8x4", "sint8x2", "sint8x4", "unorm8x2", "unorm8x4", "snorm8x2", "snorm8x4", "uint16x2", "uint16x4", "sint16x2", "sint16x4",
                                    "unorm16x2", "unorm16x4", "snorm16x2", "snorm16x4", "float16x2", "float16x4", "float32", "float32x2", "float32x3", "float32x4", "uint32",
                                    "uint32x2", "uint32x3", "uint32x4", "sint32", "sint32x2", "sint32x3", "sint32x4", "unorm10-10-10-2", "unorm8x4-bgra"};

}  // namespace

const FormatDesc* find_format(StringView name) {
  for (const FormatDesc& f : kFormats)
    if (name.equals(f.name)) return &f;
  return nullptr;
}

uint8_t vertex_format(StringView name) {
  for (uint8_t i = 0; i < sizeof(kVertexNames) / sizeof(kVertexNames[0]); ++i)
    if (name.equals(kVertexNames[i])) return i;
  return 255;
}

uint32_t texel_copy_bytes(const FormatDesc& f, uint8_t aspect) {
  if (f.stencil && aspect == 2) return 1;
  if (f.depth && f.stencil && aspect == 0) return 0;
  if (f.depth) return aspect == 2 ? 0 : f.bytes;
  return f.bytes;
}

// --- Texel codecs --------------------------------------------------------------------------------------------------------------------------------------

namespace {

int kind_bits(uint8_t k) {
  switch (k) {
    case 0: case 1: case 2: case 3: return 8;
    case 4: case 5: case 6: case 7: return 16;
    default: return 32;
  }
}


// The unsigned small floats of rg11b10ufloat: 5 exponent bits, `mant` mantissa bits, bias 15, no sign.
float small_float_to_f32(uint32_t v, int mant) {
  const uint32_t e = v >> mant, m = v & ((1u << mant) - 1);
  const float scale = static_cast<float>(1u << mant);
  if (e == 0) return m / scale / 16384.0f;   // denormal: m / 2^mant * 2^-14
  if (e == 31) return m ? u2f(0x7FC00000u) : u2f(0x7F800000u);
  return (1.0f + m / scale) * static_cast<float>(1u << (e > 15 ? e - 15 : 0)) / static_cast<float>(1u << (e < 15 ? 15 - e : 0));
}

uint32_t f32_to_small_float(float f, int mant) {
  const uint32_t top = 31u << mant;
  if (f != f) return top | 1;
  if (f <= 0) return 0;
  const float max_finite = (2.0f - 1.0f / static_cast<float>(1u << mant)) * 32768.0f;
  if (f > max_finite) return f >= u2f(0x7F800000u) ? top : top - 1;
  const uint32_t bits = f2u(f);
  const int e32 = static_cast<int>(bits >> 23) - 127;
  if (e32 < -14) {   // denormal range: a multiple of 2^-(14 + mant)
    const float units = f * 16384.0f * static_cast<float>(1u << mant);
    uint32_t q = static_cast<uint32_t>(units);
    const float rem = units - static_cast<float>(q);
    if (rem > 0.5f || (rem == 0.5f && (q & 1))) ++q;
    return q;   // may carry into the first normal value, which is correct
  }
  uint32_t m = (bits & 0x7FFFFF) >> (23 - mant);
  const uint32_t rest = bits & ((1u << (23 - mant)) - 1), half = 1u << (22 - mant);
  uint32_t e = static_cast<uint32_t>(e32 + 15);
  if (rest > half || (rest == half && (m & 1))) ++m;
  if (m >> mant) { m = 0; ++e; }
  const uint32_t r = (e << mant) | m;
  return r > top - 1 ? top - 1 : r;
}

// Unpacks one texel from a buffer into the lanes the image store takes (see webgl::store_lanes): floats for unorm/snorm/float, raw ints for integer formats.
void decode_texel(const FormatDesc& f, const uint8_t* p, uint8_t aspect, uint32_t lanes[4], bool* ints) {
  *ints = f.kind == 2 || f.kind == 3 || f.kind == 5 || f.kind == 6 || f.kind == 8 || f.kind == 9;
  const bool depth_like = f.depth || f.stencil;
  lanes[0] = lanes[1] = lanes[2] = 0;
  lanes[3] = *ints ? 1u : f2u(1.0f);
  if (depth_like) {
    *ints = false;
    if (aspect == 2 || (!f.depth && f.stencil)) { lanes[1] = p[0]; *ints = true; return; }
    if (f.kind == 4) { uint16_t v; __builtin_memcpy(&v, p, 2); lanes[0] = f2u(v / 65535.0f); }
    else __builtin_memcpy(&lanes[0], p, 4);
    return;
  }
  if (f.kind == 12 || f.kind == 13) {   // packed in one 32 bit word
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    if (f.kind == 12) {
      lanes[0] = f2u((v & 1023) / 1023.0f); lanes[1] = f2u(((v >> 10) & 1023) / 1023.0f); lanes[2] = f2u(((v >> 20) & 1023) / 1023.0f); lanes[3] = f2u((v >> 30) / 3.0f);
    } else {
      lanes[0] = f2u(small_float_to_f32(v & 2047, 6)); lanes[1] = f2u(small_float_to_f32((v >> 11) & 2047, 6)); lanes[2] = f2u(small_float_to_f32(v >> 22, 5));
    }
    return;
  }
  uint8_t px[16] = {};
  const uint32_t n = f.comps * (kind_bits(f.kind) / 8);
  for (uint32_t i = 0; i < n && i < 16; ++i) px[i] = p[i];
  if (f.bgra) { const uint8_t t = px[0]; px[0] = px[2]; px[2] = t; }
  const int bytes = kind_bits(f.kind) / 8;
  for (uint32_t c = 0; c < f.comps; ++c) {
    const uint8_t* q = px + c * bytes;
    uint32_t out = 0;
    switch (f.kind) {
      case 0: out = f2u(q[0] / 255.0f); break;
      case 1: { const int8_t v = static_cast<int8_t>(q[0]); const float x = v / 127.0f; out = f2u(x < -1 ? -1 : x); break; }
      case 2: out = q[0]; break;
      case 3: out = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(q[0]))); break;
      case 5: { uint16_t v; __builtin_memcpy(&v, q, 2); out = v; break; }
      case 6: { int16_t v; __builtin_memcpy(&v, q, 2); out = static_cast<uint32_t>(static_cast<int32_t>(v)); break; }
      case 7: { uint16_t v; __builtin_memcpy(&v, q, 2); out = f2u(shader::half_to_float(v)); break; }
      default: __builtin_memcpy(&out, q, 4); break;
    }
    lanes[c] = out;
  }
}

// The reverse: one texel of the image into buffer bytes.
void encode_texel(const FormatDesc& f, const Image& img, uint32_t x, uint32_t y, uint32_t z, uint8_t aspect, uint8_t* out) {
  uint32_t raw[4];
  webgl::read_texel(img, x, y, z, raw);
  const bool depth_like = f.depth || f.stencil;
  if (depth_like) {
    if (aspect == 2 || (!f.depth && f.stencil)) { out[0] = static_cast<uint8_t>(raw[1]); return; }
    if (f.kind == 4) {
      const float d = u2f(raw[0]);
      const uint16_t v = static_cast<uint16_t>((d < 0 ? 0 : (d > 1 ? 1 : d)) * 65535.0f + 0.5f);
      __builtin_memcpy(out, &v, 2);
    } else {
      __builtin_memcpy(out, &raw[0], 4);
    }
    return;
  }
  float fv[4];
  int64_t iv[4];
  const webgl::Store st = img.info->store;
  for (int i = 0; i < 4; ++i) {
    if (st == webgl::Store::U8x4) { fv[i] = raw[i] / 255.0f; iv[i] = raw[i]; }
    else if (st == webgl::Store::F32x4) { fv[i] = u2f(raw[i]); iv[i] = static_cast<int64_t>(fv[i]); }
    else if (st == webgl::Store::I32x4) { iv[i] = static_cast<int32_t>(raw[i]); fv[i] = static_cast<float>(iv[i]); }
    else { iv[i] = raw[i]; fv[i] = static_cast<float>(raw[i]); }
  }
  if (f.kind == 12) {
    auto q = [](float v, float m) { return static_cast<uint32_t>((v < 0 ? 0 : (v > 1 ? 1 : v)) * m + 0.5f); };
    const uint32_t v = q(fv[0], 1023) | (q(fv[1], 1023) << 10) | (q(fv[2], 1023) << 20) | (q(fv[3], 3) << 30);
    __builtin_memcpy(out, &v, 4);
    return;
  }
  if (f.kind == 13) {
    const uint32_t v = f32_to_small_float(fv[0], 6) | (f32_to_small_float(fv[1], 6) << 11) | (f32_to_small_float(fv[2], 5) << 22);
    __builtin_memcpy(out, &v, 4);
    return;
  }
  uint8_t px[16];
  const int bytes = kind_bits(f.kind) / 8;
  for (uint32_t c = 0; c < f.comps; ++c) {
    uint8_t* q = px + c * bytes;
    switch (f.kind) {
      case 0: q[0] = static_cast<uint8_t>((fv[c] < 0 ? 0 : (fv[c] > 1 ? 1 : fv[c])) * 255.0f + 0.5f); break;
      case 1: q[0] = static_cast<uint8_t>(static_cast<int8_t>((fv[c] < -1 ? -1 : (fv[c] > 1 ? 1 : fv[c])) * 127.0f + (fv[c] < 0 ? -0.5f : 0.5f))); break;
      case 2: q[0] = static_cast<uint8_t>(iv[c]); break;
      case 3: q[0] = static_cast<uint8_t>(static_cast<int8_t>(iv[c])); break;
      case 5: case 6: { const uint16_t v = static_cast<uint16_t>(iv[c]); __builtin_memcpy(q, &v, 2); break; }
      case 7: { const uint16_t v = shader::float_to_half(fv[c]); __builtin_memcpy(q, &v, 2); break; }
      case 8: case 9: { const uint32_t v = static_cast<uint32_t>(iv[c]); __builtin_memcpy(q, &v, 4); break; }
      default: { const float v = fv[c]; __builtin_memcpy(q, &v, 4); break; }
    }
  }
  if (f.bgra) { const uint8_t t = px[0]; px[0] = px[2]; px[2] = t; }
  for (int i = 0; i < f.comps * bytes; ++i) out[i] = px[i];
}

void write_decoded(const FormatDesc& f, const uint8_t* p, uint8_t aspect, Image& img, uint32_t x, uint32_t y, uint32_t z) {
  uint32_t lanes[4], out[4];
  bool ints;
  decode_texel(f, p, aspect, lanes, &ints);
  if (f.depth || f.stencil) {
    uint32_t cur[4];
    webgl::read_texel(img, x, y, z, cur);
    if (aspect == 2 || (!f.depth && f.stencil)) cur[1] = lanes[1];
    else cur[0] = lanes[0];
    webgl::write_texel(img, x, y, z, cur);
    return;
  }
  webgl::store_lanes(img.info, lanes, ints, false, out);
  // a format without alpha keeps it at one; an unsigned integer format stores raw lanes
  webgl::write_texel(img, x, y, z, out);
}

}  // namespace

// --- Resources --------------------------------------------------------------------------------------------------------------------------------------------

Device::Device(allocators::IAllocator& a)
    : error(a), a_(a), buffers_(a), textures_(a), views_(a), samplers_(a), modules_(a), bgls_(a), pls_(a), groups_(a), rps_(a), cps_(a), query_sets_(a), dyn_all_(a), bound_(a) {}

Device::~Device() {}

uint32_t Device::create_buffer(uint64_t size, uint32_t usage) {
  if (size > (256ull << 20)) { fail("buffer too large"); return 0; }
  UniquePointer<Buffer> b = UniquePointer<Buffer>::make(a_, a_);
  if (!b || !b->data.put_zeros(size)) { fail("out of memory"); return 0; }
  b->usage = usage;
  buffers_.push_back(static_cast<UniquePointer<Buffer>&&>(b));
  return static_cast<uint32_t>(buffers_.count());
}

void Device::destroy_buffer(uint32_t id) {
  Buffer* b = buf(id);
  if (b) { b->destroyed = true; b->data.clear(); }
}

bool Device::write_buffer(uint32_t id, uint64_t offset, ConstByteView data) {
  Buffer* b = buf(id);
  if (!b || b->destroyed) return fail("buffer is destroyed");
  if (offset > b->data.size() || data.size() > b->data.size() - offset) return fail("write out of range");
  if (data.size()) b->data.writable_view().subview(offset, data.size()).copy_from(data);
  return true;
}

bool Device::read_buffer(uint32_t id, uint64_t offset, ByteView out) {
  Buffer* b = buf(id);
  if (!b || b->destroyed) return fail("buffer is destroyed");
  if (offset > b->data.size() || out.size() > b->data.size() - offset) return fail("read out of range");
  if (out.size()) out.copy_from(b->data.view().subview(offset, out.size()));
  return true;
}

uint64_t Device::buffer_size(uint32_t id) const { const Buffer* b = buf(id); return b ? b->data.size() : 0; }

uint32_t Device::create_texture(uint32_t w, uint32_t h, uint32_t depth, uint32_t mips, uint32_t samples, uint8_t dimension, const FormatDesc* format, uint32_t usage) {
  if (!format || w == 0 || h == 0 || depth == 0 || mips == 0 || w > 8192 || h > 8192 || depth > 2048) { fail("bad texture size"); return 0; }
  const webgl::FormatInfo* fi = webgl::find_format(format->gl);
  if (!fi) { fail("unsupported format"); return 0; }
  if (format->block_bytes) {
    if (w % format->bw || h % format->bh) { fail("the size of a compressed texture must be a multiple of its block size"); return 0; }
    if (samples > 1 || dimension == 0 || (usage & (0x10 | 0x8))) { fail("a compressed texture is a 2d or 3d, single sampled, sampled-only texture"); return 0; }
  }
  UniquePointer<Texture> t = UniquePointer<Texture>::make(a_, a_);
  if (!t) { fail("out of memory"); return 0; }
  t->format = format;
  t->w = w; t->h = h; t->depth = depth; t->mip_count = mips; t->samples = samples; t->dimension = dimension; t->usage = usage;
  uint64_t total = 0;
  for (uint32_t l = 0; l < mips; ++l) {
    const uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = dimension == 2 ? (depth >> l ? depth >> l : 1) : depth;
    UniquePointer<Image> im = UniquePointer<Image>::make(a_, a_);
    const uint32_t ss = samples > 1 ? 2 : 1;   // four samples are kept as a 2x supersampled image
    total += uint64_t{lw} * lh * ld * 16 * ss * ss;
    if (total > (512ull << 20) || !im || !im->allocate(fi, lw * ss, lh * ss, ld)) { fail("out of memory"); return 0; }
    im->ss = ss;
    if (format->block_bytes) {
      UniquePointer<ByteBuffer> rb = UniquePointer<ByteBuffer>::make(a_, a_);
      const uint64_t bytes = uint64_t{(lw + format->bw - 1) / format->bw} * ((lh + format->bh - 1) / format->bh) * ld * format->block_bytes;
      if (!rb || !rb->put_zeros(bytes)) { fail("out of memory"); return 0; }
      t->raw.push_back(static_cast<UniquePointer<ByteBuffer>&&>(rb));
    }
    if (fi->store == webgl::Store::Depth || fi->store == webgl::Store::DepthStencil) {   // depth starts at zero, as does everything else
    }
    t->mips.push_back(static_cast<UniquePointer<Image>&&>(im));
  }
  textures_.push_back(static_cast<UniquePointer<Texture>&&>(t));
  return static_cast<uint32_t>(textures_.count());
}

void Device::destroy_texture(uint32_t id) {
  Texture* t = texture_mut(id);
  if (t) t->destroyed = true;
}

uint32_t Device::create_view(const TextureView& v) {
  UniquePointer<TextureView> tv = UniquePointer<TextureView>::make(a_);
  if (!tv) return 0;
  *tv = v;
  views_.push_back(static_cast<UniquePointer<TextureView>&&>(tv));
  return static_cast<uint32_t>(views_.count());
}

uint32_t Device::create_sampler(const Sampler& s) {
  UniquePointer<Sampler> sp = UniquePointer<Sampler>::make(a_);
  if (!sp) return 0;
  *sp = s;
  samplers_.push_back(static_cast<UniquePointer<Sampler>&&>(sp));
  return static_cast<uint32_t>(samplers_.count());
}

uint32_t Device::create_shader_module(StringView code, String* messages) {
  UniquePointer<ShaderModule> m = UniquePointer<ShaderModule>::make(a_, a_);
  if (!m) return 0;
  m->source.assign(code);
  shader::Module probe(a_);
  m->ok = shader::parse_wgsl(probe, code);
  if (!m->ok) { m->log.assign(probe.error.view()); if (messages) messages->assign(probe.error.view()); }
  modules_.push_back(static_cast<UniquePointer<ShaderModule>&&>(m));
  return static_cast<uint32_t>(modules_.count());
}

uint32_t Device::create_bind_group_layout(const List<BglEntry>& entries) {
  UniquePointer<BindGroupLayout> l = UniquePointer<BindGroupLayout>::make(a_, a_);
  if (!l) return 0;
  for (const BglEntry& e : entries.view()) l->entries.push_back(e);
  // sorted by binding (dynamic offsets follow that order)
  for (uint64_t i = 1; i < l->entries.count(); ++i)
    for (uint64_t j = i; j > 0 && l->entries[j].binding < l->entries[j - 1].binding; --j) {
      const BglEntry t = l->entries[j];
      l->entries[j] = l->entries[j - 1];
      l->entries[j - 1] = t;
    }
  bgls_.push_back(static_cast<UniquePointer<BindGroupLayout>&&>(l));
  return static_cast<uint32_t>(bgls_.count());
}

uint32_t Device::create_pipeline_layout(ArrayView<const uint32_t> bgls) {
  UniquePointer<PipelineLayout> l = UniquePointer<PipelineLayout>::make(a_, a_);
  if (!l) return 0;
  for (uint32_t b : bgls) l->bgls.push_back(b);
  pls_.push_back(static_cast<UniquePointer<PipelineLayout>&&>(l));
  return static_cast<uint32_t>(pls_.count());
}

uint32_t Device::create_bind_group(uint32_t layout, const List<BgEntry>& entries) {
  if (!layout || layout > bgls_.count()) { fail("bad bind group layout"); return 0; }
  UniquePointer<BindGroup> g = UniquePointer<BindGroup>::make(a_, a_);
  if (!g) return 0;
  g->layout = layout;
  for (const BgEntry& e : entries.view()) g->entries.push_back(e);
  groups_.push_back(static_cast<UniquePointer<BindGroup>&&>(g));
  return static_cast<uint32_t>(groups_.count());
}

uint32_t Device::create_query_set(uint8_t type, uint32_t count) {
  UniquePointer<QuerySet> q = UniquePointer<QuerySet>::make(a_, a_);
  if (!q) return 0;
  q->type = type;
  for (uint32_t i = 0; i < count && i < 4096; ++i) q->results.push_back(0);
  query_sets_.push_back(static_cast<UniquePointer<QuerySet>&&>(q));
  return static_cast<uint32_t>(query_sets_.count());
}

// --- Pipelines -------------------------------------------------------------------------------------------------------------------------------------------

namespace {

int32_t find_entry(const shader::Module& m, StringView name, shader::Stage stage) {
  int32_t found = -1;
  for (uint64_t i = 0; i < m.functions.count(); ++i) {
    const shader::Function& f = *m.functions[i];
    if (!f.is_entry || f.stage != stage) continue;
    if (!name.empty()) {
      if (f.name.view().equals(name)) return static_cast<int32_t>(i);
    } else {
      if (found >= 0) return -2;   // ambiguous
      found = static_cast<int32_t>(i);
    }
  }
  return name.empty() ? found : -1;
}

}  // namespace

uint32_t Device::create_render_pipeline(uint32_t layout, uint32_t vs_module, StringView vs_entry, uint32_t fs_module, StringView fs_entry, List<UniquePointer<VertexBufferLayout>>& buffers,
                                        List<ColorTarget>& targets, const DepthStencilState* ds, uint8_t topology, uint8_t strip_format, bool ccw, uint8_t cull, bool unclipped, uint32_t samples,
                                        const List<UniquePointer<ConstantValue>>& vs_constants, const List<UniquePointer<ConstantValue>>& fs_constants) {
  if (!vs_module || vs_module > modules_.count() || !modules_[vs_module - 1]->ok) { fail(vs_module && vs_module <= modules_.count() ? modules_[vs_module - 1]->log.c_str() : "bad vertex shader module"); return 0; }
  UniquePointer<RenderPipeline> p = UniquePointer<RenderPipeline>::make(a_, a_);
  if (!p) { fail("out of memory"); return 0; }
  p->vs = UniquePointer<shader::Module>::make(a_, a_);
  if (!p->vs || !shader::parse_wgsl(*p->vs, modules_[vs_module - 1]->source.view())) { fail("vertex shader did not compile"); return 0; }
  for (const auto& c : vs_constants.view()) shader::set_override(*p->vs, c->name.view(), c->value);
  p->vs_fn = find_entry(*p->vs, vs_entry, shader::Stage::Vertex);
  if (p->vs_fn < 0) { fail("no vertex entry point with that name"); return 0; }
  if (fs_module) {
    if (fs_module > modules_.count() || !modules_[fs_module - 1]->ok) { fail("bad fragment shader module"); return 0; }
    p->fs = UniquePointer<shader::Module>::make(a_, a_);
    if (!p->fs || !shader::parse_wgsl(*p->fs, modules_[fs_module - 1]->source.view())) { fail("fragment shader did not compile"); return 0; }
    for (const auto& c : fs_constants.view()) shader::set_override(*p->fs, c->name.view(), c->value);
    p->fs_fn = find_entry(*p->fs, fs_entry, shader::Stage::Fragment);
    if (p->fs_fn < 0) { fail("no fragment entry point with that name"); return 0; }
  }
  for (auto& b : buffers.view()) p->buffers.push_back(static_cast<UniquePointer<VertexBufferLayout>&&>(b));
  for (const ColorTarget& t : targets.view()) p->targets.push_back(t);
  if (ds && ds->format) { p->has_depth = true; p->ds = *ds; }
  p->topology = topology;
  p->strip_index_format = strip_format;
  p->front_ccw = ccw;
  p->cull = cull;
  p->unclipped_depth = unclipped;
  p->sample_count = samples;
  if (layout) {
    if (layout > pls_.count()) { fail("bad pipeline layout"); return 0; }
    for (uint32_t b : pls_[layout - 1]->bgls.view()) p->layouts.push_back(b);
  } else {   // "auto": the bindings the shaders declare
    for (uint32_t group = 0; group < 4; ++group) {
      List<BglEntry> entries(a_);
      for (int s = 0; s < 2; ++s) {
        shader::Module* m = s == 0 ? p->vs.get() : p->fs.get();
        if (!m) continue;
        for (uint64_t gi = 0; gi < m->globals.count(); ++gi) {
          const shader::Global& g = *m->globals[gi];
          if (g.group != static_cast<int32_t>(group) || g.binding < 0 || !g.shared || !g.referenced) continue;
          BglEntry* e = nullptr;
          for (BglEntry& x : entries.view()) if (x.binding == static_cast<uint32_t>(g.binding)) e = &x;
          BglEntry fresh;
          if (!e) { fresh.binding = static_cast<uint32_t>(g.binding); fresh.visibility = 0; entries.push_back(fresh); e = &entries[entries.count() - 1]; }
          e->visibility |= s == 0 ? 1 : 2;
          if (g.kind == GlobalKind::Buffer) { e->kind = 0; e->buffer_type = g.storage ? (g.is_read_only ? 2 : 1) : 0; }
          else if (g.type.base == Base::SamplerState) { e->kind = 1; }
          else if (g.type.base == Base::Texture) { e->kind = 2; e->sample_type = g.type.shadow ? 2 : (g.type.texel == Base::Int ? 3 : (g.type.texel == Base::Uint ? 4 : 0)); e->view_dim = g.type.dim == shader::kDimCube ? 3 : (g.type.dim == shader::kDim2DArray ? 2 : (g.type.dim == shader::kDim3D ? 5 : 1)); }
        }
      }
      if (entries.count() == 0 && group > 0) break;
      p->layouts.push_back(create_bind_group_layout(entries));
    }
  }
  rps_.push_back(static_cast<UniquePointer<RenderPipeline>&&>(p));
  return static_cast<uint32_t>(rps_.count());
}

uint32_t Device::create_compute_pipeline(uint32_t layout, uint32_t module, StringView entry, const List<UniquePointer<ConstantValue>>& constants) {
  if (!module || module > modules_.count() || !modules_[module - 1]->ok) { fail(module && module <= modules_.count() ? modules_[module - 1]->log.c_str() : "bad shader module"); return 0; }
  UniquePointer<ComputePipeline> p = UniquePointer<ComputePipeline>::make(a_, a_);
  p->module = UniquePointer<shader::Module>::make(a_, a_);
  if (!p->module || !shader::parse_wgsl(*p->module, modules_[module - 1]->source.view())) { fail("shader did not compile"); return 0; }
  for (const auto& c : constants.view()) shader::set_override(*p->module, c->name.view(), c->value);
  p->fn = find_entry(*p->module, entry, shader::Stage::Compute);
  if (p->fn < 0) { fail("no compute entry point with that name"); return 0; }
  if (layout) {
    if (layout > pls_.count()) { fail("bad pipeline layout"); return 0; }
    for (uint32_t b : pls_[layout - 1]->bgls.view()) p->layouts.push_back(b);
  } else {
    for (uint32_t group = 0; group < 4; ++group) {
      List<BglEntry> entries(a_);
      shader::Module* m = p->module.get();
      for (uint64_t gi = 0; gi < m->globals.count(); ++gi) {
        const shader::Global& g = *m->globals[gi];
        if (g.group != static_cast<int32_t>(group) || g.binding < 0 || !g.shared || !g.referenced) continue;
        BglEntry e;
        e.binding = static_cast<uint32_t>(g.binding);
        e.visibility = 4;
        if (g.kind == GlobalKind::Buffer) { e.kind = 0; e.buffer_type = g.storage ? (g.is_read_only ? 2 : 1) : 0; }
        else if (g.type.base == Base::SamplerState) e.kind = 1;
        else if (g.type.base == Base::Texture) e.kind = 3;
        entries.push_back(e);
      }
      if (entries.count() == 0 && group > 0) break;
      p->layouts.push_back(create_bind_group_layout(entries));
    }
  }
  cps_.push_back(static_cast<UniquePointer<ComputePipeline>&&>(p));
  return static_cast<uint32_t>(cps_.count());
}

uint32_t Device::pipeline_layout_bgl(uint32_t pipeline, bool compute, uint32_t index) {
  if (compute) return pipeline && pipeline <= cps_.count() && index < cps_[pipeline - 1]->layouts.count() ? cps_[pipeline - 1]->layouts[index] : 0;
  return pipeline && pipeline <= rps_.count() && index < rps_[pipeline - 1]->layouts.count() ? rps_[pipeline - 1]->layouts[index] : 0;
}

// --- Copies ------------------------------------------------------------------------------------------------------------------------------------------------

bool Device::texture_copy(const ImageCopy& t, uint32_t w, uint32_t h, uint32_t d, Image** img, const FormatDesc** fd) {
  Texture* tx = texture_mut(t.texture);
  if (!tx || tx->destroyed) return fail("texture is destroyed");
  if (t.mip >= tx->mips.count()) return fail("mip level out of range");
  Image* im = tx->mips[t.mip].get();
  if (im && im->ss > 1) return fail("a multisampled texture cannot be copied; resolve it first");
  if (!im || uint64_t{t.x} + w > im->w || uint64_t{t.y} + h > im->h || uint64_t{t.z} + d > im->d) return fail("copy out of the texture");
  *img = im;
  *fd = tx->format;
  return true;
}

bool Device::copy_buffer_to_buffer(uint32_t src, uint64_t so, uint32_t dst, uint64_t dof, uint64_t size) {
  Buffer *s = buf(src), *d = buf(dst);
  if (!s || !d || s->destroyed || d->destroyed) return fail("buffer is destroyed");
  if (so > s->data.size() || size > s->data.size() - so || dof > d->data.size() || size > d->data.size() - dof) return fail("copy out of range");
  if (size) d->data.writable_view().subview(dof, size).copy_from(s->data.view().subview(so, size));
  return true;
}

bool Device::clear_buffer(uint32_t id, uint64_t offset, uint64_t size) {
  Buffer* b = buf(id);
  if (!b || b->destroyed) return fail("buffer is destroyed");
  if (offset > b->data.size()) return fail("clear out of range");
  if (size == 0 || size > b->data.size() - offset) size = b->data.size() - offset;
  if (size) __builtin_memset(b->data.writable_view().data() + offset, 0, size);
  return true;
}

bool Device::block_copy_ok(const Image& img, const FormatDesc& fd, const ImageCopy& t, uint32_t w, uint32_t h) {
  if (t.aspect != 0) return fail("a compressed texture has only the color aspect");
  if (t.x % fd.bw || t.y % fd.bh || (w % fd.bw && t.x + w != img.w) || (h % fd.bh && t.y + h != img.h)) return fail("the copy is not aligned to the blocks of the format");
  return true;
}

bool Device::write_blocks(const ImageCopy& dst, ConstByteView data, const BufferLayout& layout, uint32_t w, uint32_t h, uint32_t d) {
  Image* img;
  const FormatDesc* fd;
  if (!texture_copy(dst, w, h, d, &img, &fd) || !block_copy_ok(*img, *fd, dst, w, h)) return false;
  Texture* tx = texture_mut(dst.texture);
  const webgl::CompressedFormat* cf = webgl::find_compressed(fd->cgl);
  if (!cf) return fail("unsupported format");
  const uint32_t nbx = (w + fd->bw - 1) / fd->bw, nby = (h + fd->bh - 1) / fd->bh, mbx = (img->w + fd->bw - 1) / fd->bw, mby = (img->h + fd->bh - 1) / fd->bh;
  const uint64_t line = uint64_t{nbx} * fd->block_bytes;
  const uint64_t row = layout.bytes_per_row ? layout.bytes_per_row : line;
  const uint64_t rows = layout.rows_per_image ? layout.rows_per_image : nby;
  if (row < line || rows < nby) return fail("the layout is smaller than the copy");
  ByteView raw = tx->raw[dst.mip]->writable_view();
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t by = 0; by < nby; ++by) {
      const uint64_t at = layout.offset + (uint64_t{z} * rows + by) * row;
      if (at > data.size() || line > data.size() - at) return fail("data too small");
      const ConstByteView src = data.subview(at, line);
      webgl::decode_compressed(*cf, src, nbx * fd->bw, fd->bh, 1, img, dst.x, dst.y + by * fd->bh, dst.z + z);
      raw.subview(((uint64_t{dst.z + z} * mby + dst.y / fd->bh + by) * mbx + dst.x / fd->bw) * fd->block_bytes, line).copy_from(src);
    }
  return true;
}

bool Device::read_blocks(const ImageCopy& src, Buffer* dst, const BufferLayout& layout, uint32_t w, uint32_t h, uint32_t d) {
  Image* img;
  const FormatDesc* fd;
  if (!texture_copy(src, w, h, d, &img, &fd) || !block_copy_ok(*img, *fd, src, w, h)) return false;
  const Texture* tx = texture(src.texture);
  const uint32_t nbx = (w + fd->bw - 1) / fd->bw, nby = (h + fd->bh - 1) / fd->bh, mbx = (img->w + fd->bw - 1) / fd->bw, mby = (img->h + fd->bh - 1) / fd->bh;
  const uint64_t line = uint64_t{nbx} * fd->block_bytes;
  const uint64_t row = layout.bytes_per_row ? layout.bytes_per_row : line;
  const uint64_t rows = layout.rows_per_image ? layout.rows_per_image : nby;
  if (row < line || rows < nby) return fail("the layout is smaller than the copy");
  ConstByteView raw = tx->raw[src.mip]->view();
  ByteView out = dst->data.writable_view();
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t by = 0; by < nby; ++by) {
      const uint64_t at = layout.offset + (uint64_t{z} * rows + by) * row;
      if (at > out.size() || line > out.size() - at) return fail("buffer too small");
      out.subview(at, line).copy_from(raw.subview(((uint64_t{src.z + z} * mby + src.y / fd->bh + by) * mbx + src.x / fd->bw) * fd->block_bytes, line));
    }
  return true;
}

bool Device::write_texture(const ImageCopy& dst, ConstByteView data, const BufferLayout& layout, uint32_t w, uint32_t h, uint32_t d) {
  Image* img;
  const FormatDesc* fd;
  if (!texture_copy(dst, w, h, d, &img, &fd)) return false;
  if (fd->block_bytes) return write_blocks(dst, data, layout, w, h, d);
  const uint32_t bpp = texel_copy_bytes(*fd, dst.aspect);
  if (!bpp) return fail("this format cannot be written from memory");
  const uint64_t row = layout.bytes_per_row ? layout.bytes_per_row : uint64_t{w} * bpp;
  const uint64_t rows = layout.rows_per_image ? layout.rows_per_image : h;
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t y = 0; y < h; ++y) {
      const uint64_t at = layout.offset + z * rows * row + y * row;
      if (at + uint64_t{w} * bpp > data.size()) return fail("data too small");
      for (uint32_t x = 0; x < w; ++x) write_decoded(*fd, data.data() + at + uint64_t{x} * bpp, dst.aspect, *img, dst.x + x, dst.y + y, dst.z + z);
    }
  return true;
}

bool Device::copy_buffer_to_texture(uint32_t id, const BufferLayout& layout, const ImageCopy& dst, uint32_t w, uint32_t h, uint32_t d) {
  Buffer* b = buf(id);
  if (!b || b->destroyed) return fail("buffer is destroyed");
  return write_texture(dst, b->data.view(), layout, w, h, d);
}

bool Device::copy_texture_to_buffer(const ImageCopy& src, uint32_t id, const BufferLayout& layout, uint32_t w, uint32_t h, uint32_t d) {
  Buffer* b = buf(id);
  Image* img;
  const FormatDesc* fd;
  if (!b || b->destroyed) return fail("buffer is destroyed");
  if (!texture_copy(src, w, h, d, &img, &fd)) return false;
  if (fd->block_bytes) return read_blocks(src, b, layout, w, h, d);
  const uint32_t bpp = texel_copy_bytes(*fd, src.aspect);
  if (!bpp) return fail("this format cannot be read back");
  const uint64_t row = layout.bytes_per_row ? layout.bytes_per_row : uint64_t{w} * bpp;
  const uint64_t rows = layout.rows_per_image ? layout.rows_per_image : h;
  uint8_t* base = b->data.writable_view().data();
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t y = 0; y < h; ++y) {
      const uint64_t at = layout.offset + z * rows * row + y * row;
      if (at + uint64_t{w} * bpp > b->data.size()) return fail("buffer too small");
      for (uint32_t x = 0; x < w; ++x) encode_texel(*fd, *img, src.x + x, src.y + y, src.z + z, src.aspect, base + at + uint64_t{x} * bpp);
    }
  return true;
}

bool Device::copy_texture_to_texture(const ImageCopy& src, const ImageCopy& dst, uint32_t w, uint32_t h, uint32_t d) {
  Image *si, *di;
  const FormatDesc *sf, *df;
  if (!texture_copy(src, w, h, d, &si, &sf) || !texture_copy(dst, w, h, d, &di, &df)) return false;
  if (si->info != di->info || sf != df) return fail("formats differ");
  if (sf->block_bytes) {
    if (!block_copy_ok(*si, *sf, src, w, h) || !block_copy_ok(*di, *df, dst, w, h)) return false;
    const Texture* st = texture(src.texture);
    Texture* dt = texture_mut(dst.texture);
    const uint32_t nbx = (w + sf->bw - 1) / sf->bw, nby = (h + sf->bh - 1) / sf->bh;
    const uint32_t sbx = (si->w + sf->bw - 1) / sf->bw, sby = (si->h + sf->bh - 1) / sf->bh, dbx = (di->w + sf->bw - 1) / sf->bw, dby = (di->h + sf->bh - 1) / sf->bh;
    const uint64_t line = uint64_t{nbx} * sf->block_bytes;
    const webgl::CompressedFormat* cf = webgl::find_compressed(sf->cgl);
    if (!cf) return fail("unsupported format");
    for (uint32_t z = 0; z < d; ++z)
      for (uint32_t by = 0; by < nby; ++by) {
        const ConstByteView from = st->raw[src.mip]->view().subview(((uint64_t{src.z + z} * sby + src.y / sf->bh + by) * sbx + src.x / sf->bw) * sf->block_bytes, line);
        dt->raw[dst.mip]->writable_view().subview(((uint64_t{dst.z + z} * dby + dst.y / sf->bh + by) * dbx + dst.x / sf->bw) * sf->block_bytes, line).copy_from(from);
        webgl::decode_compressed(*cf, from, nbx * sf->bw, sf->bh, 1, di, dst.x, dst.y + by * sf->bh, dst.z + z);
      }
    return true;
  }
  for (uint32_t z = 0; z < d; ++z)
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        uint32_t raw[4];
        webgl::read_texel(*si, src.x + x, src.y + y, src.z + z, raw);
        if (src.aspect != 0 && (sf->depth && sf->stencil)) {   // one aspect only
          uint32_t cur[4];
          webgl::read_texel(*di, dst.x + x, dst.y + y, dst.z + z, cur);
          if (src.aspect == 2) cur[1] = raw[1]; else cur[0] = raw[0];
          webgl::write_texel(*di, dst.x + x, dst.y + y, dst.z + z, cur);
        } else {
          webgl::write_texel(*di, dst.x + x, dst.y + y, dst.z + z, raw);
        }
      }
  return true;
}

bool Device::copy_external(uint32_t texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t layer, ConstByteView rgba, uint32_t w, uint32_t h, bool flip_y, bool premultiply) {
  ImageCopy c;
  c.texture = texture; c.mip = mip; c.x = x; c.y = y; c.z = layer;
  Image* img;
  const FormatDesc* fd;
  if (!texture_copy(c, w, h, 1, &img, &fd)) return false;
  if (rgba.size() < uint64_t{w} * h * 4) return fail("pixel data too small");
  webgl::GlContext::Unpack up;
  up.flip_y = flip_y;
  up.premultiply = premultiply;
  up.alignment = 1;
  uint32_t err = 0;
  return webgl::write_region(*img, x, y, layer, w, h, 1, 0x1908, 0x1401, rgba, up, &err) || fail("could not convert the pixels");
}

void Device::clear_texture(uint32_t id) {
  Texture* t = texture_mut(id);
  if (!t) return;
  for (auto& im : t->mips.view())
    if (im && im->px.size()) {
      __builtin_memset(im->px.writable_view().data(), 0, im->px.size());
      if (im->info->store == webgl::Store::Depth) {   // (depth clears to zero here: render passes set what they need)
      }
    }
}

bool Device::read_canvas(uint32_t id, uint32_t* dst, uint32_t pitch, bool opaque, bool unpremultiply) {
  const Texture* t = texture(id);
  if (!t || t->mips.count() == 0) return false;
  const Image& im = *t->mips[0];
  if (im.info->store != webgl::Store::U8x4 && im.info->store != webgl::Store::F32x4) return false;
  for (uint32_t y = 0; y < im.h; ++y)
    for (uint32_t x = 0; x < im.w; ++x) {
      uint32_t raw[4];
      webgl::read_texel(im, x, y, 0, raw);
      uint32_t c[4];
      for (int i = 0; i < 4; ++i) {
        float v = im.info->store == webgl::Store::U8x4 ? raw[i] / 255.0f : u2f(raw[i]);
        if (im.info->store == webgl::Store::U8x4 && im.info->srgb && i < 3) v = raw[i] / 255.0f;   // (shown as stored)
        c[i] = static_cast<uint32_t>((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f + 0.5f);
      }
      uint32_t a = opaque ? 255 : c[3];
      if (!opaque && unpremultiply && a > 0 && a < 255) {
        for (int i = 0; i < 3; ++i) { c[i] = c[i] * 255 / a; if (c[i] > 255) c[i] = 255; }
      } else if (!opaque && unpremultiply && a == 0) {
        c[0] = c[1] = c[2] = 0;
      }
      dst[uint64_t{y} * pitch + x] = (a << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
    }
  return true;
}

// --- Texture host ------------------------------------------------------------------------------------------------------------------------------------

namespace {
struct ViewSource {
  const Texture* t;
  const TextureView* v;
};
const Image* view_image(const void* ctx, uint32_t, int32_t level) {
  const ViewSource& s = *static_cast<const ViewSource*>(ctx);
  if (level < 0 || static_cast<uint32_t>(level) >= s.v->mip_count) return nullptr;
  const uint32_t idx = s.v->base_mip + static_cast<uint32_t>(level);
  return idx < s.t->mips.count() ? s.t->mips[idx].get() : nullptr;
}
}  // namespace

uint32_t Device::register_bound(const TextureView* v, const Sampler* s) {
  Bound b;
  b.view = v;
  b.sampler = s;
  if (v) { b.tex = texture(v->texture); b.tex_mut = texture_mut(v->texture); }
  bound_.push_back(b);
  return static_cast<uint32_t>(bound_.count());
}

void Device::sample(const shader::SampleRequest& rq, uint32_t out[4]) {
  using shader::SampleMode;
  const bool int_tex = rq.texel != Base::Float;
  out[0] = out[1] = out[2] = 0;
  out[3] = int_tex ? 1u : f2u(1.0f);
  if (rq.texture == 0 || rq.texture > bound_.count()) return;
  const Bound& bt = bound_[rq.texture - 1];
  if (!bt.view || !bt.tex || bt.tex->destroyed) return;
  const TextureView& v = *bt.view;
  const Texture& t = *bt.tex;
  if (rq.mode == SampleMode::Store) {
    Image* im = v.base_mip < t.mips.count() ? t.mips[v.base_mip].get() : nullptr;
    if (!im) return;
    const int32_t x = rq.icoord[0], y = rq.icoord[1], z = rq.icoord[2] + static_cast<int32_t>(v.base_layer);
    if (x < 0 || y < 0 || z < 0 || x >= static_cast<int32_t>(im->w) || y >= static_cast<int32_t>(im->h) || z >= static_cast<int32_t>(im->d)) return;
    uint32_t lanes[4] = {rq.value[0], rq.value[1], rq.value[2], rq.value[3]}, w4[4];
    const bool ints = im->info->store == webgl::Store::I32x4 || im->info->store == webgl::Store::U32x4;
    webgl::store_lanes(im->info, lanes, ints, false, w4);
    webgl::write_texel(*im, static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z), w4);
    return;
  }
  ViewSource vs{&t, &v};
  webgl::SampleSource src;
  src.get = &view_image;
  src.ctx = &vs;
  src.base_level = 0;
  src.max_level = static_cast<int32_t>(v.mip_count) - 1;
  src.layer_offset = v.base_layer;
  switch (v.dim) {
    case 3: case 4: src.target = 0x8513; src.faces_in_layers = true; break;
    case 5: src.target = 0x806F; break;
    case 2: src.target = 0x8C1A; break;
    default: src.target = 0x0DE1; break;
  }
  webgl::SamplerObj so;
  so.min_filter = 0x2600; so.mag_filter = 0x2600;
  so.wrap_s = so.wrap_t = so.wrap_r = 0x812F;
  if (rq.sampler > 0 && rq.sampler <= bound_.count() && bound_[rq.sampler - 1].sampler) so = bound_[rq.sampler - 1].sampler->params;
  shader::SampleRequest r = rq;
  if (src.target == 0x8C1A && rq.mode != SampleMode::Fetch) r.coord[2] = rq.layer;
  // the depth of a depth view sampled without a comparison is the first channel; with a comparison sampler it is compared
  if (bt.sampler == nullptr && rq.sampler > 0 && rq.sampler <= bound_.count()) {}
  webgl::sample_images(src, so, r, out);
}

}  // namespace wgpu
