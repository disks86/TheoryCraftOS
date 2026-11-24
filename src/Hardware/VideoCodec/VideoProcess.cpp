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
// The video processor (VideoProcess.hpp). How it works:
//
// - Every pixel is worked on in the output's "nominal" values: Y in 0..1 and Cb, Cr in -0.5..0.5 for a YCbCr output, R'G'B' in 0..1 for an RGB one (range not
//   applied yet), plus alpha. Blending and chroma averaging happen there; the output's range and depth are applied when a sample is written.
// - A stream's sample codes go to those values through one affine matrix when its colour space has the output's transfer and primaries (range, matrix and depth are all
//   linear), else through its own R'G'B' (clamped), linear light, a primaries matrix and the output's transfer.
// - The output is produced in chunks of columns, row by row, into a small line buffer on the stack (no allocation). A YCbCr output's chunk is aligned to its chroma
//   samples and has the rows and columns its chroma filter needs: pixels outside the target rectangle there are the output as it was (its luma, and the chroma sample
//   that covers them), so a chroma sample half inside the target is the average of the new and the old.
#include "Hardware/VideoCodec/VideoProcess.hpp"
#include "Graphics/GraphicsTypes.hpp"
#include "Allocators/IAllocator.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {

enum class Transfer : uint8_t { G22, G10, G24, Pq, Hlg };
enum class Primaries : uint8_t { Bt709, Bt2020 };   // (P601 is taken as P709: see the header)
enum class Matrix : uint8_t { Bt601, Bt709, Bt2020 };
enum class Siting : uint8_t { Centre, Left, TopLeft };

struct SpaceInfo {
  bool known = false, yuv = false, full = false;
  Transfer transfer = Transfer::G22;
  Primaries primaries = Primaries::Bt709;
  Matrix matrix = Matrix::Bt709;
  Siting siting = Siting::Centre;
};

constexpr SpaceInfo rgb_space(bool full, Transfer t, Primaries p) { return {true, false, full, t, p, p == Primaries::Bt2020 ? Matrix::Bt2020 : Matrix::Bt709, Siting::Centre}; }
constexpr SpaceInfo yuv_space(bool full, Transfer t, Primaries p, Matrix m, Siting s) { return {true, true, full, t, p, m, s}; }

constexpr SpaceInfo space_info(VpColorSpace s) {
  switch (s) {
    case kVpRgbFullG22P709: return rgb_space(true, Transfer::G22, Primaries::Bt709);
    case kVpRgbFullG10P709: return rgb_space(true, Transfer::G10, Primaries::Bt709);
    case kVpRgbStudioG22P709: return rgb_space(false, Transfer::G22, Primaries::Bt709);
    case kVpRgbStudioG22P2020: return rgb_space(false, Transfer::G22, Primaries::Bt2020);
    case kVpRgbFullG22P2020: return rgb_space(true, Transfer::G22, Primaries::Bt2020);
    case kVpRgbStudioG24P709: return rgb_space(false, Transfer::G24, Primaries::Bt709);
    case kVpRgbStudioG24P2020: return rgb_space(false, Transfer::G24, Primaries::Bt2020);
    case kVpYcbcrFullG22P709X601: return yuv_space(true, Transfer::G22, Primaries::Bt709, Matrix::Bt601, Siting::Centre);
    case kVpYcbcrStudioG22LeftP601: return yuv_space(false, Transfer::G22, Primaries::Bt709, Matrix::Bt601, Siting::Left);
    case kVpYcbcrFullG22LeftP601: return yuv_space(true, Transfer::G22, Primaries::Bt709, Matrix::Bt601, Siting::Left);
    case kVpYcbcrStudioG22LeftP709: return yuv_space(false, Transfer::G22, Primaries::Bt709, Matrix::Bt709, Siting::Left);
    case kVpYcbcrFullG22LeftP709: return yuv_space(true, Transfer::G22, Primaries::Bt709, Matrix::Bt709, Siting::Left);
    case kVpYcbcrStudioG22LeftP2020: return yuv_space(false, Transfer::G22, Primaries::Bt2020, Matrix::Bt2020, Siting::Left);
    case kVpYcbcrFullG22LeftP2020: return yuv_space(true, Transfer::G22, Primaries::Bt2020, Matrix::Bt2020, Siting::Left);
    case kVpYcbcrStudioG22TopLeftP2020: return yuv_space(false, Transfer::G22, Primaries::Bt2020, Matrix::Bt2020, Siting::TopLeft);
    case kVpYcbcrStudioG24LeftP709: return yuv_space(false, Transfer::G24, Primaries::Bt709, Matrix::Bt709, Siting::Left);
    case kVpYcbcrStudioG24LeftP2020: return yuv_space(false, Transfer::G24, Primaries::Bt2020, Matrix::Bt2020, Siting::Left);
    case kVpYcbcrStudioG24TopLeftP2020: return yuv_space(false, Transfer::G24, Primaries::Bt2020, Matrix::Bt2020, Siting::TopLeft);
    case kVpRgbFullG2084P2020: return rgb_space(true, Transfer::Pq, Primaries::Bt2020);
    case kVpRgbStudioG2084P2020: return rgb_space(false, Transfer::Pq, Primaries::Bt2020);
    case kVpYcbcrStudioG2084LeftP2020: return yuv_space(false, Transfer::Pq, Primaries::Bt2020, Matrix::Bt2020, Siting::Left);
    case kVpYcbcrStudioG2084TopLeftP2020: return yuv_space(false, Transfer::Pq, Primaries::Bt2020, Matrix::Bt2020, Siting::TopLeft);
    case kVpYcbcrStudioGhlgTopLeftP2020: return yuv_space(false, Transfer::Hlg, Primaries::Bt2020, Matrix::Bt2020, Siting::TopLeft);
    case kVpYcbcrFullGhlgTopLeftP2020: return yuv_space(true, Transfer::Hlg, Primaries::Bt2020, Matrix::Bt2020, Siting::TopLeft);
    default: return {};   // values DXGI does not define (and 4, reserved)
  }
}

// ---- arithmetic (the kernel has no libm: natural log and exp for the transfer functions, in double)

double floor_d(double v) {
  const double t = static_cast<double>(static_cast<int64_t>(v));   // (|v| is far below 2^63 wherever this is used)
  return t > v ? t - 1.0 : t;
}

double ln_d(double x) {   // x > 0: x = m * 2^e with m in [sqrt(1/2), sqrt(2)), ln m = 2 atanh((m - 1) / (m + 1))
  uint64_t bits = __builtin_bit_cast(uint64_t, x);
  int64_t e = static_cast<int64_t>((bits >> 52) & 0x7FF) - 1023;
  if (e == -1023) {   // subnormal: scale into the normal range first
    x *= 4503599627370496.0;   // 2^52
    bits = __builtin_bit_cast(uint64_t, x);
    e = static_cast<int64_t>((bits >> 52) & 0x7FF) - 1023 - 52;
  }
  double m = __builtin_bit_cast(double, (bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull);
  if (m > 1.4142135623730951) { m *= 0.5; ++e; }
  const double z = (m - 1.0) / (m + 1.0), z2 = z * z;
  double sum = 0.0;
  for (int k = 17; k >= 1; k -= 2) sum = sum * z2 + 1.0 / k;   // |z| <= 0.1716: the series' remainder is below 1e-16
  return 2.0 * z * sum + static_cast<double>(e) * 0.6931471805599453;
}

double exp_d(double y) {   // y <= 0 in this file (the powers below are of 0..1); exact enough for any y in -700..700
  if (y < -700.0) return 0.0;
  const double k = floor_d(y * 1.4426950408889634 + 0.5);
  const double r = y - k * 0.6931471805599453;   // |r| <= ln2 / 2
  double term = 1.0, sum = 1.0;
  for (int i = 1; i <= 16; ++i) { term *= r / i; sum += term; }
  const int64_t ki = static_cast<int64_t>(k);
  return sum * __builtin_bit_cast(double, static_cast<uint64_t>(ki + 1023) << 52);
}

double pow01(double x, double exponent) { return x <= 0.0 ? 0.0 : x >= 1.0 ? 1.0 : exp_d(exponent * ln_d(x)); }

float clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }   // (NaN to 0)
double clamp01(double v) { return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0; }

