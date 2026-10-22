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

// Opus CELT decoder, 48 kHz, float arithmetic. A port of the floating-point decoder of libopus v1.4 (BSD-3-Clause; Copyright Xiph.Org
// Foundation, CSIRO, Mark Borgerding, Jean-Marc Valin, Timothy B. Terriberry, Gregory Maxwell), whose constant tables (OpusTables.inc)
// are generated from its sources. The band kernels work on pointers into fixed-size buffers of the decoder object, as the transform
// code in Aac.cpp does; every buffer length is a compile-time constant and the sizes that depend on the input are bounded below it.
#include "Math/FloatMath.hpp"
#include "Media/Audio/OpusCelt.hpp"

namespace media {
namespace opus {

#include "Media/Audio/OpusCeltTables.inc"

namespace {

constexpr int kBitRes = 3;
constexpr int kMaxFineBits = 8;
constexpr int kFineOffset = 21;
constexpr int kQthetaOffset = 4;
constexpr int kQthetaOffsetTwophase = 16;
constexpr int kMaxPseudo = 40;
constexpr int kLogMaxPseudo = 6;
constexpr int kAllocSteps = 6;
constexpr int kLpcOrderUnused = 24;
constexpr float kEpsilon = 1e-15f;
constexpr float kPi = 3.141592653f;
constexpr int kSpreadNone = 0, kSpreadLight = 1, kSpreadNormal = 2, kSpreadAggressive = 3;
constexpr int kCombMinPeriod = 15;

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int32_t i32min(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t i32max(int32_t a, int32_t b) { return a > b ? a : b; }
inline float fmin_(float a, float b) { return a < b ? a : b; }
inline float fabs_(float x) { return x < 0 ? -x : x; }
inline float csqrt(float x) { return static_cast<float>(fmath::sqrt(x)); }
inline float crsqrt(float x) { return 1.f / csqrt(x); }
inline float cexp2(float x) { return static_cast<float>(fmath::exp(0.6931471805599453094 * x)); }
inline float ccos_norm(float x) { return static_cast<float>(fmath::cos((.5f * kPi) * x)); }
inline uint32_t lcg_rand(uint32_t seed) { return 1664525u * seed + 1013904223u; }
inline int ilog2(uint32_t v) { return RangeDecoder::ilog(v); }

// Cell of a flattened table of the generated tables.
inline int16_t eband(int i) { return kCeltEBands[i]; }

float eMeans(int i) {
  static const float kMeans[25] = {6.437500f, 6.250000f, 5.750000f, 5.312500f, 5.062500f, 4.812500f, 4.500000f, 4.375000f, 4.875000f,
                                   4.687500f, 4.562500f, 4.437500f, 4.875000f, 4.625000f, 4.312500f, 4.500000f, 4.375000f, 4.625000f,
                                   4.750000f, 4.437500f, 3.750000f, 3.750000f, 3.750000f, 3.750000f, 3.750000f};
  return kMeans[i];
}

// ---- pulses and the bit cache ------------------------------------------------------------------------------------------------

inline int get_pulses(int i) { return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1); }

inline const uint8_t* cache_for(int band, int LM) { return kCeltCacheBits + kCeltCacheIndex[(LM + 1) * CeltDecoder::kBands + band]; }

int bits2pulses(int band, int LM, int bits) {
  const uint8_t* cache = cache_for(band, LM);
  int lo = 0, hi = cache[0];
  bits--;
  for (int i = 0; i < kLogMaxPseudo; ++i) {
    const int mid = (lo + hi + 1) >> 1;
    if (static_cast<int>(cache[mid]) >= bits) hi = mid;
    else lo = mid;
  }
  if (bits - (lo == 0 ? -1 : static_cast<int>(cache[lo])) <= static_cast<int>(cache[hi]) - bits) return lo;
  return hi;
}

int pulses2bits(int band, int LM, int pulses) { return pulses == 0 ? 0 : cache_for(band, LM)[pulses] + 1; }

// ---- combinatorial pulse vectors (cwrs.c, the table-free variant) -----------------------------------------------------------

void unext(uint32_t* ui, unsigned len, uint32_t ui0) {
  unsigned j = 1;
  do {
    const uint32_t ui1 = ui[j] + ui[j - 1] + ui0;
    ui[j - 1] = ui0;
    ui0 = ui1;
  } while (++j < len);
  ui[j - 1] = ui0;
}

void uprev(uint32_t* ui, unsigned n, uint32_t ui0) {
  unsigned j = 1;
  do {
    const uint32_t ui1 = ui[j] - ui[j - 1] - ui0;
    ui[j - 1] = ui0;
    ui0 = ui1;
  } while (++j < n);
  ui[j - 1] = ui0;
}

// The number of pulse vectors of n dimensions with k pulses, with the row of U values left in `u` (k + 2 entries).
uint32_t ncwrs_urow(unsigned n, unsigned k, uint32_t* u) {
  const unsigned len = k + 2;
  u[0] = 0;
  u[1] = 1;
  for (unsigned i = 2; i < len; ++i) u[i] = (i << 1) - 1;
  for (unsigned i = 2; i < n; i++) unext(u + 1, k + 1, 1);
  return u[k] + u[k + 1];
}

// The pulse vector with index i; returns its energy.
float cwrsi(int n, int k, uint32_t i, int* y, uint32_t* u) {
  float yy = 0;
  int j = 0;
  do {
    uint32_t p = u[k + 1];
    const int s = -(i >= p);
    i -= p & static_cast<uint32_t>(s);
    int yj = k;
    p = u[k];
    while (p > i) p = u[--k];
    i -= p;
    yj -= k;
    const int val = (yj + s) ^ s;
    y[j] = val;
    yy += static_cast<float>(val * val);
    uprev(u, static_cast<unsigned>(k) + 2, 0);
  } while (++j < n);
  return yy;
}

float decode_pulses(int* y, int n, int k, RangeDecoder& dec) {
  uint32_t u[CeltDecoder::kMaxFrame / 4 + 140];
  const uint32_t nc = ncwrs_urow(static_cast<unsigned>(n), static_cast<unsigned>(k), u);
  return cwrsi(n, k, dec.uint_(nc), y, u);
}

// ---- spreading rotation and PVQ (vq.c) ----------------------------------------------------------------------------------------

void exp_rotation1(float* X, int len, int stride, float c, float s) {
  const float ms = -s;
  float* Xptr = X;
  for (int i = 0; i < len - stride; i++) {
    const float x1 = Xptr[0], x2 = Xptr[stride];
    Xptr[stride] = c * x2 + s * x1;
    *Xptr++ = c * x1 + ms * x2;
  }
  Xptr = &X[len - 2 * stride - 1];
  for (int i = len - 2 * stride - 1; i >= 0; i--) {
    const float x1 = Xptr[0], x2 = Xptr[stride];
    Xptr[stride] = c * x2 + s * x1;
    *Xptr-- = c * x1 + ms * x2;
  }
}

void exp_rotation(float* X, int len, int dir, int stride, int K, int spread) {
  static const int kSpreadFactor[3] = {15, 10, 5};
  if (2 * K >= len || spread == kSpreadNone) return;
  const int factor = kSpreadFactor[spread - 1];
  const float gain = static_cast<float>(len) / static_cast<float>(len + factor * K);
  const float theta = .5f * gain * gain;
  const float c = ccos_norm(theta), s = ccos_norm(1.f - theta);
  int stride2 = 0;
  if (len >= 8 * stride) {
    stride2 = 1;
    while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len) stride2++;
  }
  len = len / stride;
  for (int i = 0; i < stride; i++) {
    if (dir < 0) {
      if (stride2) exp_rotation1(X + i * len, len, stride2, s, c);
      exp_rotation1(X + i * len, len, 1, c, s);
    } else {
      exp_rotation1(X + i * len, len, 1, c, -s);
      if (stride2) exp_rotation1(X + i * len, len, stride2, s, -c);
    }
  }
}

unsigned extract_collapse_mask(const int* iy, int N, int B) {
  if (B <= 1) return 1;
  const int N0 = N / B;
  unsigned collapse_mask = 0;
  for (int i = 0; i < B; i++) {
    unsigned tmp = 0;
    for (int j = 0; j < N0; j++) tmp |= static_cast<unsigned>(iy[i * N0 + j]);
    collapse_mask |= (tmp != 0) << i;
  }
  return collapse_mask;
}

unsigned alg_unquant(float* X, int N, int K, int spread, int B, RangeDecoder& dec, float gain) {
  int iy[CeltDecoder::kMaxFrame];
  const float Ryy = decode_pulses(iy, N, K, dec);
  const float g = crsqrt(Ryy) * gain;
  for (int i = 0; i < N; i++) X[i] = g * static_cast<float>(iy[i]);
  exp_rotation(X, N, -1, B, K, spread);
  return extract_collapse_mask(iy, N, B);
}

void renormalise_vector(float* X, int N, float gain) {
  float E = kEpsilon;
  for (int i = 0; i < N; i++) E += X[i] * X[i];
  const float g = crsqrt(E) * gain;
  for (int i = 0; i < N; i++) X[i] *= g;
}

// ---- integer approximations the bit allocation depends on --------------------------------------------------------------------

inline int frac_mul16(int a, int b) { return (16384 + (static_cast<int16_t>(a) * static_cast<int16_t>(b))) >> 15; }

int bitexact_cos(int x16) {
  const int32_t tmp = (4096 + (static_cast<int32_t>(static_cast<int16_t>(x16)) * static_cast<int16_t>(x16))) >> 13;
  int x2 = static_cast<int16_t>(tmp);
  x2 = (32767 - x2) + frac_mul16(x2, (-7651 + frac_mul16(x2, (8277 + frac_mul16(-626, x2)))));
  return 1 + x2;
}

int bitexact_log2tan(int isin, int icos) {
  const int lc = RangeDecoder::ilog(static_cast<uint32_t>(icos));
  const int ls = RangeDecoder::ilog(static_cast<uint32_t>(isin));
  icos <<= 15 - lc;
  isin <<= 15 - ls;
  return (ls - lc) * (1 << 11) + frac_mul16(isin, frac_mul16(isin, -2597) + 7932) - frac_mul16(icos, frac_mul16(icos, -2597) + 7932);
}

unsigned isqrt32(uint32_t val) {
  unsigned g = 0;
  int bshift = (RangeDecoder::ilog(val) - 1) >> 1;
  unsigned b = 1u << bshift;
  do {
    const uint32_t t = ((static_cast<uint32_t>(g) << 1) + b) << bshift;
    if (t <= val) {
      g += b;
      val -= t;
    }
    b >>= 1;
    bshift--;
  } while (bshift >= 0);
  return g;
}

// ---- bit allocation (rate.c) -------------------------------------------------------------------------------------------------

int interp_bits2pulses(int start, int end, int skip_start, const int* bits1, const int* bits2, const int* thresh, const int* cap,
                       int32_t total, int32_t* balance_out, int skip_rsv, int* intensity, int intensity_rsv, int* dual_stereo,
                       int dual_stereo_rsv, int* bits, int* ebits, int* fine_priority, int C, int LM, RangeDecoder& ec) {
  const int alloc_floor = C << kBitRes;
  const int stereo = C > 1;
  const int logM = LM << kBitRes;
  int lo = 0, hi = 1 << kAllocSteps;
  for (int i = 0; i < kAllocSteps; i++) {
    const int mid = (lo + hi) >> 1;
    int32_t psum = 0;
    int done = 0;
    for (int j = end; j-- > start;) {
      const int tmp = bits1[j] + (mid * static_cast<int32_t>(bits2[j]) >> kAllocSteps);
      if (tmp >= thresh[j] || done) {
        done = 1;
        psum += imin(tmp, cap[j]);
      } else if (tmp >= alloc_floor) {
        psum += alloc_floor;
      }
    }
    if (psum > total) hi = mid;
    else lo = mid;
  }
  int32_t psum = 0;
  int done = 0;
  for (int j = end; j-- > start;) {
    int tmp = bits1[j] + (static_cast<int32_t>(lo) * bits2[j] >> kAllocSteps);
    if (tmp < thresh[j] && !done) {
      tmp = tmp >= alloc_floor ? alloc_floor : 0;
    } else {
      done = 1;
    }
    tmp = imin(tmp, cap[j]);
    bits[j] = tmp;
    psum += tmp;
  }
  int codedBands;
  for (codedBands = end;; codedBands--) {
    const int j = codedBands - 1;
    if (j <= skip_start) {
      total += skip_rsv;
      break;
    }
    int32_t left = total - psum;
    const int32_t percoeff = left / (eband(codedBands) - eband(start));
    left -= (eband(codedBands) - eband(start)) * percoeff;
    const int rem = imax(left - (eband(j) - eband(start)), 0);
    const int band_width = eband(codedBands) - eband(j);
    int band_bits = static_cast<int>(bits[j] + percoeff * band_width + rem);
    if (band_bits >= imax(thresh[j], alloc_floor + (1 << kBitRes))) {
      if (ec.bit_logp(1)) break;
      psum += 1 << kBitRes;
      band_bits -= 1 << kBitRes;
    }
    psum -= bits[j] + intensity_rsv;
    if (intensity_rsv > 0) intensity_rsv = kCeltLog2Frac[j - start];
    psum += intensity_rsv;
    if (band_bits >= alloc_floor) {
      psum += alloc_floor;
      bits[j] = alloc_floor;
    } else {
      bits[j] = 0;
    }
  }
  if (intensity_rsv > 0) *intensity = start + static_cast<int>(ec.uint_(static_cast<uint32_t>(codedBands + 1 - start)));
  else *intensity = 0;
  if (*intensity <= start) {
    total += dual_stereo_rsv;
    dual_stereo_rsv = 0;
  }
  if (dual_stereo_rsv > 0) *dual_stereo = ec.bit_logp(1);
  else *dual_stereo = 0;
  int32_t left = total - psum;
  const int32_t percoeff = left / (eband(codedBands) - eband(start));
  left -= (eband(codedBands) - eband(start)) * percoeff;
  for (int j = start; j < codedBands; j++) bits[j] += static_cast<int>(percoeff * (eband(j + 1) - eband(j)));
  for (int j = start; j < codedBands; j++) {
    const int tmp = static_cast<int>(i32min(left, eband(j + 1) - eband(j)));
    bits[j] += tmp;
    left -= tmp;
  }
  int32_t balance = 0;
  int j;
  for (j = start; j < codedBands; j++) {
    const int N0 = eband(j + 1) - eband(j);
    const int N = N0 << LM;
    const int32_t bit = static_cast<int32_t>(bits[j]) + balance;
    int32_t excess;
    if (N > 1) {
      excess = i32max(bit - cap[j], 0);
      bits[j] = static_cast<int>(bit - excess);
      const int den = C * N + ((C == 2 && N > 2 && !*dual_stereo && j < *intensity) ? 1 : 0);
      const int NClogN = den * (kCeltLogN[j] + logM);
      int offset = (NClogN >> 1) - den * kFineOffset;
      if (N == 2) offset += den << kBitRes >> 2;
      if (bits[j] + offset < den * 2 << kBitRes) offset += NClogN >> 2;
      else if (bits[j] + offset < den * 3 << kBitRes) offset += NClogN >> 3;
      ebits[j] = imax(0, bits[j] + offset + (den << (kBitRes - 1)));
      ebits[j] = ebits[j] / den >> kBitRes;
      if (C * ebits[j] > (bits[j] >> kBitRes)) ebits[j] = bits[j] >> stereo >> kBitRes;
      ebits[j] = imin(ebits[j], kMaxFineBits);
      fine_priority[j] = ebits[j] * (den << kBitRes) >= bits[j] + offset;
      bits[j] -= C * ebits[j] << kBitRes;
    } else {
      excess = i32max(0, bit - (C << kBitRes));
      bits[j] = static_cast<int>(bit - excess);
      ebits[j] = 0;
      fine_priority[j] = 1;
    }
    if (excess > 0) {
      const int extra_fine = imin(static_cast<int>(excess >> (stereo + kBitRes)), kMaxFineBits - ebits[j]);
      ebits[j] += extra_fine;
      const int extra_bits = extra_fine * C << kBitRes;
      fine_priority[j] = extra_bits >= excess - balance;
      excess -= extra_bits;
    }
    balance = excess;
  }
  *balance_out = balance;
  for (; j < end; j++) {
    ebits[j] = bits[j] >> stereo >> kBitRes;
    bits[j] = 0;
    fine_priority[j] = ebits[j] < 1;
  }
  return codedBands;
}

int compute_allocation(int start, int end, const int* offsets, const int* cap, int alloc_trim, int* intensity, int* dual_stereo,
                       int32_t total, int32_t* balance, int* pulses, int* ebits, int* fine_priority, int C, int LM, RangeDecoder& ec) {
  total = i32max(total, 0);
  const int len = CeltDecoder::kBands;
  int skip_start = start;
  const int skip_rsv = total >= 1 << kBitRes ? 1 << kBitRes : 0;
  total -= skip_rsv;
  int intensity_rsv = 0, dual_stereo_rsv = 0;
  if (C == 2) {
    intensity_rsv = kCeltLog2Frac[end - start];
    if (intensity_rsv > total) {
      intensity_rsv = 0;
    } else {
      total -= intensity_rsv;
      dual_stereo_rsv = total >= 1 << kBitRes ? 1 << kBitRes : 0;
      total -= dual_stereo_rsv;
    }
  }
  int bits1[len], bits2[len], thresh[len], trim_offset[len];
  for (int j = start; j < end; j++) {
    thresh[j] = imax(C << kBitRes, (3 * (eband(j + 1) - eband(j)) << LM << kBitRes) >> 4);
    trim_offset[j] = C * (eband(j + 1) - eband(j)) * (alloc_trim - 5 - LM) * (end - j - 1) * (1 << (LM + kBitRes)) >> 6;
    if ((eband(j + 1) - eband(j)) << LM == 1) trim_offset[j] -= C << kBitRes;
  }
  int lo = 1, hi = 11 - 1;  // nbAllocVectors - 1
  do {
    int done = 0, psum = 0;
    const int mid = (lo + hi) >> 1;
    for (int j = end; j-- > start;) {
      const int N = eband(j + 1) - eband(j);
      int bitsj = C * N * kCeltBandAllocation[mid * len + j] << LM >> 2;
      if (bitsj > 0) bitsj = imax(0, bitsj + trim_offset[j]);
      bitsj += offsets[j];
      if (bitsj >= thresh[j] || done) {
        done = 1;
        psum += imin(bitsj, cap[j]);
      } else if (bitsj >= C << kBitRes) {
        psum += C << kBitRes;
      }
    }
    if (psum > total) hi = mid - 1;
    else lo = mid + 1;
  } while (lo <= hi);
  hi = lo--;
  for (int j = start; j < end; j++) {
    const int N = eband(j + 1) - eband(j);
    int bits1j = C * N * kCeltBandAllocation[lo * len + j] << LM >> 2;
    int bits2j = hi >= 11 ? cap[j] : C * N * kCeltBandAllocation[hi * len + j] << LM >> 2;
    if (bits1j > 0) bits1j = imax(0, bits1j + trim_offset[j]);
    if (bits2j > 0) bits2j = imax(0, bits2j + trim_offset[j]);
    if (lo > 0) bits1j += offsets[j];
    bits2j += offsets[j];
    if (offsets[j] > 0) skip_start = j;
    bits2j = imax(0, bits2j - bits1j);
    bits1[j] = bits1j;
    bits2[j] = bits2j;
  }
  return interp_bits2pulses(start, end, skip_start, bits1, bits2, thresh, cap, total, balance, skip_rsv, intensity, intensity_rsv,
                            dual_stereo, dual_stereo_rsv, pulses, ebits, fine_priority, C, LM, ec);
}

void init_caps(int* cap, int LM, int C) {
  for (int i = 0; i < CeltDecoder::kBands; i++) {
    const int N = (eband(i + 1) - eband(i)) << LM;
    cap[i] = (kCeltCacheCaps[CeltDecoder::kBands * (2 * LM + C - 1) + i] + 64) * C * N >> 2;
  }
}

// ---- energy (quant_bands.c) --------------------------------------------------------------------------------------------------

const float kPredCoef[4] = {29440 / 32768.f, 26112 / 32768.f, 21248 / 32768.f, 16384 / 32768.f};
const float kBetaCoef[4] = {30147 / 32768.f, 22282 / 32768.f, 12124 / 32768.f, 6554 / 32768.f};
const float kBetaIntra = 4915 / 32768.f;
const uint8_t kSmallEnergyIcdf[3] = {2, 1, 0};

void unquant_coarse_energy(int start, int end, float* oldEBands, int intra, RangeDecoder& dec, int C, int LM) {
  const uint8_t* prob_model = kCeltEProbModel + LM * 84 + intra * 42;
  float prev[2] = {0, 0};
  float coef, beta;
  if (intra) {
    coef = 0;
    beta = kBetaIntra;
  } else {
    beta = kBetaCoef[LM];
    coef = kPredCoef[LM];
  }
  const int32_t budget = static_cast<int32_t>(dec.storage() * 8);
  for (int i = start; i < end; i++) {
    int c = 0;
    do {
      int qi;
      const int32_t tell = dec.tell();
      if (budget - tell >= 15) {
        const int pi = 2 * imin(i, 20);
        qi = dec.laplace(static_cast<unsigned>(prob_model[pi]) << 7, static_cast<int>(prob_model[pi + 1]) << 6);
      } else if (budget - tell >= 2) {
        qi = dec.icdf(kSmallEnergyIcdf, 2);
        qi = (qi >> 1) ^ -(qi & 1);
      } else if (budget - tell >= 1) {
        qi = -dec.bit_logp(1);
      } else {
        qi = -1;
      }
      const float q = static_cast<float>(qi);
      float& e = oldEBands[i + c * CeltDecoder::kBands];
      e = e < -9.f ? -9.f : e;
      const float tmp = coef * e + prev[c] + q;
      e = tmp;
      prev[c] = prev[c] + q - beta * q;
    } while (++c < C);
  }
}

void unquant_fine_energy(int start, int end, float* oldEBands, const int* fine_quant, RangeDecoder& dec, int C) {
  for (int i = start; i < end; i++) {
    if (fine_quant[i] <= 0) continue;
    int c = 0;
    do {
      const int q2 = static_cast<int>(dec.bits(static_cast<unsigned>(fine_quant[i])));
      const float offset = (q2 + .5f) * static_cast<float>(1 << (14 - fine_quant[i])) * (1.f / 16384) - .5f;
      oldEBands[i + c * CeltDecoder::kBands] += offset;
    } while (++c < C);
  }
}

void unquant_energy_finalise(int start, int end, float* oldEBands, const int* fine_quant, const int* fine_priority, int bits_left,
                             RangeDecoder& dec, int C) {
  for (int prio = 0; prio < 2; prio++) {
    for (int i = start; i < end && bits_left >= C; i++) {
      if (fine_quant[i] >= kMaxFineBits || fine_priority[i] != prio) continue;
      int c = 0;
      do {
        const int q2 = static_cast<int>(dec.bits(1));
        const float offset = (q2 - .5f) * static_cast<float>(1 << (14 - fine_quant[i] - 1)) * (1.f / 16384);
        oldEBands[i + c * CeltDecoder::kBands] += offset;
        bits_left--;
      } while (++c < C);
    }
  }
}

}  // namespace

namespace {

// ---- bands (bands.c) -------------------------------------------------------------------------------------------------------

void denormalise_bands(const float* X, float* freq, const float* bandLogE, int start, int end, int M, bool silence) {
  const int N = M * CeltDecoder::kBands * 0 + M * 120;
  int bound = M * eband(end);
  if (silence) {
    bound = 0;
    start = end = 0;
  }
  float* f = freq;
  const float* x = X + M * eband(start);
  for (int i = 0; i < M * eband(start); i++) *f++ = 0;
  for (int i = start; i < end; i++) {
    int j = M * eband(i);
    const int band_end = M * eband(i + 1);
    const float lg = bandLogE[i] + eMeans(i);
    const float g = cexp2(fmin_(32.f, lg));
    do {
      *f++ = *x++ * g;
    } while (++j < band_end);
  }
  for (int i = bound; i < N; i++) freq[i] = 0;
}

void anti_collapse(float* X_, const unsigned char* collapse_masks, int LM, int C, int size, int start, int end, const float* logE,
                   const float* prev1logE, const float* prev2logE, const int* pulses, uint32_t seed) {
  for (int i = start; i < end; i++) {
    const int N0 = eband(i + 1) - eband(i);
    const int depth = (1 + pulses[i]) / (eband(i + 1) - eband(i)) >> LM;
    const float thresh = .5f * cexp2(-.125f * static_cast<float>(depth));
    const float sqrt_1 = crsqrt(static_cast<float>(N0 << LM));
    int c = 0;
    do {
      float prev1 = prev1logE[c * CeltDecoder::kBands + i];
      float prev2 = prev2logE[c * CeltDecoder::kBands + i];
      if (C == 1) {
        prev1 = prev1 > prev1logE[CeltDecoder::kBands + i] ? prev1 : prev1logE[CeltDecoder::kBands + i];
        prev2 = prev2 > prev2logE[CeltDecoder::kBands + i] ? prev2 : prev2logE[CeltDecoder::kBands + i];
      }
      float Ediff = logE[c * CeltDecoder::kBands + i] - fmin_(prev1, prev2);
      Ediff = Ediff > 0 ? Ediff : 0;
      float r = 2.f * cexp2(-Ediff);
      if (LM == 3) r *= 1.41421356f;
      r = fmin_(thresh, r);
      r = r * sqrt_1;
      float* X = X_ + c * size + (eband(i) << LM);
      bool renormalize = false;
      for (int k = 0; k < 1 << LM; k++) {
        if (!(collapse_masks[i * C + c] & 1 << k)) {
          for (int j = 0; j < N0; j++) {
            seed = lcg_rand(seed);
            X[(j << LM) + k] = (seed & 0x8000 ? r : -r);
          }
          renormalize = true;
        }
      }
      if (renormalize) renormalise_vector(X, N0 << LM, 1.f);
    } while (++c < C);
  }
}

void stereo_merge(float* X, float* Y, float mid, int N) {
  float xp = 0, side = 0;
  for (int j = 0; j < N; j++) {
    xp += Y[j] * X[j];
    side += Y[j] * Y[j];
  }
  xp = mid * xp;
  const float mid2 = mid;
  const float El = mid2 * mid2 + side - 2 * xp;
  const float Er = mid2 * mid2 + side + 2 * xp;
  if (Er < 6e-4f || El < 6e-4f) {
    for (int j = 0; j < N; j++) Y[j] = X[j];
    return;
  }
  const float lgain = crsqrt(El), rgain = crsqrt(Er);
  for (int j = 0; j < N; j++) {
    const float l = mid * X[j];
    const float r = Y[j];
    X[j] = lgain * (l - r);
    Y[j] = rgain * (l + r);
  }
}

void deinterleave_hadamard(float* X, int N0, int stride, int hadamard) {
  float tmp[CeltDecoder::kMaxFrame];
  const int N = N0 * stride;
  if (hadamard) {
    const int* ordery = kCeltOrdery + stride - 2;
    for (int i = 0; i < stride; i++)
      for (int j = 0; j < N0; j++) tmp[ordery[i] * N0 + j] = X[j * stride + i];
  } else {
    for (int i = 0; i < stride; i++)
      for (int j = 0; j < N0; j++) tmp[i * N0 + j] = X[j * stride + i];
  }
  for (int i = 0; i < N; i++) X[i] = tmp[i];
}

void interleave_hadamard(float* X, int N0, int stride, int hadamard) {
  float tmp[CeltDecoder::kMaxFrame];
  const int N = N0 * stride;
  if (hadamard) {
    const int* ordery = kCeltOrdery + stride - 2;
    for (int i = 0; i < stride; i++)
      for (int j = 0; j < N0; j++) tmp[j * stride + i] = X[ordery[i] * N0 + j];
  } else {
    for (int i = 0; i < stride; i++)
      for (int j = 0; j < N0; j++) tmp[j * stride + i] = X[i * N0 + j];
  }
  for (int i = 0; i < N; i++) X[i] = tmp[i];
}

void haar1(float* X, int N0, int stride) {
  N0 >>= 1;
  for (int i = 0; i < stride; i++)
    for (int j = 0; j < N0; j++) {
      const float tmp1 = .70710678f * X[stride * 2 * j + i];
      const float tmp2 = .70710678f * X[stride * (2 * j + 1) + i];
      X[stride * 2 * j + i] = tmp1 + tmp2;
      X[stride * (2 * j + 1) + i] = tmp1 - tmp2;
    }
}

int compute_qn(int N, int b, int offset, int pulse_cap, int stereo) {
  static const int16_t exp2_table8[8] = {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
  int N2 = 2 * N - 1;
  if (stereo && N == 2) N2--;
  int qb = (b + N2 * offset) / N2;
  qb = imin(b - pulse_cap - (4 << kBitRes), qb);
  qb = imin(8 << kBitRes, qb);
  int qn;
  if (qb < (1 << kBitRes >> 1)) {
    qn = 1;
  } else {
    qn = exp2_table8[qb & 0x7] >> (14 - (qb >> kBitRes));
    qn = (qn + 1) >> 1 << 1;
  }
  return qn;
}

struct BandCtx {
  RangeDecoder* ec;
  int i;
  int intensity;
  int spread;
  int tf_change;
  int32_t remaining_bits;
  uint32_t seed;
  bool disable_inv;
};

struct SplitCtx {
  int inv, imid, iside, delta, itheta, qalloc;
};

void compute_theta(BandCtx* ctx, SplitCtx* sctx, int N, int* b, int B, int B0, int LM, int stereo, int* fill) {
  RangeDecoder& ec = *ctx->ec;
  const int i = ctx->i;
  const int pulse_cap = kCeltLogN[i] + LM * (1 << kBitRes);
  const int offset = (pulse_cap >> 1) - (stereo && N == 2 ? kQthetaOffsetTwophase : kQthetaOffset);
  int qn = compute_qn(N, *b, offset, pulse_cap, stereo);
  if (stereo && i >= ctx->intensity) qn = 1;
  int itheta = 0, inv = 0;
  const int32_t tell = static_cast<int32_t>(ec.tell_frac());
  if (qn != 1) {
    if (stereo && N > 2) {
      const int p0 = 3;
      int x = itheta;
      const int x0 = qn / 2;
      const int ft = p0 * (x0 + 1) + x0;
      const int fs = static_cast<int>(ec.decode(static_cast<unsigned>(ft)));
      if (fs < (x0 + 1) * p0) x = fs / p0;
      else x = x0 + 1 + (fs - (x0 + 1) * p0);
      ec.update(static_cast<unsigned>(x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0),
                static_cast<unsigned>(x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0), static_cast<unsigned>(ft));
      itheta = x;
    } else if (B0 > 1 || stereo) {
      itheta = static_cast<int>(ec.uint_(static_cast<uint32_t>(qn + 1)));
    } else {
      const int ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
      int fs, fl;
      const int fm = static_cast<int>(ec.decode(static_cast<unsigned>(ft)));
      if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
        itheta = static_cast<int>((isqrt32(8 * static_cast<uint32_t>(fm) + 1) - 1) >> 1);
        fs = itheta + 1;
        fl = itheta * (itheta + 1) >> 1;
      } else {
        itheta = static_cast<int>((2 * (qn + 1) - isqrt32(8 * static_cast<uint32_t>(ft - fm - 1) + 1)) >> 1);
        fs = qn + 1 - itheta;
        fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
      }
      ec.update(static_cast<unsigned>(fl), static_cast<unsigned>(fl + fs), static_cast<unsigned>(ft));
    }
    itheta = static_cast<int>(static_cast<int32_t>(itheta) * 16384 / qn);
  } else if (stereo) {
    if (*b > 2 << kBitRes && ctx->remaining_bits > 2 << kBitRes) inv = ec.bit_logp(2);
    else inv = 0;
    if (ctx->disable_inv) inv = 0;
    itheta = 0;
  }
  const int qalloc = static_cast<int>(ec.tell_frac() - static_cast<uint32_t>(tell));
  *b -= qalloc;
  int imid, iside, delta;
  if (itheta == 0) {
    imid = 32767;
    iside = 0;
    *fill &= (1 << B) - 1;
    delta = -16384;
  } else if (itheta == 16384) {
    imid = 0;
    iside = 32767;
    *fill &= ((1 << B) - 1) << B;
    delta = 16384;
  } else {
    imid = bitexact_cos(itheta);
    iside = bitexact_cos(16384 - itheta);
    delta = frac_mul16((N - 1) << 7, bitexact_log2tan(iside, imid));
  }
  sctx->inv = inv;
  sctx->imid = imid;
  sctx->iside = iside;
  sctx->delta = delta;
  sctx->itheta = itheta;
  sctx->qalloc = qalloc;
}

unsigned quant_band_n1(BandCtx* ctx, float* X, float* Y, float* lowband_out) {
  RangeDecoder& ec = *ctx->ec;
  const int stereo = Y != nullptr;
  float* x = X;
  int c = 0;
  do {
    int sign = 0;
    if (ctx->remaining_bits >= 1 << kBitRes) {
      sign = static_cast<int>(ec.bits(1));
      ctx->remaining_bits -= 1 << kBitRes;
    }
    x[0] = sign ? -1.f : 1.f;
    x = Y;
  } while (++c < 1 + stereo);
  if (lowband_out) lowband_out[0] = X[0];
  return 1;
}

unsigned quant_partition(BandCtx* ctx, float* X, int N, int b, int B, float* lowband, int LM, float gain, int fill) {
  RangeDecoder& ec = *ctx->ec;
  const int i = ctx->i;
  const int B0 = B;
  unsigned cm = 0;
  const uint8_t* cache = cache_for(i, LM);
  if (LM != -1 && b > cache[cache[0]] + 12 && N > 2) {
    N >>= 1;
    float* Y = X + N;
    LM -= 1;
    if (B == 1) fill = (fill & 1) | (fill << 1);
    B = (B + 1) >> 1;
    SplitCtx sctx;
    compute_theta(ctx, &sctx, N, &b, B, B0, LM, 0, &fill);
    const int imid = sctx.imid, iside = sctx.iside;
    int delta = sctx.delta;
    const int itheta = sctx.itheta, qalloc = sctx.qalloc;
    const float mid = (1.f / 32768) * static_cast<float>(imid);
    const float side = (1.f / 32768) * static_cast<float>(iside);
    if (B0 > 1 && (itheta & 0x3fff)) {
      if (itheta > 8192) delta -= delta >> (4 - LM);
      else delta = imin(0, delta + (N << kBitRes >> (5 - LM)));
    }
    int mbits = imax(0, imin(b, (b - delta) / 2));
    int sbits = b - mbits;
    ctx->remaining_bits -= qalloc;
    float* next_lowband2 = nullptr;
    if (lowband) next_lowband2 = lowband + N;
    int32_t rebalance = ctx->remaining_bits;
    if (mbits >= sbits) {
      cm = quant_partition(ctx, X, N, mbits, B, lowband, LM, gain * mid, fill);
      rebalance = mbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << kBitRes && itheta != 0) sbits += rebalance - (3 << kBitRes);
      cm |= quant_partition(ctx, Y, N, sbits, B, next_lowband2, LM, gain * side, fill >> B) << (B0 >> 1);
    } else {
      cm = quant_partition(ctx, Y, N, sbits, B, next_lowband2, LM, gain * side, fill >> B) << (B0 >> 1);
      rebalance = sbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << kBitRes && itheta != 16384) mbits += rebalance - (3 << kBitRes);
      cm |= quant_partition(ctx, X, N, mbits, B, lowband, LM, gain * mid, fill);
    }
  } else {
    int q = bits2pulses(i, LM, b);
    int curr_bits = pulses2bits(i, LM, q);
    ctx->remaining_bits -= curr_bits;
    while (ctx->remaining_bits < 0 && q > 0) {
      ctx->remaining_bits += curr_bits;
      q--;
      curr_bits = pulses2bits(i, LM, q);
      ctx->remaining_bits -= curr_bits;
    }
    if (q != 0) {
      const int K = get_pulses(q);
      cm = alg_unquant(X, N, K, ctx->spread, B, ec, gain);
    } else {
      const unsigned cm_mask = static_cast<unsigned>((1UL << B) - 1);
      fill &= static_cast<int>(cm_mask);
      if (!fill) {
        for (int j = 0; j < N; j++) X[j] = 0;
      } else {
        if (lowband == nullptr) {
          for (int j = 0; j < N; j++) {
            ctx->seed = lcg_rand(ctx->seed);
            X[j] = static_cast<float>(static_cast<int32_t>(ctx->seed) >> 20);
          }
          cm = cm_mask;
        } else {
          for (int j = 0; j < N; j++) {
            ctx->seed = lcg_rand(ctx->seed);
            float tmp = 1.0f / 256;
            tmp = (ctx->seed) & 0x8000 ? tmp : -tmp;
            X[j] = lowband[j] + tmp;
          }
          cm = static_cast<unsigned>(fill);
        }
        renormalise_vector(X, N, gain);
      }
    }
  }
  return cm;
}

