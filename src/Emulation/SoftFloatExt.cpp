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
#include "Emulation/Wide.hpp"

namespace emulation {
namespace softfloat {
namespace ext {

namespace {

typedef unsigned __int128 u128;      // (add, subtract, shift and compare only: no compiler runtime routine is needed)

constexpr int32_t kBias = 16383;
constexpr uint64_t kTop = 0x8000000000000000ull;

int clz64(uint64_t v) {      // v != 0
  int n = 0;
  if (!(v >> 32)) { n += 32; v <<= 32; }
  if (!(v >> 48)) { n += 16; v <<= 16; }
  if (!(v >> 56)) { n += 8; v <<= 8; }
  if (!(v >> 60)) { n += 4; v <<= 4; }
  if (!(v >> 62)) { n += 2; v <<= 2; }
  if (!(v >> 63)) { n += 1; }
  return n;
}
int clz128(u128 v) {      // v != 0
  const uint64_t hi = static_cast<uint64_t>(v >> 64), lo = static_cast<uint64_t>(v);
  return hi ? clz64(hi) : 64 + clz64(lo);
}
u128 jam128(u128 v, uint32_t n) {      // shift right, keeping a 1 in bit 0 if anything was lost
  if (n == 0) return v;
  if (n >= 128) return v != 0 ? 1 : 0;
  return (v >> n) | ((v << (128 - n)) != 0 ? 1 : 0);
}
u128 make128(uint64_t hi, uint64_t lo) { return (static_cast<u128>(hi) << 64) | lo; }

// A finite nonzero operand: value = mant * 2^(exp - 63), mant normalized (bit 63 set).
struct Num {
  bool sign = false;
  int32_t exp = 0;
  uint64_t mant = 0;
};

Num finite_of(const Ext& x, Env& env) {
  Num n;
  n.sign = x.sign();
  uint32_t e = x.exponent();
  uint64_t m = x.mant;
  if (e == 0) {      // denormal: the exponent is that of the smallest normal number
    env.flags |= kDenormal;
    e = 1;
  }
  const int lz = clz64(m);
  n.mant = m << lz;
  n.exp = static_cast<int32_t>(e) - kBias - lz;
  return n;
}

// sig has its leading bit at 127. Rounds to `precision` bits and packs. `sticky`: bits below sig were lost.
Ext round_pack(bool sign, int32_t exp, u128 sig, bool sticky, uint32_t precision, Env& env) {
  if (sticky) sig |= 1;
  int32_t be = exp + kBias;
  bool tiny = false;
  if (be <= 0) {      // a denormal result: shifted right until the exponent field is 0
    tiny = true;
    sig = jam128(sig, static_cast<uint32_t>(1 - be));
    be = 0;
  }
  const uint32_t drop = 128 - precision;
  const u128 rem_mask = (static_cast<u128>(1) << drop) - 1, half = static_cast<u128>(1) << (drop - 1);
  const u128 rem = sig & rem_mask;
  uint64_t keep = static_cast<uint64_t>(sig >> drop);
  bool increment = false;
  switch (env.mode) {
    case Rounding::NearestEven: increment = rem > half || (rem == half && (keep & 1)); break;
    case Rounding::Down: increment = sign && rem != 0; break;
    case Rounding::Up: increment = !sign && rem != 0; break;
    default: break;
  }
  if (rem != 0) {
    env.flags |= kInexact;
    if (tiny) env.flags |= kUnderflow;
  }
  env.rounded_up = increment;
  bool carried = false;
  if (increment) {
    ++keep;
    carried = precision < 64 ? keep == (uint64_t{1} << precision) : keep == 0;      // the significand overflowed: 1.000... x 2^1
  }
  const uint64_t mant = carried ? kTop : keep << (64 - precision);
  if (carried) ++be;
  else if (be == 0 && (mant & kTop)) be = 1;      // a denormal that rounded up to the smallest normal number
  if (be >= 0x7FFF) {      // overflow
    env.flags |= kOverflow | kInexact;
    const bool to_infinity = env.mode == Rounding::NearestEven || (env.mode == Rounding::Up && !sign) || (env.mode == Rounding::Down && sign);
    env.rounded_up = to_infinity;      // (an infinity is a result rounded away from zero)
    if (to_infinity) return infinity(sign);
    Ext m;
    m.mant = ~uint64_t{0} << (64 - precision);
    m.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | 0x7FFE);
    return m;
  }
  Ext r;
  r.mant = mant;
  r.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | be);
  return r;
}

// Which NaN comes out of an operation on a NaN: a signaling one becomes quiet; of two NaNs the one with the larger significand wins.
Ext nan_result(const Ext& a, Class ca, const Ext& b, Class cb, Env& env) {
  const bool a_nan = ca == Class::QuietNan || ca == Class::SignalingNan, b_nan = cb == Class::QuietNan || cb == Class::SignalingNan;
  if (ca == Class::SignalingNan || cb == Class::SignalingNan) env.flags |= kInvalid;
  Ext r;
  if (a_nan && b_nan) r = (b.mant > a.mant) ? b : (a.mant > b.mant) ? a : (b.sign() && !a.sign() ? a : (a.sign() && !b.sign() ? b : a));      // the larger significand; a tie goes to the positive one
  else r = a_nan ? a : b;
  r.mant |= 0x4000000000000000ull;
  return r;
}
bool is_nan_class(Class c) { return c == Class::QuietNan || c == Class::SignalingNan; }

Ext canonical(Ext x) {      // a pseudo-denormal (exponent field 0 but the integer bit set) is the smallest normal exponent
  if ((x.se & 0x7FFF) == 0 && (x.mant & kTop)) x.se = static_cast<uint16_t>(x.se | 1);
  return x;
}

void note_denormal(const Ext& a, const Ext& b, Env& env) {
  if (classify(a) == Class::Denormal || classify(b) == Class::Denormal) env.flags |= kDenormal;
}

Ext invalid(Env& env) {
  env.flags |= kInvalid;
  return indefinite();
}

// Shared by add and subtract.
Ext add_signed(const Ext& a, const Ext& b, bool negate_b, uint32_t precision, Env& env) {
  const Class ca = classify(a), cb = classify(b);
  if (ca == Class::Unsupported || cb == Class::Unsupported) return invalid(env);
  if (is_nan_class(ca) || is_nan_class(cb)) return nan_result(a, ca, b, cb, env);
  const bool sb = b.sign() != negate_b;
  if (ca == Class::Infinity || cb == Class::Infinity) {
    if (ca == Class::Infinity && cb == Class::Infinity && a.sign() != sb) return invalid(env);
    note_denormal(a, b, env);
    return ca == Class::Infinity ? infinity(a.sign()) : infinity(sb);
  }
  const bool a_zero = ca == Class::Zero, b_zero = cb == Class::Zero;
  if (a_zero && b_zero) return zero(a.sign() == sb ? sb : env.mode == Rounding::Down);
  Num x, y;
  if (a_zero) {
    y = finite_of(b, env);
    y.sign = sb;
    return round_pack(y.sign, y.exp, static_cast<u128>(y.mant) << 64, false, precision, env);
  }
  x = finite_of(a, env);
  if (b_zero) return round_pack(x.sign, x.exp, static_cast<u128>(x.mant) << 64, false, precision, env);
  y = finite_of(b, env);
  y.sign = sb;
  if (y.exp > x.exp || (y.exp == x.exp && y.mant > x.mant)) { const Num t = x; x = y; y = t; }
  // Both with the leading bit at 126, so a carry has room.
  u128 wx = static_cast<u128>(x.mant) << 63, wy = static_cast<u128>(y.mant) << 63;
  const uint32_t d = static_cast<uint32_t>(x.exp - y.exp);
  wy = jam128(wy, d);
  u128 w;
  if (x.sign == y.sign) {
    w = wx + wy;
  } else {
    w = wx - wy;
    if (w == 0) return zero(env.mode == Rounding::Down);
  }
  const int lz = clz128(w);
  const int32_t lead = x.exp + (127 - lz - 126);      // exponent of the leading bit
  return round_pack(x.sign, lead, w << lz, false, precision, env);
}

// 128/64 -> quotient and remainder, bit by bit (the quotient may be up to 128 bits).
void divide128(u128 n, uint64_t d, u128* quotient, uint64_t* remainder) {
  u128 q = 0, r = 0;
  for (int i = 127; i >= 0; --i) {
    r = (r << 1) | ((n >> i) & 1);
    q <<= 1;
    if (r >= d) {
      r -= d;
      q |= 1;
    }
  }
  *quotient = q;
  *remainder = static_cast<uint64_t>(r);
}

}  // namespace

Class classify(const Ext& x) {
  const uint32_t e = x.exponent();
  if (e == 0x7FFF) {
    if (x.mant == kTop) return Class::Infinity;
    if (!(x.mant & kTop)) return Class::Unsupported;
    return (x.mant & 0x4000000000000000ull) ? Class::QuietNan : Class::SignalingNan;
  }
  if (e == 0) return x.mant == 0 ? Class::Zero : Class::Denormal;
  return (x.mant & kTop) ? Class::Normal : Class::Unsupported;
}

Ext from_single(uint32_t bits, Env& env) {
  const bool sign = bits >> 31;
  const uint32_t e = (bits >> 23) & 0xFF, m = bits & 0x7FFFFF;
  Ext r;
  if (e == 0xFF) {
    if (m == 0) return infinity(sign);
    if (!(m & 0x400000)) env.flags |= kInvalid;
    r.mant = kTop | 0x4000000000000000ull | (uint64_t{m} << 40);
    r.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | 0x7FFF);
    return r;
  }
  if (e == 0) {
    if (m == 0) return zero(sign);
    env.flags |= kDenormal;
    const int lz = clz64(uint64_t{m});
    r.mant = uint64_t{m} << lz;
    r.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | (kBias - 126 - (lz - 40)));      // the leading bit of m is bit 63 - lz; value = m * 2^-149
    return r;
  }
  r.mant = kTop | (uint64_t{m} << 40);
  r.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | (e - 127 + kBias));
  return r;
}

