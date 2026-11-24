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
// Host tests for the video processor (src/Hardware/VideoCodec/VideoProcess.cpp). Expected values come from a reference written here in double with the published
// coefficients (BT.601 / BT.709 / BT.2020 matrices, the sRGB curve, BT.1886's 2.4 power, the BT.2087 primaries conversion), from an independent bilinear filter and
// orientation model, and from ffmpeg's zscale (tests/data/videoprocess/make_refs.sh). Every input format is converted to every output format in many colour spaces, in
// plain and in the kernel's widened plane layouts.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include "Graphics/GraphicsTypes.hpp"
#include "Hardware/VideoCodec/VideoProcess.hpp"
#include "Allocators/IAllocator.hpp"

using namespace hardware;

static int g_failed = 0;
static int g_reported = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { if (g_reported++ < 40) printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static const VpFormat kFormats[] = {VpFormat::Nv12, VpFormat::P010, VpFormat::P016, VpFormat::Bgra8, VpFormat::Bgrx8, VpFormat::Rgba8, VpFormat::Rgb10a2, VpFormat::Rgba16f};
static const char* format_name(VpFormat f) {
  static const char* const names[] = {"NV12", "P010", "P016", "BGRA8", "BGRX8", "RGBA8", "RGB10A2", "RGBA16F"};
  return names[static_cast<int>(f)];
}

// ---- pictures in memory (the test's own layout code)

struct Pic {
  VpImage img;
  std::vector<uint8_t> mem[2];
  VpFormat format = VpFormat::Nv12;
  uint32_t w = 0, h = 0;
};

static void set_plane(VpPlane& p, std::vector<uint8_t>& mem, uint32_t w, uint32_t h, uint32_t step, uint32_t pad, std::initializer_list<uint8_t> offsets, uint8_t bits, bool half) {
  p.pitch = w * step + pad;
  p.step = step;
  p.width = w;
  p.height = h;
  uint32_t c = 0;
  for (uint8_t o : offsets) p.offset[c++] = o;
  p.channels = static_cast<uint8_t>(offsets.size());
  p.bits = bits;
  p.half_float = half;
  mem.assign(size_t{p.pitch} * h, 0xA5);
  p.memory = ByteView(mem.data(), mem.size());
}

static Pic make_pic(VpFormat f, VpColorSpace space, uint32_t w, uint32_t h, bool widened = false, uint32_t pad = 3) {
  Pic pic;
  pic.format = f;
  pic.w = w;
  pic.h = h;
  pic.img.format = f;
  pic.img.color_space = space;
  pic.img.width = w;
  pic.img.height = h;
  const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
  switch (f) {
    case VpFormat::Nv12:
      if (widened) { set_plane(pic.img.plane[0], pic.mem[0], w, h, 4, 0, {0}, 8, false); set_plane(pic.img.plane[1], pic.mem[1], cw, ch, 4, 0, {0, 1}, 8, false); }
      else { set_plane(pic.img.plane[0], pic.mem[0], w, h, 1, pad, {0}, 8, false); set_plane(pic.img.plane[1], pic.mem[1], cw, ch, 2, pad, {0, 1}, 8, false); }
      break;
    case VpFormat::P010:
    case VpFormat::P016:
      if (widened) { set_plane(pic.img.plane[0], pic.mem[0], w, h, 8, 0, {0}, 16, false); set_plane(pic.img.plane[1], pic.mem[1], cw, ch, 8, 0, {0, 2}, 16, false); }
      else { set_plane(pic.img.plane[0], pic.mem[0], w, h, 2, 2 * pad, {0}, 16, false); set_plane(pic.img.plane[1], pic.mem[1], cw, ch, 4, 2 * pad, {0, 2}, 16, false); }
      break;
    case VpFormat::Bgra8: set_plane(pic.img.plane[0], pic.mem[0], w, h, 4, pad, {2, 1, 0, 3}, 8, false); break;
    case VpFormat::Bgrx8: set_plane(pic.img.plane[0], pic.mem[0], w, h, 4, pad, {2, 1, 0}, 8, false); break;
    case VpFormat::Rgba8: set_plane(pic.img.plane[0], pic.mem[0], w, h, 4, pad, {0, 1, 2, 3}, 8, false); break;
    case VpFormat::Rgb10a2: set_plane(pic.img.plane[0], pic.mem[0], w, h, 4, 4 * pad, {0, 1, 2, 3}, 10, false); break;
    case VpFormat::Rgba16f: set_plane(pic.img.plane[0], pic.mem[0], w, h, 8, 8 * pad, {0, 2, 4, 6}, 16, true); break;
  }
  return pic;
}

static bool is_yuv(VpFormat f) { return f == VpFormat::Nv12 || f == VpFormat::P010 || f == VpFormat::P016; }
static bool is_float(VpFormat f) { return f == VpFormat::Rgba16f; }
static int depth(VpFormat f) { return f == VpFormat::Nv12 ? 8 : f == VpFormat::P010 || f == VpFormat::Rgb10a2 ? 10 : f == VpFormat::P016 ? 16 : 8; }
static bool has_alpha(VpFormat f) { return f == VpFormat::Bgra8 || f == VpFormat::Rgba8 || f == VpFormat::Rgb10a2 || f == VpFormat::Rgba16f; }
static double code_max(VpFormat f, int c) {
  if (is_float(f)) return 1.0;
  if (f == VpFormat::Rgb10a2 && c == 3) return 3.0;
  return (1 << depth(f)) - 1;
}

static uint32_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
static void wr16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }

// Channel c of sample (x, y) of plane `pl`, as its code (an integer, or a float for half floats).
static double get_code(const Pic& pic, int pl, uint32_t x, uint32_t y, int c) {
  const VpPlane& p = pic.img.plane[pl];
  const uint8_t* s = pic.mem[pl].data() + size_t{y} * p.pitch + size_t{x} * p.step;
  switch (pic.format) {
    case VpFormat::Nv12: case VpFormat::Bgra8: case VpFormat::Bgrx8: case VpFormat::Rgba8: return s[p.offset[c]];
    case VpFormat::P010: return rd16(s + p.offset[c]) >> 6;
    case VpFormat::P016: return rd16(s + p.offset[c]);
    case VpFormat::Rgb10a2: { const uint32_t v = rd16(s) | rd16(s + 2) << 16; return c == 3 ? v >> 30 : (v >> (10 * c)) & 0x3FF; }
    case VpFormat::Rgba16f: return graphics::half_to_float(static_cast<uint16_t>(rd16(s + p.offset[c])));
  }
  return 0;
}
static void set_code(Pic& pic, int pl, uint32_t x, uint32_t y, int c, double code) {
  const VpPlane& p = pic.img.plane[pl];
  uint8_t* s = pic.mem[pl].data() + size_t{y} * p.pitch + size_t{x} * p.step;
  const double mx = code_max(pic.format, c);
  if (!is_float(pic.format)) code = floor(code + 0.5);
  if (code < 0) code = 0;
  if (code > mx) code = mx;
  switch (pic.format) {
    case VpFormat::Nv12: case VpFormat::Bgra8: case VpFormat::Bgrx8: case VpFormat::Rgba8: s[p.offset[c]] = static_cast<uint8_t>(code); return;
    case VpFormat::P010: wr16(s + p.offset[c], static_cast<uint32_t>(code) << 6); return;
    case VpFormat::P016: wr16(s + p.offset[c], static_cast<uint32_t>(code)); return;
    case VpFormat::Rgb10a2: {
      uint32_t v = rd16(s) | rd16(s + 2) << 16;
      const uint32_t shift = c == 3 ? 30 : 10 * c, mask = c == 3 ? 3u : 0x3FFu;
      v = (v & ~(mask << shift)) | (static_cast<uint32_t>(code) << shift);
      wr16(s, v & 0xFFFF);
      wr16(s + 2, v >> 16);
      return;
    }
    case VpFormat::Rgba16f: wr16(s + p.offset[c], graphics::float_to_half(static_cast<float>(code))); return;
  }
}

// ---- the reference model

struct Space { bool yuv, full; int transfer; bool p2020; int matrix; int siting; };   // transfer 0 sRGB (G22), 1 linear (G10), 2 power 2.4 (G24); siting 0 centre, 1 left, 2 top left
static Space space_of(VpColorSpace s) {
  switch (s) {
    case 0: return {false, true, 0, false, 709, 0};
    case 1: return {false, true, 1, false, 709, 0};
    case 2: return {false, false, 0, false, 709, 0};
    case 3: return {false, false, 0, true, 2020, 0};
    case 17: return {false, true, 0, true, 2020, 0};
    case 20: return {false, false, 2, false, 709, 0};
    case 21: return {false, false, 2, true, 2020, 0};
    case 5: return {true, true, 0, false, 601, 0};
    case 6: return {true, false, 0, false, 601, 1};
    case 7: return {true, true, 0, false, 601, 1};
    case 8: return {true, false, 0, false, 709, 1};
    case 9: return {true, true, 0, false, 709, 1};
    case 10: return {true, false, 0, true, 2020, 1};
    case 11: return {true, true, 0, true, 2020, 1};
    case 15: return {true, false, 0, true, 2020, 2};
    case 22: return {true, false, 2, false, 709, 1};
    case 23: return {true, false, 2, true, 2020, 1};
    case 24: return {true, false, 2, true, 2020, 2};
  }
  return {false, true, 0, false, 709, 0};
}
static const VpColorSpace kYuvSpaces[] = {5, 6, 7, 8, 9, 10, 11, 15, 22, 23, 24};
static const VpColorSpace kRgbSpaces[] = {0, 1, 2, 3, 17, 20, 21};

// Published forward (R'G'B' -> Y'CbCr) and inverse coefficients.
static void rgb_to_yuv(int matrix, const double rgb[3], double yuv[3]) {
  const double r = rgb[0], g = rgb[1], b = rgb[2];
  if (matrix == 601) { yuv[0] = 0.299 * r + 0.587 * g + 0.114 * b; yuv[1] = -0.168736 * r - 0.331264 * g + 0.5 * b; yuv[2] = 0.5 * r - 0.418688 * g - 0.081312 * b; }
  else if (matrix == 709) { yuv[0] = 0.2126 * r + 0.7152 * g + 0.0722 * b; yuv[1] = -0.114572 * r - 0.385428 * g + 0.5 * b; yuv[2] = 0.5 * r - 0.454153 * g - 0.045847 * b; }
  else { yuv[0] = 0.2627 * r + 0.6780 * g + 0.0593 * b; yuv[1] = -0.139630 * r - 0.360370 * g + 0.5 * b; yuv[2] = 0.5 * r - 0.459786 * g - 0.040214 * b; }
}
static void yuv_to_rgb(int matrix, const double yuv[3], double rgb[3]) {
  const double y = yuv[0], u = yuv[1], v = yuv[2];
  if (matrix == 601) { rgb[0] = y + 1.402 * v; rgb[1] = y - 0.344136 * u - 0.714136 * v; rgb[2] = y + 1.772 * u; }
  else if (matrix == 709) { rgb[0] = y + 1.5748 * v; rgb[1] = y - 0.187324 * u - 0.468124 * v; rgb[2] = y + 1.8556 * u; }
  else { rgb[0] = y + 1.4746 * v; rgb[1] = y - 0.164553 * u - 0.571353 * v; rgb[2] = y + 1.8814 * u; }
}
static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static double eotf(int t, double v) {
  v = clamp01(v);
  if (t == 1) return v;
  if (t == 2) return pow(v, 2.4);
  return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
static double oetf(int t, double v) {
  v = clamp01(v);
  if (t == 1) return v;
  if (t == 2) return pow(v, 1 / 2.4);
  return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
}
// Linear BT.709 RGB <-> BT.2020 RGB, derived here from the chromaticities and D65 (and checked against BT.2087's published four-decimal matrix in test_reference).
static void npm(bool p2020, double m[3][3]) {
  const double xy709[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}}, xy2020[3][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
  const double (*xy)[2] = p2020 ? xy2020 : xy709;
  double p[3][3];
  for (int c = 0; c < 3; ++c) { p[0][c] = xy[c][0] / xy[c][1]; p[1][c] = 1; p[2][c] = (1 - xy[c][0] - xy[c][1]) / xy[c][1]; }
  const double W[3] = {0.3127 / 0.3290, 1.0, (1 - 0.3127 - 0.3290) / 0.3290};
  // solve p * S = W by Cramer's rule
  auto det3 = [](double a[3][3]) { return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]); };
  const double d = det3(p);
  double S[3];
  for (int c = 0; c < 3; ++c) {
    double q[3][3];
    memcpy(q, p, sizeof q);
    for (int r = 0; r < 3; ++r) q[r][c] = W[r];
    S[c] = det3(q) / d;
  }
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m[r][c] = p[r][c] * S[c];
}
static void inv3(const double a[3][3], double o[3][3]) {
  const double d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
      o[r][c] = (a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]) / d;
    }
}
static void gamut(bool from2020, bool to2020, const double in[3], double out[3]) {
  double a[3][3], b[3][3], bi[3][3];
  npm(from2020, a);
  npm(to2020, b);
  inv3(b, bi);
  for (int r = 0; r < 3; ++r) {
    double xyz = 0;
    out[r] = 0;
    for (int k = 0; k < 3; ++k) {
      xyz = a[k][0] * in[0] + a[k][1] * in[1] + a[k][2] * in[2];
      out[r] += bi[r][k] * xyz;
    }
  }
}