unsigned quant_band(BandCtx* ctx, float* X, int N, int b, int B, float* lowband, int LM, float* lowband_out, float gain,
                    float* lowband_scratch, int fill) {
  const int N0 = N;
  int N_B = N;
  int B0 = B;
  int time_divide = 0;
  int recombine = 0;
  const bool longBlocks = B0 == 1;
  const int tf_change0 = ctx->tf_change;
  int tf_change = tf_change0;
  N_B = N_B / B;
  if (N == 1) return quant_band_n1(ctx, X, nullptr, lowband_out);
  if (tf_change > 0) recombine = tf_change;
  if (lowband_scratch && lowband && (recombine || ((N_B & 1) == 0 && tf_change < 0) || B0 > 1)) {
    for (int j = 0; j < N; j++) lowband_scratch[j] = lowband[j];
    lowband = lowband_scratch;
  }
  static const unsigned char kBitInterleave[16] = {0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3};
  static const unsigned char kBitDeinterleave[16] = {0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
                                                     0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF};
  for (int k = 0; k < recombine; k++) {
    if (lowband) haar1(lowband, N >> k, 1 << k);
    fill = kBitInterleave[fill & 0xF] | kBitInterleave[fill >> 4] << 2;
  }
  B >>= recombine;
  N_B <<= recombine;
  while ((N_B & 1) == 0 && tf_change < 0) {
    if (lowband) haar1(lowband, N_B, B);
    fill |= fill << B;
    B <<= 1;
    N_B >>= 1;
    time_divide++;
    tf_change++;
  }
  B0 = B;
  const int N_B0 = N_B;
  if (B0 > 1) {
    if (lowband) deinterleave_hadamard(lowband, N_B >> recombine, B0 << recombine, longBlocks);
  }
  unsigned cm = quant_partition(ctx, X, N, b, B, lowband, LM, gain, fill);
  if (B0 > 1) interleave_hadamard(X, N_B >> recombine, B0 << recombine, longBlocks);
  N_B = N_B0;
  B = B0;
  for (int k = 0; k < time_divide; k++) {
    B >>= 1;
    N_B <<= 1;
    cm |= cm >> B;
    haar1(X, N_B, B);
  }
  for (int k = 0; k < recombine; k++) {
    cm = kBitDeinterleave[cm];
    haar1(X, N0 >> k, 1 << k);
  }
  B <<= recombine;
  if (lowband_out) {
    const float n = csqrt(static_cast<float>(N0));
    for (int j = 0; j < N0; j++) lowband_out[j] = n * X[j];
  }
  cm &= (1u << B) - 1;
  return cm;
}

