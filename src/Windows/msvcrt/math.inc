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
/* msvcrt's elementary functions, double and float: exp, exp2, expm1, log, log10, log2, log1p, pow, sin, cos, tan, asin, acos, atan, atan2, sinh, cosh, tanh, and
 * expf ... tanhf, fmodf, modff, truncf, roundf (included by msvcrt.c).
 *
 * The values come from the x87 in its 64-bit precision: a thread runs with the x87 at 53 bits (what Windows starts threads with), so each function widens the
 * precision control for its computation and puts it back (x87_wide / x87_back) - computed at 53 bits, exp(1) was off by several units in the last place and a float
 * function's double was not good enough to round. The extended result is rounded once to the double or float: within one unit in the last place of the exact value
 * (nearly always the correctly rounded one), for floats practically always correctly rounded.
 *
 * Special values as msvcrt's: NaN in, NaN out; a domain error (log of a negative, asin(2), sin(inf), pow(-2, 0.5)) gives the indefinite NaN (-nan(ind)) and errno
 * EDOM; a pole (log(0), pow(0, -1)) and an overflow from finite arguments give an infinity and errno ERANGE; an underflow gives 0 (or a denormal) without errno, as
 * msvcrt's. The cores are noinline and their x87 instructions volatile, so the compiler keeps them between the changes of the control word. */

static void math_errno(int e) { g_errno = e; }
static double math_nan(void) { union { unsigned long long u; double d; } c = {0xFFF8000000000000ull}; return c.d; }      /* (the indefinite NaN) */
static int math_isinf(double x) { return x > 1.7976931348623157e308 || x < -1.7976931348623157e308; }

static unsigned short x87_wide(void) {
  unsigned short old, cw;
  __asm__ volatile("fnstcw %0" : "=m"(old));
  cw = (unsigned short)(old | 0x300);      /* (precision control 11: 64 bits) */
  __asm__ volatile("fldcw %0" : : "m"(cw));
  return old;
}
static void x87_back(unsigned short cw) { __asm__ volatile("fldcw %0" : : "m"(cw)); }

#define MATH_CORE __attribute__((noinline)) static long double
static const long double kLog2e = 1.44269504088896340735992468100189214L;
static const long double kLn2 = 0.693147180559945309417232121458176568L;

static long double ld_f2xm1(long double f) { __asm__ volatile("f2xm1" : "+t"(f)); return f; }      /* (2^f - 1, |f| <= 1) */
static long double ld_fscale(long double v, long double n) { __asm__ volatile("fscale" : "+t"(v) : "u"(n)); return v; }
static long double ld_fyl2x(long double x, long double y) { __asm__ volatile("fyl2x" : "+t"(x) : "u"(y) : "st(1)"); return x; }      /* (y * log2(x)) */
static long double ld_fyl2xp1(long double x, long double y) { __asm__ volatile("fyl2xp1" : "+t"(x) : "u"(y) : "st(1)"); return x; }      /* (y * log2(1 + x)) */
static long double ld_fsqrt(long double x) { __asm__ volatile("fsqrt" : "+t"(x)); return x; }
static long double ld_fpatan(long double y, long double x) { __asm__ volatile("fpatan" : "+t"(x) : "u"(y) : "st(1)"); return x; }      /* (atan(y / x), quadrant) */