// Nominal value <-> code for channel c (0 Y / R / G / B, 1 chroma, 2 alpha).
static double encode(VpFormat f, const Space& s, int kind, double v) {
  if (kind == 2) return v * code_max(f, 3);
  if (is_float(f)) return s.full ? v : (16 + 219 * v) / 255;
  const int n = depth(f);
  const double mx = (1 << n) - 1, k = 1 << (n - 8);
  if (s.full) return kind == 0 ? v * mx : v * mx + (1 << (n - 1));
  return kind == 0 ? (16 + 219 * v) * k : (128 + 224 * v) * k;
}
static double decode(VpFormat f, const Space& s, int kind, double code) {
  if (kind == 2) return code / code_max(f, 3);
  if (is_float(f)) return s.full ? code : (code * 255 - 16) / 219;
  const int n = depth(f);
  const double mx = (1 << n) - 1, k = 1 << (n - 8);
  if (s.full) return kind == 0 ? code / mx : (code - (1 << (n - 1))) / mx;
  return kind == 0 ? (code / k - 16) / 219 : (code / k - 128) / 224;
}

// Input nominal values (Y'CbCr or R'G'B' of the input's space) to the output's nominal values.
static void ref_convert(const Space& in, const Space& out, const double v[3], double o[3]) {
  double rgb[3];
  if (in.yuv) yuv_to_rgb(in.matrix, v, rgb);
  else for (int i = 0; i < 3; ++i) rgb[i] = v[i];
  if (in.transfer != out.transfer || in.p2020 != out.p2020) {
    double lin[3], lo[3];
    for (int i = 0; i < 3; ++i) lin[i] = eotf(in.transfer, rgb[i]);
    gamut(in.p2020, out.p2020, lin, lo);
    for (int i = 0; i < 3; ++i) rgb[i] = oetf(out.transfer, lo[i]);
  }
  if (out.yuv) rgb_to_yuv(out.matrix, rgb, o);
  else for (int i = 0; i < 3; ++i) o[i] = rgb[i];
}

static double tolerance(VpFormat f) { return is_float(f) ? 2e-3 : 1.0 + (depth(f) == 16 ? 3.0 : 0.0); }   // in codes (half floats: absolute)

static VpResult process(Pic& out, VpRect target, const std::vector<VpStream>& streams, const float background[4] = nullptr, VpAlphaFill fill = VpAlphaFill::Opaque,
                        uint32_t fill_stream = 0, bool bg_yuv = false) {
  VpOutput o;
  o.image = &out.img;
  o.target = target;
  if (background) for (int i = 0; i < 4; ++i) o.background[i] = background[i];
  o.alpha_fill = fill;
  o.alpha_fill_stream = fill_stream;
  o.background_is_yuv = bg_yuv;
  return video_process(o, ArrayView<const VpStream>(streams.data(), streams.size()));
}
static VpStream stream_of(const Pic& in, VpRect src, VpRect dst, VpOrientation o = VpOrientation::Default) {
  VpStream s;
  s.image = &in.img;
  s.source = src;
  s.destination = dst;
  s.orientation = o;
  return s;
}
static VpRect whole(const Pic& p) { return {0, 0, static_cast<int32_t>(p.w), static_cast<int32_t>(p.h)}; }

// ---- every format to every format, in many colour spaces

static const double kPatch[8][3] = {{0.75, 0.75, 0.75}, {0.75, 0.75, 0.0}, {0.0, 0.75, 0.75}, {0.0, 0.75, 0.0}, {0.75, 0.0, 0.75}, {0.75, 0.0, 0.0}, {0.3, 0.6, 0.2}, {0.9, 0.5, 0.05}};

// A 32x16 picture of eight 8x8 patches (R'G'B' in the input's own space), stored as codes; returns the codes' nominal values per patch.
static void fill_patches(Pic& pic, double nominal[8][4]) {
  const Space s = space_of(pic.img.color_space);
  for (int p = 0; p < 8; ++p) {
    const int px = (p % 4) * 8, py = (p / 4) * 8;
    double v[3];
    if (s.yuv) rgb_to_yuv(s.matrix, kPatch[p], v);
    else for (int i = 0; i < 3; ++i) v[i] = kPatch[p][i];
    const double alpha = 0.2 + 0.1 * p;
    for (int y = py; y < py + 8; ++y)
      for (int x = px; x < px + 8; ++x) {
        if (s.yuv) {
          set_code(pic, 0, x, y, 0, encode(pic.format, s, 0, v[0]));
          if (x % 2 == 0 && y % 2 == 0) { set_code(pic, 1, x / 2, y / 2, 0, encode(pic.format, s, 1, v[1])); set_code(pic, 1, x / 2, y / 2, 1, encode(pic.format, s, 1, v[2])); }
        } else {
          for (int c = 0; c < 3; ++c) set_code(pic, 0, x, y, c, encode(pic.format, s, 0, v[c]));
          if (has_alpha(pic.format)) set_code(pic, 0, x, y, 3, encode(pic.format, s, 2, alpha));
        }
      }
    // what was stored
    if (s.yuv) {
      nominal[p][0] = decode(pic.format, s, 0, get_code(pic, 0, px, py, 0));
      nominal[p][1] = decode(pic.format, s, 1, get_code(pic, 1, px / 2, py / 2, 0));
      nominal[p][2] = decode(pic.format, s, 1, get_code(pic, 1, px / 2, py / 2, 1));
      nominal[p][3] = 1;
    } else {
      for (int c = 0; c < 3; ++c) nominal[p][c] = decode(pic.format, s, 0, get_code(pic, 0, px, py, c));
      nominal[p][3] = has_alpha(pic.format) ? decode(pic.format, s, 2, get_code(pic, 0, px, py, 3)) : 1.0;
    }
  }
}

static int g_pairs = 0, g_pair_runs = 0;
static void check_pair(VpFormat fi, VpColorSpace si, VpFormat fo, VpColorSpace so, bool widened) {
  Pic in = make_pic(fi, si, 32, 16, widened);
  Pic out = make_pic(fo, so, 32, 16, !widened);
  double nominal[8][4];
  fill_patches(in, nominal);
  const VpResult r = process(out, whole(out), {stream_of(in, whole(in), whole(out))});
  CHECK(r == VpResult::Ok);
  ++g_pair_runs;
  const Space is = space_of(si), os = space_of(so);
  const double tol = tolerance(fo);
  int bad = 0;
  for (int p = 0; p < 8; ++p) {
    double e[3];
    ref_convert(is, os, nominal[p], e);
    const int px = (p % 4) * 8, py = (p / 4) * 8;
    for (int y = py + 1; y <= py + 6; ++y)
      for (int x = px + 1; x <= px + 6; ++x) {
        if (os.yuv) {
          const double want = encode(fo, os, 0, e[0]), got = get_code(out, 0, x, y, 0);
          if (fabs(clamp01(want / code_max(fo, 0)) * code_max(fo, 0) - got) > tol) ++bad;
        } else {
          for (int c = 0; c < 3; ++c) {
            const double want = clamp01(encode(fo, os, 0, e[c]) / code_max(fo, 0)) * code_max(fo, 0), got = get_code(out, 0, x, y, c);
            if (fabs(want - got) > tol) {
              if (bad == 0) printf("  %s/%u -> %s/%u patch %d c%d: want %.4f got %.4f\n", format_name(fi), si, format_name(fo), so, p, c, want, got);
              ++bad;
            }
          }
          if (has_alpha(fo) && get_code(out, 0, x, y, 3) != code_max(fo, 3)) ++bad;   // (Opaque fill)
        }
      }
    if (os.yuv)
      for (int j = py / 2 + 1; j <= py / 2 + 2; ++j)
        for (int i = px / 2 + 1; i <= px / 2 + 2; ++i)
          for (int c = 0; c < 2; ++c) {
            const double want = clamp01(encode(fo, os, 1, e[1 + c]) / code_max(fo, 0)) * code_max(fo, 0), got = get_code(out, 1, i, j, c);
            if (fabs(want - got) > tol) {
              if (bad == 0) printf("  %s/%u -> %s/%u patch %d chroma%d: want %.4f got %.4f\n", format_name(fi), si, format_name(fo), so, p, c, want, got);
              ++bad;
            }
          }
  }
  CHECK(bad == 0);
}

static void test_format_pairs() {
  int combo = 0;
  for (VpFormat fi : kFormats)
    for (VpFormat fo : kFormats) {
      ++g_pairs;
      const VpColorSpace* ins = is_yuv(fi) ? kYuvSpaces : kRgbSpaces;
      const size_t nin = is_yuv(fi) ? sizeof kYuvSpaces / sizeof *kYuvSpaces : sizeof kRgbSpaces / sizeof *kRgbSpaces;
      const VpColorSpace* outs = is_yuv(fo) ? kYuvSpaces : kRgbSpaces;
      const size_t nout = is_yuv(fo) ? sizeof kYuvSpaces / sizeof *kYuvSpaces : sizeof kRgbSpaces / sizeof *kRgbSpaces;
      for (size_t a = 0; a < nin; ++a)
        for (size_t b = 0; b < nout; ++b) check_pair(fi, ins[a], fo, outs[b], (combo++ & 1) != 0);
    }
}

// The reference itself: its gamut matrix is BT.2087's, and a few well-known conversions.
static void test_reference() {
  const double bt2087[3][3] = {{0.6274, 0.3293, 0.0433}, {0.0691, 0.9195, 0.0114}, {0.0164, 0.0880, 0.8956}};
  for (int c = 0; c < 3; ++c) {
    double in[3] = {0, 0, 0}, out[3];
    in[c] = 1;
    gamut(false, true, in, out);
    for (int r = 0; r < 3; ++r) CHECK(fabs(out[r] - bt2087[r][c]) < 1e-4);
  }
  // BT.709 studio 8-bit: 75% yellow is Y 168, Cb 44, Cr 136 (SMPTE RP 219 bars)
  const double yellow[3] = {0.75, 0.75, 0};
  double yuv[3];
  rgb_to_yuv(709, yellow, yuv);
  const Space s8 = space_of(8);
  CHECK(fabs(encode(VpFormat::Nv12, s8, 0, yuv[0]) - 168) < 0.5 && fabs(encode(VpFormat::Nv12, s8, 1, yuv[1]) - 44) < 0.5 && fabs(encode(VpFormat::Nv12, s8, 1, yuv[2]) - 136) < 0.5);
  // 10-bit studio range is 64..940 / 64..960
  CHECK(encode(VpFormat::P010, s8, 0, 0) == 64 && encode(VpFormat::P010, s8, 0, 1) == 940 && encode(VpFormat::P010, s8, 1, 0.5) == 960);
}

// Specific values through the processor: sRGB 0.5 is 0.2140 linear; RGB studio <-> full.
static void test_known_values() {
  Pic in = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, 2, 2), out = make_pic(VpFormat::Rgba16f, kVpRgbFullG10P709, 2, 2);
  for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x) { for (int c = 0; c < 3; ++c) set_code(in, 0, x, y, c, 128); set_code(in, 0, x, y, 3, 255); }
  CHECK(process(out, whole(out), {stream_of(in, whole(in), whole(out))}) == VpResult::Ok);
  const double lin = pow((128 / 255.0 + 0.055) / 1.055, 2.4);   // 0.2158
  CHECK(fabs(get_code(out, 0, 1, 1, 0) - lin) < 1e-3);
  // and back
  Pic back = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 2, 2);
  CHECK(process(back, whole(back), {stream_of(out, whole(out), whole(back))}) == VpResult::Ok);
  CHECK(get_code(back, 0, 0, 0, 0) == 128 && get_code(back, 0, 0, 0, 2) == 128);
  // full -> studio RGB: 0 -> 16, 255 -> 235
  Pic st = make_pic(VpFormat::Rgba8, kVpRgbStudioG22P709, 2, 2);
  set_code(in, 0, 0, 0, 0, 0); set_code(in, 0, 0, 0, 1, 255);
  CHECK(process(st, whole(st), {stream_of(in, whole(in), whole(st))}) == VpResult::Ok);
  CHECK(get_code(st, 0, 0, 0, 0) == 16 && get_code(st, 0, 0, 0, 1) == 235 && get_code(st, 0, 0, 0, 2) == 126);   // 16 + 219 * 128 / 255 = 125.9
  // P010 values sit in the top bits, P016's fill the word
  Pic p10 = make_pic(VpFormat::P010, kVpYcbcrStudioG22LeftP709, 2, 2), p16 = make_pic(VpFormat::P016, kVpYcbcrStudioG22LeftP709, 2, 2);
  Pic grey = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, 2, 2);
  for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x) for (int c = 0; c < 4; ++c) set_code(grey, 0, x, y, c, 255);
  CHECK(process(p10, whole(p10), {stream_of(grey, whole(grey), whole(p10))}) == VpResult::Ok);
  CHECK(process(p16, whole(p16), {stream_of(grey, whole(grey), whole(p16))}) == VpResult::Ok);
  CHECK(rd16(p10.mem[0].data()) == (940u << 6) && rd16(p10.mem[1].data()) == (512u << 6) && rd16(p10.mem[1].data() + 2) == (512u << 6));
  CHECK(rd16(p16.mem[0].data()) == 235u * 256 && rd16(p16.mem[1].data()) == 128u * 256);
  // RGB10A2: R in bits 0-9 ... A in 30-31
  Pic r10 = make_pic(VpFormat::Rgb10a2, kVpRgbFullG22P709, 2, 2);
  set_code(grey, 0, 0, 0, 0, 0); set_code(grey, 0, 0, 0, 1, 255); set_code(grey, 0, 0, 0, 2, 0);
  CHECK(process(r10, whole(r10), {stream_of(grey, whole(grey), whole(r10))}) == VpResult::Ok);
  const uint32_t word = rd16(r10.mem[0].data()) | rd16(r10.mem[0].data() + 2) << 16;
  CHECK(word == (0x3FFu << 10 | 3u << 30));
}

