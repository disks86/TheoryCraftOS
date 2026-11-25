/* What the wave-operation fixtures (tests/data/dxbc/dxil_cs_wave*.hlsl, dxil_ps_wave.hlsl) must give, worked out from what HLSL says the operations do and the waves the
 * kernel's CPU renderer makes (graphics::kWaveLanesCompute = 32 lanes of a compute group in SV_GroupIndex order; a pixel shader's wave is its 2 x 2 quad). The active lanes of
 * an operation are the lanes of its wave that reach it. Plain C, for tests/windows/d3d12test.c and the host test tests/WaveOpsTest.cpp. */
#ifndef TCOS_WAVE_MODEL_H
#define TCOS_WAVE_MODEL_H
#include <stdint.h>
#include <string.h>
#ifndef INT64_MAX
#define INT64_MAX 0x7FFFFFFFFFFFFFFFll
#define INT64_MIN (-INT64_MAX - 1)
/* dxil_ps_wave: the colour (each channel 0 - 255) of corner `corner` (x & 1 | (y & 1) << 1) of the quad whose upper left pixel is (qx, qy), whose corners the primitive covers
 * as `covered` says (bit k: corner k). */
static void wave_model_ps(uint32_t qx, uint32_t qy, uint32_t covered, uint32_t corner, uint32_t rgba[4]) {
  uint32_t active = 0;
  for (uint32_t k = 0; k < 4; ++k) {
    const uint32_t x = qx + (k & 1), y = qy + (k >> 1);
    if ((covered >> k & 1) && !((x & 3) == 3 && (y & 3) == 3)) active |= 1u << k;
  }
  const uint32_t count = (uint32_t)__builtin_popcount(active);
  const uint32_t odd = (active >> 1 & 1) + (active >> 3 & 1);
  const uint32_t prefix = (uint32_t)__builtin_popcount(active & ((1u << corner) - 1));
  const uint32_t first = active ? (uint32_t)__builtin_ctz(active) : 99;
  rgba[0] = count * 10 + 5;
  rgba[1] = ((corner & 1) ? 0 : 100) + ((corner & 2) ? 0 : 40) + ((corner & 1) ? odd * 5 : 0);
  rgba[2] = prefix * 20 + (first == corner ? 5 : 0) + 4 * 2;
  rgba[3] = active * 8;
}

#endif

#define WAVE_LANES 32u
#define WAVE_UNWRITTEN 0xDEADBEEFu