unsigned quant_band_stereo(BandCtx* ctx, float* X, float* Y, int N, int b, int B, float* lowband, int LM, float* lowband_out,
                           float* lowband_scratch, int fill) {
  RangeDecoder& ec = *ctx->ec;
  if (N == 1) return quant_band_n1(ctx, X, Y, lowband_out);
  const int orig_fill = fill;
  SplitCtx sctx;
  compute_theta(ctx, &sctx, N, &b, B, B, LM, 1, &fill);
  const int inv = sctx.inv, imid = sctx.imid, iside = sctx.iside, delta = sctx.delta, itheta = sctx.itheta, qalloc = sctx.qalloc;
  const float mid = (1.f / 32768) * static_cast<float>(imid);
  const float side = (1.f / 32768) * static_cast<float>(iside);
  unsigned cm = 0;
  if (N == 2) {
    int mbits = b, sbits = 0;
    if (itheta != 0 && itheta != 16384) sbits = 1 << kBitRes;
    mbits -= sbits;
    const int c = itheta > 8192;
    ctx->remaining_bits -= qalloc + sbits;
    float* x2 = c ? Y : X;
    float* y2 = c ? X : Y;
    int sign = 0;
    if (sbits) sign = static_cast<int>(ec.bits(1));
    sign = 1 - 2 * sign;
    cm = quant_band(ctx, x2, N, mbits, B, lowband, LM, lowband_out, 1.f, lowband_scratch, orig_fill);
    y2[0] = -sign * x2[1];
    y2[1] = sign * x2[0];
    X[0] = mid * X[0];
    X[1] = mid * X[1];
    Y[0] = side * Y[0];
    Y[1] = side * Y[1];
    float tmp = X[0];
    X[0] = tmp - Y[0];
    Y[0] = tmp + Y[0];
    tmp = X[1];
    X[1] = tmp - Y[1];
    Y[1] = tmp + Y[1];
  } else {
    int mbits = imax(0, imin(b, (b - delta) / 2));
    int sbits = b - mbits;
    ctx->remaining_bits -= qalloc;
    int32_t rebalance = ctx->remaining_bits;
    if (mbits >= sbits) {
      cm = quant_band(ctx, X, N, mbits, B, lowband, LM, lowband_out, 1.f, lowband_scratch, fill);
      rebalance = mbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << kBitRes && itheta != 0) sbits += rebalance - (3 << kBitRes);
      cm |= quant_band(ctx, Y, N, sbits, B, nullptr, LM, nullptr, side, nullptr, fill >> B);
    } else {
      cm = quant_band(ctx, Y, N, sbits, B, nullptr, LM, nullptr, side, nullptr, fill >> B);
      rebalance = sbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << kBitRes && itheta != 16384) mbits += rebalance - (3 << kBitRes);
      cm |= quant_band(ctx, X, N, mbits, B, lowband, LM, lowband_out, 1.f, lowband_scratch, fill);
    }
  }
  if (N != 2) stereo_merge(X, Y, mid, N);
  if (inv)
    for (int j = 0; j < N; j++) Y[j] = -Y[j];
  return cm;
}

