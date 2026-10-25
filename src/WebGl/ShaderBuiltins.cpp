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

// The built-in functions of GLSL and WGSL, and the float math they need (the image has no libm).
#include "Math/FloatMath.hpp"
#include "WebGl/ShaderInternal.hpp"

namespace shader {

// --- float math -------------------------------------------------------------------------------------------------------------------------------------------

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kNan = __builtin_nanf("");
constexpr float kInf = __builtin_inff();

double atan_d(double x) {
  const bool neg = x < 0;
  if (neg) x = -x;
  bool inv = false;
  if (x > 1) {
    x = 1 / x;
    inv = true;
  }
  double add = 0;
  if (x > 0.41421356237) {
    x = (x - 1) / (x + 1);
    add = kPi / 4;
  }
  const double x2 = x * x;
  double term = x, sum = 0;
  for (int k = 1; k < 40; k += 2) {
    sum += term / k;
    term *= -x2;
  }
  double r = sum + add;
  if (inv) r = kPi / 2 - r;
  return neg ? -r : r;
}

inline bool is_nan(float x) { return x != x; }
}  // namespace

float fsin(float x) { return is_nan(x) || x > 1e30f || x < -1e30f ? kNan : static_cast<float>(fmath::sin(x)); }
float fcos(float x) { return is_nan(x) || x > 1e30f || x < -1e30f ? kNan : static_cast<float>(fmath::cos(x)); }
float fsqrt(float x) {
  if (x < 0) return kNan;
  float r;
  asm("sqrtss %1, %0" : "=x"(r) : "x"(x));
  return r;
}
float flog(float x) {
  if (is_nan(x) || x < 0) return kNan;
  if (x == 0) return -kInf;
  if (x == kInf) return kInf;
  return static_cast<float>(fmath::ln(x));
}
float fexp(float x) {
  if (is_nan(x)) return kNan;
  if (x > 88.7f) return kInf;
  if (x < -104.0f) return 0;
  return static_cast<float>(fmath::exp(x));
}
float ffloor(float x) {
  if (!(x > -8388608.0f && x < 8388608.0f)) return x;
  const float t = static_cast<float>(static_cast<int32_t>(x));
  return t > x ? t - 1 : t;
}
float fround_even(float x) {
  if (!(x > -8388608.0f && x < 8388608.0f)) return x;
  const float f = ffloor(x);
  const float d = x - f;
  if (d < 0.5f) return f;
  if (d > 0.5f) return f + 1;
  return (static_cast<int32_t>(f) & 1) ? f + 1 : f;
}
float fatan2(float y, float x) {
  if (is_nan(x) || is_nan(y)) return kNan;
  if (x == 0) return y > 0 ? static_cast<float>(kPi / 2) : (y < 0 ? static_cast<float>(-kPi / 2) : 0);
  const double a = atan_d(static_cast<double>(y) / static_cast<double>(x));
  if (x > 0) return static_cast<float>(a);
  return static_cast<float>(y >= 0 ? a + kPi : a - kPi);
}
float fpow(float x, float y) {
  if (y == 0) return 1;
  if (is_nan(x) || is_nan(y)) return kNan;
  if (x == 0) return y > 0 ? 0.0f : kInf;
  if (x < 0) {
    const float fy = ffloor(y);
    if (fy != y) return kNan;
    const float r = static_cast<float>(fmath::pow(-x, y));
    return (static_cast<int64_t>(y) & 1) ? -r : r;
  }
  if (x == kInf) return y > 0 ? kInf : 0.0f;
  const double e = static_cast<double>(y) * fmath::ln(x);
  if (e > 88.7) return kInf;
  if (e < -104) return 0;
  return static_cast<float>(fmath::exp(e));
}

uint16_t float_to_half(float f) {
  const uint32_t b = f2u(f);
  const uint32_t sign = (b >> 16) & 0x8000;
  int32_t e = static_cast<int32_t>((b >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = b & 0x7FFFFF;
  if (((b >> 23) & 0xFF) == 0xFF) return static_cast<uint16_t>(sign | 0x7C00 | (mant ? 0x200 : 0));
  if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00);
  if (e <= 0) {
    if (e < -10) return static_cast<uint16_t>(sign);
    mant |= 0x800000;
    const uint32_t shift = static_cast<uint32_t>(14 - e);
    uint32_t h = mant >> shift;
    if ((mant >> (shift - 1)) & 1) ++h;
    return static_cast<uint16_t>(sign | h);
  }
  uint32_t h = sign | (static_cast<uint32_t>(e) << 10) | (mant >> 13);
  if (mant & 0x1000) ++h;
  return static_cast<uint16_t>(h);
}

float half_to_float(uint16_t h) {
  const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000) << 16;
  uint32_t e = (h >> 10) & 0x1F, mant = h & 0x3FF;
  if (e == 0) {
    if (mant == 0) return u2f(sign);
    while (!(mant & 0x400)) {
      mant <<= 1;
      --e;
    }
    ++e;
    mant &= 0x3FF;
    return u2f(sign | ((e + 112) << 23) | (mant << 13));
  }
  if (e == 31) return u2f(sign | 0x7F800000 | (mant << 13));
  return u2f(sign | ((e + 112) << 23) | (mant << 13));
}