/* dxil_cs_wave: 48 threads, 24 words each. */
#define WAVE_CS_THREADS 48u
#define WAVE_CS_WORDS 24u
static void wave_model_cs(uint32_t *o) {
  for (uint32_t k = 0; k < WAVE_CS_THREADS * WAVE_CS_WORDS; ++k) o[k] = WAVE_UNWRITTEN;
  for (uint32_t first = 0; first < WAVE_CS_THREADS; first += WAVE_LANES) {
    const uint32_t n = WAVE_CS_THREADS - first < WAVE_LANES ? WAVE_CS_THREADS - first : WAVE_LANES;
    for (uint32_t lane = 0; lane < n; ++lane) {
      const uint32_t i = first + lane, b = i * WAVE_CS_WORDS;
      uint32_t *r = o + b;
      r[0] = lane;
      r[1] = WAVE_LANES;
      r[2] = lane == 0;
      const int in_a = i % 3 != 0;
      /* the lanes of this lane's side of the branch */
      uint32_t sum = 0, product = 1, min = 0xFFFFFFFFu, max = 0, prefix = 0, odd = 0, prefix_four = 0, first_lane = 99, ballot = 0, prefix_product = 1;
      int32_t smin = 0x7FFFFFFF, smax = (int32_t)0x80000000u, ssum = 0, sprefix = 0;
      uint32_t band = 0xFFFFFFFFu, bor = 0, bxor = 0, all_gt2 = 1, any_gt40 = 0, any_gt100 = 0, all_lt100 = 1, all_same_i = 1, side = 0;
      for (uint32_t m = 0; m < n; ++m) {
        const uint32_t j = first + m;
        if ((j % 3 != 0) != in_a) continue;
        side |= 1u << m;
        if (first_lane == 99) first_lane = m;
        if (j != first + first_lane) all_same_i = 0;
        sum += j, product *= j % 4 + 1;
        if (j < min) min = j;
        if (j > max) max = j;
        if (m < lane) prefix += j, prefix_product *= j % 3, prefix_four += (j & 4) != 0, sprefix += (int32_t)j - 20;
        if (j & 1) ++odd, ballot |= 1u << m;
        const int32_t s = (int32_t)j - 20;
        if (s < smin) smin = s;
        if (s > smax) smax = s;
        ssum += s;
        band &= j | 0x100, bor |= j, bxor ^= j;
        if (!(j > 2)) all_gt2 = 0;
        if (j > 40) any_gt40 = 1;
        if (j > 100) any_gt100 = 1;
        if (!(j < 100)) all_lt100 = 0;
      }
      if (in_a) {
        r[3] = sum, r[4] = product, r[5] = min, r[6] = max, r[7] = prefix, r[8] = odd, r[9] = prefix_four, r[10] = first + first_lane, r[11] = lane == first_lane;
        r[12] = ballot, r[13] = prefix_product;
      } else {
        r[3] = (uint32_t)smin, r[4] = (uint32_t)smax, r[5] = (uint32_t)ssum, r[6] = band, r[7] = bor, r[8] = bxor;
        r[9] = all_gt2 + 2 * any_gt40 + 4 * any_gt100 + 8 * all_lt100;
        r[10] = 1, r[11] = all_same_i, r[12] = side, r[13] = (uint32_t)sprefix;
      }
      /* the loop: round k's sum over the lanes still in it (those with k < their (i % 5) + 2) */
      uint32_t acc = 0;
      for (uint32_t k = 0; k < 8 && k < i % 5 + 2; ++k)
        for (uint32_t m = 0; m < n; ++m)
          if (k < (first + m) % 5 + 2) acc += k + first + m;
      r[14] = acc;
      r[15] = (first + 5) * 3;
      r[16] = first + (lane ^ 1), r[17] = first + (lane ^ 2), r[18] = first + (lane ^ 3), r[19] = first + ((lane & ~3u) | 2);
      uint32_t ge8 = 0;
      for (uint32_t m = 0; m < n; ++m)
        if (first + m >= 8) ge8 |= 1u << m;
      r[20] = ge8;
      if (lane == 0 || lane == 9) continue;
      uint32_t before = 0;
      for (uint32_t m = 1; m < lane; ++m)
        if (m != 9) ++before;
      r[21] = n - 2, r[22] = before, r[23] = first + 1 + 1000 + (lane == 1 ? 0x10000u : 0u);
    }
  }
}

static uint32_t wave_f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void wave_put64(uint32_t *at, uint64_t v) { at[0] = (uint32_t)v, at[1] = (uint32_t)(v >> 32); }
static void wave_put_double(uint32_t *at, double d) { uint64_t u; memcpy(&u, &d, 8); wave_put64(at, u); }