// sin and cos of x radians (|x| a few turns at most): reduced to [-pi/4, pi/4] by quarter turns, then their series.
void sin_cos(double x, double* sine, double* cosine) {
  const double half_pi = 1.5707963267948966;
  const double k = floor_d(x / half_pi + 0.5);
  const double r = x - k * half_pi, r2 = r * r;
  double s = 0.0, c = 0.0, term_s = r, term_c = 1.0;
  for (int i = 0; i < 10; ++i) {
    s += term_s;
    c += term_c;
    term_s *= -r2 / ((2 * i + 2) * (2 * i + 3));
    term_c *= -r2 / ((2 * i + 1) * (2 * i + 2));
  }
  const int64_t q = static_cast<int64_t>(k) & 3;
  *sine = q == 0 ? s : q == 1 ? c : q == 2 ? -s : -c;
  *cosine = q == 0 ? c : q == 1 ? -s : q == 2 ? -c : s;
}

// ---- transfer functions. Linear light is relative to the reference white (1.0 = kVpReferenceWhiteNits cd/m2); an SDR signal's 0..1 is 0..1 of it.

constexpr double kWhite = kVpReferenceWhiteNits, kHlgPeak = kVpHlgPeakNits, kPqPeak = 10000.0;
constexpr double kPqM1 = 2610.0 / 16384.0, kPqM2 = 2523.0 / 4096.0 * 128.0, kPqC1 = 3424.0 / 4096.0, kPqC2 = 2413.0 / 4096.0 * 32.0, kPqC3 = 2392.0 / 4096.0 * 32.0;
// ST.2084: signal 0..1 <-> cd/m2 0..10000.
double pq_to_nits(double e) {
  const double p = pow01(clamp01(e), 1.0 / kPqM2);
  const double num = p - kPqC1 > 0.0 ? p - kPqC1 : 0.0, den = kPqC2 - kPqC3 * p;
  return kPqPeak * pow01(num / den, 1.0 / kPqM1);
}
double nits_to_pq(double nits) {
  const double y = pow01(clamp01(nits / kPqPeak), kPqM1);
  return pow01((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2);
}
// BT.2100 HLG: signal 0..1 <-> scene light 0..1.
constexpr double kHlgA = 0.17883277, kHlgB = 0.28466892, kHlgC = 0.55991073;
double hlg_to_scene(double e) {
  e = clamp01(e);
  return e <= 0.5 ? e * e / 3.0 : (exp_d((e - kHlgC) / kHlgA) + kHlgB) / 12.0;
}
double scene_to_hlg(double l) {
  l = clamp01(l);
  return l <= 1.0 / 12.0 ? pow01(3.0 * l, 0.5) : kHlgA * ln_d(12.0 * l - kHlgB) + kHlgC;
}

// A signal to linear light: SDR curves (clamped to 0..1), PQ to its light over the reference white, HLG to scene light (the OOTF is the caller's: it needs all three).
double to_linear(Transfer t, double v) {
  switch (t) {
    case Transfer::G10: return v > 0.0 ? v : 0.0;   // (linear float may hold more than the white: scRGB-like; what the output keeps is its `top`)
    case Transfer::G24: return pow01(clamp01(v), 2.4);
    case Transfer::G22: v = clamp01(v); return v <= 0.04045 ? v / 12.92 : pow01((v + 0.055) / 1.055, 2.4);
    case Transfer::Pq: return pq_to_nits(v) / kWhite;
    case Transfer::Hlg: return hlg_to_scene(v);
  }
  return v;
}
// The other way, from linear light clamped to 0..`top` (HLG: from scene light).
double from_linear(Transfer t, double v, double top) {
  v = v > 0.0 ? (v < top ? v : top) : 0.0;
  switch (t) {
    case Transfer::G10: return v;
    case Transfer::G24: return pow01(v, 1.0 / 2.4);
    case Transfer::G22: return v <= 0.0031308 ? v * 12.92 : 1.055 * pow01(v, 1.0 / 2.4) - 0.055;
    case Transfer::Pq: return nits_to_pq(v * kWhite);
    case Transfer::Hlg: return scene_to_hlg(v);
  }
  return v;
}
bool is_hdr(Transfer t) { return t == Transfer::Pq || t == Transfer::Hlg; }

// BT.2390's EETF: content between black `lb` and white `lw` (cd/m2) onto a display of `lmin`..`lmax`, in PQ space: a knee and a Hermite spline to the display's peak,
// a lift towards its black. Monotonic; lw goes to lmax.
struct ToneMap {
  double src_lo = 0.0, src_range = 1.0, min_lum = 0.0, max_lum = 1.0, ks = 1.0;
  void setup(double lb, double lw, double lmin, double lmax) {
    src_lo = nits_to_pq(lb);
    src_range = nits_to_pq(lw) - src_lo;
    if (src_range <= 1e-9) src_range = 1e-9;
    min_lum = (nits_to_pq(lmin) - src_lo) / src_range;
    max_lum = (nits_to_pq(lmax) - src_lo) / src_range;
    if (min_lum < 0.0) min_lum = 0.0;
    if (max_lum > 1.0) max_lum = 1.0;
    ks = 1.5 * max_lum - 0.5;
    if (ks < 0.0) ks = 0.0;
  }
  double apply(double nits) const {
    double e1 = (nits_to_pq(nits) - src_lo) / src_range;
    e1 = e1 < 0.0 ? 0.0 : e1 > 1.0 ? 1.0 : e1;
    double e2 = e1;
    if (e1 > ks && ks < 1.0) {
      const double t = (e1 - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;
      e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1.0 - ks) + (-2 * t3 + 3 * t2) * max_lum;
    }
    const double one = 1.0 - e2;
    const double e3 = e2 + min_lum * one * one * one * one;
    return pq_to_nits(e3 * src_range + src_lo);
  }
};

struct M3 { double m[3][3] = {}; };
struct A34 { double m[3][4] = {}; };   // an affine map: out = m * (in, 1)

M3 mul(const M3& a, const M3& b) {
  M3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
  return r;
}
A34 mul(const A34& a, const A34& b) {   // a after b
  A34 r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    r.m[i][3] += a.m[i][3];
  }
  return r;
}
A34 affine(const M3& a) {
  A34 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][j];
  return r;
}
M3 identity3() { M3 r; r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0; return r; }
M3 inverse(const M3& a) {   // (the matrices here are well conditioned and invertible)
  const double (&m)[3][3] = a.m;
  const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2], c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02, inv = 1.0 / det;
  M3 r;
  r.m[0][0] = c00 * inv; r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv; r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
  r.m[1][0] = c01 * inv; r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv; r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
  r.m[2][0] = c02 * inv; r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv; r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
  return r;
}