void special_hybrid_folding(float* norm, float* norm2, int start, int M, int dual_stereo) {
  const int n1 = M * (eband(start + 1) - eband(start));
  const int n2 = M * (eband(start + 2) - eband(start + 1));
  for (int j = 0; j < n2 - n1; j++) norm[n1 + j] = norm[2 * n1 - n2 + j];
  if (dual_stereo)
    for (int j = 0; j < n2 - n1; j++) norm2[n1 + j] = norm2[2 * n1 - n2 + j];
}

void quant_all_bands(int start, int end, float* X_, float* Y_, unsigned char* collapse_masks, const int* pulses, int shortBlocks, int spread,
                     int dual_stereo, int intensity, const int* tf_res, int32_t total_bits, int32_t balance, RangeDecoder& ec, int LM,
                     int codedBands, uint32_t* seed, bool disable_inv) {
  const int M = 1 << LM;
  const int B = shortBlocks ? M : 1;
  const int norm_offset = M * eband(start);
  const int C = Y_ != nullptr ? 2 : 1;
  const int norm_len = M * eband(CeltDecoder::kBands - 1) - norm_offset;
  float norm_buf[2 * 8 * 100];
  float* norm = norm_buf;
  float* norm2 = norm + norm_len;
  float* lowband_scratch = X_ + M * eband(CeltDecoder::kBands - 1);
  int lowband_offset = 0;
  bool update_lowband = true;
  BandCtx ctx;
  ctx.ec = &ec;
  ctx.intensity = intensity;
  ctx.seed = *seed;
  ctx.spread = spread;
  ctx.disable_inv = disable_inv;
  for (int i = start; i < end; i++) {
    ctx.i = i;
    const bool last = (i == end - 1);
    float* X = X_ + M * eband(i);
    float* Y = Y_ != nullptr ? Y_ + M * eband(i) : nullptr;
    const int N = M * eband(i + 1) - M * eband(i);
    const int32_t tell = static_cast<int32_t>(ec.tell_frac());
    if (i != start) balance -= tell;
    const int32_t remaining_bits = total_bits - tell - 1;
    ctx.remaining_bits = remaining_bits;
    int b;
    if (i <= codedBands - 1) {
      const int32_t curr_balance = balance / imin(3, codedBands - i);
      b = imax(0, imin(16383, static_cast<int>(i32min(remaining_bits + 1, pulses[i] + curr_balance))));
    } else {
      b = 0;
    }
    if ((M * eband(i) - N >= M * eband(start) || i == start + 1) && (update_lowband || lowband_offset == 0)) lowband_offset = i;
    if (i == start + 1) special_hybrid_folding(norm, norm2, start, M, dual_stereo);
    const int tf_change = tf_res[i];
    ctx.tf_change = tf_change;
    float* band_scratch = lowband_scratch;
    if (i >= CeltDecoder::kBands) {  // effEBands == nbEBands here, kept for fidelity with the reference
      X = norm;
      if (Y_ != nullptr) Y = norm;
      band_scratch = nullptr;
    }
    if (last) band_scratch = nullptr;
    int effective_lowband = -1;
    unsigned x_cm, y_cm;
    if (lowband_offset != 0 && (spread != kSpreadAggressive || B > 1 || tf_change < 0)) {
      effective_lowband = imax(0, M * eband(lowband_offset) - norm_offset - N);
      int fold_start = lowband_offset;
      while (M * eband(--fold_start) > effective_lowband + norm_offset) {}
      int fold_end = lowband_offset - 1;
      while (++fold_end < i && M * eband(fold_end) < effective_lowband + norm_offset + N) {}
      x_cm = y_cm = 0;
      int fold_i = fold_start;
      do {
        x_cm |= collapse_masks[fold_i * C + 0];
        y_cm |= collapse_masks[fold_i * C + C - 1];
      } while (++fold_i < fold_end);
    } else {
      x_cm = y_cm = (1u << B) - 1;
    }
    if (dual_stereo && i == intensity) {
      dual_stereo = 0;
      for (int j = 0; j < M * eband(i) - norm_offset; j++) norm[j] = .5f * (norm[j] + norm2[j]);
    }
    if (dual_stereo) {
      x_cm = quant_band(&ctx, X, N, b / 2, B, effective_lowband != -1 ? norm + effective_lowband : nullptr, LM,
                        last ? nullptr : norm + M * eband(i) - norm_offset, 1.f, band_scratch, static_cast<int>(x_cm));
      y_cm = quant_band(&ctx, Y, N, b / 2, B, effective_lowband != -1 ? norm2 + effective_lowband : nullptr, LM,
                        last ? nullptr : norm2 + M * eband(i) - norm_offset, 1.f, band_scratch, static_cast<int>(y_cm));
    } else {
      if (Y != nullptr) {
        x_cm = quant_band_stereo(&ctx, X, Y, N, b, B, effective_lowband != -1 ? norm + effective_lowband : nullptr, LM,
                                 last ? nullptr : norm + M * eband(i) - norm_offset, band_scratch, static_cast<int>(x_cm | y_cm));
      } else {
        x_cm = quant_band(&ctx, X, N, b, B, effective_lowband != -1 ? norm + effective_lowband : nullptr, LM,
                          last ? nullptr : norm + M * eband(i) - norm_offset, 1.f, band_scratch, static_cast<int>(x_cm | y_cm));
      }
      y_cm = x_cm;
    }
    collapse_masks[i * C + 0] = static_cast<unsigned char>(x_cm);
    collapse_masks[i * C + C - 1] = static_cast<unsigned char>(y_cm);
    balance += pulses[i] + tell;
    update_lowband = b > (N << kBitRes);
  }
  *seed = ctx.seed;
}

