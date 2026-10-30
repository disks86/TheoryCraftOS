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
#include "Emulation/AArch64Fp8.hpp"

namespace emulation {
namespace fp8 {

bool is_nan(Fmt f, uint8_t v) {
  return f == Fmt::E4M3 ? (v & 0x7F) == 0x7F : (v & 0x7C) == 0x7C && (v & 3) != 0;
}

bool is_signaling(Fmt f, uint8_t v) { return is_nan(f, v) && (f == Fmt::E4M3 || !(v & 2)); }

uint32_t widen(Fmt f, uint8_t v, softfloat::Format to, uint32_t scale, bool* inexact) {
  using softfloat::Format;
  const uint32_t E = to == Format::F16 ? 5 : 8, M = to == Format::F16 ? 10 : to == Format::BF16 ? 7 : 23;
  const int32_t bias = to == Format::F16 ? 15 : 127;
  const uint32_t sign = (v & 0x80) ? 1u << (E + M) : 0;
  *inexact = false;
  const uint32_t inf = ((1u << E) - 1) << M;
  if (is_nan(f, v)) return inf | (1u << (M - 1));      // (the default NaN)
  uint32_t mant;
  int32_t exp;
  if (f == Fmt::E5M2) {
    const uint32_t e = (v >> 2) & 31, m = v & 3;
    if (e == 31) return sign | inf;
    if (e == 0) { mant = m; exp = -16; } else { mant = 4 | m; exp = static_cast<int32_t>(e) - 15 - 2; }
  } else {
    const uint32_t e = (v >> 3) & 15, m = v & 7;
    if (e == 0) { mant = m; exp = -9; } else { mant = 8 | m; exp = static_cast<int32_t>(e) - 7 - 3; }
  }
  if (mant == 0) return sign;
  exp -= static_cast<int32_t>(scale);
  uint32_t p = 0;
  while ((mant >> (p + 1)) != 0) ++p;
  const int32_t unbiased = static_cast<int32_t>(p) + exp;
  const int32_t quantum = (unbiased - static_cast<int32_t>(M)) > (1 - bias - static_cast<int32_t>(M)) ? unbiased - static_cast<int32_t>(M) : 1 - bias - static_cast<int32_t>(M);
  const int32_t shift = exp - quantum;      // the integer is mant * 2^shift, rounded
  uint64_t r;
  if (shift >= 0) r = static_cast<uint64_t>(mant) << shift;
  else {
    const uint32_t s = static_cast<uint32_t>(-shift);
    if (s > 40) { r = 0; *inexact = true; }
    else {
      const uint64_t whole = static_cast<uint64_t>(mant) >> s, rest = static_cast<uint64_t>(mant) & ((uint64_t{1} << s) - 1), half = uint64_t{1} << (s - 1);
      r = whole;
      if (rest != 0) *inexact = true;
      if (rest > half || (rest == half && (whole & 1))) ++r;
    }
  }
  int32_t q = quantum;
  if (r >= (uint64_t{1} << (M + 1))) { r >>= 1; ++q; }
  if (r < (uint64_t{1} << M)) return sign | static_cast<uint32_t>(r);      // subnormal (or zero)
  const int32_t field = q + static_cast<int32_t>(M) + bias;
  if (field >= static_cast<int32_t>((1u << E) - 1)) return sign | inf;
  return sign | (static_cast<uint32_t>(field) << M) | (static_cast<uint32_t>(r) & ((1u << M) - 1));
}

uint8_t narrow(Fmt f, softfloat::Format from, uint64_t bits, int32_t scale, bool saturate, uint32_t* flags) {
  using softfloat::Format;
  const uint32_t E = from == Format::F16 ? 5 : 8, M = from == Format::F16 ? 10 : from == Format::BF16 ? 7 : 23;
  const int32_t bias = from == Format::F16 ? 15 : 127;
  const bool neg = (bits >> (E + M)) & 1;
  const uint8_t sign = neg ? 0x80 : 0;
  const uint32_t field = static_cast<uint32_t>((bits >> M) & ((1u << E) - 1));
  const uint64_t frac = bits & ((uint64_t{1} << M) - 1);
  const uint8_t max_finite = f == Fmt::E5M2 ? 0x7B : 0x7E, overflow = f == Fmt::E5M2 ? 0x7C : 0x7F;
  const uint8_t nan = f == Fmt::E5M2 ? 0x7E : 0x7F;
  if (field == (1u << E) - 1) {
    if (frac != 0) {
      if (!((frac >> (M - 1)) & 1)) *flags |= softfloat::kInvalid;
      return nan;
    }
    return sign | (saturate ? max_finite : overflow);
  }
  if (field == 0 && frac == 0) return sign;
  uint64_t mant;
  int32_t exp;      // value = mant * 2^exp
  if (field == 0) { mant = frac; exp = 1 - bias - static_cast<int32_t>(M); }
  else { mant = frac | (uint64_t{1} << M); exp = static_cast<int32_t>(field) - bias - static_cast<int32_t>(M); }
  exp += scale;
  const uint32_t mbits = f == Fmt::E5M2 ? 2 : 3;
  const int32_t min_exp = f == Fmt::E5M2 ? -14 : -6, bias8 = f == Fmt::E5M2 ? 15 : 7;
  uint32_t p = 0;
  while ((mant >> (p + 1)) != 0) ++p;
  const int32_t unbiased = static_cast<int32_t>(p) + exp;
  const int32_t quantum = (unbiased - static_cast<int32_t>(mbits)) > (min_exp - static_cast<int32_t>(mbits)) ? unbiased - static_cast<int32_t>(mbits) : min_exp - static_cast<int32_t>(mbits);
  const int32_t shift = exp - quantum;
  uint64_t r;
  bool inexact = false;
  if (shift >= 0) r = shift > 40 ? (uint64_t{1} << 62) : mant << shift;
  else {
    const uint32_t sh = static_cast<uint32_t>(-shift);
    if (sh > 62) { r = 0; inexact = true; }
    else {
      const uint64_t whole = mant >> sh, rest = mant & ((uint64_t{1} << sh) - 1), half = uint64_t{1} << (sh - 1);
      r = whole;
      inexact = rest != 0;
      if (rest > half || (rest == half && (whole & 1))) ++r;
    }
  }
  const bool tiny = unbiased < min_exp;
  int32_t q = quantum;
  if (r >= (uint64_t{1} << (mbits + 1))) { while (r >= (uint64_t{1} << (mbits + 1))) { r >>= 1; ++q; } }
  uint8_t result;
  if (r < (uint64_t{1} << mbits)) result = sign | static_cast<uint8_t>(r);
  else {
    const int32_t e8 = q + static_cast<int32_t>(mbits) + bias8;
    const uint32_t m8 = static_cast<uint32_t>(r) & ((1u << mbits) - 1);
    const int32_t top = f == Fmt::E5M2 ? 31 : 15;
    const uint8_t candidate = static_cast<uint8_t>(sign | (static_cast<uint32_t>(e8 > 0 ? e8 : 0) << mbits) | m8);
    const bool over = e8 > top || (f == Fmt::E5M2 ? e8 == top : (e8 == top && m8 == 7)) || (e8 > top);
    if (over) { *flags |= softfloat::kOverflow | softfloat::kInexact; return sign | (saturate ? max_finite : overflow); }
    result = candidate;
  }
  if (inexact) { *flags |= softfloat::kInexact; if (tiny) *flags |= softfloat::kUnderflow; }
  return result;
}

uint32_t multiply_add(Fmt fa, uint8_t a, Fmt fb, uint8_t b, uint32_t scale, softfloat::Format dst, uint32_t acc, bool saturate, softfloat::Env& env) {
  using namespace softfloat;
  bool inexact;
  const uint32_t x = widen(fa, a, Format::F32, 0, &inexact), y = widen(fb, b, Format::F32, scale, &inexact);
  Env wide = env;
  wide.flags = 0;
  uint32_t result;
  if (dst == Format::F32) {
    result = static_cast<uint32_t>(fused_multiply_add(Format::F32, x, y, acc, wide));
    if (softfloat::is_nan(Format::F32, result)) result = 0x7FC00000u;
  } else {      // half precision accumulator: the sum in binary64 (the product is exact there) and a single conversion
    const uint64_t xd = convert(Format::F32, Format::F64, x, wide), yd = convert(Format::F32, Format::F64, y, wide), ad = convert(Format::F16, Format::F64, acc, wide);
    const uint64_t sum = fused_multiply_add(Format::F64, xd, yd, ad, wide);
    result = static_cast<uint32_t>(convert(Format::F64, Format::F16, sum, wide)) & 0xFFFF;
    if (softfloat::is_nan(Format::F16, result)) result = 0x7E00;
  }
  if (saturate) {      // FPMR.OSM: an overflow gives the largest finite value instead of an infinity
    const bool f32 = dst == Format::F32;
    const uint32_t inf = f32 ? 0x7F800000u : 0x7C00u, mask = f32 ? 0x7FFFFFFFu : 0x7FFFu, sign = f32 ? 0x80000000u : 0x8000u;
    const bool inputs_finite = !softfloat::is_nan(Format::F32, x) && (x & 0x7FFFFFFFu) != 0x7F800000u && (y & 0x7FFFFFFFu) != 0x7F800000u && (acc & mask) < inf;
    if ((result & mask) == inf && inputs_finite) result = (result & sign) | (inf - 1);
  }
  (void)env;      // (the FP8 multiply-adds report no exceptions)
  return result;
}

namespace {

using I128 = __int128;
using U128 = unsigned __int128;

struct Term { bool neg; uint64_t mant; int32_t exp; };

// An 8-bit float as mant * 2^exp (zero has mant 0); kind 0 finite, 1 infinity, 2 NaN.
int decode8(Fmt f, uint8_t v, Term* t) {
  t->neg = (v & 0x80) != 0;
  if (is_nan(f, v)) { t->mant = 0; t->exp = 0; return 2; }
  if (f == Fmt::E5M2) {
    const uint32_t e = (v >> 2) & 31, m = v & 3;
    if (e == 31) { t->mant = 0; t->exp = 0; return 1; }
    if (e == 0) { t->mant = m; t->exp = -16; } else { t->mant = 4 | m; t->exp = static_cast<int32_t>(e) - 17; }
  } else {
    const uint32_t e = (v >> 3) & 15, m = v & 7;
    if (e == 0) { t->mant = m; t->exp = -9; } else { t->mant = 8 | m; t->exp = static_cast<int32_t>(e) - 10; }
  }
  return 0;
}

// An exact sum, mag * 2^exp, rounded to nearest even into the format.
uint32_t round_exact(bool neg, U128 mag, int32_t exp, softfloat::Format dst) {
  using softfloat::Format;
  const uint32_t E = dst == Format::F16 ? 5 : 8, M = dst == Format::F16 ? 10 : 23;
  const int32_t bias = dst == Format::F16 ? 15 : 127;
  const uint32_t sign = neg ? 1u << (E + M) : 0;
  if (mag == 0) return sign;
  uint32_t p = 127;
  while (!((mag >> p) & 1)) --p;
  const int32_t unbiased = static_cast<int32_t>(p) + exp;
  const int32_t lowest = 1 - bias - static_cast<int32_t>(M);
  const int32_t quantum = (unbiased - static_cast<int32_t>(M)) > lowest ? unbiased - static_cast<int32_t>(M) : lowest;
  const int32_t shift = exp - quantum;
  U128 r;
  if (shift >= 0) r = shift > 60 ? (U128{1} << 100) : mag << shift;
  else {
    const uint32_t sh = static_cast<uint32_t>(-shift);
    if (sh >= 127) r = 0;
    else {
      const U128 whole = mag >> sh, rest = mag & ((U128{1} << sh) - 1), half = U128{1} << (sh - 1);
      r = whole;
      if (rest > half || (rest == half && (whole & 1))) ++r;
    }
  }
  int32_t q = quantum;
  while (r >= (U128{1} << (M + 1))) { r >>= 1; ++q; }
  if (r < (U128{1} << M)) return sign | static_cast<uint32_t>(r);
  const int32_t field = q + static_cast<int32_t>(M) + bias;
  if (field >= static_cast<int32_t>((1u << E) - 1)) return sign | (((1u << E) - 1) << M);
  return sign | (static_cast<uint32_t>(field) << M) | (static_cast<uint32_t>(r) & ((1u << M) - 1));
}

}  // namespace

uint32_t dot_add(Fmt fa, const uint8_t* a, Fmt fb, const uint8_t* b, uint32_t n, uint32_t scale, softfloat::Format dst, uint32_t acc, bool saturate) {
  using softfloat::Format;
  const uint32_t E = dst == Format::F16 ? 5 : 8, M = dst == Format::F16 ? 10 : 23;
  const int32_t bias = dst == Format::F16 ? 15 : 127;
  const uint32_t inf = ((1u << E) - 1) << M, nan = inf | (1u << (M - 1)), sign_bit = 1u << (E + M);
  Term terms[9];
  uint32_t count = 0;
  bool nan_seen = false, pos_inf = false, neg_inf = false, all_neg = true;
  for (uint32_t i = 0; i < n; ++i) {
    Term x, y;
    const int kx = decode8(fa, a[i], &x), ky = decode8(fb, b[i], &y);
    const bool neg = x.neg != y.neg;
    if (kx == 2 || ky == 2) { nan_seen = true; continue; }
    if (kx == 1 || ky == 1) {
      if ((kx == 0 && x.mant == 0) || (ky == 0 && y.mant == 0)) { nan_seen = true; continue; }      // infinity times zero
      (neg ? neg_inf : pos_inf) = true;
      all_neg = false;
      continue;
    }
    if (!neg) all_neg = false;
    terms[count++] = Term{neg, x.mant * y.mant, x.exp + y.exp - static_cast<int32_t>(scale)};
  }
  // the accumulator
  const bool acc_neg = (acc & sign_bit) != 0;
  const uint32_t field = (acc >> M) & ((1u << E) - 1), frac = acc & ((1u << M) - 1);
  if (field == (1u << E) - 1) {
    if (frac != 0) nan_seen = true;
    else { (acc_neg ? neg_inf : pos_inf) = true; all_neg = false; }
  } else {
    if (!acc_neg) all_neg = false;
    if (field == 0) terms[count++] = Term{acc_neg, frac, 1 - bias - static_cast<int32_t>(M)};
    else terms[count++] = Term{acc_neg, frac | (1u << M), static_cast<int32_t>(field) - bias - static_cast<int32_t>(M)};
  }
  if (nan_seen || (pos_inf && neg_inf)) return nan;
  if (pos_inf || neg_inf) return neg_inf ? (sign_bit | inf) : inf;
  // align everything on the lowest exponent that still keeps the top within 120 bits
  int32_t top = -100000;
  for (uint32_t i = 0; i < count; ++i) {
    if (terms[i].mant == 0) continue;
    uint32_t p = 0;
    while ((terms[i].mant >> (p + 1)) != 0) ++p;
    if (static_cast<int32_t>(p) + terms[i].exp > top) top = static_cast<int32_t>(p) + terms[i].exp;
  }
  if (top == -100000) return all_neg ? sign_bit : 0;
  const int32_t base = top - 118;
  I128 sum = 0;
  bool lost_pos = false, lost_neg = false;      // bits that fell below the lowest position (only their sign matters)
  for (uint32_t i = 0; i < count; ++i) {
    if (terms[i].mant == 0) continue;
    I128 v;
    if (terms[i].exp >= base) v = static_cast<I128>(static_cast<U128>(terms[i].mant) << (terms[i].exp - base));
    else {
      const int32_t sh = base - terms[i].exp;
      const bool lost = sh >= 64 || (terms[i].mant & ((uint64_t{1} << sh) - 1)) != 0;
      v = sh >= 64 ? 0 : static_cast<I128>(terms[i].mant >> sh);
      if (lost) (terms[i].neg ? lost_neg : lost_pos) = true;
    }
    sum += terms[i].neg ? -v : v;
  }
  sum *= 4;      // two spare bits at the bottom carry the sticky information
  if (lost_pos != lost_neg) sum += lost_pos ? 1 : -1;
  if (sum == 0) return all_neg ? sign_bit : 0;
  const bool neg = sum < 0;
  const U128 mag = neg ? static_cast<U128>(-sum) : static_cast<U128>(sum);
  uint32_t r = round_exact(neg, mag, base - 2, dst);
  if (saturate && (r & ~sign_bit) == inf) r = (r & sign_bit) | (inf - 1);
  return r;
}

}  // namespace fp8
}  // namespace emulation