// R'G'B' (full range, 0..1) to Y (0..1), Cb, Cr (-0.5..0.5) for a matrix, from its published Kr and Kb.
M3 rgb_to_ycbcr(Matrix matrix) {
  double kr = 0.2126, kb = 0.0722;
  if (matrix == Matrix::Bt601) kr = 0.299, kb = 0.114;
  if (matrix == Matrix::Bt2020) kr = 0.2627, kb = 0.0593;
  const double kg = 1.0 - kr - kb;
  M3 r;
  r.m[0][0] = kr; r.m[0][1] = kg; r.m[0][2] = kb;
  r.m[1][0] = -kr / (2.0 * (1.0 - kb)); r.m[1][1] = -kg / (2.0 * (1.0 - kb)); r.m[1][2] = 0.5;
  r.m[2][0] = 0.5; r.m[2][1] = -kg / (2.0 * (1.0 - kr)); r.m[2][2] = -kb / (2.0 * (1.0 - kr));
  return r;
}

// Linear RGB of a set of primaries to CIE XYZ, from the chromaticities (BT.709, BT.2020) and the D65 white point.
M3 rgb_to_xyz(Primaries p) {
  const double bt709[3][2] = {{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}};
  const double bt2020[3][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
  const double (&xy)[3][2] = p == Primaries::Bt2020 ? bt2020 : bt709;
  M3 prim;
  for (int c = 0; c < 3; ++c) {
    prim.m[0][c] = xy[c][0] / xy[c][1];
    prim.m[1][c] = 1.0;
    prim.m[2][c] = (1.0 - xy[c][0] - xy[c][1]) / xy[c][1];
  }
  const double wx = 0.3127, wy = 0.3290, white[3] = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
  const M3 inv = inverse(prim);
  for (int c = 0; c < 3; ++c) {
    const double s = inv.m[c][0] * white[0] + inv.m[c][1] * white[1] + inv.m[c][2] * white[2];
    for (int r = 0; r < 3; ++r) prim.m[r][c] *= s;
  }
  return prim;
}

// ---- planes

enum class SampleKind : uint8_t { U8, U16, Half, Packed1010102, F32 };   // (F32: the processor's own scratch planes, `floats`)

struct PlaneAccess {
  ByteView memory;
  uint64_t pitch = 0, step = 0;
  uint8_t offset[4] = {};
  SampleKind kind = SampleKind::U8;
  uint8_t shift = 0;   // U16: the value's position in the word (P010: 6)
  uint32_t width = 0, height = 0;   // the samples check_plane found there (what may be read)
  uint8_t channels = 0;
  ArrayView<float> floats;          // F32: the codes, `pitch` and `step` floats apart, channel c at c

  uint32_t le16(uint64_t at) const { return uint32_t{memory[at]} | uint32_t{memory[at + 1]} << 8; }
  uint32_t le32(uint64_t at) const { return le16(at) | le16(at + 2) << 16; }
  // Channel `c` of sample (x, y) as its code (an integer, or the float of a half).
  float read(uint32_t x, uint32_t y, uint32_t c) const {
    const uint64_t at = y * pitch + x * step;
    switch (kind) {
      case SampleKind::U8: return memory[at + offset[c]];
      case SampleKind::U16: return static_cast<float>(le16(at + offset[c]) >> shift);
      case SampleKind::Half: return graphics::half_to_float(static_cast<uint16_t>(le16(at + offset[c])));
      case SampleKind::Packed1010102: return static_cast<float>((le32(at) >> (10 * c)) & (c == 3 ? 0x3u : 0x3FFu));
      case SampleKind::F32: return floats[at + c];
    }
    return 0.0f;
  }
  void put16(uint64_t at, uint32_t v) const { memory[at] = static_cast<uint8_t>(v); memory[at + 1] = static_cast<uint8_t>(v >> 8); }
  // Channel `c` of sample (x, y) from a code already rounded and clamped (packed samples: write_packed).
  void write(uint32_t x, uint32_t y, uint32_t c, float code) const {
    const uint64_t at = y * pitch + x * step + offset[c];
    switch (kind) {
      case SampleKind::U8: memory[at] = static_cast<uint8_t>(code); return;
      case SampleKind::U16: put16(at, static_cast<uint32_t>(code) << shift); return;
      case SampleKind::Half: put16(at, graphics::float_to_half(code)); return;
      case SampleKind::Packed1010102: return;
      case SampleKind::F32: return;   // (scratch planes are written by the stages that make them)
    }
  }
  void write_packed(uint32_t x, uint32_t y, const float code[4], bool alpha) const {
    const uint64_t at = y * pitch + x * step;
    uint32_t word = static_cast<uint32_t>(code[0]) | static_cast<uint32_t>(code[1]) << 10 | static_cast<uint32_t>(code[2]) << 20;
    word |= alpha ? static_cast<uint32_t>(code[3]) << 30 : (le32(at) & 0xC0000000u);
    put16(at, word & 0xFFFF);
    put16(at + 2, word >> 16);
  }
};

// Checks a plane against what its format needs (`need_w` x `need_h` samples of `min_channels`..4 channels of `kind`) and that its memory holds them.
bool check_plane(const VpPlane& p, SampleKind kind, uint8_t bits, bool half, uint8_t min_channels, uint32_t need_w, uint32_t need_h, uint8_t shift, PlaneAccess* out,
                 uint8_t* channels) {
  if (p.bits != bits || p.half_float != half || p.channels < min_channels || p.channels > 4 || p.step == 0 || p.width < need_w || p.height < need_h || need_w == 0 || need_h == 0)
    return false;
  uint64_t extent = 4;   // the bytes of one sample that are touched
  if (kind != SampleKind::Packed1010102) {
    const uint64_t bytes = kind == SampleKind::U8 ? 1 : 2;
    extent = 0;
    for (uint32_t c = 0; c < p.channels; ++c) {
      if (p.offset[c] + bytes > p.step) return false;
      if (p.offset[c] + bytes > extent) extent = p.offset[c] + bytes;
    }
  } else if (p.step < 4) {
    return false;
  }
  if (p.height > 1 && uint64_t{p.pitch} < uint64_t{p.step} * p.width) return false;   // (rows do not overlap)
  const uint64_t needed = uint64_t{need_h - 1} * p.pitch + uint64_t{need_w - 1} * p.step + extent;   // (32-bit factors: no overflow)
  if (p.memory.size() < needed) return false;
  out->memory = p.memory;
  out->pitch = p.pitch;
  out->step = p.step;
  for (uint32_t c = 0; c < 4; ++c) out->offset[c] = p.offset[c];
  out->kind = kind;
  out->shift = shift;
  out->width = need_w;
  out->height = need_h;
  out->channels = p.channels;
  *channels = p.channels;
  return true;
}

// An image with its planes checked and what its codes mean.
struct ImageAccess {
  SpaceInfo space;
  PlaneAccess plane[2];
  uint32_t width = 0, height = 0;
  bool yuv = false, has_alpha = false, is_float = false;
  // code = nominal * scale + bias, for the first channel (Y or R, G, B), the chroma channels and alpha; codes are clamped to 0..max
  float scale[3] = {}, bias[3] = {}, max[3] = {};
};

VpResult prepare_image(const VpImage& image, ImageAccess* out) {
  if (static_cast<uint8_t>(image.format) > static_cast<uint8_t>(VpFormat::Rgba16f)) return VpResult::Invalid;
  if (!vp_color_space_supported(image.format, image.color_space)) return VpResult::Unsupported;
  if (image.width == 0 || image.height == 0) return VpResult::Invalid;
  out->space = space_info(image.color_space);
  out->width = image.width;
  out->height = image.height;
  out->yuv = vp_is_yuv(image.format);
  uint32_t depth = 8;
  uint8_t channels = 0, chroma_channels = 0;
  switch (image.format) {
    case VpFormat::Nv12:
    case VpFormat::P010:
    case VpFormat::P016: {
      const bool wide = image.format != VpFormat::Nv12;
      const SampleKind kind = wide ? SampleKind::U16 : SampleKind::U8;
      const uint8_t bits = wide ? 16 : 8, shift = image.format == VpFormat::P010 ? 6 : 0;
      depth = image.format == VpFormat::Nv12 ? 8 : image.format == VpFormat::P010 ? 10 : 16;
      if (!check_plane(image.plane[0], kind, bits, false, 1, image.width, image.height, shift, &out->plane[0], &channels)) return VpResult::Invalid;
      if (!check_plane(image.plane[1], kind, bits, false, 2, (image.width + 1) / 2, (image.height + 1) / 2, shift, &out->plane[1], &chroma_channels)) return VpResult::Invalid;
      break;
    }
    case VpFormat::Bgra8:
    case VpFormat::Bgrx8:
    case VpFormat::Rgba8:
      if (!check_plane(image.plane[0], SampleKind::U8, 8, false, 3, image.width, image.height, 0, &out->plane[0], &channels)) return VpResult::Invalid;
      out->has_alpha = channels == 4 && image.format != VpFormat::Bgrx8;
      break;
    case VpFormat::Rgb10a2:
      depth = 10;
      if (!check_plane(image.plane[0], SampleKind::Packed1010102, 10, false, 3, image.width, image.height, 0, &out->plane[0], &channels)) return VpResult::Invalid;
      out->has_alpha = channels == 4;
      break;
    case VpFormat::Rgba16f:
      if (!check_plane(image.plane[0], SampleKind::Half, 16, true, 3, image.width, image.height, 0, &out->plane[0], &channels)) return VpResult::Invalid;
      out->has_alpha = channels == 4;
      out->is_float = true;
      break;
  }
  if (out->is_float) {
    out->scale[0] = out->space.full ? 1.0f : 219.0f / 255.0f;
    out->bias[0] = out->space.full ? 0.0f : 16.0f / 255.0f;
    out->max[0] = out->max[1] = out->max[2] = 1.0f;
    if (out->space.transfer == Transfer::G10) out->max[0] = static_cast<float>(kPqPeak / kWhite);   // (linear light out of PQ / HLG keeps its brightness: Conversion::top)
    out->scale[2] = 1.0f;
  } else {
    const float max = static_cast<float>((1u << depth) - 1), k = static_cast<float>(1u << (depth - 8));
    out->max[0] = out->max[1] = max;
    if (out->space.full) {
      out->scale[0] = max; out->bias[0] = 0.0f;
      out->scale[1] = max; out->bias[1] = static_cast<float>(1u << (depth - 1));
    } else {   // studio: Y (and R, G, B) 16..235, Cb and Cr 16..240 (128 +- 112), times 2^(depth - 8)
      out->scale[0] = 219.0f * k; out->bias[0] = 16.0f * k;
      out->scale[1] = 224.0f * k; out->bias[1] = 128.0f * k;
    }
    out->max[2] = image.format == VpFormat::Rgb10a2 ? 3.0f : max;
    out->scale[2] = out->max[2];
  }
  return VpResult::Ok;
}

// Codes of channels 0..2 to the image's nominal values (Y, Cb, Cr or R', G', B').
A34 decode_affine(const ImageAccess& img) {
  A34 r;
  for (int c = 0; c < 3; ++c) {
    const int k = img.yuv && c > 0 ? 1 : 0;
    r.m[c][c] = 1.0 / img.scale[k];
    r.m[c][3] = -static_cast<double>(img.bias[k]) / img.scale[k];
  }
  return r;
}

// ---- the conversion of one stream's codes to the output's nominal values

// What a stream asks of its samples besides the colour conversion (VpStream), and the output's light levels.
struct StreamEffects {
  float brightness = 0.0f, contrast = 1.0f, hue = 0.0f, saturation = 1.0f;
  bool luma_key = false;
  float luma_lower = 0.0f, luma_upper = 0.0f;
  VpHdrMetadata hdr, display;
  bool procamp() const { return brightness != 0.0f || contrast != 1.0f || hue != 0.0f || saturation != 1.0f; }
};

struct Conversion {
  float direct[3][4] = {};   // codes -> output nominal (when `linear` is false)
  float to_rgb[3][4] = {};   // codes -> the source's R'G'B'
  float gamut[3][3] = {};    // linear source RGB -> linear output RGB
  float from_rgb[3][3] = {}; // the output's R'G'B' -> output nominal
  bool linear = false;
  Transfer from = Transfer::G22, to = Transfer::G22;
  double top = 1.0;          // the brightest linear light the output's transfer is given (HLG: scene light 1)
  bool tone_map = false;
  ToneMap curve;
  float alpha_scale = 0.0f;  // 1 / the alpha code's maximum; 0 when the source has no alpha (alpha 1)
  bool key = false;          // luma key: the luma code (key_row . codes) within key_lo..key_hi is transparent
  float key_row[3] = {}, key_lo = 0.0f, key_hi = 0.0f;

  // `out`: nominal output values and alpha (0 when keyed out). True when the luma key took the sample out.
  bool convert(const float code[4], float out[4]) const {
    if (!linear) {
      for (int i = 0; i < 3; ++i) out[i] = direct[i][0] * code[0] + direct[i][1] * code[1] + direct[i][2] * code[2] + direct[i][3];
    } else {
      double lin[3], mapped[3], rgb[3];
      for (int i = 0; i < 3; ++i) lin[i] = to_linear(from, to_rgb[i][0] * code[0] + to_rgb[i][1] * code[1] + to_rgb[i][2] * code[2] + to_rgb[i][3]);
      if (from == Transfer::Hlg) {   // the OOTF (BT.2100, gamma 1.2): scene light to the display's, over the reference white
        const double ys = 0.2627 * lin[0] + 0.6780 * lin[1] + 0.0593 * lin[2];
        const double gain = ys > 0.0 ? kHlgPeak * pow01(ys, 0.2) / kWhite : 0.0;
        for (double& v : lin) v *= gain;
      }
      if (tone_map) {   // by the brightest channel: the hue is kept and no channel passes the display's peak
        const double peak = lin[0] > lin[1] ? (lin[0] > lin[2] ? lin[0] : lin[2]) : (lin[1] > lin[2] ? lin[1] : lin[2]);
        if (peak > 0.0) {
          const double gain = curve.apply(peak * kWhite) / kWhite / peak;
          for (double& v : lin) v *= gain;
        }
      }
      for (int i = 0; i < 3; ++i) mapped[i] = gamut[i][0] * lin[0] + gamut[i][1] * lin[1] + gamut[i][2] * lin[2];
      if (to == Transfer::Hlg) {     // the inverse OOTF: display light (over the peak) to scene light, Es = Fd Yd^(-(gamma - 1) / gamma)
        for (double& v : mapped) v = v > 0.0 ? v * kWhite / kHlgPeak : 0.0;
        const double yd = clamp01(0.2627 * mapped[0] + 0.6780 * mapped[1] + 0.0593 * mapped[2]);
        const double gain = yd > 1e-12 ? exp_d(-(0.2 / 1.2) * ln_d(yd)) : 0.0;
        for (double& v : mapped) v *= gain;
      }
      for (int i = 0; i < 3; ++i) rgb[i] = from_linear(to, mapped[i], top);
      for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(from_rgb[i][0] * rgb[0] + from_rgb[i][1] * rgb[1] + from_rgb[i][2] * rgb[2]);
    }
    out[3] = alpha_scale != 0.0f ? clamp01(code[3] * alpha_scale) : 1.0f;
    if (key) {
      const float y = key_row[0] * code[0] + key_row[1] * code[1] + key_row[2] * code[2];
      if (y >= key_lo && y <= key_hi) {
        out[3] = 0.0f;
        return true;
      }
    }
    return false;
  }
};

void store(const A34& a, float (&m)[3][4]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 4; ++j) m[i][j] = static_cast<float>(a.m[i][j]);
}
void store(const M3& a, float (&m)[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m[i][j] = static_cast<float>(a.m[i][j]);
}

// ProcAmp on nominal Y, Cb, Cr (DXVA's definition: Y' = C (Y - black) + B + black, (Cb', Cr') = C S R(H) (Cb, Cr), brightness in 8-bit steps of the luma range).
A34 procamp_affine(const StreamEffects& fx, bool full) {
  double sine, cosine;
  sin_cos(static_cast<double>(fx.hue) * (3.14159265358979323846 / 180.0), &sine, &cosine);
  const double c = fx.contrast, cs = static_cast<double>(fx.contrast) * fx.saturation;
  A34 r;
  r.m[0][0] = c;
  r.m[0][3] = static_cast<double>(fx.brightness) / (full ? 255.0 : 219.0);
  r.m[1][1] = cs * cosine;
  r.m[1][2] = cs * sine;
  r.m[2][1] = -cs * sine;
  r.m[2][2] = cs * cosine;
  return r;
}

Conversion make_conversion(const ImageAccess& src, const ImageAccess& dst, const StreamEffects& fx) {
  Conversion c;
  A34 decoded = decode_affine(src);   // codes -> the source's nominal values
  const Matrix rgb_matrix = src.space.primaries == Primaries::Bt2020 ? Matrix::Bt2020 : Matrix::Bt709;
  if (fx.procamp()) {
    if (src.yuv) {
      decoded = mul(procamp_affine(fx, src.space.full), decoded);
    } else {   // on the YCbCr of the R'G'B' (full range, the primaries' matrix)
      const M3 m = rgb_to_ycbcr(rgb_matrix);
      decoded = mul(affine(inverse(m)), mul(procamp_affine(fx, true), mul(affine(m), decoded)));
    }
  }
  A34 to_rgb = decoded;
  if (src.yuv) to_rgb = mul(affine(inverse(rgb_to_ycbcr(src.space.matrix))), to_rgb);
  const M3 from_rgb = dst.yuv ? rgb_to_ycbcr(dst.space.matrix) : identity3();
  c.from = src.space.transfer;
  c.to = dst.space.transfer;
  // The light the output holds (over the reference white): an SDR output its white, a PQ one the display's peak (10000 when not known), HLG its nominal peak, linear
  // float output (scRGB-like) out of HDR all of PQ's. Brighter HDR content is tone mapped.
  double out_peak = 1.0;
  if (c.to == Transfer::Pq) out_peak = (fx.display.max_mastering > 0.0f ? fx.display.max_mastering : kPqPeak) / kWhite;
  else if (c.to == Transfer::Hlg) out_peak = kHlgPeak / kWhite;
  else if (c.to == Transfer::G10 && dst.is_float && is_hdr(c.from)) out_peak = kPqPeak / kWhite;
  c.top = c.to == Transfer::Hlg ? 1.0 : c.to == Transfer::Pq ? kPqPeak / kWhite : out_peak;
  if (is_hdr(c.from)) {
    const double content = c.from == Transfer::Hlg ? kHlgPeak
                           : fx.hdr.max_content > 0.0f ? fx.hdr.max_content : fx.hdr.max_mastering > 0.0f ? fx.hdr.max_mastering : 1000.0;
    const double black = fx.hdr.min_mastering > 0.0f ? fx.hdr.min_mastering : 0.0;
    const double display_black = is_hdr(c.to) && fx.display.min_mastering > 0.0f ? fx.display.min_mastering : 0.0;
    if (content > out_peak * kWhite * 1.0001 || display_black > black) {
      c.tone_map = true;
      c.curve.setup(black, content, display_black, out_peak * kWhite);
    }
  }
  c.linear = c.from != c.to || src.space.primaries != dst.space.primaries || c.tone_map;
  if (c.linear) {
    store(to_rgb, c.to_rgb);
    store(mul(inverse(rgb_to_xyz(dst.space.primaries)), rgb_to_xyz(src.space.primaries)), c.gamut);
    store(from_rgb, c.from_rgb);
  } else {
    store(mul(affine(from_rgb), to_rgb), c.direct);
  }
  c.alpha_scale = src.has_alpha ? 1.0f / src.max[2] : 0.0f;
  if (fx.luma_key) {   // the luma code before ProcAmp: Y's, or that of R', G', B' with the primaries' matrix
    c.key = true;
    if (src.yuv) {
      c.key_row[0] = 1.0f;
    } else {
      const M3 m = rgb_to_ycbcr(rgb_matrix);
      for (int i = 0; i < 3; ++i) c.key_row[i] = static_cast<float>(m.m[0][i]);
    }
    const float lo = clamp01(fx.luma_lower), hi = clamp01(fx.luma_upper);
    if (src.is_float) {
      c.key_lo = lo;
      c.key_hi = hi;
    } else {   // (D3D11: "BYTE(max(min(1.0, Upper), 0.0) * 255.0)" - truncated to codes)
      c.key_lo = static_cast<float>(static_cast<uint32_t>(lo * src.max[0]));
      c.key_hi = static_cast<float>(static_cast<uint32_t>(hi * src.max[0]));
    }
  }
  return c;
}

// ---- streams

struct StreamState {
  ImageAccess image;
  Conversion conversion;
  VpRect source, draw;           // draw: the destination clipped to the target
  int64_t dst_left = 0, dst_top = 0, dst_w = 0, dst_h = 0;
  VpOrientation orientation = VpOrientation::Default;
  bool blend = false, alpha_source = false, active = false;
  float alpha = 1.0f;
  double anamorphic = 0.0;                 // the non-linear stretch: destination t in -1..1 shows source t (1 + k (1 - |t|))
  double chroma_x = 0.0, chroma_y = 0.0;   // the luma position of chroma sample 0 (0 co-sited, 0.5 centred)

  // The source position (in its pixels, centres at integers) that output pixel (x, y) shows.
  void source_position(int32_t x, int32_t y, double* sx, double* sy) const {
    double a = static_cast<double>(2 * (x - dst_left) + 1);
    const double b = static_cast<double>(2 * (y - dst_top) + 1);
    const double du = static_cast<double>(2 * dst_w), dv = static_cast<double>(2 * dst_h);
    if (anamorphic != 0.0) {   // (across the destination, before the orientation: the stretch is the display's)
      const double t = 2.0 * a / du - 1.0, at = t < 0.0 ? -t : t;
      a = (t * (1.0 + anamorphic * (1.0 - at)) + 1.0) * 0.5 * du;
    }
    // u = a / du, v = b / dv across the destination; s, t across the source, per orientation (rotations come first, then the horizontal flip, as D3D12 says)
    double sn = a, sd = du, tn = b, td = dv;
    switch (orientation) {
      case VpOrientation::Default: break;
      case VpOrientation::FlipHorizontal: sn = du - a; break;
      case VpOrientation::Clockwise90: sn = b; sd = dv; tn = du - a; td = du; break;
      case VpOrientation::Clockwise90FlipHorizontal: sn = b; sd = dv; tn = a; td = du; break;
      case VpOrientation::Clockwise180: sn = du - a; tn = dv - b; break;
      case VpOrientation::FlipVertical: tn = dv - b; break;
      case VpOrientation::Clockwise270: sn = dv - b; sd = dv; tn = a; td = du; break;
      case VpOrientation::Clockwise270FlipHorizontal: sn = dv - b; sd = dv; tn = du - a; td = du; break;
    }
    const double sw = static_cast<double>(source.right - source.left), sh = static_cast<double>(source.bottom - source.top);
    *sx = source.left + sn * sw / sd - 0.5;
    *sy = source.top + tn * sh / td - 0.5;
  }

  // Bilinear sample of the source at (sx, sy), clamped to the source rectangle: codes of channels 0..3.
  void sample(double sx, double sy, float code[4]) const {
    const double lo_x = source.left, hi_x = source.right - 1, lo_y = source.top, hi_y = source.bottom - 1;
    sx = sx < lo_x ? lo_x : sx > hi_x ? hi_x : sx;
    sy = sy < lo_y ? lo_y : sy > hi_y ? hi_y : sy;
    const double fx0 = floor_d(sx), fy0 = floor_d(sy);
    const uint32_t x0 = static_cast<uint32_t>(fx0), y0 = static_cast<uint32_t>(fy0);
    const uint32_t x1 = x0 + 1 < static_cast<uint32_t>(source.right) ? x0 + 1 : x0, y1 = y0 + 1 < static_cast<uint32_t>(source.bottom) ? y0 + 1 : y0;
    const float wx = static_cast<float>(sx - fx0), wy = static_cast<float>(sy - fy0);
    const PlaneAccess& p0 = image.plane[0];
    auto bilinear = [&](const PlaneAccess& p, uint32_t ax, uint32_t bx, uint32_t ay, uint32_t by, float fx, float fy, uint32_t c) {
      const float top = p.read(ax, ay, c) * (1.0f - fx) + p.read(bx, ay, c) * fx;
      const float bottom = p.read(ax, by, c) * (1.0f - fx) + p.read(bx, by, c) * fx;
      return top * (1.0f - fy) + bottom * fy;
    };
    if (!image.yuv) {
      for (uint32_t c = 0; c < 3; ++c) code[c] = bilinear(p0, x0, x1, y0, y1, wx, wy, c);
      code[3] = image.has_alpha ? bilinear(p0, x0, x1, y0, y1, wx, wy, 3) : 0.0f;
      return;
    }
    code[0] = bilinear(p0, x0, x1, y0, y1, wx, wy, 0);
    code[3] = 0.0f;
    // chroma sample i sits at luma position 2 i + chroma_x: (clamped to the chroma samples of the source rectangle)
    double cx = (sx - chroma_x) * 0.5, cy = (sy - chroma_y) * 0.5;
    const double clo_x = source.left / 2, chi_x = (source.right - 1) / 2, clo_y = source.top / 2, chi_y = (source.bottom - 1) / 2;
    cx = cx < clo_x ? clo_x : cx > chi_x ? chi_x : cx;
    cy = cy < clo_y ? clo_y : cy > chi_y ? chi_y : cy;
    const double fcx = floor_d(cx), fcy = floor_d(cy);
    const uint32_t cx0 = static_cast<uint32_t>(fcx), cy0 = static_cast<uint32_t>(fcy);
    const uint32_t cx1 = cx0 + 1 <= static_cast<uint32_t>(chi_x) ? cx0 + 1 : cx0, cy1 = cy0 + 1 <= static_cast<uint32_t>(chi_y) ? cy0 + 1 : cy0;
    const float wcx = static_cast<float>(cx - fcx), wcy = static_cast<float>(cy - fcy);
    code[1] = bilinear(image.plane[1], cx0, cx1, cy0, cy1, wcx, wcy, 0);
    code[2] = bilinear(image.plane[1], cx0, cx1, cy0, cy1, wcx, wcy, 1);
  }
};

double siting_x(Siting s) { return s == Siting::Centre ? 0.5 : 0.0; }
double siting_y(Siting s) { return s == Siting::TopLeft ? 0.0 : 0.5; }

bool rect_ordered(const VpRect& r) { return r.left <= r.right && r.top <= r.bottom; }
bool rect_empty(const VpRect& r) { return r.left >= r.right || r.top >= r.bottom; }
VpRect intersect(const VpRect& a, const VpRect& b) {
  VpRect r{a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top, a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom};
  if (r.right < r.left) r.right = r.left;
  if (r.bottom < r.top) r.bottom = r.top;
  return r;
}

// ---- the output

struct Px { float v[4]; };
constexpr int32_t kChunk = 32;               // luma columns per chunk (16 chroma samples)
constexpr int32_t kLine = kChunk + 2;        // plus the chroma filter's column on either side

enum class AlphaRule : uint8_t { One, Background, Keep, FromStream };

struct Context {
  ImageAccess out;
  VpRect target;
  float background[4] = {};   // nominal output values
  AlphaRule alpha_rule = AlphaRule::One;
  StreamState* streams = nullptr;
  uint32_t stream_count = 0;
  uint32_t alpha_stream = 0;
  bool write_alpha = false;   // an RGB output with an alpha channel

  bool in_target(int32_t x, int32_t y) const { return x >= target.left && x < target.right && y >= target.top && y < target.bottom; }

  // The output as it is at (x, y), in nominal values (its alpha: 1 when it has none).
  void existing(int32_t x, int32_t y, Px* px) const {
    const uint32_t ux = static_cast<uint32_t>(x), uy = static_cast<uint32_t>(y);
    if (out.yuv) {
      px->v[0] = (out.plane[0].read(ux, uy, 0) - out.bias[0]) / out.scale[0];
      px->v[1] = (out.plane[1].read(ux / 2, uy / 2, 0) - out.bias[1]) / out.scale[1];
      px->v[2] = (out.plane[1].read(ux / 2, uy / 2, 1) - out.bias[1]) / out.scale[1];
      px->v[3] = 1.0f;
      return;
    }
    for (uint32_t c = 0; c < 3; ++c) px->v[c] = (out.plane[0].read(ux, uy, c) - out.bias[0]) / out.scale[0];
    px->v[3] = out.has_alpha ? out.plane[0].read(ux, uy, 3) / out.max[2] : 1.0f;
  }

  // Row y, columns x0..x1 (exclusive, at most kLine) into line[0..]: the output as it was outside the target, the background and the streams inside it.
  void compute_row(int32_t y, int32_t x0, int32_t x1, Px* line) const {
    const bool row_in = y >= target.top && y < target.bottom;
    for (int32_t x = x0; x < x1; ++x) {
      Px& px = line[x - x0];
      if (!row_in || x < target.left || x >= target.right) { existing(x, y, &px); continue; }
      for (int c = 0; c < 3; ++c) px.v[c] = background[c];
      if (alpha_rule == AlphaRule::Keep) {
        Px old;
        existing(x, y, &old);
        px.v[3] = old.v[3];
      } else {
        px.v[3] = alpha_rule == AlphaRule::One ? 1.0f : background[3];
      }
    }
    if (!row_in) return;
    for (uint32_t s = 0; s < stream_count; ++s) {
      const StreamState& st = streams[s];
      if (!st.active || y < st.draw.top || y >= st.draw.bottom) continue;
      const int32_t from = st.draw.left > x0 ? st.draw.left : x0, to = st.draw.right < x1 ? st.draw.right : x1;
      const bool alpha_from_this = alpha_rule == AlphaRule::FromStream && s == alpha_stream;
      for (int32_t x = from; x < to; ++x) {
        double sx, sy;
        st.source_position(x, y, &sx, &sy);
        float code[4], value[4];
        st.sample(sx, sy, code);
        const bool keyed = st.conversion.convert(code, value);
        Px& px = line[x - x0];
        const float a = keyed ? 0.0f : st.blend ? st.alpha * value[3] : 1.0f;
        for (int c = 0; c < 3; ++c) px.v[c] = value[c] * a + px.v[c] * (1.0f - a);
        if (alpha_from_this) px.v[3] = value[3];
      }
    }
  }

  float code_of(float nominal, int k) const {
    float v = nominal * out.scale[k] + out.bias[k];
    if (!(v >= 0.0f)) v = 0.0f;   // (and NaN)
    if (v > out.max[k]) v = out.max[k];
    return out.is_float ? v : static_cast<float>(static_cast<uint32_t>(v + 0.5f));
  }

  void write_rgb_row(int32_t y, int32_t x0, int32_t x1, const Px* line) const {
    const PlaneAccess& p = out.plane[0];
    for (int32_t x = x0; x < x1; ++x) {
      const Px& px = line[x - x0];
      float code[4];
      for (int c = 0; c < 3; ++c) code[c] = code_of(px.v[c], 0);
      code[3] = code_of(px.v[3], 2);
      const uint32_t ux = static_cast<uint32_t>(x), uy = static_cast<uint32_t>(y);
      if (p.kind == SampleKind::Packed1010102) {
        p.write_packed(ux, uy, code, write_alpha);
      } else {
        for (uint32_t c = 0; c < 3; ++c) p.write(ux, uy, c, code[c]);
        if (write_alpha) p.write(ux, uy, 3, code[3]);
      }
    }
  }

  void run_rgb() const {
    Px line[kLine];
    for (int32_t y = target.top; y < target.bottom; ++y)
      for (int32_t x0 = target.left; x0 < target.right; x0 += kChunk) {
        const int32_t x1 = target.right - x0 > kChunk ? x0 + kChunk : target.right;
        compute_row(y, x0, x1, line);
        write_rgb_row(y, x0, x1, line);
      }
  }

  void run_yuv() const {
    const int32_t w = static_cast<int32_t>(out.width), h = static_cast<int32_t>(out.height);
    const Siting siting = out.space.siting;
    const int32_t ci0 = target.left / 2, ci1 = (target.right + 1) / 2, cj0 = target.top / 2, cj1 = (target.bottom + 1) / 2;
    Px rows[3][kLine];
    int32_t row_of[3];
    for (int32_t i0 = ci0; i0 < ci1; i0 += kChunk / 2) {
      const int32_t i1 = ci1 - i0 > kChunk / 2 ? i0 + kChunk / 2 : ci1;
      const int32_t lx0 = 2 * i0 - 1 < 0 ? 0 : 2 * i0 - 1, lx1 = 2 * i1 < w ? 2 * i1 : w;
      const int32_t own0 = 2 * i0 > target.left ? 2 * i0 : target.left, own1 = 2 * i1 < target.right ? 2 * i1 : target.right;   // the luma this chunk writes
      row_of[0] = row_of[1] = row_of[2] = -1;
      auto row = [&](int32_t r) -> const Px* {
        r = r < 0 ? 0 : r >= h ? h - 1 : r;
        const int32_t slot = r % 3;
        if (row_of[slot] != r) {
          row_of[slot] = r;
          compute_row(r, lx0, lx1, rows[slot]);
          if (r >= target.top && r < target.bottom)
            for (int32_t x = own0; x < own1; ++x)
              out.plane[0].write(static_cast<uint32_t>(x), static_cast<uint32_t>(r), 0, code_of(rows[slot][x - lx0].v[0], 0));
        }
        return rows[slot];
      };
      auto column = [&](int32_t x) { return (x < 0 ? 0 : x >= w ? w - 1 : x) - lx0; };
      for (int32_t j = cj0; j < cj1; ++j) {
        // the rows of chroma row j and their weights (in order: rows are computed top to bottom, so a row's old chroma is read before it is replaced)
        const Px* taps[3];
        float vw[3];
        int vn = 0;
        if (siting == Siting::TopLeft) {
          taps[0] = row(2 * j - 1); taps[1] = row(2 * j); taps[2] = row(2 * j + 1);
          vw[0] = 0.25f; vw[1] = 0.5f; vw[2] = 0.25f; vn = 3;
        } else {
          taps[0] = row(2 * j); taps[1] = row(2 * j + 1);
          vw[0] = 0.5f; vw[1] = 0.5f; vn = 2;
        }
        for (int32_t i = i0; i < i1; ++i) {
          int32_t cols[3];
          float hw[3];
          int hn = 0;
          if (siting == Siting::Centre) {
            cols[0] = column(2 * i); cols[1] = column(2 * i + 1);
            hw[0] = 0.5f; hw[1] = 0.5f; hn = 2;
          } else {
            cols[0] = column(2 * i - 1); cols[1] = column(2 * i); cols[2] = column(2 * i + 1);
            hw[0] = 0.25f; hw[1] = 0.5f; hw[2] = 0.25f; hn = 3;
          }
          float cb = 0.0f, cr = 0.0f;
          for (int a = 0; a < vn; ++a)
            for (int b = 0; b < hn; ++b) {
              const Px& px = taps[a][cols[b]];
              cb += vw[a] * hw[b] * px.v[1];
              cr += vw[a] * hw[b] * px.v[2];
            }
          out.plane[1].write(static_cast<uint32_t>(i), static_cast<uint32_t>(j), 0, code_of(cb, 1));
          out.plane[1].write(static_cast<uint32_t>(i), static_cast<uint32_t>(j), 1, code_of(cr, 1));
        }
      }
    }
  }
};

#include "Hardware/VideoCodec/VideoProcessStages.inc"

bool finite_in(float v, float lo, float hi) { return v >= lo && v <= hi; }   // (false for NaN)
bool hdr_ok(const VpHdrMetadata& m) {
  return finite_in(m.max_mastering, 0.0f, 1e6f) && finite_in(m.min_mastering, 0.0f, 1e6f) && finite_in(m.max_content, 0.0f, 1e6f) && finite_in(m.max_frame_average, 0.0f, 1e6f);
}
// The ranges of a stream's interlacing, stereo and filter values (VpStream).
bool stream_effects_ok(const VpStream& in) {
  if (static_cast<uint8_t>(in.field_order) > static_cast<uint8_t>(VpFieldOrder::BottomFieldFirst) || static_cast<uint8_t>(in.deinterlace) > static_cast<uint8_t>(VpDeinterlace::Adaptive) ||
      in.field > 1 || static_cast<uint8_t>(in.stereo) > static_cast<uint8_t>(VpStereoFormat::MonoOffset) || in.stereo_frame > 1)
    return false;
  if (in.stereo_offset < -65536 || in.stereo_offset > 65536) return false;
  if (!finite_in(in.brightness, -100.0f, 100.0f) || !finite_in(in.contrast, 0.0f, 10.0f) || !finite_in(in.hue, -180.0f, 180.0f) || !finite_in(in.saturation, 0.0f, 10.0f) ||
      !finite_in(in.noise_reduction, 0.0f, 1.0f) || !finite_in(in.edge_enhancement, 0.0f, 1.0f) || !finite_in(in.anamorphic, 0.0f, 1.0f))
    return false;
  if (in.luma_key && !(finite_in(in.luma_lower, 0.0f, 1.0f) && finite_in(in.luma_upper, in.luma_lower, 1.0f))) return false;
  return hdr_ok(in.hdr);
}

}  // namespace