/* 2^t (t not NaN). */
MATH_CORE ld_exp2(long double t) {
  if (t > 16400.0L) return 1e4000L * 1e4000L;      /* (past the extended range: an infinity) */
  if (t < -16500.0L) return 0.0L;
  const long double n = ld_rint(t);
  return ld_fscale(ld_f2xm1(t - n) + 1.0L, n);
}
MATH_CORE ld_exp(long double x) {
  if (x > 12000.0L) return 1e4000L * 1e4000L;
  if (x < -12000.0L) return 0.0L;
  /* (x log2(e) as n + f, f from products that are exact: x and log2(e) split in a 24-bit high part and the rest, so that the fraction keeps its digits when
   * n is large - exp(700) right to the last place) */
  const long double t = x * kLog2e, n = ld_rint(t);
  const long double l_hi = 1.44269502162933349609375L, l_lo = kLog2e - l_hi;
  const long double hi = (long double)(float)x, lo = x - hi;
  const long double f = (hi * l_hi - n) + (hi * l_lo + lo * kLog2e);
  return ld_fscale(ld_f2xm1(f) + 1.0L, n);
}
MATH_CORE ld_expm1(long double x) {
  if (x > -0.5L && x < 0.5L) return ld_f2xm1(x * kLog2e);      /* (|x log2(e)| < 1: f2xm1 keeps the digits near 0) */
  return ld_exp(x) - 1.0L;
}
MATH_CORE ld_log2(long double x) { return ld_fyl2x(x, 1.0L); }
MATH_CORE ld_ln(long double x) { return ld_fyl2x(x, kLn2); }
MATH_CORE ld_log1p(long double x) {
  if (x > -0.29L && x < 0.29L) return ld_fyl2xp1(x, kLn2);      /* (fyl2xp1's range: |x| < 1 - sqrt(2) / 2) */
  return ld_fyl2x(1.0L + x, kLn2);
}
/* sin, cos, tan of a finite x. fsin and its kin reduce with the x87's pi of 66 bits, which is off by enough to lose every digit of sin(1e22) (and some of sin(1e5)), so
 * the argument is reduced here exactly - Payne and Hanek's way: x times the bits of 2/pi that matter, as integers - to x = n pi/2 + r, |r| <= pi/4, r to 64 bits;
 * fsin / fcos / fptan of r are then accurate in extended precision. */
static const unsigned kTwoOverPiWords[48] = {      /* (2/pi = sum of w[k] 2^(-32 (k + 1))) */
  0xA2F9836E, 0x4E441529, 0xFC2757D1, 0xF534DDC0, 0xDB629599, 0x3C439041, 0xFE5163AB, 0xDEBBC561, 0xB7246E3A, 0x424DD2E0, 0x06492EEA, 0x09D1921C,
  0xFE1DEB1C, 0xB129A73E, 0xE88235F5, 0x2EBB4484, 0xE99C7026, 0xB45F7E41, 0x3991D639, 0x835339F4, 0x9C845F8B, 0xBDF9283B, 0x1FF897FF, 0xDE05980F,
  0xEF2F118B, 0x5A0A6D1F, 0x6D367ECF, 0x27CB09B7, 0x4F463F66, 0x9E5FEA2D, 0x7527BAC7, 0xEBE5F17B, 0x3D0739F7, 0x8A5292EA, 0x6BFB5FB1, 0x1F8D5D08,
  0x56033046, 0xFC7B6BAB, 0xF0CFBC20, 0x9AF4361D, 0xA9E39161, 0x5EE61B08, 0x6599855F, 0x14A06840, 0x8DFFD880, 0x4D732731, 0x06061556, 0xCA73A8C9};