// ---- ffmpeg (zscale) references

static std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (!f) return bytes;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

static void test_ffmpeg() {
  const std::vector<uint8_t> nv12 = read_file("tests/data/videoprocess/gradient_709.nv12"), bgra = read_file("tests/data/videoprocess/gradient.bgra");
  const std::vector<uint8_t> to709 = read_file("tests/data/videoprocess/nv12_709_to.gbrp"), to2020 = read_file("tests/data/videoprocess/nv12_2020_to.gbrp");
  CHECK(nv12.size() == 64 * 32 * 3 / 2 && bgra.size() == 64 * 32 * 4 && to709.size() == 64 * 32 * 3 && to2020.size() == to709.size());
  if (g_failed) return;
  // NV12 -> BGRA, BT.709 and BT.2020 matrices (the 2020 one as P2020 RGB: no primaries conversion, like zscale's)
  for (int pass = 0; pass < 2; ++pass) {
    Pic in = make_pic(VpFormat::Nv12, pass ? kVpYcbcrStudioG22LeftP2020 : kVpYcbcrStudioG22LeftP709, 64, 32);
    for (uint32_t y = 0; y < 32; ++y) for (uint32_t x = 0; x < 64; ++x) set_code(in, 0, x, y, 0, nv12[y * 64 + x]);
    for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 32; ++x) for (int c = 0; c < 2; ++c) set_code(in, 1, x, y, c, nv12[64 * 32 + y * 64 + 2 * x + c]);
    Pic out = make_pic(VpFormat::Bgra8, pass ? kVpRgbFullG22P2020 : kVpRgbFullG22P709, 64, 32);
    CHECK(process(out, whole(out), {stream_of(in, whole(in), whole(out))}) == VpResult::Ok);
    const std::vector<uint8_t>& ref = pass ? to2020 : to709;
    int worst = 0;
    double sum = 0;
    for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 64; ++x)
        for (int c = 0; c < 3; ++c) {
          const int plane = c == 0 ? 2 : c == 1 ? 0 : 1;   // (planar G, B, R)
          const int d = abs(static_cast<int>(get_code(out, 0, x, y, c)) - ref[plane * 64 * 32 + y * 64 + x]);
          worst = d > worst ? d : worst;
          sum += d;
        }
    printf("  ffmpeg zscale NV12 BT.%s -> BGRA: worst difference %d, mean %.3f\n", pass ? "2020" : "709", worst, sum / (64 * 32 * 3));
    CHECK(worst <= 1);   // (zimg computes in float too: the same model, rounded in another order)
  }
  // BGRA -> NV12 (BT.709 studio): luma exactly per pixel, chroma by the LEFT-sited filter
  Pic in = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 64, 32);
  for (uint32_t y = 0; y < 32; ++y) for (uint32_t x = 0; x < 64; ++x) for (int c = 0; c < 4; ++c) set_code(in, 0, x, y, c, bgra[(y * 64 + x) * 4 + (c == 3 ? 3 : 2 - c)]);
  Pic out = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 64, 32, true);
  CHECK(process(out, whole(out), {stream_of(in, whole(in), whole(out))}) == VpResult::Ok);
  int worst_y = 0, worst_c = 0;
  for (uint32_t y = 0; y < 32; ++y) for (uint32_t x = 0; x < 64; ++x) worst_y = fmax(worst_y, fabs(get_code(out, 0, x, y, 0) - nv12[y * 64 + x]));
  for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 32; ++x) for (int c = 0; c < 2; ++c) worst_c = fmax(worst_c, fabs(get_code(out, 1, x, y, c) - nv12[64 * 32 + y * 64 + 2 * x + c]));
  printf("  ffmpeg zscale BGRA -> NV12 BT.709: worst luma difference %d, chroma %d\n", worst_y, worst_c);
  CHECK(worst_y <= 1 && worst_c <= 2);
}

// ---- chroma siting: upsampling and downsampling at each siting

static void test_siting() {
  // upsampling: constant luma, chroma ramps (Cb across, Cr down); the RGB output's pixels show the chroma interpolated at the siting
  for (VpColorSpace space : {kVpYcbcrFullG22P709X601, kVpYcbcrFullG22LeftP709, kVpYcbcrStudioG22TopLeftP2020}) {
    const Space s = space_of(space);
    Pic in = make_pic(VpFormat::P016, space, 12, 8);
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 12; ++x) set_code(in, 0, x, y, 0, encode(in.format, s, 0, 0.5));
    for (uint32_t j = 0; j < 4; ++j)
      for (uint32_t i = 0; i < 6; ++i) { set_code(in, 1, i, j, 0, encode(in.format, s, 1, -0.2 + 0.06 * i)); set_code(in, 1, i, j, 1, encode(in.format, s, 1, -0.15 + 0.08 * j)); }
    Pic out = make_pic(VpFormat::Rgba16f, s.p2020 ? kVpRgbFullG22P2020 : kVpRgbFullG22P709, 12, 8);
    CHECK(process(out, whole(out), {stream_of(in, whole(in), whole(out))}) == VpResult::Ok);
    const double ox = s.siting == 0 ? 0.5 : 0.0, oy = s.siting == 2 ? 0.0 : 0.5;
    int bad = 0;
    for (uint32_t y = 0; y < 8; ++y)
      for (uint32_t x = 0; x < 12; ++x) {
        const double ci = fmin(fmax((x - ox) / 2, 0), 5), cj = fmin(fmax((y - oy) / 2, 0), 3);
        const double v[3] = {decode(in.format, s, 0, get_code(in, 0, x, y, 0)), decode(in.format, s, 1, encode(in.format, s, 1, -0.2 + 0.06 * ci)),
                             decode(in.format, s, 1, encode(in.format, s, 1, -0.15 + 0.08 * cj))};
        double e[3];
        ref_convert(s, space_of(out.img.color_space), v, e);
        for (int c = 0; c < 3; ++c) if (fabs(get_code(out, 0, x, y, c) - clamp01(e[c])) > 2e-3) ++bad;
      }
    CHECK(bad == 0);
  }
  // downsampling: an RGB picture whose chroma changes per column / row; each output chroma sample is the filter at its siting over the pixels' chroma
  for (VpColorSpace space : {kVpYcbcrFullG22P709X601, kVpYcbcrFullG22LeftP709, kVpYcbcrStudioG22TopLeftP2020}) {
    const Space s = space_of(space);
    const uint32_t W = 11, H = 7;   // (odd: the last chroma sample has one column / row)
    Pic in = make_pic(VpFormat::Rgba16f, s.p2020 ? kVpRgbFullG22P2020 : kVpRgbFullG22P709, W, H);
    double cb[H][W], cr[H][W], yy[H][W];
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        const double rgb[3] = {0.2 + 0.05 * x, 0.5, 0.3 + 0.07 * y};
        for (int c = 0; c < 3; ++c) set_code(in, 0, x, y, c, rgb[c]);
        set_code(in, 0, x, y, 3, 1);
        double st[3], yuv[3];
        for (int c = 0; c < 3; ++c) st[c] = get_code(in, 0, x, y, c);
        rgb_to_yuv(s.matrix, st, yuv);
        yy[y][x] = yuv[0]; cb[y][x] = yuv[1]; cr[y][x] = yuv[2];
      }
    Pic out = make_pic(VpFormat::P016, space, W, H, true);
    CHECK(process(out, whole(out), {stream_of(in, whole(in), whole(out))}) == VpResult::Ok);
    int bad = 0;
    auto at = [&](double (*m)[W], int x, int y) { x = x < 0 ? 0 : x >= int(W) ? W - 1 : x; y = y < 0 ? 0 : y >= int(H) ? H - 1 : y; return m[y][x]; };
    for (int j = 0; j < int(H + 1) / 2; ++j)
      for (int i = 0; i < int(W + 1) / 2; ++i) {
        double e[2] = {0, 0};
        const double h3[3] = {0.25, 0.5, 0.25}, h2[2] = {0.5, 0.5};
        const int nh = s.siting == 0 ? 2 : 3, nv = s.siting == 2 ? 3 : 2;
        for (int a = 0; a < nv; ++a)
          for (int b = 0; b < nh; ++b) {
            const int x = nh == 2 ? 2 * i + b : 2 * i - 1 + b, y = nv == 2 ? 2 * j + a : 2 * j - 1 + a;
            const double w = (nv == 2 ? h2[a] : h3[a]) * (nh == 2 ? h2[b] : h3[b]);
            e[0] += w * at(cb, x, y);
            e[1] += w * at(cr, x, y);
          }
        for (int c = 0; c < 2; ++c) if (fabs(get_code(out, 1, i, j, c) - encode(out.format, s, 1, e[c])) > 3) ++bad;
      }
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) if (fabs(get_code(out, 0, x, y, 0) - encode(out.format, s, 0, yy[y][x])) > 3) ++bad;
    CHECK(bad == 0);
  }
}

// ---- scaling

static double ref_bilinear(const std::vector<double>& img, int w, VpRect src, VpRect dst, int x, int y) {
  const double sw = src.right - src.left, sh = src.bottom - src.top, dw = dst.right - dst.left, dh = dst.bottom - dst.top;
  double sx = src.left + (x - dst.left + 0.5) * sw / dw - 0.5, sy = src.top + (y - dst.top + 0.5) * sh / dh - 0.5;
  sx = fmin(fmax(sx, src.left), src.right - 1);
  sy = fmin(fmax(sy, src.top), src.bottom - 1);
  const int x0 = static_cast<int>(floor(sx)), y0 = static_cast<int>(floor(sy));
  const int x1 = x0 + 1 < src.right ? x0 + 1 : x0, y1 = y0 + 1 < src.bottom ? y0 + 1 : y0;
  const double fx = sx - x0, fy = sy - y0;
  auto p = [&](int a, int b) { return img[b * w + a]; };
  return (p(x0, y0) * (1 - fx) + p(x1, y0) * fx) * (1 - fy) + (p(x0, y1) * (1 - fx) + p(x1, y1) * fx) * fy;
}

static void test_scaling() {
  const int W = 9, H = 7;
  Pic in = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, W, H);
  std::vector<double> ch[4];
  for (auto& v : ch) v.resize(W * H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      for (int c = 0; c < 4; ++c) {
        const double v = (x * 37 + y * 91 + c * 53 + (x * y) % 7 * 13) % 256;
        set_code(in, 0, x, y, c, v);
        ch[c][y * W + x] = v;
      }
  struct Case { VpRect src; int ow, oh; VpRect dst; } cases[] = {
      {{0, 0, W, H}, 23, 17, {0, 0, 23, 17}},   // up
      {{0, 0, W, H}, 4, 3, {0, 0, 4, 3}},       // down
      {{2, 1, 8, 6}, 13, 11, {1, 2, 12, 9}},    // a part, to a part
      {{1, 1, 2, 2}, 5, 5, {0, 0, 5, 5}},       // one pixel, everywhere
      {{0, 0, W, H}, 30, 3, {0, 0, 30, 3}},     // up across, down down
  };
  for (const Case& k : cases) {
    Pic out = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, k.ow, k.oh);
    CHECK(process(out, whole(out), {stream_of(in, k.src, k.dst)}) == VpResult::Ok);
    int bad = 0;
    for (int y = k.dst.top; y < k.dst.bottom; ++y)
      for (int x = k.dst.left; x < k.dst.right; ++x)
        for (int c = 0; c < 3; ++c)
          if (fabs(get_code(out, 0, x, y, c) - ref_bilinear(ch[c], W, k.src, k.dst, x, y)) > 0.51) ++bad;
    CHECK(bad == 0);
  }
  // YUV scaling: NV12 -> NV12 in the same space carries luma through the bilinear filter alone
  Pic yin = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 10, 6);
  std::vector<double> luma(60);
  for (int y = 0; y < 6; ++y) for (int x = 0; x < 10; ++x) { luma[y * 10 + x] = 16 + (x * 23 + y * 41) % 220; set_code(yin, 0, x, y, 0, luma[y * 10 + x]); }
  for (int j = 0; j < 3; ++j) for (int i = 0; i < 5; ++i) { set_code(yin, 1, i, j, 0, 128); set_code(yin, 1, i, j, 1, 128); }
  Pic yout = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 17, 5, true);
  CHECK(process(yout, whole(yout), {stream_of(yin, whole(yin), whole(yout))}) == VpResult::Ok);
  int bad = 0;
  for (int y = 0; y < 5; ++y) for (int x = 0; x < 17; ++x) if (fabs(get_code(yout, 0, x, y, 0) - ref_bilinear(luma, 10, whole(yin), whole(yout), x, y)) > 0.51) ++bad;
  for (int j = 0; j < 3; ++j) for (int i = 0; i < 9; ++i) if (get_code(yout, 1, i, j, 0) != 128 || get_code(yout, 1, i, j, 1) != 128) ++bad;
  CHECK(bad == 0);
}