bool vp_color_space_supported(VpFormat format, VpColorSpace space) {
  if (static_cast<uint8_t>(format) > static_cast<uint8_t>(VpFormat::Rgba16f)) return false;
  const SpaceInfo info = space_info(space);
  return info.known && info.yuv == vp_is_yuv(format);
}

VpResult video_process(const VpOutput& output, ArrayView<const VpStream> streams) {
  if (!output.image) return VpResult::Invalid;
  if (streams.size() > kVpMaxStreams) return VpResult::Unsupported;
  Context ctx;
  VpResult r = prepare_image(*output.image, &ctx.out);
  if (r != VpResult::Ok) return r;
  if (!rect_ordered(output.target)) return VpResult::Invalid;
  for (float v : output.background)
    if (!(v >= 0.0f && v <= 1.0f)) return VpResult::Invalid;
  if (static_cast<uint8_t>(output.alpha_fill) > static_cast<uint8_t>(VpAlphaFill::SourceStream)) return VpResult::Invalid;
  if (output.alpha_fill == VpAlphaFill::SourceStream && output.alpha_fill_stream >= streams.size()) return VpResult::Invalid;
  ctx.target = intersect(output.target, VpRect{0, 0, static_cast<int32_t>(ctx.out.width < 0x7FFFFFFFu ? ctx.out.width : 0x7FFFFFFFu),
                                              static_cast<int32_t>(ctx.out.height < 0x7FFFFFFFu ? ctx.out.height : 0x7FFFFFFFu)});

  if (!hdr_ok(output.hdr)) return VpResult::Invalid;
  StreamState states[kVpMaxStreams];
  ScratchPlanes scratch[kVpMaxStreams][2];   // (the stages' pictures: given back when the call returns)
  for (uint32_t s = 0; s < streams.size(); ++s) {
    const VpStream& in = streams[s];
    StreamState& st = states[s];
    if (!in.image) return VpResult::Invalid;
    r = prepare_image(*in.image, &st.image);
    if (r != VpResult::Ok) return r;
    if (!stream_effects_ok(in)) return VpResult::Invalid;
    if (needs_stages(in) && !rect_empty(in.source) && !rect_empty(in.destination)) {
      uint32_t vw = 0, vh = 0;
      view_size(in.stereo, st.image.width, st.image.height, &vw, &vh);
      if (!rect_ordered(in.source) || in.source.left < 0 || in.source.top < 0 || static_cast<int64_t>(in.source.right) > static_cast<int64_t>(vw) ||
          static_cast<int64_t>(in.source.bottom) > static_cast<int64_t>(vh))
        return VpResult::Invalid;
      // the frames before and after: pictures like the current one
      ImageAccess refs[2];
      const VpImage* given[2] = {in.past, in.future};
      for (int k = 0; k < 2; ++k) {
        if (!given[k]) continue;
        if (given[k]->format != in.image->format || given[k]->color_space != in.image->color_space || given[k]->width != in.image->width || given[k]->height != in.image->height)
          return VpResult::Invalid;
        r = prepare_image(*given[k], &refs[k]);
        if (r != VpResult::Ok) return r;
      }
      r = run_stages(in, &st.image, in.past ? &refs[0] : nullptr, in.future ? &refs[1] : nullptr, output.allocator, scratch[s]);
      if (r != VpResult::Ok) return r;
    }
    if (!rect_ordered(in.source) || !rect_ordered(in.destination) || in.source.left < 0 || in.source.top < 0 ||
        static_cast<int64_t>(in.source.right) > static_cast<int64_t>(st.image.width) || static_cast<int64_t>(in.source.bottom) > static_cast<int64_t>(st.image.height))
      return VpResult::Invalid;
    if (static_cast<uint8_t>(in.orientation) > static_cast<uint8_t>(VpOrientation::Clockwise270FlipHorizontal)) return VpResult::Invalid;
    if (!(in.alpha >= 0.0f && in.alpha <= 1.0f)) return VpResult::Invalid;
    StreamEffects fx;
    fx.brightness = in.brightness;
    fx.contrast = in.contrast;
    fx.hue = in.hue;
    fx.saturation = in.saturation;
    fx.luma_key = in.luma_key;
    fx.luma_lower = in.luma_lower;
    fx.luma_upper = in.luma_upper;
    fx.hdr = in.hdr;
    fx.display = output.hdr;
    st.conversion = make_conversion(st.image, ctx.out, fx);
    st.anamorphic = 0.5 * in.anamorphic;
    st.source = in.source;
    st.draw = intersect(in.destination, ctx.target);
    st.dst_left = in.destination.left;
    st.dst_top = in.destination.top;
    st.dst_w = int64_t{in.destination.right} - in.destination.left;
    st.dst_h = int64_t{in.destination.bottom} - in.destination.top;
    st.orientation = in.orientation;
    st.blend = in.alpha_blend;
    st.alpha = in.alpha;
    st.chroma_x = siting_x(st.image.space.siting);
    st.chroma_y = siting_y(st.image.space.siting);
    st.active = !rect_empty(in.source) && !rect_empty(in.destination) && !rect_empty(st.draw);
  }
  ctx.streams = states;
  ctx.stream_count = static_cast<uint32_t>(streams.size());

  switch (output.alpha_fill) {
    case VpAlphaFill::Opaque: ctx.alpha_rule = AlphaRule::One; break;
    case VpAlphaFill::Background: ctx.alpha_rule = AlphaRule::Background; break;
    case VpAlphaFill::Destination: ctx.alpha_rule = AlphaRule::Keep; break;
    case VpAlphaFill::SourceStream: {
      const VpStream& in = streams[output.alpha_fill_stream];
      ctx.alpha_stream = output.alpha_fill_stream;
      if (rect_empty(in.source)) ctx.alpha_rule = AlphaRule::Keep;                                     // (D3D: "not modified")
      else if (!states[output.alpha_fill_stream].image.has_alpha) ctx.alpha_rule = AlphaRule::One;    // (D3D: no alpha data: opaque)
      else ctx.alpha_rule = AlphaRule::FromStream;
      break;
    }
  }
  ctx.write_alpha = !ctx.out.yuv && ctx.out.has_alpha;

  // The background in the output's nominal values.
  const float* bg = output.background;
  if (ctx.out.yuv) {
    if (output.background_is_yuv) {
      ctx.background[0] = bg[0]; ctx.background[1] = bg[1] - 0.5f; ctx.background[2] = bg[2] - 0.5f;
    } else {
      const M3 m = rgb_to_ycbcr(ctx.out.space.matrix);
      for (int i = 0; i < 3; ++i) ctx.background[i] = static_cast<float>(m.m[i][0] * bg[0] + m.m[i][1] * bg[1] + m.m[i][2] * bg[2]);
    }
  } else if (output.background_is_yuv) {   // YCbCr with the matrix of the output's primaries
    const M3 m = inverse(rgb_to_ycbcr(ctx.out.space.primaries == Primaries::Bt2020 ? Matrix::Bt2020 : Matrix::Bt709));
    const double yuv[3] = {bg[0], bg[1] - 0.5, bg[2] - 0.5};
    for (int i = 0; i < 3; ++i) ctx.background[i] = clamp01(static_cast<float>(m.m[i][0] * yuv[0] + m.m[i][1] * yuv[1] + m.m[i][2] * yuv[2]));
  } else {
    for (int i = 0; i < 3; ++i) ctx.background[i] = bg[i];
  }
  ctx.background[3] = bg[3];

  if (rect_empty(ctx.target)) return VpResult::Ok;
  if (ctx.out.yuv) ctx.run_yuv();
  else ctx.run_rgb();
  return VpResult::Ok;
}

}  // namespace hardware