#define TRIG_WINDOW 6      /* (words of 2/pi a product takes: 192 bits, past the 64 + 61 the closest double to a multiple of pi/2 needs) */
static long double ld_pow2(int e) {      /* (2^e, |e| < 16000) */
  long double r = 1.0L, b = e < 0 ? 0.5L : 2.0L;
  unsigned n = (unsigned)(e < 0 ? -e : e);
  while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
  return r;
}
/* |x| (a finite double, > pi/4) = n pi/2 + r: *quadrant = n mod 4, the return r. */
static long double trig_reduce(double x, unsigned *quadrant) {
  union { double d; unsigned long long u; } c = {x};
  const int biased = (int)((c.u >> 52) & 0x7FF);
  const unsigned long long m = (c.u & 0xFFFFFFFFFFFFFull) | (biased ? 1ull << 52 : 0);
  const int e = (biased ? biased : 1) - 1075;      /* (|x| = m 2^e) */
  /* (words before k0 give multiples of 8 - nothing to n mod 4 or the fraction -; the product's lowest bit is worth 2^(e - 32 (k0 + TRIG_WINDOW))) */
  const int k0 = e >= 3 ? (e - 3) / 32 : 0;
  unsigned p[2 + TRIG_WINDOW] = {0};      /* (little-endian 32-bit limbs of m * window) */
  const unsigned mw[2] = {(unsigned)m, (unsigned)(m >> 32)};
  for (int i = 0; i < 2; ++i) {
    unsigned long long carry = 0;
    for (int k = 0; k < TRIG_WINDOW; ++k) {
      const unsigned w = kTwoOverPiWords[k0 + TRIG_WINDOW - 1 - k];      /* (limb k of the window, from its low end) */
      const unsigned long long t = (unsigned long long)mw[i] * w + p[i + k] + carry;
      p[i + k] = (unsigned)t;
      carry = t >> 32;
    }
    for (int k = i + TRIG_WINDOW; carry && k < 2 + TRIG_WINDOW; ++k) { const unsigned long long t = (unsigned long long)p[k] + carry; p[k] = (unsigned)t; carry = t >> 32; }
  }
  const int frac_bits = 32 * (k0 + TRIG_WINDOW) - e;      /* (bits below the binary point: 157 ... 245) */
  /* (n mod 4: the two bits over the point; the fraction: the bits below it) */
  unsigned n = 0;
  for (int b = 0; b < 2; ++b) { const int bit = frac_bits + b; if (bit < 32 * (2 + TRIG_WINDOW) && ((p[bit / 32] >> (bit % 32)) & 1)) n |= 1u << b; }
  for (int bit = frac_bits; bit < 32 * (2 + TRIG_WINDOW); ++bit) p[bit / 32] &= ~(1u << (bit % 32));
  int negative = 0;
  if ((p[(frac_bits - 1) / 32] >> ((frac_bits - 1) % 32)) & 1) {      /* (fraction >= 1/2: n + 1 and the fraction minus 1, so |r| <= pi/4) */
    ++n;
    negative = 1;
    unsigned long long borrow = 1;      /* (2^frac_bits - fraction, as the two's complement within frac_bits bits) */
    for (int k = 0; k < 2 + TRIG_WINDOW; ++k) { const unsigned long long t = (unsigned long long)(unsigned)~p[k] + borrow; p[k] = (unsigned)t; borrow = t >> 32; }
    for (int bit = frac_bits; bit < 32 * (2 + TRIG_WINDOW); ++bit) p[bit / 32] &= ~(1u << (bit % 32));
  }
  *quadrant = n & 3;
  /* (the fraction's top 64 bits from its first set bit, as an extended value) */
  int top = frac_bits - 1;
  while (top >= 0 && !((p[top / 32] >> (top % 32)) & 1)) --top;
  if (top < 0) return 0.0L;
  unsigned long long mant = 0;
  for (int i = 0; i < 64; ++i) {
    const int bit = top - i;
    mant = (mant << 1) | (bit >= 0 ? (p[bit / 32] >> (bit % 32)) & 1 : 0);
  }
  const long double fraction = (long double)mant * ld_pow2(top - 63 - frac_bits);
  const long double r = fraction * 1.57079632679489661923132169163975144L;
  return negative ? -r : r;
}
enum { TRIG_SIN, TRIG_COS, TRIG_TAN };
MATH_CORE ld_trig(double x, int which) {
  const int odd = x < 0;
  const double a = odd ? -x : x;
  unsigned q = 0;
  long double r = a;
  if (a > 0.785398163397448279) r = trig_reduce(a, &q);      /* (pi/4 rounded down: what is below needs no reduction) */
  long double sn = r, cs = r, value;
  __asm__ volatile("fsin" : "+t"(sn));
  __asm__ volatile("fcos" : "+t"(cs));
  if (which == TRIG_SIN) {
    value = q == 0 ? sn : q == 1 ? cs : q == 2 ? -sn : -cs;
    return odd ? -value : value;
  }
  if (which == TRIG_COS) return q == 0 ? cs : q == 1 ? -sn : q == 2 ? -cs : sn;
  long double t = r;
  __asm__ volatile("fptan\n fstp %%st(0)" : "+t"(t));      /* (fptan pushes a 1 over the tangent) */
  value = q & 1 ? -1.0L / t : t;
  return odd ? -value : value;
}
MATH_CORE ld_sin(double x) { return ld_trig(x, TRIG_SIN); }
MATH_CORE ld_cos(double x) { return ld_trig(x, TRIG_COS); }
MATH_CORE ld_tan(double x) { return ld_trig(x, TRIG_TAN); }
MATH_CORE ld_atan2(long double y, long double x) { return ld_fpatan(y, x); }
MATH_CORE ld_asin(long double x) { return ld_fpatan(x, ld_fsqrt((1.0L - x) * (1.0L + x))); }
MATH_CORE ld_acos(long double x) { return ld_fpatan(ld_fsqrt((1.0L - x) * (1.0L + x)), x); }
MATH_CORE ld_sinh(long double x) {
  const long double a = x < 0 ? -x : x;
  long double r;
  if (a < 1.0L) { const long double e = ld_expm1(a); r = (e + e / (e + 1.0L)) / 2; }      /* (no cancellation near 0) */
  else { const long double e = ld_exp(a); r = (e - 1.0L / e) / 2; }
  return x < 0 ? -r : r;
}
MATH_CORE ld_cosh(long double x) { const long double e = ld_exp(x < 0 ? -x : x); return (e + 1.0L / e) / 2; }
MATH_CORE ld_tanh(long double x) {
  const long double a = x < 0 ? -x : x;
  long double r;
  if (a > 23.0L) r = 1.0L;      /* (tanh is 1 in extended precision past that) */
  else { const long double e = ld_expm1(2 * a); r = e / (e + 2.0L); }
  return x < 0 ? -r : r;
}
/* y log2|x| for pow: finite x != 0, finite y. */
MATH_CORE ld_pow_abs(long double x, long double y) {
  if (x < 0) x = -x;
  /* (an integer power up to 2^16 by squaring: exact where the result is, as Windows' pow(10, 2) == 100) */
  if (y == ld_rint(y) && y >= -65536.0L && y <= 65536.0L) {
    long double r = 1.0L, b = x;
    long long n = (long long)(y < 0 ? -y : y);
    while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
    return y < 0 ? 1.0L / r : r;
  }
  return ld_exp2(ld_fyl2x(x, y));
}