Ext from_double(uint64_t bits, Env& env) {
  const bool sign = bits >> 63;
  const uint32_t e = static_cast<uint32_t>((bits >> 52) & 0x7FF);
  const uint64_t m = bits & 0xFFFFFFFFFFFFFull;
  Ext r;
  if (e == 0x7FF) {
    if (m == 0) return infinity(sign);
    if (!(m & 0x8000000000000ull)) env.flags |= kInvalid;
    r.mant = kTop | 0x4000000000000000ull | (m << 11);
    r.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | 0x7FFF);
    return r;
  }
  if (e == 0) {
    if (m == 0) return zero(sign);
    env.flags |= kDenormal;
    const int lz = clz64(m);      // m = (bit 63 - lz) ... value = m * 2^-1074; leading bit position p = 63 - lz -> exponent p - 1074
    r.mant = m << lz;
    r.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | (63 - lz - 1074 + kBias));
    return r;
  }
  r.mant = kTop | (m << 11);
  r.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | (static_cast<int32_t>(e) - 1023 + kBias));
  return r;
}

Ext from_integer(int64_t value) {
  if (value == 0) return zero(false);
  const bool sign = value < 0;
  const uint64_t mag = sign ? ~static_cast<uint64_t>(value) + 1 : static_cast<uint64_t>(value);
  const int lz = clz64(mag);
  Ext r;
  r.mant = mag << lz;
  r.se = static_cast<uint16_t>((sign ? 0x8000 : 0) | (63 - lz + kBias));
  return r;
}