void tf_decode(int start, int end, int isTransient, int* tf_res, int LM, RangeDecoder& dec) {
  uint32_t budget = dec.storage() * 8;
  uint32_t tell = static_cast<uint32_t>(dec.tell());
  int logp = isTransient ? 2 : 4;
  const int tf_select_rsv = LM > 0 && tell + static_cast<uint32_t>(logp) + 1 <= budget;
  budget -= static_cast<uint32_t>(tf_select_rsv);
  int tf_changed = 0, curr = 0;
  for (int i = start; i < end; i++) {
    if (tell + static_cast<uint32_t>(logp) <= budget) {
      curr ^= dec.bit_logp(static_cast<unsigned>(logp));
      tell = static_cast<uint32_t>(dec.tell());
      tf_changed |= curr;
    }
    tf_res[i] = curr;
    logp = isTransient ? 4 : 5;
  }
  int tf_select = 0;
  if (tf_select_rsv && kCeltTfSelect[LM * 8 + 4 * isTransient + 0 + tf_changed] != kCeltTfSelect[LM * 8 + 4 * isTransient + 2 + tf_changed])
    tf_select = dec.bit_logp(1);
  for (int i = start; i < end; i++) tf_res[i] = kCeltTfSelect[LM * 8 + 4 * isTransient + 2 * tf_select + tf_res[i]];
}

}  // namespace

// ---- transforms (kiss_fft.c, mdct.c) ------------------------------------------------------------------------------------------