// ---- orientations: pixel exact on a 3x2 picture

typedef std::vector<std::vector<int>> Grid;   // [y][x]
static Grid rotate_cw(const Grid& g) {
  const size_t h = g.size(), w = g[0].size();
  Grid r(w, std::vector<int>(h));
  for (size_t y = 0; y < h; ++y) for (size_t x = 0; x < w; ++x) r[x][h - 1 - y] = g[y][x];
  return r;
}
static Grid flip_h(const Grid& g) {
  Grid r = g;
  for (auto& row : r) for (size_t x = 0; x < row.size() / 2; ++x) { const int t = row[x]; row[x] = row[row.size() - 1 - x]; row[row.size() - 1 - x] = t; }
  return r;
}
static Grid flip_v(const Grid& g) { return Grid(g.rbegin(), g.rend()); }

static void test_orientations() {
  const int W = 3, H = 2;
  Grid g(H, std::vector<int>(W));
  Pic in = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, W, H);
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { g[y][x] = 10 + 30 * (y * W + x); for (int c = 0; c < 4; ++c) set_code(in, 0, x, y, c, g[y][x] + c); }
  const Grid expected[8] = {g, flip_h(g), rotate_cw(g), flip_h(rotate_cw(g)), rotate_cw(rotate_cw(g)), flip_v(g), rotate_cw(rotate_cw(rotate_cw(g))), flip_h(rotate_cw(rotate_cw(rotate_cw(g))))};
  for (int o = 0; o < 8; ++o) {
    const Grid& e = expected[o];
    const int ow = static_cast<int>(e[0].size()), oh = static_cast<int>(e.size());
    Pic out = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, ow + 2, oh + 1);   // (drawn at an offset)
    CHECK(process(out, whole(out), {stream_of(in, whole(in), {1, 1, ow + 1, oh + 1}, static_cast<VpOrientation>(o))}) == VpResult::Ok);
    int bad = 0;
    for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) for (int c = 0; c < 3; ++c) if (get_code(out, 0, x + 1, y + 1, c) != e[y][x] + c) ++bad;
    if (bad) printf("  orientation %d wrong\n", o);
    CHECK(bad == 0);
  }
  // rotated and scaled at once: a 90 degree turn of a 4x2 to 2x4 at twice the size
  Pic in2 = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, 4, 2);
  for (int y = 0; y < 2; ++y) for (int x = 0; x < 4; ++x) for (int c = 0; c < 4; ++c) set_code(in2, 0, x, y, c, 20 * (y * 4 + x) + 5);
  Pic out2 = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, 4, 8);
  CHECK(process(out2, whole(out2), {stream_of(in2, whole(in2), whole(out2), VpOrientation::Clockwise90)}) == VpResult::Ok);
  // output (x, y) shows source (y / 2, 1 - x / 2) at the pixel centres away from the edges of the 2x upscale: check the corners, which clamp
  CHECK(get_code(out2, 0, 3, 0, 0) == 5 && get_code(out2, 0, 0, 0, 0) == 85 && get_code(out2, 0, 0, 7, 0) == 145 && get_code(out2, 0, 3, 7, 0) == 65);
}

// ---- clipping, background, alpha

static void test_clipping_and_background() {
  const float bg[4] = {0.25f, 0.5f, 0.75f, 0.5f};
  Pic in = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, 8, 8);
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) for (int c = 0; c < 4; ++c) set_code(in, 0, x, y, c, 1 + x * 10 + y * 20 + c);
  Pic out = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, 8, 8);
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) for (int c = 0; c < 4; ++c) set_code(out, 0, x, y, c, 7);
  // target 2..6 (and partly outside the output on the right in a second run); the stream's destination -3..5 is partly outside both
  CHECK(process(out, {2, 2, 6, 6}, {stream_of(in, whole(in), {-3, -3, 5, 5})}, bg) == VpResult::Ok);
  int bad = 0;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      const bool target = x >= 2 && x < 6 && y >= 2 && y < 6, drawn = target && x < 5 && y < 5;
      for (int c = 0; c < 4; ++c) {
        double want = 7;
        if (drawn) want = c == 3 ? 255 : 1 + (x + 3) * 10 + (y + 3) * 20 + c;
        else if (target) want = c == 3 ? 255 : floor(bg[c] * 255 + 0.5);
        if (get_code(out, 0, x, y, c) != want) ++bad;
      }
    }
  CHECK(bad == 0);
  // a target past the output's edge is clipped to it
  CHECK(process(out, {6, -4, 100, 100}, {stream_of(in, whole(in), whole(in))}, bg) == VpResult::Ok);
  CHECK(get_code(out, 0, 7, 7, 0) == 1 + 70 + 140 && get_code(out, 0, 6, 0, 1) == 1 + 60 + 1 && get_code(out, 0, 5, 7, 0) == 7);
  // a stream wholly outside the target leaves the background
  CHECK(process(out, {0, 0, 2, 2}, {stream_of(in, whole(in), {4, 4, 8, 8})}, bg) == VpResult::Ok);
  CHECK(get_code(out, 0, 1, 1, 2) == 191);
  // no streams: background alone; a YCbCr background on an RGB output (BT.709 matrix): Y 0.5, Cb Cr neutral is grey 0.5
  const float ybg[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  CHECK(process(out, whole(out), {}, ybg, VpAlphaFill::Opaque, 0, true) == VpResult::Ok);
  CHECK(get_code(out, 0, 3, 3, 0) == 128 && get_code(out, 0, 3, 3, 1) == 128 && get_code(out, 0, 3, 3, 2) == 128);
  // an RGB background on a YCbCr output, and a YCbCr one
  Pic yout = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 6, 4);
  CHECK(process(yout, whole(yout), {}, bg) == VpResult::Ok);
  {
    const double rgb[3] = {bg[0], bg[1], bg[2]};
    double yuv[3];
    rgb_to_yuv(709, rgb, yuv);
    const Space s = space_of(kVpYcbcrStudioG22LeftP709);
    CHECK(fabs(get_code(yout, 0, 5, 3, 0) - encode(VpFormat::Nv12, s, 0, yuv[0])) <= 0.51 && fabs(get_code(yout, 1, 2, 1, 0) - encode(VpFormat::Nv12, s, 1, yuv[1])) <= 0.51 &&
          fabs(get_code(yout, 1, 2, 1, 1) - encode(VpFormat::Nv12, s, 1, yuv[2])) <= 0.51);
  }
  const float ybg2[4] = {0.25f, 0.75f, 0.5f, 1.0f};
  CHECK(process(yout, whole(yout), {}, ybg2, VpAlphaFill::Opaque, 0, true) == VpResult::Ok);
  CHECK(get_code(yout, 0, 0, 0, 0) == 71 && get_code(yout, 1, 0, 0, 0) == 184 && get_code(yout, 1, 0, 0, 1) == 128);   // 16 + 219/4 = 70.75; 128 + 224/4 = 184
}

static void test_alpha() {
  const float bg[4] = {0.0f, 0.0f, 0.0f, 0.25f};
  Pic in = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, 4, 4);
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) { set_code(in, 0, x, y, 0, 200); set_code(in, 0, x, y, 1, 100); set_code(in, 0, x, y, 2, 40); set_code(in, 0, x, y, 3, 51 * x); }
  Pic noalpha = make_pic(VpFormat::Bgrx8, kVpRgbFullG22P709, 4, 4);
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) for (int c = 0; c < 3; ++c) set_code(noalpha, 0, x, y, c, 90);
  auto fresh = [] {
    Pic out = make_pic(VpFormat::Rgba8, kVpRgbFullG22P709, 6, 4);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 6; ++x) for (int c = 0; c < 4; ++c) set_code(out, 0, x, y, c, c == 3 ? 33 : 10);
    return out;
  };
  // blending: stream alpha 0.5 times the pixel's alpha, over the background
  {
    Pic out = fresh();
    VpStream s = stream_of(in, whole(in), {0, 0, 4, 4});
    s.alpha_blend = true;
    s.alpha = 0.5f;
    CHECK(process(out, whole(out), {s}, bg) == VpResult::Ok);
    int bad = 0;
    for (int x = 0; x < 4; ++x) {
      const double a = 0.5 * (51 * x) / 255.0;
      if (fabs(get_code(out, 0, x, 2, 0) - 200 * a) > 0.51 || fabs(get_code(out, 0, x, 2, 1) - 100 * a) > 0.51 || get_code(out, 0, x, 2, 3) != 255) ++bad;
    }
    CHECK(bad == 0);
    // without blending, the pixels replace what is under them whatever their alpha
    s.alpha_blend = false;
    CHECK(process(out, whole(out), {s}, bg) == VpResult::Ok);
    CHECK(get_code(out, 0, 0, 0, 0) == 200 && get_code(out, 0, 1, 1, 2) == 40);
  }
  // the four alpha fill modes
  {
    Pic out = fresh();
    CHECK(process(out, {0, 0, 5, 4}, {stream_of(in, whole(in), {0, 0, 4, 4})}, bg, VpAlphaFill::Opaque) == VpResult::Ok);
    CHECK(get_code(out, 0, 2, 2, 3) == 255 && get_code(out, 0, 4, 2, 3) == 255 && get_code(out, 0, 5, 2, 3) == 33);
    CHECK(process(out, {0, 0, 5, 4}, {stream_of(in, whole(in), {0, 0, 4, 4})}, bg, VpAlphaFill::Background) == VpResult::Ok);
    CHECK(get_code(out, 0, 2, 2, 3) == 64 && get_code(out, 0, 4, 2, 3) == 64 && get_code(out, 0, 5, 2, 3) == 33);
  }
  {
    Pic out = fresh();
    CHECK(process(out, {0, 0, 5, 4}, {stream_of(in, whole(in), {0, 0, 4, 4})}, bg, VpAlphaFill::Destination) == VpResult::Ok);
    CHECK(get_code(out, 0, 2, 2, 3) == 33 && get_code(out, 0, 4, 2, 3) == 33 && get_code(out, 0, 2, 2, 0) == 200);
  }
  {
    Pic out = fresh();
    // stream 1's alpha (the per-pixel ramp) inside its destination, the background's elsewhere in the target
    CHECK(process(out, {0, 0, 6, 4}, {stream_of(noalpha, whole(noalpha), whole(noalpha)), stream_of(in, whole(in), {1, 0, 5, 4})}, bg, VpAlphaFill::SourceStream, 1) == VpResult::Ok);
    CHECK(get_code(out, 0, 0, 1, 3) == 64 && get_code(out, 0, 1, 1, 3) == 0 && get_code(out, 0, 4, 1, 3) == 153 && get_code(out, 0, 5, 1, 3) == 64);
    // a stream without alpha: opaque
    CHECK(process(out, {0, 0, 6, 4}, {stream_of(noalpha, whole(noalpha), whole(noalpha))}, bg, VpAlphaFill::SourceStream, 0) == VpResult::Ok);
    CHECK(get_code(out, 0, 0, 1, 3) == 255 && get_code(out, 0, 5, 1, 3) == 255);
    // an empty source rectangle: alpha left as it was
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 6; ++x) set_code(out, 0, x, y, 3, 33);
    CHECK(process(out, {0, 0, 6, 4}, {stream_of(in, {1, 1, 1, 3}, whole(in))}, bg, VpAlphaFill::SourceStream, 0) == VpResult::Ok);
    CHECK(get_code(out, 0, 2, 2, 3) == 33 && get_code(out, 0, 2, 2, 0) == 0);
  }
  // two streams: the second blended over the first
  {
    Pic out = fresh();
    VpStream top = stream_of(in, whole(in), {2, 0, 6, 4});
    top.alpha_blend = true;
    CHECK(process(out, whole(out), {stream_of(noalpha, whole(noalpha), {0, 0, 4, 4}), top}, bg) == VpResult::Ok);
    int bad = 0;
    for (int x = 0; x < 6; ++x) {
      double want;
      if (x < 2) want = 90;
      else {
        const double a = 51 * (x - 2) / 255.0, under = x < 4 ? 90 : 0;
        want = 200 * a + under * (1 - a);
      }
      if (fabs(get_code(out, 0, x, 3, 0) - want) > 0.51) ++bad;
    }
    CHECK(bad == 0);
  }
  // RGB10A2 and RGBA16F alpha
  {
    Pic o10 = make_pic(VpFormat::Rgb10a2, kVpRgbFullG22P709, 4, 4), o16 = make_pic(VpFormat::Rgba16f, kVpRgbFullG22P709, 4, 4);
    CHECK(process(o10, whole(o10), {stream_of(in, whole(in), whole(in))}, bg, VpAlphaFill::SourceStream, 0) == VpResult::Ok);
    CHECK(process(o16, whole(o16), {stream_of(in, whole(in), whole(in))}, bg, VpAlphaFill::SourceStream, 0) == VpResult::Ok);
    CHECK(get_code(o10, 0, 0, 0, 3) == 0 && get_code(o10, 0, 3, 0, 3) == 2 && get_code(o10, 0, 2, 0, 0) == floor(200 / 255.0 * 1023 + 0.5));   // 153/255*3 = 1.8
    CHECK(fabs(get_code(o16, 0, 1, 0, 3) - 0.2) < 1e-3);
  }
}