namespace {

// Rounds an extended value to a 32- or 64-bit float with `man_bits` fraction bits and exponent bias `bias` (the emax is 2 * bias).
uint64_t pack_float(const Ext& x, uint32_t man_bits, int32_t bias, Env& env) {
  const uint32_t exp_bits = man_bits == 23 ? 8 : 11;
  const uint64_t sign_bit = uint64_t{1} << (exp_bits + man_bits);
  const uint64_t exp_all = (uint64_t{1} << exp_bits) - 1;
  const Class c = classify(x);
  const bool sign = x.sign();
  const uint64_t s = sign ? sign_bit : 0;
  if (c == Class::Zero) return s;
  if (c == Class::Infinity) return s | (exp_all << man_bits);
  if (c == Class::QuietNan || c == Class::SignalingNan) {
    if (c == Class::SignalingNan) env.flags |= kInvalid;
    return s | (exp_all << man_bits) | (uint64_t{1} << (man_bits - 1)) | ((x.mant >> (63 - man_bits)) & ((uint64_t{1} << man_bits) - 1));
  }
  if (c == Class::Unsupported) { env.flags |= kInvalid; return sign_bit | (exp_all << man_bits) | (uint64_t{1} << (man_bits - 1)); }
  Env quiet_denormals;
  const Num n = finite_of(x, quiet_denormals);
  int32_t be = n.exp + bias;      // the exponent field this value would have as a normal number
  if (be >= static_cast<int32_t>(exp_all)) {      // far too big
    env.flags |= kOverflow | kInexact;
    const bool to_infinity = env.mode == Rounding::NearestEven || (env.mode == Rounding::Up && !sign) || (env.mode == Rounding::Down && sign);
    env.rounded_up = to_infinity;
    return s | (to_infinity ? (exp_all << man_bits) : (((exp_all - 1) << man_bits) | ((uint64_t{1} << man_bits) - 1)));
  }
  u128 sig = static_cast<u128>(n.mant) << 64;
  bool tiny = false;
  if (be <= 0) {
    tiny = true;
    sig = jam128(sig, static_cast<uint32_t>(1 - be));
    be = 0;
  }
  const uint32_t drop = 128 - (man_bits + 1);      // bits below the integer bit and the fraction
  const u128 rem = sig & ((static_cast<u128>(1) << drop) - 1), half = static_cast<u128>(1) << (drop - 1);
  uint64_t keep = static_cast<uint64_t>(sig >> drop);      // integer bit + fraction
  bool increment = false;
  switch (env.mode) {
    case Rounding::NearestEven: increment = rem > half || (rem == half && (keep & 1)); break;
    case Rounding::Down: increment = sign && rem != 0; break;
    case Rounding::Up: increment = !sign && rem != 0; break;
    default: break;
  }
  if (rem != 0) {
    env.flags |= kInexact;
    if (tiny) env.flags |= kUnderflow;
  }
  env.rounded_up = increment;
  keep += increment ? 1 : 0;
  // keep has the integer bit at position man_bits for a normal number (or none for a denormal); adding the exponent field minus the implicit bit packs it.
  uint64_t packed;
  if (be == 0) packed = keep;      // (a carry into the integer bit makes the smallest normal number, which is the right encoding)
  else packed = (static_cast<uint64_t>(be - 1) << man_bits) + keep;
  if (be > 0 && (packed >> man_bits) >= exp_all) {
    env.flags |= kOverflow | kInexact;
    const bool to_infinity = env.mode == Rounding::NearestEven || (env.mode == Rounding::Up && !sign) || (env.mode == Rounding::Down && sign);
    env.rounded_up = to_infinity;
    return s | (to_infinity ? (exp_all << man_bits) : (((exp_all - 1) << man_bits) | ((uint64_t{1} << man_bits) - 1)));
  }
  return s | packed;
}

}  // namespace