namespace {

int kf_factor(int n, int16_t* facbuf) {
  int p = 4, stages = 0;
  const int nbak = n;
  do {
    while (n % p) {
      switch (p) {
        case 4: p = 2; break;
        case 2: p = 3; break;
        default: p += 2; break;
      }
      if (p > 32000 || static_cast<int32_t>(p) * p > n) p = n;
    }
    n /= p;
    if (p > 5) return 0;
    facbuf[2 * stages] = static_cast<int16_t>(p);
    if (p == 2 && stages > 1) {
      facbuf[2 * stages] = 4;
      facbuf[2] = 2;
    }
    stages++;
  } while (n > 1);
  n = nbak;
  for (int i = 0; i < stages / 2; i++) {
    const int16_t tmp = facbuf[2 * i];
    facbuf[2 * i] = facbuf[2 * (stages - i - 1)];
    facbuf[2 * (stages - i - 1)] = tmp;
  }
  for (int i = 0; i < stages; i++) {
    n /= facbuf[2 * i];
    facbuf[2 * i + 1] = static_cast<int16_t>(n);
  }
  return 1;
}

void compute_bitrev_table(int Fout, int16_t* table, size_t index, size_t fstride, int in_stride, const int16_t* factors) {
  const int p = *factors++;
  const int m = *factors++;
  if (m == 1) {
    for (int j = 0; j < p; j++) {
      table[index] = static_cast<int16_t>(Fout + j);
      index += fstride * static_cast<size_t>(in_stride);
    }
  } else {
    for (int j = 0; j < p; j++) {
      compute_bitrev_table(Fout, table, index, fstride * static_cast<size_t>(p), in_stride, factors);
      index += fstride * static_cast<size_t>(in_stride);
      Fout += m;
    }
  }
}

}  // namespace

CeltDecoder::CeltDecoder() { build_tables(); }

void CeltDecoder::build_tables() {
  if (tables_built_) return;
  const double pi = 3.14159265358979323846264338327;
  for (int i = 0; i < kOverlap; i++) {
    const double s = fmath::sin(.5 * pi * (i + .5) / kOverlap);
    window_[i] = static_cast<float>(fmath::sin(.5 * pi * s * s));
  }
  for (int i = 0; i < 480; i++) {
    const double phase = (-2 * pi / 480) * i;
    twiddles_[i].r = static_cast<float>(fmath::cos(phase));
    twiddles_[i].i = static_cast<float>(fmath::sin(phase));
  }
  int offset = 0;
  for (int shift = 0; shift < 4; shift++) {
    const int N = 1920 >> shift;
    for (int i = 0; i < N / 2; i++) trig_[offset + i] = static_cast<float>(fmath::cos(2 * pi * (i + .125) / N));
    offset += N / 2;
    FftState& st = fft_[shift];
    st.nfft = 480 >> shift;
    st.scale = 1.f / static_cast<float>(st.nfft);
    st.shift = shift == 0 ? -1 : shift;
    for (int16_t& f : st.factors) f = 0;
    kf_factor(st.nfft, st.factors);
    compute_bitrev_table(0, st.bitrev, 0, 1, 1, st.factors);
  }
  tables_built_ = true;
}

void CeltDecoder::bfly2(Cpx* fout, int m, int N) {
  (void)m;
  if (m == 1) {
    for (int i = 0; i < N; i++) {
      Cpx* fout2 = fout + 1;
      const Cpx t = *fout2;
      fout2->r = fout->r - t.r;
      fout2->i = fout->i - t.i;
      fout->r += t.r;
      fout->i += t.i;
      fout += 2;
    }
    return;
  }
  const float tw = 0.7071067812f;
  for (int i = 0; i < N; i++) {
    Cpx* fout2 = fout + 4;
    Cpx t = fout2[0];
    fout2[0].r = fout[0].r - t.r;
    fout2[0].i = fout[0].i - t.i;
    fout[0].r += t.r;
    fout[0].i += t.i;
    t.r = (fout2[1].r + fout2[1].i) * tw;
    t.i = (fout2[1].i - fout2[1].r) * tw;
    fout2[1].r = fout[1].r - t.r;
    fout2[1].i = fout[1].i - t.i;
    fout[1].r += t.r;
    fout[1].i += t.i;
    t.r = fout2[2].i;
    t.i = -fout2[2].r;
    fout2[2].r = fout[2].r - t.r;
    fout2[2].i = fout[2].i - t.i;
    fout[2].r += t.r;
    fout[2].i += t.i;
    t.r = (fout2[3].i - fout2[3].r) * tw;
    t.i = -(fout2[3].i + fout2[3].r) * tw;
    fout2[3].r = fout[3].r - t.r;
    fout2[3].i = fout[3].i - t.i;
    fout[3].r += t.r;
    fout[3].i += t.i;
    fout += 8;
  }
}

static inline CeltDecoder::Cpx cmul(CeltDecoder::Cpx a, CeltDecoder::Cpx b) { return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r}; }

void CeltDecoder::bfly4(Cpx* fout, size_t fstride, const FftState& st, int m, int N, int mm) {
  (void)st;
  if (m == 1) {
    for (int i = 0; i < N; i++) {
      Cpx scratch0, scratch1;
      scratch0 = {fout[0].r - fout[2].r, fout[0].i - fout[2].i};
      fout[0].r += fout[2].r;
      fout[0].i += fout[2].i;
      scratch1 = {fout[1].r + fout[3].r, fout[1].i + fout[3].i};
      fout[2] = {fout[0].r - scratch1.r, fout[0].i - scratch1.i};
      fout[0].r += scratch1.r;
      fout[0].i += scratch1.i;
      scratch1 = {fout[1].r - fout[3].r, fout[1].i - fout[3].i};
      fout[1].r = scratch0.r + scratch1.i;
      fout[1].i = scratch0.i - scratch1.r;
      fout[3].r = scratch0.r - scratch1.i;
      fout[3].i = scratch0.i + scratch1.r;
      fout += 4;
    }
    return;
  }
  const int m2 = 2 * m, m3 = 3 * m;
  Cpx* fout_beg = fout;
  for (int i = 0; i < N; i++) {
    fout = fout_beg + i * mm;
    size_t i1 = 0, i2 = 0, i3 = 0;
    for (int j = 0; j < m; j++) {
      Cpx scratch[6];
      scratch[0] = cmul(fout[m], twiddles_[i1]);
      scratch[1] = cmul(fout[m2], twiddles_[i2]);
      scratch[2] = cmul(fout[m3], twiddles_[i3]);
      scratch[5] = {fout->r - scratch[1].r, fout->i - scratch[1].i};
      fout->r += scratch[1].r;
      fout->i += scratch[1].i;
      scratch[3] = {scratch[0].r + scratch[2].r, scratch[0].i + scratch[2].i};
      scratch[4] = {scratch[0].r - scratch[2].r, scratch[0].i - scratch[2].i};
      fout[m2] = {fout->r - scratch[3].r, fout->i - scratch[3].i};
      i1 += fstride;
      i2 += fstride * 2;
      i3 += fstride * 3;
      fout->r += scratch[3].r;
      fout->i += scratch[3].i;
      fout[m].r = scratch[5].r + scratch[4].i;
      fout[m].i = scratch[5].i - scratch[4].r;
      fout[m3].r = scratch[5].r - scratch[4].i;
      fout[m3].i = scratch[5].i + scratch[4].r;
      ++fout;
    }
  }
}

void CeltDecoder::bfly3(Cpx* fout, size_t fstride, const FftState& st, int m, int N, int mm) {
  (void)st;
  const size_t m2 = 2 * static_cast<size_t>(m);
  const Cpx epi3 = twiddles_[fstride * static_cast<size_t>(m)];
  Cpx* fout_beg = fout;
  for (int i = 0; i < N; i++) {
    fout = fout_beg + i * mm;
    size_t i1 = 0, i2 = 0;
    int k = m;
    do {
      Cpx scratch[5];
      scratch[1] = cmul(fout[m], twiddles_[i1]);
      scratch[2] = cmul(fout[m2], twiddles_[i2]);
      scratch[3] = {scratch[1].r + scratch[2].r, scratch[1].i + scratch[2].i};
      scratch[0] = {scratch[1].r - scratch[2].r, scratch[1].i - scratch[2].i};
      i1 += fstride;
      i2 += fstride * 2;
      fout[m].r = fout->r - scratch[3].r * .5f;
      fout[m].i = fout->i - scratch[3].i * .5f;
      scratch[0].r *= epi3.i;
      scratch[0].i *= epi3.i;
      fout->r += scratch[3].r;
      fout->i += scratch[3].i;
      fout[m2].r = fout[m].r + scratch[0].i;
      fout[m2].i = fout[m].i - scratch[0].r;
      fout[m].r = fout[m].r - scratch[0].i;
      fout[m].i = fout[m].i + scratch[0].r;
      ++fout;
    } while (--k);
  }
}