/* ---- the double functions ---- */

#define MATH_EVAL(expr) ({ const unsigned short cw_ = x87_wide(); const long double r_ = (expr); x87_back(cw_); r_; })
/* The double of an extended result of a finite argument: ERANGE when it overflowed. */
static double math_finish(long double r) {
  const double d = (double)r;
  if (math_isinf(d)) math_errno(ERANGE);
  return d;
}

API double exp(double x) {
  if (x != x) return x;
  if (math_isinf(x)) return x > 0 ? x : 0.0;
  return math_finish(MATH_EVAL(ld_exp(x)));
}
API double exp2(double x) {
  if (x != x) return x;
  if (math_isinf(x)) return x > 0 ? x : 0.0;
  return math_finish(MATH_EVAL(ld_exp2(x)));
}
API double expm1(double x) {
  if (x != x || x == 0) return x;      /* (expm1(-0) is -0) */
  if (math_isinf(x)) return x > 0 ? x : -1.0;
  return math_finish(MATH_EVAL(ld_expm1(x)));
}
/* The logarithms: NaN in, NaN out; below 0 EDOM; 0 a pole (-inf, ERANGE); +inf stays. */
static int math_log_special(double x, double *out) {
  if (x != x) { *out = x; return 1; }
  if (x < 0) { math_errno(EDOM); *out = math_nan(); return 1; }
  if (x == 0) { math_errno(ERANGE); *out = -1.0 / 0.0; return 1; }
  if (math_isinf(x)) { *out = x; return 1; }
  return 0;
}
API double log(double x) { double r; return math_log_special(x, &r) ? r : (double)MATH_EVAL(ld_ln(x)); }
API double log2(double x) { double r; return math_log_special(x, &r) ? r : (double)MATH_EVAL(ld_log2(x)); }
API double log10(double x) {
  double r;
  if (math_log_special(x, &r)) return r;
  /* (log10 of a power of ten is that integer exactly: log2(x) / log2(10) in extended is, after rounding) */
  return (double)MATH_EVAL(ld_fyl2x(x, 0.301029995663981195213738894724493027L));
}
API double log1p(double x) {
  if (x != x || x == 0) return x;
  if (x < -1) { math_errno(EDOM); return math_nan(); }
  if (x == -1) { math_errno(ERANGE); return -1.0 / 0.0; }
  if (math_isinf(x)) return x;
  return (double)MATH_EVAL(ld_log1p(x));
}
static int math_is_odd_integer(double y) {
  if (y != floor(y) || fabs(y) >= 9007199254740992.0) return 0;      /* (from 2^53 on every double is even) */
  const double h = y / 2;
  return h != floor(h);
}
API double pow(double x, double y) {
  if (y == 0 || x == 1) return 1.0;      /* (even of a NaN, as C99 and Windows) */
  if (x != x || y != y) return x != x ? x : y;
  const int odd = math_is_odd_integer(y), integer = y == floor(y);
  if (math_isinf(y)) {
    const double a = fabs(x);
    if (a == 1) return 1.0;
    return (a < 1) == (y < 0) ? 1.0 / 0.0 : 0.0;
  }
  if (math_isinf(x)) {
    if (x > 0) return y > 0 ? x : 0.0;
    if (y > 0) return odd ? x : -x;
    return odd ? -0.0 : 0.0;
  }
  if (x == 0) {
    if (y > 0) return odd ? x : 0.0;
    math_errno(ERANGE);      /* (a pole: pow(0, -1)) */
    return odd && 1 / x < 0 ? -1.0 / 0.0 : 1.0 / 0.0;
  }
  if (x < 0 && !integer) { math_errno(EDOM); return math_nan(); }
  const double r = math_finish(MATH_EVAL(ld_pow_abs(x, y)));
  return x < 0 && odd ? -r : r;
}
/* sin, cos, tan: NaN stays; an infinity is a domain error. */
#define MATH_TRIG(name, core)                                         \
  API double name(double x) {                                         \
    if (x != x) return x;                                             \
    if (math_isinf(x)) { math_errno(EDOM); return math_nan(); }       \
    if (x == 0 && #name[0] != 'c') return x;      /* (sin(-0) = -0) */ \
    return (double)MATH_EVAL(core(x));                                \
  }
MATH_TRIG(sin, ld_sin)
MATH_TRIG(cos, ld_cos)
MATH_TRIG(tan, ld_tan)
API double atan(double x) {
  if (x != x || x == 0) return x;
  return (double)MATH_EVAL(ld_atan2(x, 1.0L));      /* (fpatan takes atan(+-inf) to +-pi/2) */
}
API double atan2(double y, double x) {
  if (x != x || y != y) return x != x ? x : y;
  return (double)MATH_EVAL(ld_atan2(y, x));      /* (fpatan has the signed zeros and infinities of C99's atan2) */
}
API double asin(double x) {
  if (x != x || x == 0) return x;
  if (x > 1 || x < -1) { math_errno(EDOM); return math_nan(); }
  return (double)MATH_EVAL(ld_asin(x));
}
API double acos(double x) {
  if (x != x) return x;
  if (x > 1 || x < -1) { math_errno(EDOM); return math_nan(); }
  return (double)MATH_EVAL(ld_acos(x));
}
API double sinh(double x) {
  if (x != x || x == 0 || math_isinf(x)) return x;
  return math_finish(MATH_EVAL(ld_sinh(x)));
}
API double cosh(double x) {
  if (x != x) return x;
  if (math_isinf(x)) return fabs(x);
  return math_finish(MATH_EVAL(ld_cosh(x)));
}
API double tanh(double x) {
  if (x != x || x == 0) return x;
  if (math_isinf(x)) return x > 0 ? 1.0 : -1.0;
  return (double)MATH_EVAL(ld_tanh(x));
}

/* ---- the float functions: the double's special cases, the extended value rounded once to a float ---- */

static int math_isinff(float x) { return x > 3.40282347e38f || x < -3.40282347e38f; }
static float math_finishf(long double r) {
  const float f = (float)r;
  if (math_isinff(f)) math_errno(ERANGE);
  return f;
}
API float expf(float x) {
  if (x != x) return x;
  if (math_isinff(x)) return x > 0 ? x : 0.0f;
  return math_finishf(MATH_EVAL(ld_exp(x)));
}
API float exp2f(float x) {
  if (x != x) return x;
  if (math_isinff(x)) return x > 0 ? x : 0.0f;
  return math_finishf(MATH_EVAL(ld_exp2(x)));
}
API float expm1f(float x) {
  if (x != x || x == 0) return x;
  if (math_isinff(x)) return x > 0 ? x : -1.0f;
  return math_finishf(MATH_EVAL(ld_expm1(x)));
}
API float logf(float x) { double r; return math_log_special(x, &r) ? (float)r : (float)MATH_EVAL(ld_ln(x)); }
API float log2f(float x) { double r; return math_log_special(x, &r) ? (float)r : (float)MATH_EVAL(ld_log2(x)); }
API float log10f(float x) { double r; return math_log_special(x, &r) ? (float)r : (float)MATH_EVAL(ld_fyl2x(x, 0.301029995663981195213738894724493027L)); }
API float log1pf(float x) { return x != x || x == 0 ? x : x < -1 || x == -1 || math_isinff(x) ? (float)log1p(x) : (float)MATH_EVAL(ld_log1p(x)); }
API float powf(float x, float y) {
  /* (the special cases are the double's; a float's power is a double's power of floats rounded - except where that double overflowed or underflowed) */
  if (y == 0 || x == 1) return 1.0f;
  if (x != x || y != y || math_isinff(x) || math_isinff(y) || x == 0 || (x < 0 && y != floor(y))) return (float)pow(x, y);
  const long double r = MATH_EVAL(ld_pow_abs(x, y));
  const float f = math_finishf(r);
  return x < 0 && math_is_odd_integer(y) ? -f : f;
}
#define MATH_TRIGF(name, core)                                        \
  API float name(float x) {                                           \
    if (x != x) return x;                                             \
    if (math_isinff(x)) { math_errno(EDOM); return (float)math_nan(); } \
    if (x == 0 && #name[0] != 'c') return x;                          \
    return (float)MATH_EVAL(core(x));                                 \
  }
MATH_TRIGF(sinf, ld_sin)
MATH_TRIGF(cosf, ld_cos)
MATH_TRIGF(tanf, ld_tan)
API float atanf(float x) { return x != x || x == 0 ? x : (float)MATH_EVAL(ld_atan2(x, 1.0L)); }
API float atan2f(float y, float x) { return x != x || y != y ? (x != x ? x : y) : (float)MATH_EVAL(ld_atan2(y, x)); }
API float asinf(float x) {
  if (x != x || x == 0) return x;
  if (x > 1 || x < -1) { math_errno(EDOM); return (float)math_nan(); }
  return (float)MATH_EVAL(ld_asin(x));
}
API float acosf(float x) {
  if (x != x) return x;
  if (x > 1 || x < -1) { math_errno(EDOM); return (float)math_nan(); }
  return (float)MATH_EVAL(ld_acos(x));
}
API float sinhf(float x) { return x != x || x == 0 || math_isinff(x) ? x : math_finishf(MATH_EVAL(ld_sinh(x))); }
API float coshf(float x) { return x != x ? x : math_isinff(x) ? (x < 0 ? -x : x) : math_finishf(MATH_EVAL(ld_cosh(x))); }
API float tanhf(float x) { return x != x || x == 0 ? x : math_isinff(x) ? (x > 0 ? 1.0f : -1.0f) : (float)MATH_EVAL(ld_tanh(x)); }
API float fmodf(float a, float b) { return (float)fmod(a, b); }      /* (exact in double: the remainder of floats is a float) */
API float truncf(float x) { return (float)trunc(x); }
API float roundf(float x) { return (float)round(x); }
API float modff(float x, float *ip) {
  if (math_isinff(x)) { *ip = x; return x < 0 ? -0.0f : 0.0f; }
  const float t = (float)trunc(x);
  *ip = t;
  return x != x ? x : x - t;
}