uint32_t to_single(const Ext& x, Env& env) { return static_cast<uint32_t>(pack_float(x, 23, 127, env)); }
uint64_t to_double(const Ext& x, Env& env) { return pack_float(x, 52, 1023, env); }

bool to_integer(const Ext& x, uint32_t bits, Rounding mode, Env& env, int64_t* out) {
  const Class c = classify(x);
  if (c == Class::Zero) { *out = 0; return true; }
  if (c != Class::Normal && c != Class::Denormal) { env.flags |= kInvalid; return false; }
  Env quiet_denormals;
  const Num n = finite_of(x, quiet_denormals);
  uint64_t mag = 0;
  bool round_bit = false, sticky = false;
  if (n.exp >= 64) {
    env.flags |= kInvalid;
    return false;
  } else if (n.exp == 63) {
    mag = n.mant;
  } else if (n.exp >= 0) {
    const uint32_t shift = static_cast<uint32_t>(63 - n.exp);
    mag = n.mant >> shift;
    const uint64_t rest = n.mant << (64 - shift);
    round_bit = (rest >> 63) != 0;
    sticky = (rest << 1) != 0;
  } else if (n.exp == -1) {
    round_bit = true;
    sticky = (n.mant << 1) != 0;
  } else {
    sticky = true;
  }
  const bool inexact = round_bit || sticky;
  bool inc = false;
  switch (mode) {
    case Rounding::NearestEven: inc = round_bit && (sticky || (mag & 1)); break;
    case Rounding::Down: inc = n.sign && inexact; break;
    case Rounding::Up: inc = !n.sign && inexact; break;
    default: break;
  }
  env.rounded_up = inc;
  if (inc) {
    if (mag == ~uint64_t{0}) { env.flags |= kInvalid; return false; }
    ++mag;
  }
  const uint64_t limit = uint64_t{1} << (bits - 1);
  if (n.sign ? mag > limit : mag >= limit) { env.flags |= kInvalid; return false; }
  if (inexact) env.flags |= kInexact;
  *out = n.sign ? static_cast<int64_t>(~mag + 1) : static_cast<int64_t>(mag);
  return true;
}