void CeltDecoder::bfly5(Cpx* fout, size_t fstride, const FftState& st, int m, int N, int mm) {
  (void)st;
  const Cpx ya = twiddles_[fstride * static_cast<size_t>(m)];
  const Cpx yb = twiddles_[fstride * 2 * static_cast<size_t>(m)];
  Cpx* fout_beg = fout;
  for (int i = 0; i < N; i++) {
    fout = fout_beg + i * mm;
    Cpx *fout0 = fout, *fout1 = fout0 + m, *fout2 = fout0 + 2 * m, *fout3 = fout0 + 3 * m, *fout4 = fout0 + 4 * m;
    for (int u = 0; u < m; ++u) {
      Cpx scratch[13];
      scratch[0] = *fout0;
      scratch[1] = cmul(*fout1, twiddles_[static_cast<size_t>(u) * fstride]);
      scratch[2] = cmul(*fout2, twiddles_[2 * static_cast<size_t>(u) * fstride]);
      scratch[3] = cmul(*fout3, twiddles_[3 * static_cast<size_t>(u) * fstride]);
      scratch[4] = cmul(*fout4, twiddles_[4 * static_cast<size_t>(u) * fstride]);
      scratch[7] = {scratch[1].r + scratch[4].r, scratch[1].i + scratch[4].i};
      scratch[10] = {scratch[1].r - scratch[4].r, scratch[1].i - scratch[4].i};
      scratch[8] = {scratch[2].r + scratch[3].r, scratch[2].i + scratch[3].i};
      scratch[9] = {scratch[2].r - scratch[3].r, scratch[2].i - scratch[3].i};
      fout0->r += scratch[7].r + scratch[8].r;
      fout0->i += scratch[7].i + scratch[8].i;
      scratch[5].r = scratch[0].r + scratch[7].r * ya.r + scratch[8].r * yb.r;
      scratch[5].i = scratch[0].i + scratch[7].i * ya.r + scratch[8].i * yb.r;
      scratch[6].r = scratch[10].i * ya.i + scratch[9].i * yb.i;
      scratch[6].i = -(scratch[10].r * ya.i + scratch[9].r * yb.i);
      *fout1 = {scratch[5].r - scratch[6].r, scratch[5].i - scratch[6].i};
      *fout4 = {scratch[5].r + scratch[6].r, scratch[5].i + scratch[6].i};
      scratch[11].r = scratch[0].r + scratch[7].r * yb.r + scratch[8].r * ya.r;
      scratch[11].i = scratch[0].i + scratch[7].i * yb.r + scratch[8].i * ya.r;
      scratch[12].r = scratch[9].i * ya.i - scratch[10].i * yb.i;
      scratch[12].i = scratch[10].r * yb.i - scratch[9].r * ya.i;
      *fout2 = {scratch[11].r + scratch[12].r, scratch[11].i + scratch[12].i};
      *fout3 = {scratch[11].r - scratch[12].r, scratch[11].i - scratch[12].i};
      ++fout0;
      ++fout1;
      ++fout2;
      ++fout3;
      ++fout4;
    }
  }
}

void CeltDecoder::fft_impl(const FftState& st, Cpx* fout) {
  int fstride[8];
  const int shift = st.shift > 0 ? st.shift : 0;
  fstride[0] = 1;
  int L = 0, m;
  do {
    const int p = st.factors[2 * L];
    m = st.factors[2 * L + 1];
    fstride[L + 1] = fstride[L] * p;
    L++;
  } while (m != 1);
  m = st.factors[2 * L - 1];
  for (int i = L - 1; i >= 0; i--) {
    const int m2 = i != 0 ? st.factors[2 * i - 1] : 1;
    switch (st.factors[2 * i]) {
      case 2: bfly2(fout, m, fstride[i]); break;
      case 4: bfly4(fout, static_cast<size_t>(fstride[i]) << shift, st, m, fstride[i], m2); break;
      case 3: bfly3(fout, static_cast<size_t>(fstride[i]) << shift, st, m, fstride[i], m2); break;
      case 5: bfly5(fout, static_cast<size_t>(fstride[i]) << shift, st, m, fstride[i], m2); break;
      default: break;
    }
    m = m2;
  }
}

// The inverse MDCT of CELT (windowed, with the half-overlap in place): `in` is read with `stride` between coefficients, `out` receives
// N/2 + overlap/2 samples that add to what is already there.
void CeltDecoder::mdct_backward(const float* in, float* out, int shift, int stride) {
  int N = 1920;
  const float* trig = trig_;
  for (int i = 0; i < shift; i++) {
    N >>= 1;
    trig += N;
  }
  const int N2 = N >> 1, N4 = N >> 2;
  const int overlap = kOverlap;
  {
    const float* xp1 = in;
    const float* xp2 = in + stride * (N2 - 1);
    float* yp = out + (overlap >> 1);
    const float* t = &trig[0];
    const int16_t* bitrev = fft_[shift].bitrev;
    for (int i = 0; i < N4; i++) {
      const int rev = *bitrev++;
      const float yr = *xp2 * t[i] + *xp1 * t[N4 + i];
      const float yi = *xp1 * t[i] - *xp2 * t[N4 + i];
      yp[2 * rev + 1] = yr;
      yp[2 * rev] = yi;
      xp1 += 2 * stride;
      xp2 -= 2 * stride;
    }
  }
  // Cpx is two floats (alignment of float, no padding), so the interleaved results of the loop above are an array of Cpx.
  fft_impl(fft_[shift], reinterpret_cast<Cpx*>(out + (overlap >> 1)));
  {
    float* yp0 = out + (overlap >> 1);
    float* yp1 = out + (overlap >> 1) + N2 - 2;
    const float* t = &trig[0];
    for (int i = 0; i < (N4 + 1) >> 1; i++) {
      float re = yp0[1], im = yp0[0];
      float t0 = t[i], t1 = t[N4 + i];
      float yr = re * t0 + im * t1;
      float yi = re * t1 - im * t0;
      re = yp1[1];
      im = yp1[0];
      yp0[0] = yr;
      yp1[1] = yi;
      t0 = t[(N4 - i - 1)];
      t1 = t[(N2 - i - 1)];
      yr = re * t0 + im * t1;
      yi = re * t1 - im * t0;
      yp1[0] = yr;
      yp0[1] = yi;
      yp0 += 2;
      yp1 -= 2;
    }
  }
  {
    float* xp1 = out + overlap - 1;
    float* yp1 = out;
    const float* wp1 = window_;
    const float* wp2 = window_ + overlap - 1;
    for (int i = 0; i < overlap / 2; i++) {
      const float x1 = *xp1, x2 = *yp1;
      *yp1++ = *wp2 * x2 - *wp1 * x1;
      *xp1-- = *wp1 * x2 + *wp2 * x1;
      wp1++;
      wp2--;
    }
  }
}

void CeltDecoder::synthesis(const float* X, float* out_syn[2], const float* old_band_e, int start, int eff_end, int C, int CC,
                            bool transient, int LM, bool silence) {
  const int M = 1 << LM;
  const int N = 120 << LM;
  int B, NB, shift;
  if (transient) {
    B = M;
    NB = 120;
    shift = 3;
  } else {
    B = 1;
    NB = 120 << LM;
    shift = 3 - LM;
  }
  float freq[kMaxFrame];
  if (CC == 2 && C == 1) {
    denormalise_bands(X, freq, old_band_e, start, eff_end, M, silence);
    float* freq2 = out_syn[1] + kOverlap / 2;
    for (int i = 0; i < N; i++) freq2[i] = freq[i];
    for (int b = 0; b < B; b++) mdct_backward(&freq2[b], out_syn[0] + NB * b, shift, B);
    for (int b = 0; b < B; b++) mdct_backward(&freq[b], out_syn[1] + NB * b, shift, B);
  } else if (CC == 1 && C == 2) {
    float* freq2 = out_syn[0] + kOverlap / 2;
    denormalise_bands(X, freq, old_band_e, start, eff_end, M, silence);
    denormalise_bands(X + N, freq2, old_band_e + kBands, start, eff_end, M, silence);
    for (int i = 0; i < N; i++) freq[i] = .5f * freq[i] + .5f * freq2[i];
    for (int b = 0; b < B; b++) mdct_backward(&freq[b], out_syn[0] + NB * b, shift, B);
  } else {
    int c = 0;
    do {
      denormalise_bands(X + c * N, freq, old_band_e + c * kBands, start, eff_end, M, silence);
      for (int b = 0; b < B; b++) mdct_backward(&freq[b], out_syn[c] + NB * b, shift, B);
    } while (++c < CC);
  }
  for (int c = 0; c < CC; c++)
    for (int i = 0; i < N; i++) out_syn[c][i] = out_syn[c][i] > 300000000.f ? 300000000.f : out_syn[c][i] < -300000000.f ? -300000000.f : out_syn[c][i];
}

void CeltDecoder::deemphasis(float* in[2], float* pcm, int N) {
  const float coef0 = 0.85000610f;
  for (int c = 0; c < channels_; c++) {
    float m = preemph_mem_[c];
    const float* x = in[c];
    float* y = pcm + c;
    for (int j = 0; j < N; j++) {
      const float tmp = x[j] + 1e-30f + m;
      m = coef0 * tmp;
      y[j * channels_] = tmp * (1.f / 32768.f);
    }
    preemph_mem_[c] = m;
  }
}

void CeltDecoder::comb_filter(float* y, float* x, int T0, int T1, int N, float g0, float g1, int tapset0, int tapset1) {
  static const float gains[3][3] = {{0.3066406250f, 0.2170410156f, 0.1296386719f}, {0.4638671875f, 0.2680664062f, 0.f}, {0.7998046875f, 0.1000976562f, 0.f}};
  if (g0 == 0 && g1 == 0) {
    if (x != y)
      for (int i = 0; i < N; i++) y[i] = x[i];
    return;
  }
  T0 = imax(T0, kCombMinPeriod);
  T1 = imax(T1, kCombMinPeriod);
  const float g00 = g0 * gains[tapset0][0], g01 = g0 * gains[tapset0][1], g02 = g0 * gains[tapset0][2];
  const float g10 = g1 * gains[tapset1][0], g11 = g1 * gains[tapset1][1], g12 = g1 * gains[tapset1][2];
  float x1 = x[-T1 + 1], x2 = x[-T1], x3 = x[-T1 - 1], x4 = x[-T1 - 2];
  int overlap = kOverlap;
  if (g0 == g1 && T0 == T1 && tapset0 == tapset1) overlap = 0;
  int i;
  for (i = 0; i < overlap; i++) {
    const float x0 = x[i - T1 + 2];
    const float f = window_[i] * window_[i];
    y[i] = x[i] + ((1.f - f) * g00) * x[i - T0] + ((1.f - f) * g01) * (x[i - T0 + 1] + x[i - T0 - 1]) +
           ((1.f - f) * g02) * (x[i - T0 + 2] + x[i - T0 - 2]) + (f * g10) * x2 + (f * g11) * (x1 + x3) + (f * g12) * (x0 + x4);
    x4 = x3;
    x3 = x2;
    x2 = x1;
    x1 = x0;
  }
  if (g1 == 0) {
    if (x != y)
      for (int k = overlap; k < N; k++) y[k] = x[k];
    return;
  }
  // The constant part: y = x + g10 x[-T] + g11 (x[-T+1] + x[-T-1]) + g12 (x[-T+2] + x[-T-2]), with x the already filtered signal.
  float *yy = y + i, *xx = x + i;
  x4 = xx[-T1 - 2];
  x3 = xx[-T1 - 1];
  x2 = xx[-T1];
  x1 = xx[-T1 + 1];
  for (int k = 0; k < N - i; k++) {
    const float x0 = xx[k - T1 + 2];
    yy[k] = xx[k] + g10 * x2 + g11 * (x1 + x3) + g12 * (x0 + x4);
    x4 = x3;
    x3 = x2;
    x2 = x1;
    x1 = x0;
  }
}

