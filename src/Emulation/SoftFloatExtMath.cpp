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
#include "Emulation/SoftFloatExt.hpp"

// The x87 transcendental instructions, from series in the extended format itself. They are accurate to a unit or two in the last place; the hardware's own results are not
// exactly rounded either, and nothing relies on matching them bit for bit.
namespace emulation {
namespace softfloat {
namespace ext {

namespace {

// Arithmetic that rounds to nearest at the full 64 bits and ignores the flags.
struct Quiet {
  Env env;
  Quiet() { env.mode = Rounding::NearestEven; }
  Ext add(const Ext& a, const Ext& b) { return ext::add(a, b, 64, env); }
  Ext sub(const Ext& a, const Ext& b) { return ext::sub(a, b, 64, env); }
  Ext mul(const Ext& a, const Ext& b) { return ext::mul(a, b, 64, env); }
  Ext div(const Ext& a, const Ext& b) { return ext::div(a, b, 64, env); }
  Ext sqrt(const Ext& a) { return ext::sqrt(a, 64, env); }
};

constexpr Ext make(uint64_t mant, int32_t biased_exp, bool negative = false) {
  Ext e;
  e.mant = mant;
  e.se = static_cast<uint16_t>((negative ? 0x8000 : 0) | biased_exp);
  return e;
}
// value = mant * 2^(exp - 63)
constexpr Ext constant(uint64_t mant, int32_t exp) { return make(mant, exp + 16383); }

constexpr Ext kPi = constant(0xC90FDAA22168C235ull, 1);
constexpr Ext kHalfPi = constant(0xC90FDAA22168C235ull, 0);
constexpr Ext kQuarterPi = constant(0xC90FDAA22168C235ull, -1);
constexpr Ext kLn2 = constant(0xB17217F7D1CF79ACull, -1);
constexpr Ext kLog2e = constant(0xB8AA3B295C17F0BCull, 0);
constexpr Ext kTwoOverPi = constant(0xA2F9836E4E441529ull, -1);      // 2 / pi
// pi/2 as the sum of six pieces of 32 significant bits each, so that a 32-bit multiple of each is exact in the 64-bit significand.
constexpr Ext kHalfPiPieces[6] = {constant(0x921FB54400000000ull, 0), constant(0x42D1846900000000ull, -32), constant(0x898CC51700000000ull, -64),
                              constant(0x01B839A200000000ull, -96), constant(0x52049C1100000000ull, -128), constant(0x14CF98E800000000ull, -160)};

bool is_zero(const Ext& x) { return classify(x) == Class::Zero; }

int32_t exponent_of(const Ext& x) {      // unbiased exponent of a normal value
  return static_cast<int32_t>(x.exponent()) - 16383;
}

Ext scale_by_power_of_two(const Ext& x, int32_t n) {
  Ext r = x;
  r.se = static_cast<uint16_t>((x.se & 0x8000) | static_cast<uint32_t>(static_cast<int32_t>(x.exponent()) + n));
  return r;
}

// exp(y) - 1 for |y| <= 1.
Ext expm1_small(Quiet& q, const Ext& y) {
  if (is_zero(y)) return y;
  Ext term = y, sum = y;
  for (int n = 2; n < 40; ++n) {
    term = q.div(q.mul(term, y), from_integer(n));
    sum = q.add(sum, term);
    if (classify(term) == Class::Zero || exponent_of(term) < exponent_of(sum) - 70) break;
  }
  return sum;
}

// 2 * atanh(z) = 2 * (z + z^3/3 + z^5/5 ...) for small |z|.
Ext twice_atanh(Quiet& q, const Ext& z) {
  if (is_zero(z)) return z;
  const Ext z2 = q.mul(z, z);
  Ext power = z, sum = z;
  for (int n = 3; n < 200; n += 2) {
    power = q.mul(power, z2);
    const Ext term = q.div(power, from_integer(n));
    sum = q.add(sum, term);
    if (exponent_of(term) < exponent_of(sum) - 70) break;
  }
  return scale_by_power_of_two(sum, 1);
}

// sin and cos of |r| <= ~0.8 by their series.
void sin_cos_small(Quiet& q, const Ext& r, Ext* sine, Ext* cosine) {
  const Ext r2 = q.mul(r, r);
  Ext term = r, sum = r;
  for (int n = 1; n < 40; ++n) {
    term = q.div(q.mul(term, r2), from_integer(static_cast<int64_t>(2 * n) * (2 * n + 1)));
    term = negated(term);
    sum = q.add(sum, term);
    if (is_zero(term) || (!is_zero(sum) && exponent_of(term) < exponent_of(sum) - 70)) break;
  }
  *sine = sum;
  term = one();
  Ext csum = one();
  for (int n = 1; n < 40; ++n) {
    term = q.div(q.mul(term, r2), from_integer(static_cast<int64_t>(2 * n - 1) * (2 * n)));
    term = negated(term);
    csum = q.add(csum, term);
    if (is_zero(term) || exponent_of(term) < exponent_of(csum) - 70) break;
  }
  *cosine = csum;
}

// atan(t) for t >= 0 (any size).
Ext atan_positive(Quiet& q, Ext t) {
  if (is_zero(t)) return t;
  bool invert = false;
  if (compare(t, one(), false, q.env) > 0) {      // atan(t) = pi/2 - atan(1/t)
    t = q.div(one(), t);
    invert = true;
  }
  // Halve the angle twice: tan(a/2) = t / (1 + sqrt(1 + t^2)).
  for (int i = 0; i < 2; ++i) {
    const Ext root = q.sqrt(q.add(one(), q.mul(t, t)));
    t = q.div(t, q.add(one(), root));
  }
  const Ext t2 = q.mul(t, t);
  Ext power = t, sum = t;
  for (int n = 3; n < 100; n += 2) {
    power = negated(q.mul(power, t2));
    const Ext term = q.div(power, from_integer(n));
    sum = q.add(sum, term);
    if (exponent_of(term) < exponent_of(sum) - 70) break;
  }
  Ext angle = scale_by_power_of_two(sum, 2);
  if (invert) angle = q.sub(kHalfPi, angle);
  return angle;
}

}  // namespace

Ext f2xm1(const Ext& x, Env& env) {
  const Class c = classify(x);
  if (c == Class::Zero || c == Class::QuietNan || c == Class::SignalingNan) {
    if (c == Class::SignalingNan) env.flags |= kInvalid;
    Ext r = x;
    if (c == Class::SignalingNan) r.mant |= 0x4000000000000000ull;
    return r;
  }
  if (c == Class::Infinity) return x.sign() ? make(0x8000000000000000ull, 0x3FFF, true) : x;      // 2^-inf - 1 = -1
  if (c == Class::Unsupported) { env.flags |= kInvalid; return indefinite(); }
  Quiet q;
  const Ext y = q.mul(x, kLn2);
  Ext r = expm1_small(q, y);
  env.flags |= kInexact;
  return r;
}

Ext fyl2x(const Ext& x, const Ext& y, Env& env) {
  const Class cx = classify(x), cy = classify(y);
  if (cx == Class::Unsupported || cy == Class::Unsupported) { env.flags |= kInvalid; return indefinite(); }
  if (cx == Class::QuietNan || cx == Class::SignalingNan || cy == Class::QuietNan || cy == Class::SignalingNan) return mul(x, y, 64, env);      // (a NaN comes out)
  if (cx == Class::Zero) {      // log2(0) = -inf
    if (cy == Class::Zero) { env.flags |= kInvalid; return indefinite(); }
    env.flags |= kDivideByZero;
    return infinity(!y.sign());
  }
  if (x.sign()) { env.flags |= kInvalid; return indefinite(); }
  if (cx == Class::Infinity) {
    if (cy == Class::Zero) { env.flags |= kInvalid; return indefinite(); }
    return infinity(y.sign());
  }
  Quiet q;
  Env scratch;
  // x = m * 2^e with m in [sqrt(1/2), sqrt(2))
  Ext m = x;
  int32_t e;
  if (cx == Class::Denormal) {
    // normalize through multiplication by 2^64
    m = mul(x, constant(0x8000000000000000ull, 64), 64, scratch);
    e = exponent_of(m) - 64;
  } else {
    e = exponent_of(x);
  }
  m.se = 0x3FFF;      // m in [1, 2)
  if (compare(m, constant(0xB504F333F9DE6484ull, 0), false, q.env) > 0) {      // above sqrt(2): use m/2 and e + 1
    m = constant(m.mant, -1);
    ++e;
  }
  const Ext z = q.div(q.sub(m, one()), q.add(m, one()));
  const Ext ln_m = twice_atanh(q, z);
  Ext log2_value = q.mul(ln_m, kLog2e);
  if (e != 0) log2_value = q.add(from_integer(e), log2_value);
  Ext r = mul(log2_value, y, 64, env);
  env.flags |= kInexact;
  if (is_zero(log2_value)) env.flags &= ~kInexact;
  return r;
}

Ext fyl2xp1(const Ext& x, const Ext& y, Env& env) {
  const Class cx = classify(x), cy = classify(y);
  if (cx == Class::Unsupported || cy == Class::Unsupported) { env.flags |= kInvalid; return indefinite(); }
  if (cx == Class::QuietNan || cx == Class::SignalingNan || cy == Class::QuietNan || cy == Class::SignalingNan) return mul(x, y, 64, env);
  if (cx == Class::Zero) return mul(x, y, 64, env);      // log2(1) = 0, with the sign of x * y
  if (cx == Class::Infinity) { env.flags |= kInvalid; return indefinite(); }
  Quiet q;
  const Ext z = q.div(x, q.add(constant(0x8000000000000000ull, 1), x));      // x / (2 + x)
  const Ext ln1p = twice_atanh(q, z);
  const Ext log2_value = q.mul(ln1p, kLog2e);
  Ext r = mul(log2_value, y, 64, env);
  env.flags |= kInexact;
  return r;
}

Ext fpatan(const Ext& y, const Ext& x, Env& env) {
  const Class cy = classify(y), cx = classify(x);
  if (cx == Class::Unsupported || cy == Class::Unsupported) { env.flags |= kInvalid; return indefinite(); }
  if (cx == Class::QuietNan || cx == Class::SignalingNan || cy == Class::QuietNan || cy == Class::SignalingNan) return add(x, y, 64, env);
  const bool ys = y.sign(), xs = x.sign();
  Quiet q;
  auto signed_value = [&](Ext v) { return ys ? negated(v) : v; };
  if (cy == Class::Zero) return xs ? signed_value(kPi) : y;      // atan2(+-0, -x) = +-pi, atan2(+-0, +x) = +-0
  if (cx == Class::Zero) return signed_value(kHalfPi);
  if (cy == Class::Infinity) {
    if (cx == Class::Infinity) return signed_value(xs ? q.mul(kQuarterPi, from_integer(3)) : kQuarterPi);
    return signed_value(kHalfPi);
  }
  if (cx == Class::Infinity) return xs ? signed_value(kPi) : zero(ys);
  Ext t = div(absolute(y), absolute(x), 64, q.env);
  Ext angle = atan_positive(q, t);
  if (xs) angle = q.sub(kPi, angle);
  env.flags |= kInexact;
  (void)env;
  return signed_value(angle);
}

namespace {

// x = n * (pi/2) + r with |r| <= about pi/4; *quadrant = n mod 4. False if |x| is 2^63 or more.
bool reduce(Quiet& q, Ext x, Ext* r, uint32_t* quadrant) {
  *quadrant = 0;
  const bool neg = x.sign();
  x = absolute(x);
  if (exponent_of(x) >= 63) return false;
  uint64_t total = 0;      // n mod 4 so far
  for (int round = 0; round < 6; ++round) {
    if (compare(x, kQuarterPi, false, q.env) <= 0) break;
    Ext estimate = q.mul(x, kTwoOverPi);      // x / (pi/2), approximately
    int64_t n;
    int32_t shift = 0;
    if (exponent_of(estimate) >= 31) {      // keep the top 32 bits: n = nh * 2^shift
      shift = exponent_of(estimate) - 31;
      Ext top = scale_by_power_of_two(estimate, -shift);
      Env tmp;
      tmp.mode = Rounding::TowardZero;
      if (!to_integer(top, 64, Rounding::TowardZero, tmp, &n)) return false;
    } else {
      Env tmp;
      if (!to_integer(estimate, 64, Rounding::NearestEven, tmp, &n)) return false;
    }
    if (n == 0) break;
    // x -= n * (pi/2), piece by piece: n has at most 32 significant bits, so each product is exact
    const Ext nh = scale_by_power_of_two(from_integer(n), shift);
    for (int i = 0; i < 6; ++i) x = q.sub(x, q.mul(nh, kHalfPiPieces[i]));
    if (shift == 0) total += static_cast<uint64_t>(n) & 3;
    else if (shift == 1) total += (static_cast<uint64_t>(n) << 1) & 3;
  }
  // x may have gone slightly negative: |r| is what matters
  *quadrant = static_cast<uint32_t>(total & 3);
  *r = x;
  if (neg) {      // sin and cos of -x: reflect
    *r = negated(x);
    *quadrant = static_cast<uint32_t>((4 - *quadrant) & 3);
    // (x = -(n pi/2 + r) = (-n) pi/2 + (-r))
  }
  return true;
}

}  // namespace

bool sin_cos(const Ext& x, Ext* sine, Ext* cosine, Env& env) {
  const Class c = classify(x);
  if (c == Class::QuietNan || c == Class::SignalingNan) {
    if (c == Class::SignalingNan) env.flags |= kInvalid;
    Ext r = x;
    r.mant |= 0x4000000000000000ull;
    *sine = *cosine = r;
    return true;
  }
  if (c == Class::Infinity || c == Class::Unsupported) { env.flags |= kInvalid; *sine = *cosine = indefinite(); return true; }
  if (c == Class::Zero) { *sine = x; *cosine = one(); return true; }
  Quiet q;
  Ext r;
  uint32_t quadrant;
  if (!reduce(q, x, &r, &quadrant)) return false;
  Ext s, co;
  sin_cos_small(q, r, &s, &co);
  switch (quadrant) {
    case 0: *sine = s; *cosine = co; break;
    case 1: *sine = co; *cosine = negated(s); break;
    case 2: *sine = negated(s); *cosine = negated(co); break;
    default: *sine = negated(co); *cosine = s; break;
  }
  env.flags |= kInexact;
  return true;
}

bool tangent(const Ext& x, Ext* tan, Env& env) {
  Ext s, c;
  if (!sin_cos(x, &s, &c, env)) return false;
  const Class cls = classify(x);
  if (cls == Class::Zero || cls == Class::QuietNan || cls == Class::SignalingNan || cls == Class::Infinity || cls == Class::Unsupported) { *tan = s; return true; }
  Env scratch;
  *tan = div(s, c, 64, scratch);
  return true;
}

}  // namespace ext
}  // namespace softfloat
}  // namespace emulation
