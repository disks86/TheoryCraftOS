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

// The texture formats numbered from 64 (Graphics/GraphicsTypes.hpp, Graphics/WidePixels.hpp): every code of the packed 32-bit formats (R8, R8G8, A8, B5G6R5, B5G5R5A1, B4G4R4A4) through
// 0xAARRGGBB and back, every code of the 8- and 16-bit wide formats through Vec4s and back, the conversions of the integer and normalised ones (saturation, the most negative snorm
// code), R11G11B10's small floats (every code, rounding to nearest even, infinity, NaN, negative numbers, subnormals), RGB9E5's shared exponent (values worked out by hand, and
// random values within the format's precision), and the sizes of the formats.
#include <initializer_list>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "Graphics/WidePixels.hpp"
#include "Kernel/Dxgkrnl/DxgTypes.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

namespace {
int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

using graphics::Format;
using graphics::Vec4;

uint32_t bits(float f) { return __builtin_bit_cast(uint32_t, f); }
float flt(uint32_t b) { return __builtin_bit_cast(float, b); }

Vec4 decode(Format f, const uint8_t* bytes, uint32_t n) {
  ByteReader in{ConstByteView(bytes, n)};
  return graphics::decode_wide_pixel(f, in);
}
// The bytes encode_wide_pixel writes (as a little-endian number of up to 8 bytes; `n` gets how many).
uint64_t encode(Format f, Vec4 c, uint32_t* n) {
  uint8_t out[16] = {};
  ByteWriter w{ByteView(out, sizeof out)};
  graphics::encode_wide_pixel(f, c, w);
  *n = static_cast<uint32_t>(w.position());
  uint64_t v = 0;
  for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{out[i]} << (8 * i);
  return v;
}

void test_packed32() {
  int bad = 0;
  for (Format f : {Format::B5G6R5Unorm, Format::B5G5R5A1Unorm, Format::B4G4R4A4Unorm}) {
    for (uint32_t v = 0; v < 65536; ++v) bad += graphics::packed32_from_argb(f, graphics::packed32_to_argb(f, v)) != v;
    CHECK(graphics::bytes_per_pixel(f) == 2 && graphics::is_packed32_format(f) && !graphics::is_wide_format(f));
  }
  CHECK(bad == 0);
  for (uint32_t v = 0; v < 256; ++v) {
    CHECK(graphics::packed32_to_argb(Format::R8Unorm, v) == (0xFF000000u | v << 16));
    CHECK(graphics::packed32_to_argb(Format::A8Unorm, v) == v << 24);
    CHECK(graphics::packed32_from_argb(Format::R8Unorm, 0x12000034u | v << 16) == v);
  }
  for (uint32_t v = 0; v < 65536; ++v) bad += graphics::packed32_from_argb(Format::R8G8Unorm, graphics::packed32_to_argb(Format::R8G8Unorm, v)) != v;
  CHECK(bad == 0);
  // (the channels expand to the nearest 8-bit value: 31 -> 255, 16 -> 132, 1 of 4 bits -> 17; a 1-bit alpha is 255 or 0, and an 8-bit alpha of 127 narrows to 0, 128 to 1)
  CHECK(graphics::packed32_to_argb(Format::B5G6R5Unorm, 0xFFFF) == 0xFFFFFFFFu && graphics::packed32_to_argb(Format::B5G6R5Unorm, 16u << 11) == 0xFF840000u);
  CHECK(graphics::packed32_to_argb(Format::B4G4R4A4Unorm, 0x1111) == 0x11111111u);
  CHECK((graphics::packed32_from_argb(Format::B5G5R5A1Unorm, 0x7F000000u) >> 15) == 0 && (graphics::packed32_from_argb(Format::B5G5R5A1Unorm, 0x80000000u) >> 15) == 1);
  // (what a device keeps of a colour: A8's colour channels and R8's green, blue and alpha are gone)
  CHECK(graphics::quantize_packed32(Format::A8Unorm, 0x80FFFFFFu) == 0x80000000u && graphics::quantize_packed32(Format::R8Unorm, 0x10203040u) == 0xFF200000u);
  graphics::Surface s;
  s.format = Format::R8G8Unorm;
  CHECK(s.encode(0x11223344u) == 0xFF223300u && s.decode(0xFF223300u) == 0xFF223300u);
}

void test_wide_codes() {
  // (every code of the 8- and 16-bit formats decodes and encodes back to itself - but the most negative snorm code, which means -1 as the one above it does)
  struct Case { Format f; uint32_t bytes; bool snorm; };
  const Case cases[] = {{Format::R8Snorm, 1, true}, {Format::R8Uint, 1, false}, {Format::R8Sint, 1, false}, {Format::R8G8Snorm, 2, true}, {Format::R8G8Uint, 2, false}, {Format::R8G8Sint, 2, false},
                        {Format::R16Unorm, 2, false}, {Format::R16Snorm, 2, true}, {Format::R16Uint, 2, false}, {Format::R16Sint, 2, false}, {Format::R16Float, 2, false}};
  for (const Case& c : cases) {
    int bad = 0;
    CHECK(graphics::bytes_per_pixel(c.f) == c.bytes && graphics::is_wide_format(c.f));
    for (uint32_t v = 0; v < (1u << (8 * c.bytes)); ++v) {
      const uint8_t in[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
      const Vec4 p = decode(c.f, in, c.bytes);
      if (c.f == Format::R16Float && ((v >> 10) & 31) == 31 && (v & 0x3FF)) continue;   // (a NaN: some NaN)
      uint32_t n = 0;
      uint64_t back = encode(c.f, p, &n);
      if (c.snorm && c.bytes == 1 && v == 0x80) { bad += back != 0x81; continue; }
      if (c.snorm && c.bytes == 2 && c.f != Format::R16Snorm && (v & 0xFF) == 0x80) continue;
      if (c.snorm && c.bytes == 2 && c.f != Format::R16Snorm && (v >> 8) == 0x80) continue;
      if (c.f == Format::R16Snorm && v == 0x8000) { bad += back != 0x8001; continue; }
      bad += n != c.bytes || back != v;
    }
    CHECK(bad == 0);
  }
  // (the channels a format does not have: 0, and alpha 1 - an integer 1 for the integer formats)
  const uint8_t one[4] = {7, 9, 0, 0};
  Vec4 p = decode(Format::R8G8Uint, one, 2);
  CHECK(bits(p.x) == 7 && bits(p.y) == 9 && p.z == 0.0f && bits(p.w) == 1);
  p = decode(Format::R16Unorm, one, 2);
  CHECK(p.y == 0.0f && p.z == 0.0f && p.w == 1.0f);
  // (saturation of what does not fit; normalised values rounded to nearest, NaN 0)
  uint32_t n = 0;
  CHECK(encode(Format::R8Uint, {flt(300), 0, 0, 0}, &n) == 255 && n == 1);
  CHECK(encode(Format::R16Sint, {flt(static_cast<uint32_t>(-40000)), 0, 0, 0}, &n) == 0x8000);
  CHECK(encode(Format::R8G8Sint, {flt(static_cast<uint32_t>(-5)), flt(200), 0, 0}, &n) == (0xFBu | 0x7Fu << 8));
  CHECK(encode(Format::R8Snorm, {-2.0f, 0, 0, 0}, &n) == 0x81 && encode(Format::R8Snorm, {0.5f, 0, 0, 0}, &n) == 64 && encode(Format::R8Snorm, {NAN, 0, 0, 0}, &n) == 0);
  CHECK(encode(Format::R8G8B8A8Snorm, {1.0f, -1.0f, 0.0f, 2.0f}, &n) == (0x7Fu | 0x81u << 8 | 0x7Fu << 24) && n == 4);
  CHECK(encode(Format::R8G8B8A8Sint, {flt(1), flt(static_cast<uint32_t>(-1)), flt(128), flt(static_cast<uint32_t>(-129))}, &n) == (0x01u | 0xFFu << 8 | 0x7Fu << 16 | 0x80u << 24));
  CHECK(encode(Format::R16G16Unorm, {0.5f, 2.0f, 0, 0}, &n) == (0x8000u | 0xFFFFu << 16) && n == 4);
  CHECK(encode(Format::R16G16Float, {1.0f, -2.0f, 0, 0}, &n) == (0x3C00u | 0xC000u << 16));
  CHECK(encode(Format::R32G32Float, {1.5f, -0.25f, 0, 0}, &n) == (uint64_t{bits(1.5f)} | uint64_t{bits(-0.25f)} << 32) && n == 8);
  CHECK(encode(Format::R32G32B32Float, {1.0f, 2.0f, 3.0f, 4.0f}, &n) == (uint64_t{bits(1.0f)} | uint64_t{bits(2.0f)} << 32) && n == 12);
  CHECK(encode(Format::R10G10B10A2Uint, {flt(1023), flt(5000), flt(7), flt(9)}, &n) == (1023u | 1023u << 10 | 7u << 20 | 3u << 30));
  const uint8_t rgb10[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  p = decode(Format::R10G10B10A2Uint, rgb10, 4);
  CHECK(bits(p.x) == 1023 && bits(p.y) == 1023 && bits(p.z) == 1023 && bits(p.w) == 3);
  const uint8_t rgb32[12] = {1, 0, 0, 0, 2, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
  p = decode(Format::R32G32B32Sint, rgb32, 12);
  CHECK(bits(p.x) == 1 && bits(p.y) == 2 && bits(p.z) == 0xFFFFFFFFu && bits(p.w) == 1);
  // (quantize is decode(encode()))
  const Vec4 q = graphics::quantize_wide_pixel(Format::R8G8Snorm, {0.3f, 5.0f, 9.0f, 9.0f});
  CHECK(q.x == 38.0f / 127.0f && q.y == 1.0f && q.z == 0.0f && q.w == 1.0f);
}

// R11G11B10 (6-bit mantissas for red and green, 5 for blue).
void test_r11g11b10() {
  auto r11 = [](float x) { uint32_t n = 0; return static_cast<uint32_t>(encode(Format::R11G11B10Float, {x, 0, 0, 0}, &n) & 0x7FF); };
  auto b10 = [](float x) { uint32_t n = 0; return static_cast<uint32_t>(encode(Format::R11G11B10Float, {0, 0, x, 0}, &n) >> 22); };
  int bad = 0;
  for (uint32_t v = 0; v < 2048; ++v) {
    if ((v >> 6) == 31 && (v & 63)) continue;   // (NaN)
    const uint8_t in[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), 0, 0};
    bad += r11(decode(Format::R11G11B10Float, in, 4).x) != v;
  }
  for (uint32_t v = 0; v < 1024; ++v) {
    if ((v >> 5) == 31 && (v & 31)) continue;
    const uint32_t word = v << 22;
    const uint8_t in[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
    bad += b10(decode(Format::R11G11B10Float, in, 4).z) != v;
  }
  CHECK(bad == 0);
  CHECK(r11(1.0f) == (15u << 6) && b10(1.0f) == (15u << 5));
  CHECK(r11(65024.0f) == 0x7BF && r11(65536.0f) == 0x7C0 && r11(INFINITY) == 0x7C0 && b10(1e9f) == 0x3E0);      // (the largest; too large: infinity)
  CHECK(r11(-1.0f) == 0 && r11(-INFINITY) == 0 && r11(-0.0f) == 0 && (r11(NAN) >> 6) == 31 && (r11(NAN) & 63) != 0);
  CHECK(r11(1.0f + 1.0f / 128) == (15u << 6) && r11(1.0f + 3.0f / 128) == ((15u << 6) | 2));   // (ties to even)
  CHECK(r11(ldexpf(1.0f, -20)) == 1 && r11(ldexpf(1.0f, -21)) == 0 && r11(ldexpf(3.0f, -21)) == 2);   // (subnormals: 2^-20 is the smallest step; half of it ties to 0)
  const uint8_t ones[4] = {0xC0, 0x03, 0x00, 0x00};
  const Vec4 p = decode(Format::R11G11B10Float, ones, 4);
  CHECK(p.x == 1.0f && p.y == 0.0f && p.z == 0.0f && p.w == 1.0f);
}

void test_rgb9e5() {
  uint32_t n = 0;
  auto enc = [&](float r, float g, float b) { return static_cast<uint32_t>(encode(Format::R9G9B9E5Float, {r, g, b, 1}, &n)); };
  CHECK(enc(1, 0, 0) == (256u | 16u << 27) && enc(0, 0, 0) == 0);
  CHECK(enc(65408.0f, 0, 0) == (511u | 31u << 27) && enc(1e10f, -5.0f, NAN) == (511u | 31u << 27));   // (clamped to the largest, negative numbers and NaN 0)
  CHECK(enc(0.5f, 0.25f, 1.0f) == (128u | 64u << 9 | 256u << 18 | 16u << 27));
  CHECK(enc(0.9995f, 0, 0) == (256u | 16u << 27) && enc(0.9985f, 0, 0) == (511u | 15u << 27));   // (0.9995 rounds up to 512 steps: the exponent goes up)
  const uint32_t word = 3u | 5u << 9 | 7u << 18 | 20u << 27;
  const uint8_t in[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
  const Vec4 p = decode(Format::R9G9B9E5Float, in, 4);
  CHECK(p.x == 3.0f / 16 && p.y == 5.0f / 16 && p.z == 7.0f / 16 && p.w == 1.0f);   // (2^(20 - 15 - 9))
  // (random values come back within half a step of the largest channel's exponent)
  uint32_t seed = 12345, bad = 0;
  for (int i = 0; i < 20000; ++i) {
    float c[3];
    for (float& x : c) {
      seed = seed * 1664525u + 1013904223u;
      x = ldexpf(static_cast<float>(seed >> 8) / 16777216.0f, static_cast<int>(seed % 40) - 24);
    }
    const Vec4 q = graphics::quantize_wide_pixel(Format::R9G9B9E5Float, {c[0], c[1], c[2], 1});
    const float largest = fmaxf(c[0], fmaxf(c[1], c[2]));
    float step = largest < ldexpf(1.0f, -15) ? ldexpf(1.0f, -24) : ldexpf(1.0f, static_cast<int>(floorf(log2f(largest))) - 8);
    if (largest / step + 0.5f >= 512.0f) step *= 2;   // (the largest channel rounded up to 512 steps: the next exponent)
    const bool off = fabsf(q.x - c[0]) > step * 0.5f + 1e-12f || fabsf(q.y - c[1]) > step * 0.5f + 1e-12f || fabsf(q.z - c[2]) > step * 0.5f + 1e-12f;
    if (off && bad < 3) printf("  rgb9e5 %g %g %g -> %g %g %g (step %g)\n", c[0], c[1], c[2], q.x, q.y, q.z, step);
    bad += off;
  }
  CHECK(bad == 0);
}

void test_sizes() {
  using dxgk::pixel_bytes;
  CHECK(pixel_bytes(dxgk::Format::R8Unorm) == 1 && pixel_bytes(dxgk::Format::B5G6R5Unorm) == 2 && pixel_bytes(dxgk::Format::R16G16Float) == 4 && pixel_bytes(dxgk::Format::R32G32Uint) == 8 &&
        pixel_bytes(dxgk::Format::R32G32B32Float) == 12 && pixel_bytes(dxgk::Format::R11G11B10Float) == 4);
  CHECK(dxgk::image_row_bytes(dxgk::Format::Bc4Unorm, 5) == 16 && dxgk::image_row_bytes(dxgk::Format::Bc7Srgb, 5) == 32 && dxgk::image_rows(dxgk::Format::Bc6hUfloat, 5) == 2);
  CHECK(dxgk::is_known_format(27) && !dxgk::is_known_format(28) && !dxgk::is_known_format(63) && dxgk::is_known_format(64) && dxgk::is_known_format(dxgk::kLastExtraFormat) &&
        !dxgk::is_known_format(dxgk::kLastExtraFormat + 1));
  // (the kernel's numbers are graphics::Format's)
  CHECK(static_cast<uint32_t>(dxgk::Format::R10G10B10A2Uint) == static_cast<uint32_t>(Format::R10G10B10A2Uint) && static_cast<uint32_t>(dxgk::Format::Bc7Srgb) == static_cast<uint32_t>(Format::Bc7Srgb));
  CHECK(dxgk::is_packed32(dxgk::Format::A8Unorm) && dxgk::is_wide(dxgk::Format::R8Snorm) && dxgk::is_color(dxgk::Format::B4G4R4A4Unorm) && dxgk::is_compressed(dxgk::Format::Bc5Snorm) &&
        dxgk::is_integer(dxgk::Format::R16G16Uint) && !dxgk::is_integer(dxgk::Format::R16G16Float));
}
}  // namespace

int main() {
  test_packed32();
  test_wide_codes();
  test_r11g11b10();
  test_rgb9e5();
  test_sizes();
  if (g_failed) {
    printf("TextureFormatsTest: %d of %d checks failed\n", g_failed, g_checks);
    return 1;
  }
  printf("TextureFormatsTest: all %d checks passed\n", g_checks);
  return 0;
}