Ext add(const Ext& a, const Ext& b, uint32_t precision, Env& env) { return add_signed(a, b, false, precision, env); }
Ext sub(const Ext& a, const Ext& b, uint32_t precision, Env& env) { return add_signed(a, b, true, precision, env); }

Ext mul(const Ext& a, const Ext& b, uint32_t precision, Env& env) {
  const Class ca = classify(a), cb = classify(b);
  if (ca == Class::Unsupported || cb == Class::Unsupported) return invalid(env);
  if (is_nan_class(ca) || is_nan_class(cb)) return nan_result(a, ca, b, cb, env);
  const bool sign = a.sign() != b.sign();
  if (ca == Class::Infinity || cb == Class::Infinity) {
    if (ca == Class::Zero || cb == Class::Zero) return invalid(env);
    note_denormal(a, b, env);
    return infinity(sign);
  }
  if (ca == Class::Zero || cb == Class::Zero) { note_denormal(a, b, env); return zero(sign); }
  const Num x = finite_of(a, env), y = finite_of(b, env);
  uint64_t hi, lo;
  multiply_unsigned(x.mant, y.mant, &hi, &lo);
  const u128 p = make128(hi, lo);
  const int lz = clz128(p);      // 0 or 1
  return round_pack(sign, x.exp + y.exp + (127 - lz - 126), p << lz, false, precision, env);
}

Ext div(const Ext& a, const Ext& b, uint32_t precision, Env& env) {
  const Class ca = classify(a), cb = classify(b);
  if (ca == Class::Unsupported || cb == Class::Unsupported) return invalid(env);
  if (is_nan_class(ca) || is_nan_class(cb)) return nan_result(a, ca, b, cb, env);
  const bool sign = a.sign() != b.sign();
  if (ca == Class::Infinity) {
    if (cb == Class::Infinity) return invalid(env);
    note_denormal(a, b, env);
    return infinity(sign);
  }
  if (cb == Class::Infinity) { note_denormal(a, b, env); return zero(sign); }
  if (cb == Class::Zero) {
    if (ca == Class::Zero) return invalid(env);
    env.flags |= kDivideByZero;
    return infinity(sign);
  }
  if (ca == Class::Zero) { note_denormal(a, b, env); return zero(sign); }
  const Num x = finite_of(a, env), y = finite_of(b, env);
  // x.mant * 2^127 / y.mant, as two 64-bit digits so that each division fits.
  uint64_t q1 = 0, r1 = 0, q2 = 0, r2 = 0;
  divide_unsigned(x.mant >> 1, x.mant << 63, y.mant, &q1, &r1);      // (x.mant * 2^63) / y.mant < 2^64
  divide_unsigned(r1, 0, y.mant, &q2, &r2);
  const u128 q = make128(q1, q2);      // = x/y * 2^127, in (2^126, 2^128)
  const int lz = clz128(q);
  return round_pack(sign, x.exp - y.exp + (127 - lz - 127), q << lz, r2 != 0, precision, env);
}

Ext sqrt(const Ext& a, uint32_t precision, Env& env) {
  const Class c = classify(a);
  if (c == Class::Unsupported) return invalid(env);
  if (is_nan_class(c)) return nan_result(a, c, a, c, env);
  if (c == Class::Zero) return a;
  if (a.sign()) return invalid(env);
  if (c == Class::Infinity) return a;
  const Num x = finite_of(a, env);
  // value = mant * 2^(exp-63). Make exp - 63 even by using mant << 1 when it is odd: value = X * 2^(2k) with X = mant or mant<<1, k = (exp - 63 [- 1]) / 2.
  int32_t e2 = x.exp - 63;
  u128 X = x.mant;
  if (e2 & 1) { X <<= 1; --e2; }
  const int32_t k = e2 / 2;
  // sqrt(X) to 66 bits: the digit-by-digit method on 33 digit pairs of X (a 66-bit number) followed by 33 pairs of zeros.
  u128 root = 0, rem = 0;
  for (int i = 0; i < 66; ++i) {
    const int pair_index = 32 - i;      // pairs 32..0 hold X, the rest are zero
    const uint32_t pair = pair_index >= 0 ? static_cast<uint32_t>((X >> (2 * pair_index)) & 3) : 0;
    rem = (rem << 2) | pair;
    const u128 trial = (root << 2) | 1;
    root <<= 1;
    if (rem >= trial) {
      rem -= trial;
      root |= 1;
    }
  }
  // root = floor(sqrt(X) * 2^33); value = sqrt(X) * 2^k.
  const int lz = clz128(root);
  const int32_t lead = k - 33 + (127 - lz);      // exponent of the leading bit of root * 2^(k-33)
  return round_pack(false, lead, root << lz, rem != 0, precision, env);
}

