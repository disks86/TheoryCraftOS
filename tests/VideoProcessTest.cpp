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
  for (VpColorSpace bad : {4u, 12u, 13u, 14u, 16u, 18u, 19u, 25u, 0xFFFFFFFFu}) {
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
  test_speed();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("VideoProcessTest passed\n");
  return 0;
}