// ---- YUV outputs: a target with odd edges; widened and plain planes; odd sizes

static void test_yuv_targets() {
  for (bool widened : {false, true}) {
    const Space s = space_of(kVpYcbcrStudioG22LeftP709);
    Pic out = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 9, 7, widened);
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 9; ++x) set_code(out, 0, x, y, 0, 50);
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 5; ++i) { set_code(out, 1, i, j, 0, 100); set_code(out, 1, i, j, 1, 150); }
    // a white background in 3..6 x 1..4 (odd left and top)
    const float white[4] = {1, 1, 1, 1};
    CHECK(process(out, {3, 1, 6, 4}, {}, white) == VpResult::Ok);
    int bad = 0;
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 9; ++x) {
      const bool in = x >= 3 && x < 6 && y >= 1 && y < 4;
      if (get_code(out, 0, x, y, 0) != (in ? 235 : 50)) ++bad;
    }
    // chroma samples the target does not reach are untouched; those it reaches are the LEFT filter over new (128) and old pixels' chroma
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 5; ++i) {
      const bool touched = i >= 1 && i <= 2 && j >= 0 && j <= 1;
      if (!touched) { if (get_code(out, 1, i, j, 0) != 100 || get_code(out, 1, i, j, 1) != 150) ++bad; continue; }
      // which pixels of the filter are in the target
      double cb = 0, cr = 0;
      for (int a = 0; a < 2; ++a) for (int b = 0; b < 3; ++b) {
        const int x = 2 * i - 1 + b, y = 2 * j + a;
        const bool in = x >= 3 && x < 6 && y >= 1 && y < 4;
        const double w = 0.5 * (b == 1 ? 0.5 : 0.25);
        cb += w * (in ? 0.0 : decode(VpFormat::Nv12, s, 1, 100));
        cr += w * (in ? 0.0 : decode(VpFormat::Nv12, s, 1, 150));
      }
      if (fabs(get_code(out, 1, i, j, 0) - encode(VpFormat::Nv12, s, 1, cb)) > 0.51 || fabs(get_code(out, 1, i, j, 1) - encode(VpFormat::Nv12, s, 1, cr)) > 0.51) ++bad;
    }
    CHECK(bad == 0);
    // the widened planes' other bytes are left alone
    if (widened) CHECK(out.mem[0][1] == 0xA5 && out.mem[0][3] == 0xA5 && out.mem[1][2] == 0xA5 && out.mem[1][3] == 0xA5);
  }
  // odd sizes, every YUV format both ways, a flat colour: every sample is exact
  for (VpFormat f : {VpFormat::Nv12, VpFormat::P010, VpFormat::P016})
    for (bool widened : {false, true}) {
      const Space s = space_of(kVpYcbcrStudioG22LeftP2020);
      Pic in = make_pic(VpFormat::Rgba8, kVpRgbFullG22P2020, 5, 3);
      for (int y = 0; y < 3; ++y) for (int x = 0; x < 5; ++x) { set_code(in, 0, x, y, 0, 180); set_code(in, 0, x, y, 1, 60); set_code(in, 0, x, y, 2, 220); set_code(in, 0, x, y, 3, 255); }
      Pic out = make_pic(f, kVpYcbcrStudioG22LeftP2020, 5, 3, widened);
      CHECK(process(out, whole(out), {stream_of(in, whole(in), whole(out))}) == VpResult::Ok);
      const double rgb[3] = {180 / 255.0, 60 / 255.0, 220 / 255.0};
      double yuv[3];
      rgb_to_yuv(2020, rgb, yuv);
      int bad = 0;
      for (int y = 0; y < 3; ++y) for (int x = 0; x < 5; ++x) if (fabs(get_code(out, 0, x, y, 0) - encode(f, s, 0, yuv[0])) > tolerance(f)) ++bad;
      for (int j = 0; j < 2; ++j) for (int i = 0; i < 3; ++i) for (int c = 0; c < 2; ++c) if (fabs(get_code(out, 1, i, j, c) - encode(f, s, 1, yuv[1 + c])) > tolerance(f)) ++bad;
      // and back to RGB from the odd-sized YUV picture
      Pic back = make_pic(VpFormat::Rgba8, kVpRgbFullG22P2020, 5, 3);
      CHECK(process(back, whole(back), {stream_of(out, whole(out), whole(back))}) == VpResult::Ok);
      for (int y = 0; y < 3; ++y) for (int x = 0; x < 5; ++x) if (fabs(get_code(back, 0, x, y, 0) - 180) > 1 || fabs(get_code(back, 0, x, y, 2) - 220) > 1) ++bad;
      CHECK(bad == 0);
    }
}

// ---- refusals

static void test_refusals() {
  Pic in = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 8, 8), out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 8, 8);
  auto run = [&](const VpStream& s, VpRect target = {0, 0, 8, 8}) { return process(out, target, {s}); };
  const VpStream good = stream_of(in, whole(in), whole(out));
  CHECK(run(good) == VpResult::Ok);
  VpStream s = good;
  s.source = {0, 0, 9, 8}; CHECK(run(s) == VpResult::Invalid);           // outside the image
  s.source = {-1, 0, 4, 4}; CHECK(run(s) == VpResult::Invalid);
  s.source = {4, 0, 2, 4}; CHECK(run(s) == VpResult::Invalid);           // inverted
  s = good; s.destination = {5, 5, 2, 8}; CHECK(run(s) == VpResult::Invalid);
  CHECK(run(good, {4, 4, 2, 2}) == VpResult::Invalid);
  s = good; s.alpha = 1.5f; CHECK(run(s) == VpResult::Invalid);
  s.alpha = __builtin_nanf(""); CHECK(run(s) == VpResult::Invalid);
  s = good; s.image = nullptr; CHECK(run(s) == VpResult::Invalid);
  s = good; s.orientation = static_cast<VpOrientation>(8); CHECK(run(s) == VpResult::Invalid);
  // empty rectangles leave the stream out
  s = good; s.destination = {3, 3, 3, 6}; CHECK(run(s) == VpResult::Ok);
  CHECK(process(out, whole(out), {good}, nullptr, VpAlphaFill::SourceStream, 1) == VpResult::Invalid);
  const float badbg[4] = {0, 2, 0, 1};
  CHECK(process(out, whole(out), {good}, badbg) == VpResult::Invalid);
  VpOutput none;
  CHECK(video_process(none, ArrayView<const VpStream>()) == VpResult::Invalid);
  // planes
  auto with = [&](auto mutate) { Pic p = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 8, 8); mutate(p.img); VpStream t = good; t.image = &p.img; return run(t); };
  CHECK(with([](VpImage& i) { i.plane[0].memory = i.plane[0].memory.first(7 * i.plane[0].pitch + 7); }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.plane[1].memory = i.plane[1].memory.first(10); }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.plane[1].width = 3; }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.plane[1].channels = 1; }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.plane[0].bits = 16; }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.plane[0].step = 0; }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.plane[1].offset[1] = 2; }) == VpResult::Invalid);   // past the 2-byte sample
  CHECK(with([](VpImage& i) { i.plane[0].pitch = 4; }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.width = 0; }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.format = static_cast<VpFormat>(9); }) == VpResult::Invalid);
  CHECK(with([](VpImage& i) { i.plane[0].half_float = true; }) == VpResult::Invalid);
  // colour spaces
  for (VpColorSpace bad : {4u, 25u, 0xFFFFFFFFu}) {   // (12..19 without 15 and 17: PQ and HLG, see test_hdr)
    CHECK(with([bad](VpImage& i) { i.color_space = bad; }) == VpResult::Unsupported);
    CHECK(!vp_color_space_supported(VpFormat::Nv12, bad) && !vp_color_space_supported(VpFormat::Rgba16f, bad));
  }
  CHECK(with([](VpImage& i) { i.color_space = kVpRgbFullG22P709; }) == VpResult::Unsupported);   // RGB space, YUV format
  out.img.color_space = kVpYcbcrStudioG22LeftP709;
  CHECK(run(good) == VpResult::Unsupported);
  out.img.color_space = kVpRgbFullG22P709;
  for (VpColorSpace ok : kYuvSpaces) CHECK(vp_color_space_supported(VpFormat::P010, ok) && !vp_color_space_supported(VpFormat::Bgra8, ok));
  for (VpColorSpace ok : kRgbSpaces) CHECK(vp_color_space_supported(VpFormat::Rgb10a2, ok) && !vp_color_space_supported(VpFormat::Nv12, ok));
  // too many streams
  std::vector<VpStream> many(kVpMaxStreams + 1, good);
  CHECK(process(out, whole(out), many) == VpResult::Unsupported);
  many.resize(kVpMaxStreams);
  CHECK(process(out, whole(out), many) == VpResult::Ok);
}

// ---- garbage: random descriptions and rectangles must not fault

static uint32_t g_seed = 12345;
static uint32_t rnd() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }

static void test_garbage() {
  int ok = 0;
  for (int iter = 0; iter < 4000; ++iter) {
    const VpFormat fi = kFormats[rnd() % 8], fo = kFormats[rnd() % 8];
    const uint32_t w = 1 + rnd() % 9, h = 1 + rnd() % 9;
    Pic in = make_pic(fi, is_yuv(fi) ? kYuvSpaces[rnd() % 11] : kRgbSpaces[rnd() % 7], w, h, rnd() & 1);
    Pic out = make_pic(fo, is_yuv(fo) ? kYuvSpaces[rnd() % 11] : kRgbSpaces[rnd() % 7], 1 + rnd() % 9, 1 + rnd() % 9, rnd() & 1);
    for (auto& m : in.mem) for (auto& b : m) b = static_cast<uint8_t>(rnd());   // (half floats: NaN and infinity included)
    // flip bits in the description
    const int flips = static_cast<int>(rnd() % 3);
    for (int k = 0; k < flips; ++k) {
      VpImage& img = (rnd() & 1) ? in.img : out.img;
      VpPlane& p = img.plane[rnd() & 1];
      switch (rnd() % 12) {
        case 0: p.pitch ^= 1u << (rnd() % 32); break;
        case 1: p.step ^= 1u << (rnd() % 32); break;
        case 2: p.width ^= 1u << (rnd() % 32); break;
        case 3: p.height ^= 1u << (rnd() % 32); break;
        case 4: p.offset[rnd() % 4] ^= static_cast<uint8_t>(1u << (rnd() % 8)); break;
        case 5: p.bits ^= static_cast<uint8_t>(1u << (rnd() % 8)); break;
        case 6: p.channels ^= static_cast<uint8_t>(1u << (rnd() % 8)); break;
        case 7: p.memory = p.memory.first(p.memory.size() / 2); break;
        case 8: img.width ^= 1u << (rnd() % 32); break;
        case 9: img.height ^= 1u << (rnd() % 32); break;
        case 10: img.color_space ^= 1u << (rnd() % 5); break;
        default: break;
      }
    }
    auto rect = [] { const int32_t a = static_cast<int32_t>(rnd() % 24) - 6, b = static_cast<int32_t>(rnd() % 24) - 6; return VpRect{a, b, a + static_cast<int32_t>(rnd() % 16) - 2, b + static_cast<int32_t>(rnd() % 16) - 2}; };
    VpStream s = stream_of(in, rect(), rect(), static_cast<VpOrientation>(rnd() % 8));
    if (rnd() % 3 == 0) s.source = whole(in);
    s.alpha_blend = rnd() & 1;
    s.alpha = (rnd() % 11) / 10.0f;
    float bg[4] = {(rnd() % 5) / 4.0f, (rnd() % 5) / 4.0f, (rnd() % 5) / 4.0f, (rnd() % 5) / 4.0f};
    const VpResult r = process(out, rect(), {s, s}, bg, static_cast<VpAlphaFill>(rnd() % 4), rnd() % 3, rnd() & 1);
    if (r == VpResult::Ok) ++ok;
  }
  printf("  garbage: %d of 4000 accepted, none faulted\n", ok);
  CHECK(ok > 100);
}

// ==== the stages and effects: deinterlacing, stereo views, filters, luma key, ProcAmp, HDR

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al + al);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static TestAllocator g_alloc;

static VpResult process_fx(Pic& out, const std::vector<VpStream>& streams, const VpHdrMetadata* display = nullptr, const float background[4] = nullptr) {
  VpOutput o;
  o.image = &out.img;
  o.target = whole(out);
  if (background) for (int i = 0; i < 4; ++i) o.background[i] = background[i];
  if (display) o.hdr = *display;
  o.allocator = &g_alloc;
  const VpResult r = video_process(o, ArrayView<const VpStream>(streams.data(), streams.size()));
  CHECK(g_alloc.live == 0);   // (scratch given back)
  return r;
}
static Pic nv12_frame(const std::vector<uint8_t>& raw, size_t at, uint32_t w, uint32_t h, bool widened = false) {
  Pic p = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, w, h, widened);
  for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) set_code(p, 0, x, y, 0, raw[at + y * w + x]);
  for (uint32_t y = 0; y < h / 2; ++y) for (uint32_t x = 0; x < w / 2; ++x) for (int c = 0; c < 2; ++c) set_code(p, 1, x, y, c, raw[at + w * h + y * w + 2 * x + c]);
  return p;
}
static bool same_pictures(const Pic& a, const Pic& b, int planes, int* worst = nullptr) {
  int w = 0;
  for (int pl = 0; pl < planes; ++pl) {
    const uint32_t pw = pl ? (a.w + 1) / 2 : a.w, ph = pl ? (a.h + 1) / 2 : a.h, nc = pl ? 2 : (is_yuv(a.format) ? 1 : 3);
    for (uint32_t y = 0; y < ph; ++y) for (uint32_t x = 0; x < pw; ++x) for (uint32_t c = 0; c < nc; ++c)
      w = fmax(w, fabs(get_code(a, pl, x, y, c) - get_code(b, pl, x, y, c)));
  }
  if (worst) *worst = w;
  return w == 0;
}