// ---- the frame (celt_decoder.c) ----------------------------------------------------------------------------------------------

void CeltDecoder::init(int channels) {
  channels_ = stream_channels_ = channels;
  start_ = 0;
  end_ = kBands;
  disable_inv_ = channels == 1;
  reset();
}

void CeltDecoder::reset() {
  rng_ = 0;
  postfilter_period_ = postfilter_period_old_ = 0;
  postfilter_gain_ = postfilter_gain_old_ = 0;
  postfilter_tapset_ = postfilter_tapset_old_ = 0;
  preemph_mem_[0] = preemph_mem_[1] = 0;
  for (auto& ch : decode_mem_)
    for (float& f : ch) f = 0;
  for (int i = 0; i < 2 * kBands; i++) {
    old_band_e_[i] = 0;
    background_log_e_[i] = 0;
    old_log_e_[i] = old_log_e2_[i] = -28.f;
  }
}

int CeltDecoder::decode(RangeDecoder& dec, int len, float* pcm, int frame_size) {
  const int CC = channels_;
  int C = stream_channels_;
  const int start = start_, end = end_;
  int LM;
  for (LM = 0; LM <= 3; LM++)
    if (120 << LM == frame_size) break;
  if (LM > 3) return -1;
  const int M = 1 << LM;
  if (len < 0 || len > 1275 || pcm == nullptr) return -1;
  const int N = M * 120;
  float* decode_mem[2];
  float* out_syn[2];
  for (int c = 0; c < CC; c++) {
    decode_mem[c] = decode_mem_[c];
    out_syn[c] = decode_mem[c] + kDecodeBuffer - N;
  }
  int effEnd = end;
  if (effEnd > kBands) effEnd = kBands;
  if (len <= 1) return -1;
  if (C == 1)
    for (int i = 0; i < kBands; i++) old_band_e_[i] = old_band_e_[i] > old_band_e_[kBands + i] ? old_band_e_[i] : old_band_e_[kBands + i];
  const int32_t total_bits0 = len * 8;
  int32_t tell = dec.tell();
  int silence;
  if (tell >= total_bits0) silence = 1;
  else if (tell == 1) silence = dec.bit_logp(15);
  else silence = 0;
  if (silence) {
    tell = len * 8;
    dec.add_bits(tell - dec.tell());
  }
  float postfilter_gain = 0;
  int postfilter_pitch = 0, postfilter_tapset = 0;
  if (start == 0 && tell + 16 <= total_bits0) {
    if (dec.bit_logp(1)) {
      const int octave = static_cast<int>(dec.uint_(6));
      postfilter_pitch = (16 << octave) + static_cast<int>(dec.bits(static_cast<unsigned>(4 + octave))) - 1;
      const int qg = static_cast<int>(dec.bits(3));
      if (dec.tell() + 2 <= total_bits0) postfilter_tapset = dec.icdf(kCeltTapsetIcdf, 2);
      postfilter_gain = 0.09375f * (qg + 1);
    }
    tell = dec.tell();
  }
  int isTransient = 0;
  if (LM > 0 && tell + 3 <= total_bits0) {
    isTransient = dec.bit_logp(3);
    tell = dec.tell();
  }
  const int shortBlocks = isTransient ? M : 0;
  const int intra_ener = tell + 3 <= total_bits0 ? dec.bit_logp(3) : 0;
  unquant_coarse_energy(start, end, old_band_e_, intra_ener, dec, C, LM);
  int tf_res[kBands];
  tf_decode(start, end, isTransient, tf_res, LM, dec);
  tell = dec.tell();
  int spread_decision = kSpreadNormal;
  if (tell + 4 <= total_bits0) spread_decision = dec.icdf(kCeltSpreadIcdf, 5);
  int cap[kBands], offsets[kBands];
  init_caps(cap, LM, C);
  int dynalloc_logp = 6;
  int32_t total_bits = total_bits0 << kBitRes;
  int32_t tellf = static_cast<int32_t>(dec.tell_frac());
  for (int i = start; i < end; i++) {
    const int width = C * (eband(i + 1) - eband(i)) << LM;
    const int quanta = imin(width << kBitRes, imax(6 << kBitRes, width));
    int dynalloc_loop_logp = dynalloc_logp;
    int boost = 0;
    while (tellf + (dynalloc_loop_logp << kBitRes) < total_bits && boost < cap[i]) {
      const int flag = dec.bit_logp(static_cast<unsigned>(dynalloc_loop_logp));
      tellf = static_cast<int32_t>(dec.tell_frac());
      if (!flag) break;
      boost += quanta;
      total_bits -= quanta;
      dynalloc_loop_logp = 1;
    }
    offsets[i] = boost;
    if (boost > 0) dynalloc_logp = imax(2, dynalloc_logp - 1);
  }
  int fine_quant[kBands];
  const int alloc_trim = tellf + (6 << kBitRes) <= total_bits ? dec.icdf(kCeltTrimIcdf, 7) : 5;
  int32_t bits = ((static_cast<int32_t>(len) * 8) << kBitRes) - static_cast<int32_t>(dec.tell_frac()) - 1;
  const int anti_collapse_rsv = isTransient && LM >= 2 && bits >= ((LM + 2) << kBitRes) ? (1 << kBitRes) : 0;
  bits -= anti_collapse_rsv;
  int pulses[kBands], fine_priority[kBands];
  int intensity = 0, dual_stereo = 0;
  int32_t balance = 0;
  const int codedBands = compute_allocation(start, end, offsets, cap, alloc_trim, &intensity, &dual_stereo, bits, &balance, pulses,
                                            fine_quant, fine_priority, C, LM, dec);
  unquant_fine_energy(start, end, old_band_e_, fine_quant, dec, C);
  for (int c = 0; c < CC; c++) {
    float* dm = decode_mem[c];
    for (int i = 0; i < kDecodeBuffer - N + kOverlap / 2; i++) dm[i] = dm[i + N];
  }
  unsigned char collapse_masks[2 * kBands];
  float X[2 * kMaxFrame];
  quant_all_bands(start, end, X, C == 2 ? X + N : nullptr, collapse_masks, pulses, shortBlocks, spread_decision, dual_stereo, intensity,
                  tf_res, len * (8 << kBitRes) - anti_collapse_rsv, balance, dec, LM, codedBands, &rng_, disable_inv_);
  int anti_collapse_on = 0;
  if (anti_collapse_rsv > 0) anti_collapse_on = static_cast<int>(dec.bits(1));
  unquant_energy_finalise(start, end, old_band_e_, fine_quant, fine_priority, len * 8 - dec.tell(), dec, C);
  if (anti_collapse_on) anti_collapse(X, collapse_masks, LM, C, N, start, end, old_band_e_, old_log_e_, old_log_e2_, pulses, rng_);
  if (silence)
    for (int i = 0; i < C * kBands; i++) old_band_e_[i] = -28.f;
  synthesis(X, out_syn, old_band_e_, start, effEnd, C, CC, isTransient != 0, LM, silence != 0);
  for (int c = 0; c < CC; c++) {
    postfilter_period_ = imax(postfilter_period_, kCombMinPeriod);
    postfilter_period_old_ = imax(postfilter_period_old_, kCombMinPeriod);
    comb_filter(out_syn[c], out_syn[c], postfilter_period_old_, postfilter_period_, 120, postfilter_gain_old_, postfilter_gain_,
                postfilter_tapset_old_, postfilter_tapset_);
    if (LM != 0)
      comb_filter(out_syn[c] + 120, out_syn[c] + 120, postfilter_period_, postfilter_pitch, N - 120, postfilter_gain_, postfilter_gain,
                  postfilter_tapset_, postfilter_tapset);
  }
  postfilter_period_old_ = postfilter_period_;
  postfilter_gain_old_ = postfilter_gain_;
  postfilter_tapset_old_ = postfilter_tapset_;
  postfilter_period_ = postfilter_pitch;
  postfilter_gain_ = postfilter_gain;
  postfilter_tapset_ = postfilter_tapset;
  if (LM != 0) {
    postfilter_period_old_ = postfilter_period_;
    postfilter_gain_old_ = postfilter_gain_;
    postfilter_tapset_old_ = postfilter_tapset_;
  }
  if (C == 1)
    for (int i = 0; i < kBands; i++) old_band_e_[kBands + i] = old_band_e_[i];
  if (!isTransient) {
    for (int i = 0; i < 2 * kBands; i++) old_log_e2_[i] = old_log_e_[i];
    for (int i = 0; i < 2 * kBands; i++) old_log_e_[i] = old_band_e_[i];
  } else {
    for (int i = 0; i < 2 * kBands; i++) old_log_e_[i] = fmin_(old_log_e_[i], old_band_e_[i]);
  }
  const float max_background_increase = static_cast<float>(imin(160, 0 + M)) * 0.001f;
  for (int i = 0; i < 2 * kBands; i++) background_log_e_[i] = fmin_(background_log_e_[i] + max_background_increase, old_band_e_[i]);
  for (int c = 0; c < 2; c++) {
    for (int i = 0; i < start; i++) {
      old_band_e_[c * kBands + i] = 0;
      old_log_e_[c * kBands + i] = old_log_e2_[c * kBands + i] = -28.f;
    }
    for (int i = end; i < kBands; i++) {
      old_band_e_[c * kBands + i] = 0;
      old_log_e_[c * kBands + i] = old_log_e2_[c * kBands + i] = -28.f;
    }
  }
  rng_ = dec.range();
  deemphasis(out_syn, pcm, N);
  if (dec.tell() > 8 * len) return -1;
  return frame_size;
}

}  // namespace opus
}  // namespace media