Ext round_to_integer(const Ext& a, Rounding mode, Env& env) {
  const Class c = classify(a);
  if (c == Class::Unsupported) return invalid(env);
  if (is_nan_class(c)) return nan_result(a, c, a, c, env);
  if (c == Class::Zero || c == Class::Infinity) return a;
  const Num n = finite_of(a, env);
  if (n.exp >= 63) return a;      // already an integer
  uint64_t mag = 0;
  bool round_bit = false, sticky = false;
  if (n.exp >= 0) {
    const uint32_t shift = static_cast<uint32_t>(63 - n.exp);
    mag = n.mant >> shift;
    const uint64_t rest = n.mant << (64 - shift);
    round_bit = (rest >> 63) != 0;
    sticky = (rest << 1) != 0;
  } else if (n.exp == -1) {
    round_bit = true;
    sticky = (n.mant << 1) != 0;
  } else {
    sticky = true;
  }
  bool inc = false;
  switch (mode) {
    case Rounding::NearestEven: inc = round_bit && (sticky || (mag & 1)); break;
    case Rounding::Down: inc = n.sign && (round_bit || sticky); break;
    case Rounding::Up: inc = !n.sign && (round_bit || sticky); break;
    default: break;
  }
  if (round_bit || sticky) env.flags |= kInexact;
  env.rounded_up = inc;
  mag += inc ? 1 : 0;
  if (mag == 0) return zero(n.sign);
  Ext r = from_integer(static_cast<int64_t>(mag));
  r.se = static_cast<uint16_t>(r.se | (n.sign ? 0x8000 : 0));
  return r;
}

Ext scale(const Ext& a, const Ext& b, Env& env) {
  const Class ca = classify(a), cb = classify(b);
  if (ca == Class::Unsupported || cb == Class::Unsupported) return invalid(env);
  if (is_nan_class(ca) || is_nan_class(cb)) return nan_result(a, ca, b, cb, env);
  if (ca == Class::Infinity) {
    if (cb == Class::Infinity && b.sign()) return invalid(env);
    note_denormal(a, b, env);
    return a;
  }
  if (cb == Class::Infinity) {
    if (!b.sign() && ca == Class::Zero) return invalid(env);
    note_denormal(a, b, env);
    return b.sign() ? zero(a.sign()) : infinity(a.sign());
  }
  if (ca == Class::Zero) { note_denormal(a, b, env); return a; }
  int64_t n = 0;
  if (cb != Class::Zero) {
    // trunc(b), saturated: anything beyond +-2^20 overflows or underflows whatever a is
    const Num y = finite_of(b, env);
    if (y.exp >= 20) n = y.sign ? -(1 << 21) : (1 << 21);
    else if (y.exp < 0) n = 0;
    else { n = static_cast<int64_t>(y.mant >> (63 - y.exp)); if (y.sign) n = -n; }
  }
  const Num x = finite_of(a, env);
  return round_pack(x.sign, x.exp + static_cast<int32_t>(n), static_cast<u128>(x.mant) << 64, false, 64, env);
}