/* dxil_cs_wave_types: 40 threads, 28 words each. */
#define WAVE_TYPES_THREADS 40u
#define WAVE_TYPES_WORDS 28u
static void wave_model_cs_types(uint32_t *o) {
  for (uint32_t k = 0; k < WAVE_TYPES_THREADS * WAVE_TYPES_WORDS; ++k) o[k] = WAVE_UNWRITTEN;
  for (uint32_t first = 0; first < WAVE_TYPES_THREADS; first += WAVE_LANES) {
    const uint32_t n = WAVE_TYPES_THREADS - first < WAVE_LANES ? WAVE_TYPES_THREADS - first : WAVE_LANES;
    for (uint32_t lane = 0; lane < n; ++lane) {
      const uint32_t i = first + lane;
      uint32_t *r = o + i * WAVE_TYPES_WORDS;
      float fsum = 0, fmin = 1e30f, fmax = -1e30f, fprefix = 0, fproduct = 1, fprefix_product = 1, nfsum = 0;
      double dsum = 0, dmin = 1e300, dmax = -1e300, dprefix = 0, dproduct = 1, dprefix_product = 1;
      uint64_t lsum = 0, lprefix = 0, lumax = 0, lumin = ~(uint64_t)0, lor = 0, land = ~(uint64_t)0, lxor = 0, lproduct = 1;
      int64_t lmin = INT64_MAX, lmax = INT64_MIN;
      for (uint32_t m = 0; m < n; ++m) {
        const uint32_t j = first + m;
        if ((j & 1) != (i & 1)) continue;
        const float f = (float)j * 0.5f - 3.0f;
        const double d = (double)j * 0.25 - 1.0;
        const int64_t l = (int64_t)j * 0x100000001ll - 50;
        const uint64_t ul = (uint64_t)l;
        fsum += f, nfsum += -f;
        if (f < fmin) fmin = f;
        if (f > fmax) fmax = f;
        fproduct *= j % 3 == 0 ? 2.0f : 1.0f;
        dsum += d;
        if (d < dmin) dmin = d;
        if (d > dmax) dmax = d;
        dproduct *= j % 4 == 0 ? 2.0 : 1.0;
        lsum += ul, lor |= ul, land &= ul | 0xF0000000F0ull, lxor ^= ul, lproduct *= (uint64_t)(j % 3 + 1);
        if (l < lmin) lmin = l;
        if (l > lmax) lmax = l;
        if (ul > lumax) lumax = ul;
        if (ul < lumin) lumin = ul;
        if (m < lane) {
          fprefix += f, fprefix_product *= j % 5 == 0 ? -2.0f : 1.0f;
          dprefix += d, dprefix_product *= j % 6 == 0 ? 0.5 : 1.0;
          lprefix += ul;
        }
      }
      if (i & 1) {
        r[0] = wave_f32_bits(fsum), r[1] = wave_f32_bits(fmin), r[2] = wave_f32_bits(fmax), r[3] = wave_f32_bits(fprefix), r[4] = wave_f32_bits(fproduct);
        r[5] = wave_f32_bits(fprefix_product);
        wave_put_double(r + 6, dsum), wave_put_double(r + 8, dmin), wave_put_double(r + 10, dprefix);
        wave_put64(r + 12, lsum), wave_put64(r + 14, (uint64_t)lmin), wave_put64(r + 16, lumax), wave_put64(r + 18, lprefix);
      } else {
        wave_put_double(r + 0, dmax), wave_put_double(r + 2, dproduct), wave_put_double(r + 4, dprefix_product);
        wave_put64(r + 6, (uint64_t)lmax), wave_put64(r + 8, lumin), wave_put64(r + 10, lor), wave_put64(r + 12, land), wave_put64(r + 14, lxor), wave_put64(r + 16, lproduct);
        r[18] = wave_f32_bits(nfsum);
        r[19] = 2 + 8 + 16;   /* (every lane's d * 0, (i / 32) and f * 0 are equal; f and l are not) */
      }
      wave_put_double(r + 20, (double)first * 0.25 - 1.0);
      wave_put_double(r + 22, (double)(first + (lane ^ 1)) * 0.25 - 1.0);
      wave_put64(r + 24, (uint64_t)((int64_t)(first + 3) * 0x100000001ll - 50));
      r[26] = (uint32_t)((int64_t)(first + ((lane & ~3u) | 1)) * 0x100000001ll - 50);
      r[27] = wave_f32_bits((float)(first + (lane ^ 3)) * 0.5f - 3.0f);
    }
  }
}

/* dxil_cs_wave16: 36 threads, 8 words each. */
#define WAVE16_THREADS 36u
#define WAVE16_WORDS 8u
static void wave_model_cs16(uint32_t *o) {
  for (uint32_t k = 0; k < WAVE16_THREADS * WAVE16_WORDS; ++k) o[k] = WAVE_UNWRITTEN;
  for (uint32_t first = 0; first < WAVE16_THREADS; first += WAVE_LANES) {
    const uint32_t n = WAVE16_THREADS - first < WAVE_LANES ? WAVE16_THREADS - first : WAVE_LANES;
    for (uint32_t lane = 0; lane < n; ++lane) {
      const uint32_t i = first + lane;
      if ((i / 4) % 3 == 1) continue;
      uint32_t *r = o + i * WAVE16_WORDS;
      float hsum = 0, hmax = -1e30f;
      int32_t smin = 0x7FFFFFFF, smax = -0x7FFFFFFF;
      uint32_t usum = 0, umax = 0, sprefix = 0, first_u = 99999;
      for (uint32_t m = 0; m < n; ++m) {
        const uint32_t j = first + m;
        if ((j / 4) % 3 == 1) continue;
        const float h = (float)j * 0.25f - 2.0f;
        const int32_t sv = (int32_t)j * 1500 - 20000;
        const uint32_t u = (j * 3000) & 0xFFFF;
        if (first_u == 99999) first_u = u;
        hsum += h;
        if (h > hmax) hmax = h;
        if (sv < smin) smin = sv;
        if (sv > smax) smax = sv;
        usum += u;
        if (u > umax) umax = u;
        if (m < lane) sprefix += (uint32_t)sv;
      }
      r[0] = wave_f32_bits(hsum), r[1] = wave_f32_bits(hmax), r[2] = (uint32_t)smin, r[3] = (uint32_t)smax, r[4] = usum & 0xFFFF, r[5] = umax;
      r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)(sprefix & 0xFFFF);
      r[7] = first_u + ((((first + (lane ^ 1)) * 3000) & 0xFFFF) << 16);
    }
  }
}