// The float values of up to 16 slots (slots are plain 32-bit words; this reads them as floats without aliasing a uint32_t as a float).
struct FloatCopy {
  float v[16];
  FloatCopy(const uint32_t* p, uint32_t n) {
    for (uint32_t i = 0; i < 16; ++i) v[i] = i < n ? u2f(p[i]) : 0.0f;
  }
};

// --- helpers ----------------------------------------------------------------------------------------------------------------------------------------------

namespace {

struct Ctx {
  Interp& in;
  Ref fa[12];
  bool ok = true;
};

// The float view of an argument.
bool as_float(Interp& in, const Ref& a, Ref* out) { return in.convert_to(a, Base::Float, out); }

template <class F>
bool map1(Interp& in, const Ref& a, Ref* out, F f) {
  Ref fa;
  if (!as_float(in, a, &fa)) return false;
  Type t = fa.t;
  uint32_t* w;
  if (!in.new_value(t, out, &w)) return false;
  const uint32_t n = count_of(t);
  for (uint32_t i = 0; i < n; ++i) w[i] = f2u(f(u2f(fa.p[i])));
  return true;
}

bool shape_of(const Ref& a, const Ref& b, Type* t) {
  const uint32_t na = count_of(a.t), nb = count_of(b.t);
  if (na == nb) *t = a.t;
  else if (na == 1) *t = b.t;
  else if (nb == 1) *t = a.t;
  else return false;
  return true;
}

template <class F>
bool map2(Interp& in, const Ref& a, const Ref& b, Ref* out, F f) {
  Ref fa, fb;
  Type t;
  if (!as_float(in, a, &fa) || !as_float(in, b, &fb)) return false;
  if (!shape_of(fa, fb, &t)) return in.fail("argument sizes do not match");
  t.base = Base::Float;
  uint32_t* w;
  if (!in.new_value(t, out, &w)) return false;
  const uint32_t n = count_of(t), na = count_of(fa.t), nb = count_of(fb.t);
  for (uint32_t i = 0; i < n; ++i) w[i] = f2u(f(u2f(fa.p[na == 1 ? 0 : i]), u2f(fb.p[nb == 1 ? 0 : i])));
  return true;
}

template <class F>
bool map3(Interp& in, const Ref& a, const Ref& b, const Ref& c, Ref* out, F f) {
  Ref fa, fb, fc;
  Type t, t2;
  if (!as_float(in, a, &fa) || !as_float(in, b, &fb) || !as_float(in, c, &fc)) return false;
  if (!shape_of(fa, fb, &t) || !shape_of(Ref{nullptr, t}, fc, &t2)) return in.fail("argument sizes do not match");
  t2.base = Base::Float;
  uint32_t* w;
  if (!in.new_value(t2, out, &w)) return false;
  const uint32_t n = count_of(t2), na = count_of(fa.t), nb = count_of(fb.t), nc = count_of(fc.t);
  for (uint32_t i = 0; i < n; ++i) w[i] = f2u(f(u2f(fa.p[na == 1 ? 0 : i]), u2f(fb.p[nb == 1 ? 0 : i]), u2f(fc.p[nc == 1 ? 0 : i])));
  return true;
}

bool scalar_result(Interp& in, Base b, uint32_t value, Ref* out) {
  uint32_t* w;
  if (!in.new_value(make_type(b), out, &w)) return false;
  w[0] = value;
  return true;
}

float dotf(const Ref& a, const Ref& b) {
  float s = 0;
  const uint32_t n = count_of(a.t);
  for (uint32_t i = 0; i < n; ++i) s += u2f(a.p[i]) * u2f(b.p[i]);
  return s;
}

float absf(float x) { return u2f(f2u(x) & 0x7FFFFFFFu); }

float determinant_n(const float* m, uint32_t n) {
  if (n == 2) return m[0] * m[3] - m[2] * m[1];
  if (n == 3)
    return m[0] * (m[4] * m[8] - m[7] * m[5]) - m[3] * (m[1] * m[8] - m[7] * m[2]) + m[6] * (m[1] * m[5] - m[4] * m[2]);
  // 4x4: expand along the first column
  float d = 0;
  for (uint32_t r = 0; r < 4; ++r) {
    float sub[9];
    uint32_t k = 0;
    for (uint32_t c = 1; c < 4; ++c)
      for (uint32_t rr = 0; rr < 4; ++rr)
        if (rr != r) sub[k++] = m[c * 4 + rr];
    const float cof = determinant_n(sub, 3) * ((r & 1) ? -1.0f : 1.0f);
    d += m[r] * cof;
  }
  return d;
}

bool invert_n(const float* m, uint32_t n, float* out) {
  const float d = determinant_n(m, n);
  if (d == 0) {
    for (uint32_t i = 0; i < n * n; ++i) out[i] = kNan;
    return true;
  }
  const float inv = 1.0f / d;
  if (n == 2) {
    out[0] = m[3] * inv;
    out[1] = -m[1] * inv;
    out[2] = -m[2] * inv;
    out[3] = m[0] * inv;
    return true;
  }
  // cofactor expansion (adjugate): out[c][r] = cofactor(r, c) / det
  for (uint32_t r = 0; r < n; ++r)
    for (uint32_t c = 0; c < n; ++c) {
      float sub[9];
      uint32_t k = 0;
      for (uint32_t cc = 0; cc < n; ++cc) {
        if (cc == c) continue;
        for (uint32_t rr = 0; rr < n; ++rr)
          if (rr != r) sub[k++] = m[cc * n + rr];
      }
      const float cof = determinant_n(sub, n - 1) * (((r + c) & 1) ? -1.0f : 1.0f);
      out[r * n + c] = cof * inv;   // transposed: adjugate
    }
  return true;
}

uint32_t popcount(uint32_t v) {
  v = v - ((v >> 1) & 0x55555555u);
  v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
  return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

uint32_t reverse_bits(uint32_t v) {
  v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
  v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
  v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
  v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
  return (v >> 16) | (v << 16);
}

uint32_t clz(uint32_t v) {
  if (!v) return 32;
  uint32_t n = 0;
  while (!(v & 0x80000000u)) {
    v <<= 1;
    ++n;
  }
  return n;
}

uint32_t ctz(uint32_t v) {
  if (!v) return 32;
  uint32_t n = 0;
  while (!(v & 1)) {
    v >>= 1;
    ++n;
  }
  return n;
}

float sat(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
uint32_t pack_unorm(float x, uint32_t bits) {
  const float mx = static_cast<float>((1u << bits) - 1);
  return static_cast<uint32_t>(fround_even(sat(x) * mx));
}
uint32_t pack_snorm(float x, uint32_t bits) {
  const float mx = static_cast<float>((1u << (bits - 1)) - 1);
  const float c = x < -1 ? -1 : (x > 1 ? 1 : x);
  return static_cast<uint32_t>(static_cast<int32_t>(fround_even(c * mx))) & ((1u << bits) - 1);
}
float unpack_snorm(uint32_t v, uint32_t bits) {
  const int32_t s = static_cast<int32_t>(v << (32 - bits)) >> (32 - bits);
  const float x = static_cast<float>(s) / static_cast<float>((1u << (bits - 1)) - 1);
  return x < -1 ? -1 : x;
}

}  // namespace

// --- the built-ins ----------------------------------------------------------------------------------------------------------------------------------------

bool Interp::builtin(BuiltinFn fn, const Ref* a, uint32_t n, Ref* out, const Node& node) {
  const bool wgsl = mod.language == Language::Wgsl;
  uint32_t* w;
  Ref t0;
  auto need = [&](uint32_t k) { return n >= k || fail("too few arguments to a built-in"); };
  switch (fn) {
    // --- trigonometry and exponentials
    case BuiltinFn::Radians: return need(1) && map1(*this, a[0], out, [](float x) { return x * 0.017453292519943295f; });
    case BuiltinFn::Degrees: return need(1) && map1(*this, a[0], out, [](float x) { return x * 57.29577951308232f; });
    case BuiltinFn::Sin: return need(1) && map1(*this, a[0], out, [](float x) { return fsin(x); });
    case BuiltinFn::Cos: return need(1) && map1(*this, a[0], out, [](float x) { return fcos(x); });
    case BuiltinFn::Tan: return need(1) && map1(*this, a[0], out, [](float x) { return fsin(x) / fcos(x); });
    case BuiltinFn::Asin: return need(1) && map1(*this, a[0], out, [](float x) { return fatan2(x, fsqrt(1 - x * x)); });
    case BuiltinFn::Acos: return need(1) && map1(*this, a[0], out, [](float x) { return fatan2(fsqrt(1 - x * x), x); });
    case BuiltinFn::Atan:
      if (n >= 2) return map2(*this, a[0], a[1], out, [](float y, float x) { return fatan2(y, x); });
      return need(1) && map1(*this, a[0], out, [](float x) { return fatan2(x, 1.0f); });
    case BuiltinFn::Atan2: return need(2) && map2(*this, a[0], a[1], out, [](float y, float x) { return fatan2(y, x); });
    case BuiltinFn::Sinh: return need(1) && map1(*this, a[0], out, [](float x) { return (fexp(x) - fexp(-x)) * 0.5f; });
    case BuiltinFn::Cosh: return need(1) && map1(*this, a[0], out, [](float x) { return (fexp(x) + fexp(-x)) * 0.5f; });
    case BuiltinFn::Tanh: return need(1) && map1(*this, a[0], out, [](float x) {
      if (x > 20) return 1.0f;
      if (x < -20) return -1.0f;
      const float e = fexp(2 * x);
      return (e - 1) / (e + 1);
    });
    case BuiltinFn::Asinh: return need(1) && map1(*this, a[0], out, [](float x) { return flog(x + fsqrt(x * x + 1)); });
    case BuiltinFn::Acosh: return need(1) && map1(*this, a[0], out, [](float x) { return flog(x + fsqrt(x * x - 1)); });
    case BuiltinFn::Atanh: return need(1) && map1(*this, a[0], out, [](float x) { return 0.5f * flog((1 + x) / (1 - x)); });
    case BuiltinFn::Pow: return need(2) && map2(*this, a[0], a[1], out, [](float x, float y) { return fpow(x, y); });
    case BuiltinFn::Exp: return need(1) && map1(*this, a[0], out, [](float x) { return fexp(x); });
    case BuiltinFn::Exp2: return need(1) && map1(*this, a[0], out, [](float x) { return fexp(x * 0.6931471805599453f); });
    case BuiltinFn::Log: return need(1) && map1(*this, a[0], out, [](float x) { return flog(x); });
    case BuiltinFn::Log2: return need(1) && map1(*this, a[0], out, [](float x) { return flog(x) * 1.4426950408889634f; });
    case BuiltinFn::Sqrt: return need(1) && map1(*this, a[0], out, [](float x) { return fsqrt(x); });
    case BuiltinFn::InverseSqrt: return need(1) && map1(*this, a[0], out, [](float x) { return 1.0f / fsqrt(x); });
    case BuiltinFn::Floor: return need(1) && map1(*this, a[0], out, [](float x) { return ffloor(x); });
    case BuiltinFn::Ceil: return need(1) && map1(*this, a[0], out, [](float x) { return -ffloor(-x); });
    case BuiltinFn::Fract: return need(1) && map1(*this, a[0], out, [](float x) { return x - ffloor(x); });
    case BuiltinFn::Trunc: return need(1) && map1(*this, a[0], out, [](float x) { return x < 0 ? -ffloor(-x) : ffloor(x); });
    case BuiltinFn::Round:
      if (wgsl) return need(1) && map1(*this, a[0], out, [](float x) { return fround_even(x); });
      return need(1) && map1(*this, a[0], out, [](float x) { return x < 0 ? -ffloor(-x + 0.5f) : ffloor(x + 0.5f); });
    case BuiltinFn::RoundEven: return need(1) && map1(*this, a[0], out, [](float x) { return fround_even(x); });
    case BuiltinFn::Saturate: return need(1) && map1(*this, a[0], out, [](float x) { return sat(x); });
    case BuiltinFn::IsNan: case BuiltinFn::IsInf: {
      if (!need(1)) return false;
      Ref fa;
      if (!as_float(*this, a[0], &fa)) return false;
      Type t = fa.t;
      t.base = Base::Bool;
      if (!new_value(t, out, &w)) return false;
      for (uint32_t i = 0; i < count_of(t); ++i) {
        const float x = u2f(fa.p[i]);
        w[i] = fn == BuiltinFn::IsNan ? (x != x) : (x == kInf || x == -kInf);
      }
      return true;
    }
    case BuiltinFn::Step:
      return need(2) && map2(*this, a[0], a[1], out, [](float edge, float x) { return x < edge ? 0.0f : 1.0f; });
    case BuiltinFn::Smoothstep:
      return need(3) && map3(*this, a[0], a[1], a[2], out, [](float e0, float e1, float x) {
        const float t = sat((x - e0) / (e1 - e0));
        return t * t * (3 - 2 * t);
      });
    case BuiltinFn::Fma: return need(3) && map3(*this, a[0], a[1], a[2], out, [](float x, float y, float z) { return x * y + z; });
    case BuiltinFn::Mix:
      if (!need(3)) return false;
      if (a[2].t.base == Base::Bool) {   // select between x and y
        Type t;
        if (!shape_of(a[0], a[1], &t)) return fail("mix sizes do not match");
        if (!new_value(t, out, &w)) return false;
        const uint32_t cn = count_of(a[2].t);
        for (uint32_t i = 0; i < count_of(t); ++i) w[i] = a[2].p[cn == 1 ? 0 : i] ? a[1].p[count_of(a[1].t) == 1 ? 0 : i] : a[0].p[count_of(a[0].t) == 1 ? 0 : i];
        return true;
      }
      return map3(*this, a[0], a[1], a[2], out, [](float x, float y, float t) { return x * (1 - t) + y * t; });

    // --- component-wise arithmetic that also works on integers
    case BuiltinFn::Abs: case BuiltinFn::Sign: {
      if (!need(1)) return false;
      if (a[0].t.base == Base::Float) {
        if (fn == BuiltinFn::Abs) return map1(*this, a[0], out, [](float x) { return absf(x); });
        return map1(*this, a[0], out, [](float x) { return x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f); });
      }
      if (!new_value(a[0].t, out, &w)) return false;
      for (uint32_t i = 0; i < count_of(a[0].t); ++i) {
        const int32_t v = u2i(a[0].p[i]);
        if (a[0].t.base == Base::Uint) w[i] = fn == BuiltinFn::Abs ? a[0].p[i] : (a[0].p[i] != 0);
        else w[i] = fn == BuiltinFn::Abs ? static_cast<uint32_t>(v < 0 ? -v : v) : static_cast<uint32_t>(v > 0 ? 1 : (v < 0 ? -1 : 0));
      }
      return true;
    }
    case BuiltinFn::Min: case BuiltinFn::Max: case BuiltinFn::Mod: {
      if (!need(2)) return false;
      const bool isint = a[0].t.base != Base::Float && a[1].t.base != Base::Float;
      if (!isint) {
        if (fn == BuiltinFn::Min) return map2(*this, a[0], a[1], out, [](float x, float y) { return y < x ? y : x; });
        if (fn == BuiltinFn::Max) return map2(*this, a[0], a[1], out, [](float x, float y) { return y > x ? y : x; });
        return map2(*this, a[0], a[1], out, [](float x, float y) { return x - y * ffloor(x / y); });
      }
      Type t;
      if (!shape_of(a[0], a[1], &t)) return fail("argument sizes do not match");
      if (a[1].t.base == Base::Uint) t.base = Base::Uint;
      if (!new_value(t, out, &w)) return false;
      const bool sgn = t.base == Base::Int;
      for (uint32_t i = 0; i < count_of(t); ++i) {
        const uint32_t x = a[0].p[count_of(a[0].t) == 1 ? 0 : i], y = a[1].p[count_of(a[1].t) == 1 ? 0 : i];
        bool less = sgn ? u2i(y) < u2i(x) : y < x;
        if (fn == BuiltinFn::Min) w[i] = less ? y : x;
        else if (fn == BuiltinFn::Max) w[i] = (sgn ? u2i(y) > u2i(x) : y > x) ? y : x;
        else w[i] = y ? x % y : 0;
      }
      return true;
    }
    case BuiltinFn::Clamp: {
      if (!need(3)) return false;
      if (a[0].t.base == Base::Float || a[1].t.base == Base::Float)
        return map3(*this, a[0], a[1], a[2], out, [](float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); });
      Type t = a[0].t;
      if (!new_value(t, out, &w)) return false;
      const bool sgn = t.base == Base::Int;
      for (uint32_t i = 0; i < count_of(t); ++i) {
        const uint32_t x = a[0].p[i], lo = a[1].p[count_of(a[1].t) == 1 ? 0 : i], hi = a[2].p[count_of(a[2].t) == 1 ? 0 : i];
        uint32_t r = x;
        if (sgn ? u2i(x) < u2i(lo) : x < lo) r = lo;
        if (sgn ? u2i(r) > u2i(hi) : r > hi) r = hi;
        w[i] = r;
      }
      return true;
    }

    // --- geometry
    case BuiltinFn::Length: {
      if (!need(1)) return false;
      Ref fa;
      if (!as_float(*this, a[0], &fa)) return false;
      return scalar_result(*this, Base::Float, f2u(fsqrt(dotf(fa, fa))), out);
    }
    case BuiltinFn::Distance: {
      if (!need(2)) return false;
      Ref d;
      if (!map2(*this, a[0], a[1], &d, [](float x, float y) { return x - y; })) return false;
      return scalar_result(*this, Base::Float, f2u(fsqrt(dotf(d, d))), out);
    }
    case BuiltinFn::Dot: {
      if (!need(2)) return false;
      Ref fa, fb;
      if (!as_float(*this, a[0], &fa) || !as_float(*this, a[1], &fb)) return false;
      return scalar_result(*this, Base::Float, f2u(dotf(fa, fb)), out);
    }
    case BuiltinFn::Cross: {
      if (!need(2)) return false;
      Ref x, y;
      if (!as_float(*this, a[0], &x) || !as_float(*this, a[1], &y) || count_of(x.t) < 3 || count_of(y.t) < 3) return fail("cross needs vec3");
      if (!new_value(make_type(Base::Float, 3), out, &w)) return false;
      const FloatCopy cx(x.p, 3), cy(y.p, 3);
      const float *p = cx.v, *q = cy.v;
      w[0] = f2u(p[1] * q[2] - p[2] * q[1]);
      w[1] = f2u(p[2] * q[0] - p[0] * q[2]);
      w[2] = f2u(p[0] * q[1] - p[1] * q[0]);
      return true;
    }
    case BuiltinFn::Normalize: {
      if (!need(1)) return false;
      Ref fa;
      if (!as_float(*this, a[0], &fa)) return false;
      const float len = fsqrt(dotf(fa, fa));
      return map1(*this, fa, out, [len](float x) { return x / len; });
    }
    case BuiltinFn::FaceForward: {
      if (!need(3)) return false;
      Ref nref, i;
      if (!as_float(*this, a[2], &nref) || !as_float(*this, a[1], &i)) return false;
      const bool flip = dotf(nref, i) >= 0;
      return map1(*this, a[0], out, [flip](float x) { return flip ? -x : x; });
    }
    case BuiltinFn::Reflect: {
      if (!need(2)) return false;
      Ref i, nn;
      if (!as_float(*this, a[0], &i) || !as_float(*this, a[1], &nn)) return false;
      const float d = dotf(nn, i);
      return map2(*this, i, nn, out, [d](float iv, float nv) { return iv - 2 * d * nv; });
    }
    case BuiltinFn::Refract: {
      if (!need(3)) return false;
      Ref i, nn, et;
      if (!as_float(*this, a[0], &i) || !as_float(*this, a[1], &nn) || !as_float(*this, a[2], &et)) return false;
      const float eta = u2f(et.p[0]);
      const float d = dotf(nn, i);
      const float k = 1 - eta * eta * (1 - d * d);
      if (k < 0) {
        Type t = i.t;
        return new_value(t, out, &w);
      }
      const float s = eta * d + fsqrt(k);
      return map2(*this, i, nn, out, [eta, s](float iv, float nv) { return eta * iv - s * nv; });
    }
    case BuiltinFn::Ldexp: return need(2) && map2(*this, a[0], a[1], out, [](float x, float e) { return x * fexp(e * 0.6931471805599453f); });

    // --- matrices
    case BuiltinFn::MatrixCompMult: {
      if (!need(2)) return false;
      return map2(*this, a[0], a[1], out, [](float x, float y) { return x * y; });
    }
    case BuiltinFn::OuterProduct: {
      if (!need(2)) return false;
      Ref c, r;
      if (!as_float(*this, a[0], &c) || !as_float(*this, a[1], &r)) return false;
      const uint32_t rows = c.t.rows, cols = r.t.rows;
      if (!new_value(make_type(Base::Float, static_cast<uint8_t>(rows), static_cast<uint8_t>(cols)), out, &w)) return false;
      for (uint32_t j = 0; j < cols; ++j)
        for (uint32_t i = 0; i < rows; ++i) w[j * rows + i] = f2u(u2f(c.p[i]) * u2f(r.p[j]));
      return true;
    }
    case BuiltinFn::Transpose: {
      if (!need(1)) return false;
      Ref f;
      if (!as_float(*this, a[0], &f)) return false;
      const uint32_t cols = f.t.cols, rows = f.t.rows;
      if (!new_value(make_type(Base::Float, static_cast<uint8_t>(cols), static_cast<uint8_t>(rows)), out, &w)) return false;
      for (uint32_t c = 0; c < cols; ++c)
        for (uint32_t r = 0; r < rows; ++r) w[r * cols + c] = f.p[c * rows + r];
      return true;
    }
    case BuiltinFn::Determinant: {
      if (!need(1)) return false;
      Ref f;
      if (!as_float(*this, a[0], &f) || f.t.cols != f.t.rows || f.t.cols < 2) return fail("determinant needs a square matrix");
      return scalar_result(*this, Base::Float, f2u(determinant_n(FloatCopy(f.p, count_of(f.t)).v, f.t.cols)), out);
    }
    case BuiltinFn::Inverse: {
      if (!need(1)) return false;
      Ref f;
      if (!as_float(*this, a[0], &f) || f.t.cols != f.t.rows || f.t.cols < 2) return fail("inverse needs a square matrix");
      if (!new_value(f.t, out, &w)) return false;
      float tmp[16];
      invert_n(FloatCopy(f.p, count_of(f.t)).v, f.t.cols, tmp);
      for (uint32_t i = 0; i < count_of(f.t); ++i) w[i] = f2u(tmp[i]);
      return true;
    }

    // --- relational
    case BuiltinFn::LessThan: case BuiltinFn::LessThanEqual: case BuiltinFn::GreaterThan: case BuiltinFn::GreaterThanEqual: case BuiltinFn::Equal: case BuiltinFn::NotEqual: {
      if (!need(2)) return false;
      Op op = fn == BuiltinFn::LessThan ? Op::Lt : fn == BuiltinFn::LessThanEqual ? Op::Le : fn == BuiltinFn::GreaterThan ? Op::Gt : fn == BuiltinFn::GreaterThanEqual ? Op::Ge : fn == BuiltinFn::Equal ? Op::Eq : Op::Ne;
      // componentwise: the binary operator does that for WGSL; GLSL's Eq/Ne on vectors fold, so do it by hand
      Type t;
      if (!shape_of(a[0], a[1], &t)) return fail("argument sizes do not match");
      t.base = Base::Bool;
      if (!new_value(t, out, &w)) return false;
      for (uint32_t i = 0; i < count_of(t); ++i) {
        Ref x, y, r;
        Ref xs = a[0], ys = a[1];
        xs.p = a[0].p + (count_of(a[0].t) == 1 ? 0 : i);
        xs.t = make_type(a[0].t.base);
        ys.p = a[1].p + (count_of(a[1].t) == 1 ? 0 : i);
        ys.t = make_type(a[1].t.base);
        if (!binary(op, xs, ys, &r)) return false;
        w[i] = r.p[0];
      }
      return true;
    }
    case BuiltinFn::Any: case BuiltinFn::All: {
      if (!need(1)) return false;
      bool any = false, all = true;
      for (uint32_t i = 0; i < count_of(a[0].t); ++i) {
        any = any || a[0].p[i];
        all = all && a[0].p[i];
      }
      return scalar_result(*this, Base::Bool, fn == BuiltinFn::Any ? any : all, out);
    }
    case BuiltinFn::Not: {
      if (!need(1)) return false;
      return unary(Op::Not, a[0], out);
    }
    case BuiltinFn::Select: {   // select(f, t, cond)
      if (!need(3)) return false;
      Type t;
      if (!shape_of(a[0], a[1], &t)) return fail("select sizes do not match");
      if (!new_value(t, out, &w)) return false;
      const uint32_t cn = count_of(a[2].t);
      for (uint32_t i = 0; i < count_of(t); ++i) w[i] = a[2].p[cn == 1 ? 0 : i] ? a[1].p[count_of(a[1].t) == 1 ? 0 : i] : a[0].p[count_of(a[0].t) == 1 ? 0 : i];
      return true;
    }

    // --- bit casts and packing
    case BuiltinFn::FloatBitsToInt: case BuiltinFn::FloatBitsToUint: case BuiltinFn::IntBitsToFloat: case BuiltinFn::UintBitsToFloat: {
      if (!need(1)) return false;
      Type t = a[0].t;
      t.base = fn == BuiltinFn::FloatBitsToInt ? Base::Int : fn == BuiltinFn::FloatBitsToUint ? Base::Uint : Base::Float;
      if (!new_value(t, out, &w)) return false;
      for (uint32_t i = 0; i < count_of(t); ++i) w[i] = a[0].p[i];
      return true;
    }
    case BuiltinFn::PackUnorm2x16: case BuiltinFn::PackSnorm2x16: case BuiltinFn::PackHalf2x16: case BuiltinFn::PackUnorm4x8: case BuiltinFn::PackSnorm4x8: {
      if (!need(1)) return false;
      Ref f;
      if (!as_float(*this, a[0], &f)) return false;
      uint32_t r = 0;
      const FloatCopy fc(f.p, count_of(f.t));
      const float* v = fc.v;
      switch (fn) {
        case BuiltinFn::PackUnorm2x16: r = pack_unorm(v[0], 16) | (pack_unorm(v[1], 16) << 16); break;
        case BuiltinFn::PackSnorm2x16: r = pack_snorm(v[0], 16) | (pack_snorm(v[1], 16) << 16); break;
        case BuiltinFn::PackHalf2x16: r = float_to_half(v[0]) | (static_cast<uint32_t>(float_to_half(v[1])) << 16); break;
        case BuiltinFn::PackUnorm4x8: for (uint32_t i = 0; i < 4; ++i) r |= pack_unorm(v[i], 8) << (8 * i); break;
        default: for (uint32_t i = 0; i < 4; ++i) r |= pack_snorm(v[i], 8) << (8 * i); break;
      }
      return scalar_result(*this, Base::Uint, r, out);
    }
    case BuiltinFn::UnpackUnorm2x16: case BuiltinFn::UnpackSnorm2x16: case BuiltinFn::UnpackHalf2x16: case BuiltinFn::UnpackUnorm4x8: case BuiltinFn::UnpackSnorm4x8: {
      if (!need(1)) return false;
      const uint32_t v = a[0].p[0];
      const bool four = fn == BuiltinFn::UnpackUnorm4x8 || fn == BuiltinFn::UnpackSnorm4x8;
      if (!new_value(make_type(Base::Float, four ? 4 : 2), out, &w)) return false;
      for (uint32_t i = 0; i < (four ? 4u : 2u); ++i) {
        switch (fn) {
          case BuiltinFn::UnpackUnorm2x16: w[i] = f2u(static_cast<float>((v >> (16 * i)) & 0xFFFF) / 65535.0f); break;
          case BuiltinFn::UnpackSnorm2x16: w[i] = f2u(unpack_snorm((v >> (16 * i)) & 0xFFFF, 16)); break;
          case BuiltinFn::UnpackHalf2x16: w[i] = f2u(half_to_float(static_cast<uint16_t>(v >> (16 * i)))); break;
          case BuiltinFn::UnpackUnorm4x8: w[i] = f2u(static_cast<float>((v >> (8 * i)) & 0xFF) / 255.0f); break;
          default: w[i] = f2u(unpack_snorm((v >> (8 * i)) & 0xFF, 8)); break;
        }
      }
      return true;
    }

    // --- bits
    case BuiltinFn::BitCount: case BuiltinFn::FindLsb: case BuiltinFn::FindMsb: case BuiltinFn::BitfieldReverse: case BuiltinFn::ReverseBits: case BuiltinFn::CountLeadingZeros:
    case BuiltinFn::CountTrailingZeros: case BuiltinFn::FirstLeadingBit: case BuiltinFn::FirstTrailingBit: {
      if (!need(1)) return false;
      Type t = a[0].t;
      if (fn == BuiltinFn::BitCount) t.base = wgsl ? a[0].t.base : Base::Int;
      if (!new_value(t, out, &w)) return false;
      const bool sgn = a[0].t.base == Base::Int;
      for (uint32_t i = 0; i < count_of(t); ++i) {
        const uint32_t v = a[0].p[i];
        switch (fn) {
          case BuiltinFn::BitCount: w[i] = popcount(v); break;
          case BuiltinFn::BitfieldReverse: case BuiltinFn::ReverseBits: w[i] = reverse_bits(v); break;
          case BuiltinFn::CountLeadingZeros: w[i] = clz(v); break;
          case BuiltinFn::CountTrailingZeros: w[i] = ctz(v); break;
          case BuiltinFn::FindLsb: case BuiltinFn::FirstTrailingBit: w[i] = v ? ctz(v) : (wgsl ? 0xFFFFFFFFu : 0xFFFFFFFFu); break;
          default: {   // findMSB / firstLeadingBit
            uint32_t x = v;
            if (sgn && u2i(v) < 0) x = ~v;
            w[i] = x ? 31 - clz(x) : 0xFFFFFFFFu;
            break;
          }
        }
      }
      return true;
    }
    case BuiltinFn::BitfieldExtract: case BuiltinFn::ExtractBits: {
      if (!need(3)) return false;
      if (!new_value(a[0].t, out, &w)) return false;
      const bool sgn = a[0].t.base == Base::Int;
      const uint32_t off = a[1].p[0] & 0xFFFF, bits = a[2].p[0] & 0xFFFF;
      for (uint32_t i = 0; i < count_of(a[0].t); ++i) {
        const uint32_t v = a[0].p[i];
        if (bits == 0) { w[i] = 0; continue; }
        if (off + bits >= 32 && off >= 32) { w[i] = 0; continue; }
        const uint32_t sh = (v >> off) << (32 - (bits > 32 ? 32 : bits)) ;
        w[i] = sgn ? static_cast<uint32_t>(static_cast<int32_t>(sh) >> (32 - (bits > 32 ? 32 : bits))) : sh >> (32 - (bits > 32 ? 32 : bits));
      }
      return true;
    }
    case BuiltinFn::BitfieldInsert: case BuiltinFn::InsertBits: {
      if (!need(4)) return false;
      if (!new_value(a[0].t, out, &w)) return false;
      const uint32_t off = a[2].p[0], bits = a[3].p[0];
      const uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (((1u << bits) - 1) << off);
      for (uint32_t i = 0; i < count_of(a[0].t); ++i) w[i] = bits == 0 ? a[0].p[i] : ((a[0].p[i] & ~mask) | ((a[1].p[i] << off) & mask));
      return true;
    }
    case BuiltinFn::UaddCarry: case BuiltinFn::UsubBorrow: {
      if (!need(4)) return false;
      if (!new_value(a[0].t, out, &w)) return false;
      for (uint32_t i = 0; i < count_of(a[0].t); ++i) {
        const uint64_t s = fn == BuiltinFn::UaddCarry ? uint64_t{a[0].p[i]} + a[1].p[i] : uint64_t{a[0].p[i]} - a[1].p[i];
        w[i] = static_cast<uint32_t>(s);
        if (wr[2]) wr[2][i] = fn == BuiltinFn::UaddCarry ? (s >> 32) != 0 : a[0].p[i] < a[1].p[i];
      }
      return true;
    }
    case BuiltinFn::Modf: {   // GLSL modf(x, out i)
      if (!need(2)) return false;
      Ref f;
      if (!as_float(*this, a[0], &f)) return false;
      if (!new_value(f.t, out, &w)) return false;
      for (uint32_t i = 0; i < count_of(f.t); ++i) {
        const float x = u2f(f.p[i]);
        const float ip = x < 0 ? -ffloor(-x) : ffloor(x);
        if (wr[1]) wr[1][i] = f2u(ip);
        w[i] = f2u(x - ip);
      }
      return true;
    }

    // --- derivatives
    case BuiltinFn::Dfdx: case BuiltinFn::Dfdy: case BuiltinFn::Fwidth: {
      if (!need(1)) return false;
      Ref f;
      if (!as_float(*this, a[0], &f)) return false;
      const uint32_t cnt = count_of(f.t);
      if (!new_value(f.t, out, &w)) return false;
      if (m.record_) {
        for (uint32_t i = 0; i < cnt; ++i) m.record_->values.push_back(u2f(f.p[i]));
        m.site_ += cnt;
        return true;
      }
      if (m.replay_x_ && m.replay_y_) {
        const uint32_t s = m.site_;
        m.site_ += cnt;
        if (s + cnt > m.replay_x_->values.count() || s + cnt > m.replay_y_->values.count()) return true;
        for (uint32_t i = 0; i < cnt; ++i) {
          const float dx = m.replay_x_->values[s + i] - u2f(f.p[i]), dy = m.replay_y_->values[s + i] - u2f(f.p[i]);
          w[i] = f2u(fn == BuiltinFn::Dfdx ? dx : (fn == BuiltinFn::Dfdy ? dy : absf(dx) + absf(dy)));
        }
      }
      return true;
    }

    // --- compute
    case BuiltinFn::Barrier: case BuiltinFn::TextureBarrier: return true;
    case BuiltinFn::ArrayLength: return need(1) && scalar_result(*this, Base::Uint, a[0].t.array, out);
    case BuiltinFn::AtomicLoad: {
      if (!need(1)) return false;
      return scalar_result(*this, a[0].t.base, wr[0] ? wr[0][0] : 0, out);
    }
    case BuiltinFn::AtomicStore: {
      if (!need(2) || !wr[0]) return fail("bad atomic");
      wr[0][0] = convert_scalar(a[1].p[0], a[1].t.base, a[0].t.base);
      return true;
    }
    case BuiltinFn::AtomicAdd: case BuiltinFn::AtomicSub: case BuiltinFn::AtomicMax: case BuiltinFn::AtomicMin: case BuiltinFn::AtomicAnd: case BuiltinFn::AtomicOr:
    case BuiltinFn::AtomicXor: case BuiltinFn::AtomicExchange: {
      if (!need(2) || !wr[0]) return fail("bad atomic");
      const Base k = a[0].t.base;
      const uint32_t old = wr[0][0], v = convert_scalar(a[1].p[0], a[1].t.base, k);
      uint32_t nv = old;
      switch (fn) {
        case BuiltinFn::AtomicAdd: nv = old + v; break;
        case BuiltinFn::AtomicSub: nv = old - v; break;
        case BuiltinFn::AtomicMax: nv = k == Base::Int ? (u2i(v) > u2i(old) ? v : old) : (v > old ? v : old); break;
        case BuiltinFn::AtomicMin: nv = k == Base::Int ? (u2i(v) < u2i(old) ? v : old) : (v < old ? v : old); break;
        case BuiltinFn::AtomicAnd: nv = old & v; break;
        case BuiltinFn::AtomicOr: nv = old | v; break;
        case BuiltinFn::AtomicXor: nv = old ^ v; break;
        default: nv = v; break;
      }
      wr[0][0] = nv;
      return scalar_result(*this, k, old, out);
    }
    case BuiltinFn::AtomicCompareExchange: {   // returns the old value (the WGSL struct is not modelled)
      if (!need(3) || !wr[0]) return fail("bad atomic");
      const uint32_t old = wr[0][0];
      if (old == a[1].p[0]) wr[0][0] = a[2].p[0];
      return scalar_result(*this, a[0].t.base, old, out);
    }
    default: break;
  }
  (void)t0;
  (void)node;
  return fail("unsupported built-in function");
}

}  // namespace shader
