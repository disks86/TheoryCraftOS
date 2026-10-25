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

// The numeric operations of WebAssembly that C++ does not have as they are (the image has no libm): rounding, minimum and maximum with NaN and signed zero rules,
// square root, and the float to integer conversions with their ranges. Shared by the interpreter and the SIMD operations.
#pragma once
#include <stdint.h>

namespace wasm::num {

inline uint32_t bits(float f) { return __builtin_bit_cast(uint32_t, f); }
inline uint64_t bits(double d) { return __builtin_bit_cast(uint64_t, d); }
inline float f32_from(uint32_t b) { return __builtin_bit_cast(float, b); }
inline double f64_from(uint64_t b) { return __builtin_bit_cast(double, b); }

inline bool is_nan(float x) { return x != x; }
inline bool is_nan(double x) { return x != x; }
inline bool negative(float x) { return (bits(x) >> 31) != 0; }
inline bool negative(double x) { return (bits(x) >> 63) != 0; }

inline float with_sign(float magnitude, bool neg) { return f32_from((bits(magnitude) & 0x7FFFFFFFu) | (neg ? 0x80000000u : 0u)); }
inline double with_sign(double magnitude, bool neg) { return f64_from((bits(magnitude) & 0x7FFFFFFFFFFFFFFFull) | (neg ? 0x8000000000000000ull : 0ull)); }

inline float f_abs(float x) { return f32_from(bits(x) & 0x7FFFFFFFu); }
inline double f_abs(double x) { return f64_from(bits(x) & 0x7FFFFFFFFFFFFFFFull); }
inline float f_neg(float x) { return f32_from(bits(x) ^ 0x80000000u); }
inline double f_neg(double x) { return f64_from(bits(x) ^ 0x8000000000000000ull); }

inline float f_trunc(float x) {
  if (is_nan(x) || f_abs(x) >= 8388608.0f) return x;   // (2^23: every float from here on is a whole number)
  const float r = static_cast<float>(static_cast<int32_t>(x));
  return r == 0 ? with_sign(0.0f, negative(x)) : r;
}
inline double f_trunc(double x) {
  if (is_nan(x) || f_abs(x) >= 4503599627370496.0) return x;
  const double r = static_cast<double>(static_cast<int64_t>(x));
  return r == 0 ? with_sign(0.0, negative(x)) : r;
}
template <class T>
inline T f_floor(T x) {
  const T t = f_trunc(x);
  if (is_nan(x)) return x;
  return t > x ? t - 1 : t;
}
template <class T>
inline T f_ceil(T x) {
  const T t = f_trunc(x);
  if (is_nan(x)) return x;
  const T r = t < x ? t + 1 : t;
  return r == 0 ? with_sign(static_cast<T>(0), negative(x)) : r;
}
inline float f_nearest(float x) {
  if (is_nan(x) || f_abs(x) >= 8388608.0f) return x;
  const float magic = 8388608.0f;
  volatile float t = negative(x) ? (x - magic) + magic : (x + magic) - magic;   // (the addition rounds to even)
  const float r = t;
  return r == 0 ? with_sign(0.0f, negative(x)) : r;
}
inline double f_nearest(double x) {
  if (is_nan(x) || f_abs(x) >= 4503599627370496.0) return x;
  const double magic = 4503599627370496.0;
  volatile double t = negative(x) ? (x - magic) + magic : (x + magic) - magic;
  const double r = t;
  return r == 0 ? with_sign(0.0, negative(x)) : r;
}
// (the instructions themselves: the compiler would call sqrt() to set errno for a negative number, and the image has no libm)
inline float f_sqrt(float x) {
  float r;
  asm("sqrtss %1, %0" : "=x"(r) : "x"(x));
  return r;
}
inline double f_sqrt(double x) {
  double r;
  asm("sqrtsd %1, %0" : "=x"(r) : "x"(x));
  return r;
}

template <class T>
inline T f_min(T a, T b) {
  if (is_nan(a) || is_nan(b)) return a + b;   // (a NaN)
  if (a == b) return negative(a) ? a : b;     // (-0 is smaller than +0)
  return a < b ? a : b;
}
template <class T>
inline T f_max(T a, T b) {
  if (is_nan(a) || is_nan(b)) return a + b;
  if (a == b) return negative(a) ? b : a;
  return a > b ? a : b;
}
template <class T>
inline T f_copysign(T a, T b) {
  return with_sign(a, negative(b));
}

// Float to integer, trapping (the *_ok functions say whether the value is in range; NaN is not) and saturating.
inline bool trunc_ok_i32(float x) { return x > -2147483904.0f && x < 2147483648.0f; }
inline bool trunc_ok_i32(double x) { return x > -2147483649.0 && x < 2147483648.0; }
inline bool trunc_ok_u32(float x) { return x > -1.0f && x < 4294967296.0f; }
inline bool trunc_ok_u32(double x) { return x > -1.0 && x < 4294967296.0; }
inline bool trunc_ok_i64(float x) { return x >= -9223372036854775808.0f && x < 9223372036854775808.0f; }
inline bool trunc_ok_i64(double x) { return x >= -9223372036854775808.0 && x < 9223372036854775808.0; }
inline bool trunc_ok_u64(float x) { return x > -1.0f && x < 18446744073709551616.0f; }
inline bool trunc_ok_u64(double x) { return x > -1.0 && x < 18446744073709551616.0; }

template <class T>
inline int32_t sat_i32(T x) {
  if (is_nan(x)) return 0;
  if (x <= -2147483648.0) return INT32_MIN;
  if (x >= 2147483648.0) return INT32_MAX;
  return static_cast<int32_t>(x);
}
template <class T>
inline uint32_t sat_u32(T x) {
  if (is_nan(x) || x <= 0) return 0;
  if (x >= 4294967296.0) return UINT32_MAX;
  return static_cast<uint32_t>(x);
}
template <class T>
inline int64_t sat_i64(T x) {
  if (is_nan(x)) return 0;
  if (x <= -9223372036854775808.0) return INT64_MIN;
  if (x >= 9223372036854775808.0) return INT64_MAX;
  return static_cast<int64_t>(x);
}
template <class T>
inline uint64_t sat_u64(T x) {
  if (is_nan(x) || x <= 0) return 0;
  if (x >= 18446744073709551616.0) return UINT64_MAX;
  return static_cast<uint64_t>(x);
}

inline uint32_t rotl32(uint32_t x, uint32_t n) {
  n &= 31;
  return n ? (x << n) | (x >> (32 - n)) : x;
}
inline uint32_t rotr32(uint32_t x, uint32_t n) {
  n &= 31;
  return n ? (x >> n) | (x << (32 - n)) : x;
}
inline uint64_t rotl64(uint64_t x, uint32_t n) {
  n &= 63;
  return n ? (x << n) | (x >> (64 - n)) : x;
}
inline uint64_t rotr64(uint64_t x, uint32_t n) {
  n &= 63;
  return n ? (x >> n) | (x << (64 - n)) : x;
}
inline uint32_t clz32(uint32_t x) { return x ? static_cast<uint32_t>(__builtin_clz(x)) : 32; }
inline uint32_t ctz32(uint32_t x) { return x ? static_cast<uint32_t>(__builtin_ctz(x)) : 32; }
inline uint32_t clz64(uint64_t x) { return x ? static_cast<uint32_t>(__builtin_clzll(x)) : 64; }
inline uint32_t ctz64(uint64_t x) { return x ? static_cast<uint32_t>(__builtin_ctzll(x)) : 64; }
// (counted by hand: the compiler's helper for a CPU without the instruction is in libgcc, which the image does not link)
inline uint32_t popcnt64(uint64_t x) {
  x = x - ((x >> 1) & 0x5555555555555555ull);
  x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
  x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
  return static_cast<uint32_t>((x * 0x0101010101010101ull) >> 56);
}
inline uint32_t popcnt32(uint32_t x) { return popcnt64(x); }

// Little-endian access to bytes in memory.
inline uint64_t load_le(const uint8_t* p, uint32_t n) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}
inline void store_le(uint8_t* p, uint32_t n, uint64_t v) {
  for (uint32_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace wasm::num