// yadif: every field of six interlaced frames, as top field first and bottom field first, against ffmpeg's yadif output processed as progressive frames. The stage's
// planes must be ffmpeg's exactly (so the rest of the processor makes the same output of both).
static void test_deinterlace_ffmpeg() {
  const std::vector<uint8_t> in = read_file("tests/data/videoprocess/interlaced.nv12");
  const std::vector<uint8_t> refs[2] = {read_file("tests/data/videoprocess/yadif_tff.nv12"), read_file("tests/data/videoprocess/yadif_bff.nv12")};
  const uint32_t w = 64, h = 32, frame = w * h * 3 / 2;
  CHECK(in.size() == 6 * frame && refs[0].size() == 12 * frame && refs[1].size() == 12 * frame);
  if (g_failed) return;
  int matched = 0;
  for (int order = 0; order < 2; ++order)
    for (int widened = 0; widened < 2; ++widened)
      for (uint32_t k = 0; k < 6; ++k)
        for (uint8_t field = 0; field < 2; ++field) {
          Pic cur = nv12_frame(in, k * frame, w, h, widened), prev = nv12_frame(in, (k ? k - 1 : 0) * frame, w, h, widened), next = nv12_frame(in, (k < 5 ? k + 1 : 5) * frame, w, h, widened);
          Pic out = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, w, h, true), expect = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, w, h, true);
          VpStream s = stream_of(cur, whole(cur), whole(out));
          s.field_order = order ? VpFieldOrder::BottomFieldFirst : VpFieldOrder::TopFieldFirst;
          s.deinterlace = VpDeinterlace::Adaptive;
          s.field = field;
          if (k > 0) s.past = &prev.img;   // (none: the current frame, as yadif does at the ends)
          if (k < 5) s.future = &next.img;
          CHECK(process_fx(out, {s}) == VpResult::Ok);
          Pic ref = nv12_frame(refs[order], (2 * k + field) * frame, w, h);
          CHECK(process_fx(expect, {stream_of(ref, whole(ref), whole(expect))}) == VpResult::Ok);
          int worst = 0;
          if (same_pictures(out, expect, 2, &worst)) ++matched;
          else if (g_reported < 40) printf("  yadif %s frame %u field %u: worst difference %d\n", order ? "BFF" : "TFF", k, field, worst);
        }
  printf("  ffmpeg yadif: %d of 48 fields the same\n", matched);
  CHECK(matched == 48);
}

// Bob, and the adaptive deinterlacer's two extremes: a still picture is the weave (the frame as it is), a moving bar the bob.
static void test_deinterlace_properties() {
  const uint32_t w = 32, h = 16;
  auto luma_pic = [&](auto f) {
    Pic p = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) set_code(p, 0, x, y, 0, f(x, y));
    for (uint32_t y = 0; y < h / 2; ++y) for (uint32_t x = 0; x < w / 2; ++x) for (int c = 0; c < 2; ++c) set_code(p, 1, x, y, c, 128);
    return p;
  };
  // bob: the shown field's rows as they are, the others the mean of the rows above and below (mirrored at the edges)
  Pic in = luma_pic([](uint32_t x, uint32_t y) { return (x * 7 + y * y * 3) % 200 + 20; });
  for (int order = 0; order < 2; ++order)
    for (uint8_t field = 0; field < 2; ++field) {
      Pic out = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h);
      VpStream s = stream_of(in, whole(in), whole(out));
      s.field_order = order ? VpFieldOrder::BottomFieldFirst : VpFieldOrder::TopFieldFirst;
      s.deinterlace = VpDeinterlace::Bob;
      s.field = field;
      CHECK(process_fx(out, {s}) == VpResult::Ok);
      const uint32_t shown = (order == 0) == (field == 0) ? 0 : 1;
      int bad = 0;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          double expect = get_code(in, 0, x, y, 0);
          if ((y & 1) != shown) {
            const uint32_t a = y > 0 ? y - 1 : y + 1, b = y + 1 < h ? y + 1 : y - 1;
            expect = floor((get_code(in, 0, x, a, 0) + get_code(in, 0, x, b, 0)) * 0.5 + 0.5);
          }
          bad += get_code(out, 0, x, y, 0) != expect;
        }
      CHECK(bad == 0);
    }
  // a still picture (each row's value between those of the rows above and below, any texture across): the adaptive deinterlacer gives the frame back
  Pic still = luma_pic([](uint32_t x, uint32_t y) { return 16 + y * 12 + (x * 37 % 11); });
  for (uint8_t field = 0; field < 2; ++field) {
    Pic out = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h), weave = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h);
    VpStream s = stream_of(still, whole(still), whole(out));
    s.field_order = VpFieldOrder::TopFieldFirst;
    s.deinterlace = VpDeinterlace::Adaptive;
    s.field = field;
    s.past = s.future = &still.img;
    CHECK(process_fx(out, {s}) == VpResult::Ok);
    CHECK(process_fx(weave, {stream_of(still, whole(still), whole(weave))}) == VpResult::Ok);
    CHECK(same_pictures(out, weave, 2));
  }
  // a bar moving to the right by 16 pixels a field: in the missing rows the shown field's bar is interpolated (bob) and the other field's bar is not there (as in a weave)
  auto bar_at = [&](uint32_t top_x, uint32_t bottom_x) {
    return luma_pic([=](uint32_t x, uint32_t y) { const uint32_t at = (y & 1) ? bottom_x : top_x; return x >= at && x < at + 6 ? 220u : 40u; });
  };
  Pic prev = bar_at(0, 0), cur = bar_at(4, 20), next = bar_at(26, 26);
  Pic out = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h);
  VpStream s = stream_of(cur, whole(cur), whole(out));
  s.field_order = VpFieldOrder::TopFieldFirst;
  s.deinterlace = VpDeinterlace::Adaptive;
  s.field = 0;   // the top field (bar at 4..9) is shown
  s.past = &prev.img;
  s.future = &next.img;
  CHECK(process_fx(out, {s}) == VpResult::Ok);
  int bad = 0;
  for (uint32_t y = 1; y < h; y += 2) {
    for (uint32_t x = 5; x < 9; ++x) bad += get_code(out, 0, x, y, 0) != 220;
    for (uint32_t x = 21; x < 25; ++x) bad += get_code(out, 0, x, y, 0) != 40;
  }
  CHECK(bad == 0);
  // what is refused: an interlaced row-interleaved or checkerboard stereo picture, references of another size
  Pic small = luma_pic([](uint32_t, uint32_t) { return 100u; });
  Pic other = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h / 2);
  VpStream r = stream_of(small, {0, 0, 32, 8}, whole(out));
  r.field_order = VpFieldOrder::TopFieldFirst;
  r.stereo = VpStereoFormat::RowInterleaved;
  CHECK(process_fx(out, {r}) == VpResult::Invalid);
  r.stereo = VpStereoFormat::None;
  r.source = whole(small);
  r.past = &other.img;
  r.deinterlace = VpDeinterlace::Adaptive;
  CHECK(process_fx(out, {r}) == VpResult::Invalid);
  // a reference with fewer channels than the picture (BGRA described with and without its alpha): refused, not read past its samples
  Pic with_alpha = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 8, 8), without = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 8, 8);
  without.img.plane[0].channels = 3;
  Pic rgb_out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 8, 8);
  VpStream ra = stream_of(with_alpha, whole(with_alpha), whole(rgb_out));
  ra.field_order = VpFieldOrder::TopFieldFirst;
  ra.deinterlace = VpDeinterlace::Adaptive;
  ra.future = &without.img;
  CHECK(process_fx(rgb_out, {ra}) == VpResult::Invalid);
  ra.future = nullptr;
  ra.past = &with_alpha.img;
  CHECK(process_fx(rgb_out, {ra}) == VpResult::Ok);
  r.past = nullptr;
  VpOutput no_alloc;
  no_alloc.image = &out.img;
  no_alloc.target = whole(out);
  CHECK(video_process(no_alloc, ArrayView<const VpStream>(&r, 1)) == VpResult::Unsupported);   // (no scratch memory to deinterlace into)
  printf("  deinterlacing: bob, still and moving pictures checked\n");
}

// Stereo: each packing's frames, flipped views, the mono offset.
static void test_stereo() {
  const uint32_t w = 16, h = 8;
  auto pic = [&](auto f) {
    Pic p = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, w, h);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) for (int c = 0; c < 4; ++c) set_code(p, 0, x, y, c, c == 3 ? 255 : f(x, y, c));
    return p;
  };
  struct Case { VpStereoFormat format; uint32_t vw, vh; int (*frame_of)(uint32_t, uint32_t); };
  const Case cases[] = {
      {VpStereoFormat::Horizontal, w / 2, h, [](uint32_t x, uint32_t) { return x >= 8 ? 1 : 0; }},
      {VpStereoFormat::Vertical, w, h / 2, [](uint32_t, uint32_t y) { return y >= 4 ? 1 : 0; }},
      {VpStereoFormat::RowInterleaved, w, h / 2, [](uint32_t, uint32_t y) { return static_cast<int>(y & 1); }},
      {VpStereoFormat::ColumnInterleaved, w / 2, h, [](uint32_t x, uint32_t) { return static_cast<int>(x & 1); }},
      {VpStereoFormat::Checkerboard, w, h, [](uint32_t x, uint32_t y) { return static_cast<int>((x + y) & 1); }},
  };
  for (const Case& k : cases) {
    // frame 0 is red, frame 1 blue (each with a ramp in green)
    Pic in = pic([&](uint32_t x, uint32_t y, int c) { const int f = k.frame_of(x, y); return c == 0 ? (f ? 0 : 200) : c == 2 ? (f ? 200 : 0) : 10 + y * 5; });
    for (uint8_t frame = 0; frame < 2; ++frame)
      for (int flip = 0; flip < 2; ++flip) {
        Pic out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, k.vw, k.vh);
        VpStream s = stream_of(in, {0, 0, static_cast<int32_t>(k.vw), static_cast<int32_t>(k.vh)}, whole(out));
        s.stereo = k.format;
        s.stereo_frame = frame;
        s.stereo_flip = flip;
        CHECK(process_fx(out, {s}) == VpResult::Ok);
        int bad = 0;
        for (uint32_t y = 0; y < k.vh; ++y)
          for (uint32_t x = 0; x < k.vw; ++x) {
            bad += get_code(out, 0, x, y, 0) != (frame ? 0 : 200) || get_code(out, 0, x, y, 2) != (frame ? 200 : 0);
            // the green ramp: the view's row (flipped: from the bottom) of the frame's rows
            const uint32_t vy = flip ? k.vh - 1 - y : y;
            uint32_t sy = vy;
            if (k.format == VpStereoFormat::Vertical) sy = vy + frame * 4;
            if (k.format == VpStereoFormat::RowInterleaved) sy = 2 * vy + frame;
            bad += get_code(out, 0, x, y, 1) != 10 + sy * 5;
          }
        CHECK(bad == 0);
      }
    // a source rectangle is in the view's pixels: one outside the view is refused
    Pic out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, w, h);
    VpStream s = stream_of(in, whole(in), whole(out));
    s.stereo = k.format;
    CHECK(process_fx(out, {s}) == (k.format == VpStereoFormat::Checkerboard ? VpResult::Ok : VpResult::Invalid));
  }
  // mono offset: the view is the picture moved right by the offset (its edge column repeated)
  Pic ramp = pic([](uint32_t x, uint32_t, int c) { return c == 0 ? 10 + x * 10 : 0; });
  for (int32_t offset : {-3, 0, 2, 5}) {
    Pic out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, w, h);
    VpStream s = stream_of(ramp, whole(ramp), whole(out));
    s.stereo = VpStereoFormat::MonoOffset;
    s.stereo_offset = offset;
    CHECK(process_fx(out, {s}) == VpResult::Ok);
    int bad = 0;
    for (uint32_t x = 0; x < w; ++x) {
      int32_t sx = static_cast<int32_t>(x) - offset;
      sx = sx < 0 ? 0 : sx >= static_cast<int32_t>(w) ? static_cast<int32_t>(w) - 1 : sx;
      bad += get_code(out, 0, x, 3, 0) != 10 + sx * 10;
    }
    CHECK(bad == 0);
  }
  // NV12 side by side: the views' luma and chroma come from their halves
  Pic yuv = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 32, 8);
  for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 32; ++x) set_code(yuv, 0, x, y, 0, x < 16 ? 50 : 200);
  for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 16; ++x) { set_code(yuv, 1, x, y, 0, x < 8 ? 100 : 160); set_code(yuv, 1, x, y, 1, 128); }
  for (uint8_t frame = 0; frame < 2; ++frame) {
    Pic out = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 16, 8, true);
    VpStream s = stream_of(yuv, {0, 0, 16, 8}, whole(out));
    s.stereo = VpStereoFormat::Horizontal;
    s.stereo_frame = frame;
    CHECK(process_fx(out, {s}) == VpResult::Ok);
    int bad = 0;
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 16; ++x) bad += get_code(out, 0, x, y, 0) != (frame ? 200 : 50);
    for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 8; ++x) bad += fabs(get_code(out, 1, x, y, 0) - (frame ? 160 : 100)) > 0.5;
    CHECK(bad == 0);
  }
  printf("  stereo: five packings x two frames x flips, mono offset, NV12 views checked\n");
}