/* dxil_cs_wave_match: 40 threads, 12 words each. */
#define WAVE_MATCH_THREADS 40u
#define WAVE_MATCH_WORDS 12u
static void wave_model_cs_match(uint32_t *o) {
  for (uint32_t k = 0; k < WAVE_MATCH_THREADS * WAVE_MATCH_WORDS; ++k) o[k] = WAVE_UNWRITTEN;
  uint32_t shared_sum[2] = {0, 0};
  for (uint32_t first = 0; first < WAVE_MATCH_THREADS; first += WAVE_LANES) {
    const uint32_t n = WAVE_MATCH_THREADS - first < WAVE_LANES ? WAVE_MATCH_THREADS - first : WAVE_LANES;
    for (uint32_t m = 0; m < n; ++m)
      if (first + m != 5) shared_sum[first / 32] += first + m;
  }
  for (uint32_t first = 0; first < WAVE_MATCH_THREADS; first += WAVE_LANES) {
    const uint32_t n = WAVE_MATCH_THREADS - first < WAVE_LANES ? WAVE_MATCH_THREADS - first : WAVE_LANES;
    for (uint32_t lane = 0; lane < n; ++lane) {
      const uint32_t i = first + lane;
      uint32_t *r = o + i * WAVE_MATCH_WORDS;
      r[10] = shared_sum[i / 32];
      r[11] = n * (shared_sum[0] + shared_sum[1]);
      if (i == 5) continue;
      uint32_t mask = 0, sum = 0, product = 1, count = 0, band = 0xFFFFFFFFu, bor = 0, bxor = 0, quarter = 0;
      float fsum = 0;
      for (uint32_t m = 0; m < n; ++m) {
        const uint32_t j = first + m;
        if (j == 5) continue;
        if (j / 4 == i / 4) quarter |= 1u << m;
        if (j % 3 != i % 3) continue;
        mask |= 1u << m;
        if (m >= lane) continue;
        sum += j, product *= j % 4 + 1, count += (j & 2) != 0, band &= j | 0x40, bor |= j, bxor ^= j, fsum += (float)j * 0.5f;
      }
      r[0] = mask, r[1] = 0, r[2] = sum, r[3] = product, r[4] = count, r[5] = band, r[6] = bor, r[7] = bxor, r[8] = wave_f32_bits(fsum), r[9] = quarter;
    }
  }
}

/* dxil_ps_wave: the colour (each channel 0 - 255) of corner `corner` (x & 1 | (y & 1) << 1) of the quad whose upper left pixel is (qx, qy), whose corners the primitive covers
 * as `covered` says (bit k: corner k). */
static void wave_model_ps(uint32_t qx, uint32_t qy, uint32_t covered, uint32_t corner, uint32_t rgba[4]) {
  uint32_t active = 0;
  for (uint32_t k = 0; k < 4; ++k) {
    const uint32_t x = qx + (k & 1), y = qy + (k >> 1);
    if ((covered >> k & 1) && !((x & 3) == 3 && (y & 3) == 3)) active |= 1u << k;
  }
  const uint32_t count = (uint32_t)__builtin_popcount(active);
  const uint32_t odd = (active >> 1 & 1) + (active >> 3 & 1);
  const uint32_t prefix = (uint32_t)__builtin_popcount(active & ((1u << corner) - 1));
  const uint32_t first = active ? (uint32_t)__builtin_ctz(active) : 99;
  rgba[0] = count * 10 + 5;
  rgba[1] = ((corner & 1) ? 0 : 100) + ((corner & 2) ? 0 : 40) + ((corner & 1) ? odd * 5 : 0);
  rgba[2] = prefix * 20 + (first == corner ? 5 : 0) + 4 * 2;
  rgba[3] = active * 8;
}

#endif