Ext remainder(const Ext& a, const Ext& b, bool nearest, bool* complete, uint32_t* quotient_bits, Env& env) {
  *complete = true;
  *quotient_bits = 0;
  const Class ca = classify(a), cb = classify(b);
  if (ca == Class::Unsupported || cb == Class::Unsupported) return invalid(env);
  if (is_nan_class(ca) || is_nan_class(cb)) return nan_result(a, ca, b, cb, env);
  if (ca == Class::Infinity || cb == Class::Zero) return invalid(env);
  note_denormal(a, b, env);
  if (cb == Class::Infinity || ca == Class::Zero) return canonical(a);
  const Num x = finite_of(a, env), y = finite_of(b, env);
  const int32_t d = x.exp - y.exp;
  if (d < 0) {
    // |a| < |b|. FPREM leaves a alone; FPREM1 rounds the quotient to nearest, which is 1 when |a| is above |b| / 2 (a tie goes to the even quotient 0).
    if (nearest && d == -1 && x.mant > y.mant) {
      Env exact;
      Ext diff = sub(absolute(a), absolute(b), 64, exact);      // |a| - |b|, negative: exactly representable
      if (a.sign()) diff = negated(diff);
      *quotient_bits = 1;
      return diff;
    }
    return canonical(a);
  }
  // a / b = (x.mant << d) / y.mant. At most 63 bits of exponent difference are reduced at once; more is left for another round (C2 set).
  uint32_t steps = static_cast<uint32_t>(d);
  if (d >= 64) {
    *complete = false;
    steps = 32 + ((static_cast<uint32_t>(d) - 32) & 31);      // the hardware reduces the exponent difference by a multiple of 32, leaving 32..63 of it for the next round
  }
  u128 q;
  uint64_t r;
  divide128(static_cast<u128>(x.mant) << steps, y.mant, &q, &r);
  bool negative = x.sign;
  u128 magnitude = r;
  if (*complete && nearest) {
    const u128 twice = static_cast<u128>(r) << 1;
    if (twice > y.mant || (twice == y.mant && (q & 1))) {      // the nearest quotient is one more: the remainder is r - |b|, negative
      q += 1;
      negative = !negative;
      magnitude = static_cast<u128>(y.mant) - r;
    }
  }
  if (*complete) *quotient_bits = static_cast<uint32_t>(q & 7);
  if (magnitude == 0) return zero(a.sign());
  // the remainder is magnitude * 2^(y.exp - 63 + (d - steps))
  const int lz = clz128(magnitude);
  return round_pack(negative, y.exp + (d - static_cast<int32_t>(steps)) + (127 - lz - 63), magnitude << lz, false, 64, env);
}

void extract(const Ext& a, Ext* significand, Ext* exponent, Env& env) {
  const Class c = classify(a);
  if (c == Class::Zero) {
    *exponent = infinity(true);
    env.flags |= kDivideByZero;
    *significand = a;
    return;
  }
  if (c == Class::Infinity) { *significand = a; *exponent = infinity(false); return; }
  if (c != Class::Normal && c != Class::Denormal) {
    Env tmp;
    *significand = *exponent = nan_result(a, c, a, c, env);
    (void)tmp;
    return;
  }
  const Num n = finite_of(a, env);
  *exponent = from_integer(n.exp);
  significand->mant = n.mant;
  significand->se = static_cast<uint16_t>((n.sign ? 0x8000 : 0) | kBias);
}

int compare(const Ext& a, const Ext& b, bool ordered_only, Env& env) {
  const Class ca = classify(a), cb = classify(b);
  if (ca == Class::Unsupported || cb == Class::Unsupported) { env.flags |= kInvalid; return 2; }
  if (is_nan_class(ca) || is_nan_class(cb)) {
    if (ordered_only || ca == Class::SignalingNan || cb == Class::SignalingNan) env.flags |= kInvalid;
    return 2;
  }
  if (ca == Class::Denormal || cb == Class::Denormal) env.flags |= kDenormal;
  if (ca == Class::Zero && cb == Class::Zero) return 0;
  const bool sa = a.sign(), sb = b.sign();
  if (sa != sb) return sa ? -1 : 1;
  // same sign: compare magnitudes (exponent field, then significand)
  const uint32_t ea = a.exponent() == 0 && a.mant ? 1 : a.exponent(), eb = b.exponent() == 0 && b.mant ? 1 : b.exponent();
  int mag;
  if (ca == Class::Zero) mag = -1;
  else if (cb == Class::Zero) mag = 1;
  else if (ea != eb) mag = ea < eb ? -1 : 1;
  else mag = a.mant == b.mant ? 0 : (a.mant < b.mant ? -1 : 1);
  return sa ? -mag : mag;
}

}  // namespace ext
}  // namespace softfloat
}  // namespace emulation