// ProcAmp against DXVA's formulas computed by ffmpeg's geq on the codes (both outputs through the same chroma filters: ProcAmp is affine, so it commutes with them).
static void test_procamp() {
  const std::vector<uint8_t> nv12 = read_file("tests/data/videoprocess/gradient_709.nv12"), ref = read_file("tests/data/videoprocess/procamp.nv12");
  CHECK(nv12.size() == 64 * 32 * 3 / 2 && ref.size() == nv12.size());
  if (g_failed) return;
  Pic in = nv12_frame(nv12, 0, 64, 32), expected_in = nv12_frame(ref, 0, 64, 32);
  for (VpFormat fo : {VpFormat::Nv12, VpFormat::Bgra8}) {
    Pic out = make_pic(fo, fo == VpFormat::Nv12 ? kVpYcbcrStudioG22LeftP709 : kVpRgbFullG22P709, 64, 32), expect = out;
    expect.img.plane[0].memory = ByteView(expect.mem[0].data(), expect.mem[0].size());
    expect.img.plane[1].memory = ByteView(expect.mem[1].data(), expect.mem[1].size());
    VpStream s = stream_of(in, whole(in), whole(out));
    s.brightness = 10.0f;
    s.contrast = 1.2f;
    s.hue = 20.0f;
    s.saturation = 0.8f;
    CHECK(process_fx(out, {s}) == VpResult::Ok);
    CHECK(process_fx(expect, {stream_of(expected_in, whole(expected_in), whole(expect))}) == VpResult::Ok);
    int worst = 0;
    same_pictures(out, expect, is_yuv(fo) ? 2 : 1, &worst);
    printf("  ffmpeg geq ProcAmp -> %s: worst difference %d\n", format_name(fo), worst);
    CHECK(worst <= 2);   // (geq rounds the codes; the processor keeps them exact until the output)
  }
  // RGB: saturation 0 is grey, brightness lifts all three, contrast 0 leaves the brightness alone
  Pic rgb = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 8, 1);
  for (uint32_t x = 0; x < 8; ++x) { set_code(rgb, 0, x, 0, 0, 30 * x); set_code(rgb, 0, x, 0, 1, 200 - 20 * x); set_code(rgb, 0, x, 0, 2, 90); set_code(rgb, 0, x, 0, 3, 255); }
  Pic out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 8, 1);
  VpStream s = stream_of(rgb, whole(rgb), whole(out));
  s.saturation = 0.0f;
  CHECK(process_fx(out, {s}) == VpResult::Ok);
  int bad = 0;
  for (uint32_t x = 0; x < 8; ++x) {
    const double y = 0.2126 * 30 * x + 0.7152 * (200 - 20 * x) + 0.0722 * 90;
    for (int c = 0; c < 3; ++c) bad += fabs(get_code(out, 0, x, 0, c) - y) > 1.0;
  }
  CHECK(bad == 0);
  s.saturation = 1.0f;
  s.contrast = 0.0f;
  s.brightness = 51.0f;   // (in full range: 51 / 255 = 0.2)
  CHECK(process_fx(out, {s}) == VpResult::Ok);
  bad = 0;
  for (uint32_t x = 0; x < 8; ++x) for (int c = 0; c < 3; ++c) bad += get_code(out, 0, x, 0, c) != 51;
  CHECK(bad == 0);
  // out of range: refused
  s.contrast = 11.0f;
  CHECK(process_fx(out, {s}) == VpResult::Invalid);
  s.contrast = 1.0f;
  s.hue = 181.0f;
  CHECK(process_fx(out, {s}) == VpResult::Invalid);
  s.hue = 0.0f;
  s.brightness = NAN;
  CHECK(process_fx(out, {s}) == VpResult::Invalid);
}

// Luma key: a luma ramp over a background; the codes from lower to upper (both truncated to codes, inclusive) show the background.
static void test_luma_key() {
  Pic in = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, 256, 2);
  for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 256; ++x) set_code(in, 0, x, y, 0, x);
  for (uint32_t x = 0; x < 128; ++x) { set_code(in, 1, x, 0, 0, 128); set_code(in, 1, x, 0, 1, 128); }
  Pic out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 256, 2);
  const float bg[4] = {0.0f, 1.0f, 0.0f, 1.0f};   // green
  VpStream s = stream_of(in, whole(in), whole(out));
  s.luma_key = true;
  s.luma_lower = 0.25f;   // 63.75: code 63
  s.luma_upper = 0.5f;    // 127.5: code 127
  CHECK(process_fx(out, {s}, nullptr, bg) == VpResult::Ok);
  int bad = 0;
  for (uint32_t x = 0; x < 256; ++x) {
    const bool keyed = x >= 63 && x <= 127;
    bad += keyed ? (get_code(out, 0, x, 0, 1) != 255 || get_code(out, 0, x, 0, 0) != 0) : (get_code(out, 0, x, 0, 0) != x || get_code(out, 0, x, 0, 1) != x);
  }
  CHECK(bad == 0);
  // RGB: the luma of R'G'B' (BT.709)
  Pic rgb = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 4, 1);
  const int grey[4] = {10, 100, 150, 240};
  for (uint32_t x = 0; x < 4; ++x) for (int c = 0; c < 4; ++c) set_code(rgb, 0, x, 0, c, c == 3 ? 255 : grey[x]);
  Pic out2 = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 4, 1);
  VpStream r = stream_of(rgb, whole(rgb), whole(out2));
  r.luma_key = true;
  r.luma_lower = 0.3f;
  r.luma_upper = 0.7f;
  CHECK(process_fx(out2, {r}, nullptr, bg) == VpResult::Ok);
  CHECK(get_code(out2, 0, 0, 0, 0) == 10 && get_code(out2, 0, 1, 0, 1) == 255 && get_code(out2, 0, 2, 0, 1) == 255 && get_code(out2, 0, 3, 0, 0) == 240);
  r.luma_lower = 0.8f;   // (lower above upper)
  CHECK(process_fx(out2, {r}, nullptr, bg) == VpResult::Invalid);
}

// Noise reduction keeps edges and smooths noise; edge enhancement steepens edges and leaves flat areas; anamorphic scaling stretches the sides more than the middle.
static void test_spatial_filters() {
  const uint32_t w = 32, h = 16;
  Pic in = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h);
  g_seed = 99;
  for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) set_code(in, 0, x, y, 0, (x < 16 ? 80 : 180) + static_cast<int>(rnd() % 9) - 4);
  for (uint32_t y = 0; y < h / 2; ++y) for (uint32_t x = 0; x < w / 2; ++x) for (int c = 0; c < 2; ++c) set_code(in, 1, x, y, c, 128);
  auto variance = [&](const Pic& p, uint32_t x0, uint32_t x1) {
    double sum = 0, sq = 0, n = 0;
    for (uint32_t y = 1; y + 1 < h; ++y) for (uint32_t x = x0; x < x1; ++x) { const double v = get_code(p, 0, x, y, 0); sum += v; sq += v * v; ++n; }
    return sq / n - (sum / n) * (sum / n);
  };
  Pic plain = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h), nr = plain, ee = plain;
  for (Pic* p : {&nr, &ee}) { p->img.plane[0].memory = ByteView(p->mem[0].data(), p->mem[0].size()); p->img.plane[1].memory = ByteView(p->mem[1].data(), p->mem[1].size()); }
  CHECK(process_fx(plain, {stream_of(in, whole(in), whole(plain))}) == VpResult::Ok);
  VpStream s = stream_of(in, whole(in), whole(nr));
  s.noise_reduction = 1.0f;
  CHECK(process_fx(nr, {s}) == VpResult::Ok);
  const double v0 = variance(plain, 1, 15), v1 = variance(nr, 1, 15);
  int crossed = 0;   // (no pixel took the other side's level: the edge stays)
  for (uint32_t y = 0; y < h; ++y) { crossed += get_code(nr, 0, 15, y, 0) > 90; crossed += get_code(nr, 0, 16, y, 0) < 170; }
  printf("  noise reduction: variance %.2f -> %.2f, %d pixels across the edge changed side\n", v0, v1, crossed);
  CHECK(v1 < v0 * 0.5 && crossed == 0);
  s.noise_reduction = 0.0f;
  s.edge_enhancement = 1.0f;
  // on a clean step: an overshoot either side of the edge, flat areas as they were
  Pic step = make_pic(VpFormat::Nv12, kVpYcbcrFullG22LeftP709, w, h);
  for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) set_code(step, 0, x, y, 0, x < 16 ? 80 : 180);
  for (uint32_t y = 0; y < h / 2; ++y) for (uint32_t x = 0; x < w / 2; ++x) for (int c = 0; c < 2; ++c) set_code(step, 1, x, y, c, 128);
  s.image = &step.img;
  CHECK(process_fx(ee, {s}) == VpResult::Ok);
  // (3x3 mean beside the edge: 80 + 100 / 3; amount 2: 80 - 2 * 33.3 = 13.3, 180 + 66.7 = 246.7)
  CHECK(get_code(ee, 0, 15, 5, 0) == 13 && get_code(ee, 0, 16, 5, 0) == 247 && get_code(ee, 0, 5, 5, 0) == 80 && get_code(ee, 0, 25, 5, 0) == 180);
  // anamorphic: a ramp 0..63 across 64 pixels into 64: the middle advances 1 + k per pixel, the sides 1 - k (k = half the level), the ends stay
  Pic ramp = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 64, 1), out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 64, 1);
  for (uint32_t x = 0; x < 64; ++x) for (int c = 0; c < 4; ++c) set_code(ramp, 0, x, 0, c, c == 3 ? 255 : x * 4);
  VpStream a = stream_of(ramp, whole(ramp), whole(out));
  a.anamorphic = 1.0f;
  CHECK(process_fx(out, {a}) == VpResult::Ok);
  const double mid = get_code(out, 0, 33, 0, 0) - get_code(out, 0, 31, 0, 0), side = get_code(out, 0, 2, 0, 0) - get_code(out, 0, 0, 0, 0);
  printf("  anamorphic: middle step %.1f, side step %.1f (codes per two pixels)\n", mid, side);
  CHECK(mid > 11.0 && mid < 13.0 && side >= 2.0 && side < 5.5 && get_code(out, 0, 0, 0, 0) <= 2 && get_code(out, 0, 63, 0, 0) >= 250);
  for (uint32_t x = 1; x < 64; ++x) CHECK(get_code(out, 0, x, 0, 0) >= get_code(out, 0, x - 1, 0, 0));
}

// HDR: PQ and HLG against BT.2100's formulas and BT.2408's reference levels, round trips, tone mapping (monotonic, the content's peak to the display's).
static double ref_pq_nits(double e) {
  const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
  const double p = pow(e, 1 / m2);
  return 10000 * pow(fmax(p - c1, 0) / (c2 - c3 * p), 1 / m1);
}
static double ref_nits_pq(double l) {
  const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
  const double y = pow(l / 10000, m1);
  return pow((c1 + c2 * y) / (1 + c3 * y), m2);
}
static void test_hdr() {
  CHECK(vp_color_space_supported(VpFormat::P010, kVpYcbcrStudioG2084LeftP2020) && vp_color_space_supported(VpFormat::Rgb10a2, kVpRgbFullG2084P2020) &&
        vp_color_space_supported(VpFormat::P010, kVpYcbcrStudioGhlgTopLeftP2020) && !vp_color_space_supported(VpFormat::Rgb10a2, kVpYcbcrStudioG2084LeftP2020));
  // PQ grey ramp (RGB10A2) -> linear float (relative to 203 cd/m2): BT.2100's EOTF
  Pic pq = make_pic(VpFormat::Rgb10a2, kVpRgbFullG2084P2020, 64, 1);
  for (uint32_t x = 0; x < 64; ++x) for (int c = 0; c < 4; ++c) set_code(pq, 0, x, 0, c, c == 3 ? 3 : x * 1023 / 63);
  Pic lin = make_pic(VpFormat::Rgba16f, kVpRgbFullG10P709, 64, 1);
  CHECK(process_fx(lin, {stream_of(pq, whole(pq), whole(lin))}) == VpResult::Ok);
  double worst = 0;
  for (uint32_t x = 0; x < 64; ++x) {
    const double expect = ref_pq_nits((x * 1023 / 63) / 1023.0) / 203.0;
    for (int c = 0; c < 3; ++c) worst = fmax(worst, fabs(get_code(lin, 0, x, 0, c) - expect) / fmax(expect, 0.01));
  }
  printf("  PQ -> linear: worst relative error %.5f (10000 cd/m2 = %.2f)\n", worst, get_code(lin, 0, 63, 0, 0));
  CHECK(worst < 2e-3);
  // and back to PQ: the codes return (to a code)
  Pic back = make_pic(VpFormat::Rgb10a2, kVpRgbFullG2084P2020, 64, 1);
  Pic lin_p709 = lin;
  lin_p709.img.plane[0].memory = ByteView(lin_p709.mem[0].data(), lin_p709.mem[0].size());
  CHECK(process_fx(back, {stream_of(lin_p709, whole(lin_p709), whole(back))}) == VpResult::Ok);
  worst = 0;
  for (uint32_t x = 1; x < 64; ++x) worst = fmax(worst, fabs(get_code(back, 0, x, 0, 1) - get_code(pq, 0, x, 0, 1)));
  printf("  PQ round trip through half floats: worst %g codes\n", worst);
  CHECK(worst <= 1.0);
  // SDR white in PQ is the reference white (BT.2408: 203 cd/m2, PQ 58%)
  Pic white = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 1, 1);
  for (int c = 0; c < 4; ++c) set_code(white, 0, 0, 0, c, 255);
  Pic wpq = make_pic(VpFormat::Rgb10a2, kVpRgbFullG2084P2020, 1, 1);
  CHECK(process_fx(wpq, {stream_of(white, whole(white), whole(wpq))}) == VpResult::Ok);
  CHECK(fabs(get_code(wpq, 0, 0, 0, 0) - floor(ref_nits_pq(203) * 1023 + 0.5)) <= 1);
  // HLG: 75% is the reference white (BT.2408) on the 1000 cd/m2 display: linear 1.0; HLG -> linear -> HLG returns
  Pic hlg = make_pic(VpFormat::P010, kVpYcbcrFullGhlgTopLeftP2020, 2, 2);
  for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x) set_code(hlg, 0, x, y, 0, floor(0.75 * 1023 + 0.5));
  set_code(hlg, 1, 0, 0, 0, 512); set_code(hlg, 1, 0, 0, 1, 512);
  Pic hl = make_pic(VpFormat::Rgba16f, kVpRgbFullG10P709, 2, 2);
  CHECK(process_fx(hl, {stream_of(hlg, whole(hlg), whole(hl))}) == VpResult::Ok);
  printf("  HLG 75%%: linear %.4f (reference white 1)\n", get_code(hl, 0, 0, 0, 1));
  CHECK(fabs(get_code(hl, 0, 0, 0, 1) - 1.0) < 0.01);
  Pic hlg_back = make_pic(VpFormat::P010, kVpYcbcrFullGhlgTopLeftP2020, 2, 2, true);
  Pic hl2 = hl;
  hl2.img.plane[0].memory = ByteView(hl2.mem[0].data(), hl2.mem[0].size());
  CHECK(process_fx(hlg_back, {stream_of(hl2, whole(hl2), whole(hlg_back))}) == VpResult::Ok);
  CHECK(fabs(get_code(hlg_back, 0, 0, 0, 0) - floor(0.75 * 1023 + 0.5)) <= 1);
  // tone mapping PQ (content 1000 cd/m2, MaxCLL) -> SDR: monotonic, the content's peak to white, dark levels untouched
  Pic ramp = make_pic(VpFormat::Rgb10a2, kVpRgbFullG2084P2020, 1024, 1);
  for (uint32_t x = 0; x < 1024; ++x) for (int c = 0; c < 4; ++c) set_code(ramp, 0, x, 0, c, c == 3 ? 3 : x);
  Pic sdr = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 1024, 1);
  VpStream s = stream_of(ramp, whole(ramp), whole(sdr));
  s.hdr.max_content = 1000.0f;
  s.hdr.max_mastering = 4000.0f;
  CHECK(process_fx(sdr, {s}) == VpResult::Ok);
  int non_monotonic = 0;
  for (uint32_t x = 1; x < 1024; ++x) non_monotonic += get_code(sdr, 0, x, 0, 1) < get_code(sdr, 0, x - 1, 0, 1);
  const uint32_t peak = static_cast<uint32_t>(floor(ref_nits_pq(1000) * 1023 + 0.5)), dark = static_cast<uint32_t>(floor(ref_nits_pq(20) * 1023 + 0.5));
  const double dark_expect = 255 * (1.055 * pow(ref_pq_nits(dark / 1023.0) / 203, 1 / 2.4) - 0.055);
  printf("  tone mapping 1000 cd/m2 -> SDR: peak (PQ code %u) -> %g, 20 cd/m2 -> %g (untouched: %.1f), %d steps down\n", peak, get_code(sdr, 0, peak, 0, 1),
         get_code(sdr, 0, dark, 0, 1), dark_expect, non_monotonic);
  CHECK(non_monotonic == 0 && get_code(sdr, 0, peak, 0, 1) == 255 && get_code(sdr, 0, static_cast<uint32_t>(floor(ref_nits_pq(300) * 1023 + 0.5)), 0, 1) < 250 && fabs(get_code(sdr, 0, dark, 0, 1) - dark_expect) <= 1.0);
  // PQ -> PQ onto a 600 cd/m2 display (SetOutputHDRMetaData): the content's peak to 600, monotonic
  Pic pq_out = make_pic(VpFormat::Rgb10a2, kVpRgbFullG2084P2020, 1024, 1);
  VpHdrMetadata display;
  display.max_mastering = 600.0f;
  s.destination = whole(pq_out);
  CHECK(process_fx(pq_out, {s}, &display) == VpResult::Ok);
  non_monotonic = 0;
  for (uint32_t x = 1; x < 1024; ++x) non_monotonic += get_code(pq_out, 0, x, 0, 0) < get_code(pq_out, 0, x - 1, 0, 0);
  printf("  tone mapping 1000 -> 600 cd/m2 (PQ): peak -> %.0f cd/m2\n", ref_pq_nits(get_code(pq_out, 0, peak, 0, 0) / 1023.0));
  CHECK(non_monotonic == 0 && fabs(get_code(pq_out, 0, peak, 0, 0) - floor(ref_nits_pq(600) * 1023 + 0.5)) <= 1 && get_code(pq_out, 0, dark, 0, 0) == dark);
  // without metadata a PQ output holds all of PQ: the codes pass (no tone mapping)
  CHECK(process_fx(pq_out, {s}) == VpResult::Ok);
  int moved = 0;
  for (uint32_t x = 0; x < 1024; ++x) moved += get_code(pq_out, 0, x, 0, 2) != x;
  CHECK(moved == 0);
  // HLG -> SDR: tone mapped from HLG's 1000 cd/m2 peak; HLG 1.0 is white
  Pic hlg1 = make_pic(VpFormat::P010, kVpYcbcrStudioGhlgTopLeftP2020, 2, 2);
  for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x) set_code(hlg1, 0, x, y, 0, 940);
  set_code(hlg1, 1, 0, 0, 0, 512); set_code(hlg1, 1, 0, 0, 1, 512);
  Pic sdr1 = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 2, 2);
  CHECK(process_fx(sdr1, {stream_of(hlg1, whole(hlg1), whole(sdr1))}) == VpResult::Ok);
  CHECK(get_code(sdr1, 0, 0, 0, 0) == 255 && get_code(sdr1, 0, 0, 0, 1) == 255);
  // BT.2020 PQ red -> BT.709 SDR: out of gamut, clipped to the gamut's red
  Pic red = make_pic(VpFormat::Rgb10a2, kVpRgbFullG2084P2020, 1, 1);
  set_code(red, 0, 0, 0, 0, floor(ref_nits_pq(100) * 1023 + 0.5)); set_code(red, 0, 0, 0, 1, 0); set_code(red, 0, 0, 0, 2, 0); set_code(red, 0, 0, 0, 3, 3);
  Pic red_out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 1, 1);
  CHECK(process_fx(red_out, {stream_of(red, whole(red), whole(red_out))}) == VpResult::Ok);
  CHECK(get_code(red_out, 0, 0, 0, 0) > 200 && get_code(red_out, 0, 0, 0, 1) == 0 && get_code(red_out, 0, 0, 0, 2) == 0);
  VpHdrMetadata bad;
  bad.max_content = -1.0f;
  CHECK(process_fx(red_out, {stream_of(red, whole(red), whole(red_out))}, &bad) == VpResult::Invalid);
}

// Random stages on random small pictures: nothing outside a plane is touched (the sanitizers watch), scratch always given back.
static void test_stage_garbage() {
  int ok = 0;
  g_seed = 4242;
  for (int iter = 0; iter < 3000; ++iter) {
    const VpFormat fi = kFormats[rnd() % 8], fo = kFormats[rnd() % 8];
    const uint32_t w = 1 + rnd() % 13, h = 1 + rnd() % 13;
    const VpColorSpace si = is_yuv(fi) ? kYuvSpaces[rnd() % 11] : kRgbSpaces[rnd() % 7];
    Pic in = make_pic(fi, si, w, h, rnd() & 1), past = make_pic(fi, si, w, h, rnd() & 1), future = make_pic(fi, si, w, h, rnd() & 1);
    Pic out = make_pic(fo, is_yuv(fo) ? kYuvSpaces[rnd() % 11] : kRgbSpaces[rnd() % 7], 1 + rnd() % 9, 1 + rnd() % 9, rnd() & 1);
    for (Pic* p : {&in, &past, &future}) for (auto& m : p->mem) for (auto& b : m) b = static_cast<uint8_t>(rnd());
    VpStream s = stream_of(in, {0, 0, static_cast<int32_t>(rnd() % (w + 2)), static_cast<int32_t>(rnd() % (h + 2))}, whole(out), static_cast<VpOrientation>(rnd() % 8));
    s.field_order = static_cast<VpFieldOrder>(rnd() % 3);
    s.deinterlace = static_cast<VpDeinterlace>(rnd() % 3);
    s.field = rnd() & 1;
    if (rnd() & 1) s.past = &past.img;
    if (rnd() & 1) s.future = &future.img;
    s.stereo = static_cast<VpStereoFormat>(rnd() % 7);
    s.stereo_frame = rnd() & 1;
    s.stereo_flip = rnd() & 1;
    s.stereo_offset = static_cast<int32_t>(rnd() % 40) - 20;
    if (rnd() & 1) { s.noise_reduction = (rnd() % 5) / 4.0f; s.edge_enhancement = (rnd() % 5) / 4.0f; }
    if (rnd() & 1) { s.brightness = static_cast<float>(rnd() % 201) - 100; s.contrast = (rnd() % 30) / 3.0f; s.hue = static_cast<float>(rnd() % 361) - 180; s.saturation = (rnd() % 30) / 3.0f; }
    s.anamorphic = (rnd() % 3) / 2.0f;
    s.luma_key = rnd() & 1;
    s.luma_lower = 0.2f;
    s.luma_upper = 0.6f;
    if (process_fx(out, {s, s}) == VpResult::Ok) ++ok;
  }
  printf("  random stages: %d of 3000 accepted, none faulted\n", ok);
  CHECK(ok > 500);
}

// ---- speed

static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void test_speed() {
  Pic in = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 1920, 1080, false, 0);
  for (uint32_t y = 0; y < 1080; ++y) for (uint32_t x = 0; x < 1920; ++x) in.mem[0][y * in.img.plane[0].pitch + x] = static_cast<uint8_t>(16 + (x + y) % 220);
  for (auto& b : in.mem[1]) b = 128;
  Pic out = make_pic(VpFormat::Bgra8, kVpRgbFullG22P709, 1280, 720, false, 0);
  const double t0 = now();
  CHECK(process(out, whole(out), {stream_of(in, whole(in), whole(out))}) == VpResult::Ok);
  const double t1 = now();
  // and the kernel's widened NV12 out of the same
  Pic yout = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 1280, 720, true);
  CHECK(process(yout, whole(yout), {stream_of(in, whole(in), whole(yout))}) == VpResult::Ok);
  const double t2 = now();
  printf("  1920x1080 NV12 -> 1280x720 BGRA8: %.1f ms; -> 1280x720 NV12 (widened): %.1f ms\n", (t1 - t0) * 1e3, (t2 - t1) * 1e3);
  CHECK(t1 - t0 < 1.0);
  // motion adaptive deinterlacing of 1080i into 1080p NV12, with ProcAmp
  Pic full = make_pic(VpFormat::Nv12, kVpYcbcrStudioG22LeftP709, 1920, 1080, true);
  VpStream d = stream_of(in, whole(in), whole(full));
  d.field_order = VpFieldOrder::TopFieldFirst;
  d.deinterlace = VpDeinterlace::Adaptive;
  d.past = d.future = &in.img;
  d.contrast = 1.1f;
  const double t3 = now();
  CHECK(process_fx(full, {d}) == VpResult::Ok);
  const double t4 = now();
  printf("  1920x1080 interlaced NV12 -> 1920x1080 NV12, motion adaptive: %.1f ms\n", (t4 - t3) * 1e3);
}

int main() {
  test_reference();
  test_known_values();
  test_format_pairs();
  printf("  %d format pairs, %d conversions checked\n", g_pairs, g_pair_runs);
  test_ffmpeg();
  test_siting();
  test_scaling();
  test_orientations();
  test_clipping_and_background();
  test_alpha();
  test_yuv_targets();
  test_refusals();
  test_garbage();
  test_deinterlace_ffmpeg();
  test_deinterlace_properties();
  test_stereo();
  test_procamp();
  test_luma_key();
  test_spatial_filters();
  test_hdr();
  test_stage_garbage();
  test_speed();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("VideoProcessTest passed\n");
  return 0;
}
