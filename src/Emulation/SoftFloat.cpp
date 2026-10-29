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
#include "Emulation/SoftFloat.hpp"
#include "Emulation/Wide.hpp"

namespace emulation {
namespace softfloat {

namespace {

typedef unsigned __int128 u128;      // (only add, subtract, shift and compare are used: no runtime routine is needed)

struct Fmt {
  uint32_t exp_bits, man_bits;
  int32_t bias() const { return (1 << (exp_bits - 1)) - 1; }
  uint32_t exp_max() const { return (1u << exp_bits) - 1; }
  uint64_t man_mask() const { return (uint64_t{1} << man_bits) - 1; }
  uint64_t quiet_bit() const { return uint64_t{1} << (man_bits - 1); }
  uint64_t sign_bit() const { return uint64_t{1} << (exp_bits + man_bits); }
  uint64_t inf(bool sign) const { return (sign ? sign_bit() : 0) | (uint64_t{exp_max()} << man_bits); }
  uint64_t zero(bool sign) const { return sign ? sign_bit() : 0; }
  uint64_t default_nan() const { return sign_bit() | (uint64_t{exp_max()} << man_bits) | quiet_bit(); }
  uint64_t max_finite(bool sign) const { return (sign ? sign_bit() : 0) | (uint64_t{exp_max() - 1} << man_bits) | man_mask(); }
};
constexpr Fmt kF16{5, 10}, kF32{8, 23}, kF64{11, 52}, kBF16{8, 7};
const Fmt& fmt_of(Format f) { return f == Format::F16 ? kF16 : f == Format::BF16 ? kBF16 : f == Format::F32 ? kF32 : kF64; }

enum Class : uint8_t { kZero, kFinite, kInf, kQuietNan, kSignalingNan };

struct Unpacked {
  bool sign = false;
  Class cls = kZero;
  int32_t exp = 0;      // value = sig * 2^(exp - 63), unbiased
  uint64_t sig = 0;     // bit 63 set for a finite number (subnormals are normalized)
};

uint32_t count_leading_zeros(uint64_t v) {      // v != 0
  uint32_t n = 0;
  if (!(v >> 32)) { n += 32; v <<= 32; }
  if (!(v >> 48)) { n += 16; v <<= 16; }
  if (!(v >> 56)) { n += 8; v <<= 8; }
  if (!(v >> 60)) { n += 4; v <<= 4; }
  if (!(v >> 62)) { n += 2; v <<= 2; }
  if (!(v >> 63)) { n += 1; }
  return n;
}

uint64_t shift_right_jam(uint64_t x, uint32_t n) {
  if (n == 0) return x;
  if (n < 64) return (x >> n) | ((x << (64 - n)) != 0 ? 1 : 0);
  return x != 0 ? 1 : 0;
}

// With MXCSR.DAZ a denormal operand is a zero of its sign, also for the operations that hand an operand back (min, max, round, sqrt of a zero).
uint64_t flush_denormal(const Fmt& f, uint64_t bits, const Env& env) {
  if (env.daz && ((bits >> f.man_bits) & f.exp_max()) == 0) return bits & f.sign_bit();
  return bits;
}

Unpacked unpack(const Fmt& f, uint64_t bits, Env& env) {
  Unpacked u;
  u.sign = (bits & f.sign_bit()) != 0;
  const uint32_t be = static_cast<uint32_t>((bits >> f.man_bits) & f.exp_max());
  const uint64_t man = bits & f.man_mask();
  if (be == f.exp_max()) {
    u.cls = man == 0 ? kInf : (man & f.quiet_bit()) ? kQuietNan : kSignalingNan;
    return u;
  }
  if (be == 0) {
    if (man == 0 || env.daz) return u;
    env.flags |= kDenormal;
    const uint32_t p = 63 - count_leading_zeros(man);      // highest set bit
    u.cls = kFinite;
    u.sig = man << (63 - p);
    u.exp = static_cast<int32_t>(p) + 1 - f.bias() - static_cast<int32_t>(f.man_bits);
    return u;
  }
  u.cls = kFinite;
  u.sig = (man | (uint64_t{1} << f.man_bits)) << (63 - f.man_bits);
  u.exp = static_cast<int32_t>(be) - f.bias();
  return u;
}

// value = sig * 2^(exp - 62) with bit 62 of sig set (low bits below the format's precision are the rounding bits; bit 0 is sticky).
uint64_t round_pack(const Fmt& f, bool sign, int32_t exp, uint64_t sig, Env& env) {
  const uint32_t round_bits = 62 - f.man_bits;
  int32_t be = exp + f.bias();
  bool tiny = false;
  if (be <= 0) {
    tiny = true;
    if (env.ftz) {
      env.flags |= kUnderflow | kInexact;
      return sign ? f.sign_bit() : 0;
    }
    if (be == 0 && env.tininess_after) {      // a value that rounds up to the smallest normal number (with an unbounded exponent range) is not tiny
      const uint64_t rem0 = sig & ((uint64_t{1} << round_bits) - 1), half0 = uint64_t{1} << (round_bits - 1), kept0 = sig >> round_bits;
      bool up = false;
      switch (env.mode) {
        case Rounding::NearestEven: up = rem0 > half0 || (rem0 == half0 && (kept0 & 1)); break;
        case Rounding::TieAway: up = rem0 >= half0; break;
        case Rounding::Down: up = sign && rem0; break;
        case Rounding::Up: up = !sign && rem0; break;
        default: break;
      }
      if (up && kept0 + 1 == (uint64_t{1} << (f.man_bits + 1))) tiny = false;
    }
    const uint32_t shift = static_cast<uint32_t>(1 - be);
    sig = shift_right_jam(sig, shift);
    be = 0;
  }
  const uint64_t round_mask = (uint64_t{1} << round_bits) - 1, half = uint64_t{1} << (round_bits - 1);
  const uint64_t remainder = sig & round_mask;
  sig >>= round_bits;
  bool increment = false;
  switch (env.mode) {
    case Rounding::NearestEven: increment = remainder > half || (remainder == half && (sig & 1)); break;
    case Rounding::Down: increment = sign && remainder; break;
    case Rounding::Up: increment = !sign && remainder; break;
    case Rounding::TieAway: increment = remainder >= half; break;
    default: break;
  }
  if (remainder) {
    env.flags |= kInexact;
    if (tiny) env.flags |= kUnderflow;
  }
  sig += increment ? 1 : 0;
  uint64_t magnitude;
  if (be == 0) {
    magnitude = sig;      // subnormal; a carry into the implicit bit makes the smallest normal, which is the right encoding
  } else {
    magnitude = (static_cast<uint64_t>(be - 1) << f.man_bits) + sig;
  }
  if (be > 0 && (magnitude >> f.man_bits) >= f.exp_max()) {      // overflow (before or by rounding)
    env.flags |= kOverflow | kInexact;
    const bool to_infinity = env.mode == Rounding::NearestEven || env.mode == Rounding::TieAway || (env.mode == Rounding::Up && !sign) || (env.mode == Rounding::Down && sign);
    return to_infinity ? f.inf(sign) : f.max_finite(sign);
  }
  return magnitude | (sign ? f.sign_bit() : 0);
}

// The result when an operand is a NaN: the first NaN operand, quieted. Raises invalid for a signaling one.
uint64_t propagate_nan(const Fmt& f, uint64_t a, Class ca, uint64_t b, Class cb, Env& env) {
  if (ca == kSignalingNan || cb == kSignalingNan) env.flags |= kInvalid;
  if (env.arm) {      // a signaling NaN first, then the first quiet one
    if (ca == kSignalingNan) return a | f.quiet_bit();
    if (cb == kSignalingNan) return b | f.quiet_bit();
    return ca == kQuietNan ? a : b;
  }
  if (ca == kQuietNan || ca == kSignalingNan) return a | f.quiet_bit();
  return b | f.quiet_bit();
}
bool is_nan(Class c) { return c == kQuietNan || c == kSignalingNan; }

uint64_t invalid(const Fmt& f, Env& env) {
  env.flags |= kInvalid;
  return env.arm ? f.default_nan() & ~f.sign_bit() : f.default_nan();
}

uint64_t add_sub(const Fmt& f, uint64_t a, uint64_t b, bool subtract, Env& env) {
  const Unpacked ua = unpack(f, a, env), ub = unpack(f, b, env);
  if (is_nan(ua.cls) || is_nan(ub.cls)) return propagate_nan(f, a, ua.cls, b, ub.cls, env);
  const bool sb = ub.sign != subtract;      // the sign of b as an addend
  if (ua.cls == kInf || ub.cls == kInf) {
    if (ua.cls == kInf && ub.cls == kInf && ua.sign != sb) return invalid(f, env);
    return ua.cls == kInf ? f.inf(ua.sign) : f.inf(sb);
  }
  if (ua.cls == kZero && ub.cls == kZero) return f.zero(ua.sign == sb ? ua.sign : env.mode == Rounding::Down);
  // (a sum with a zero is the other operand as it is; with MXCSR.FTZ a denormal one is tiny and flushed)
  auto exact = [&](uint64_t bits) {
    if (env.ftz && ((bits >> f.man_bits) & f.exp_max()) == 0) {
      env.flags |= kUnderflow | kInexact;
      return bits & f.sign_bit();
    }
    return bits;
  };
  if (ua.cls == kZero) return exact((b & ~f.sign_bit()) | (sb ? f.sign_bit() : 0));
  if (ub.cls == kZero) return exact(a);
  Unpacked x = ua, y = ub;
  y.sign = sb;
  if (y.exp > x.exp) { const Unpacked t = x; x = y; y = t; }
  uint64_t sx = x.sig >> 1, sy = y.sig >> 1;
  const uint32_t d = static_cast<uint32_t>(x.exp - y.exp);
  sy = d > 63 ? (sy != 0 ? 1 : 0) : shift_right_jam(sy, d);
  int32_t e = x.exp;
  bool sign = x.sign;
  uint64_t s;
  if (x.sign == y.sign) {
    s = sx + sy;
    if (s >> 63) {
      s = shift_right_jam(s, 1);
      ++e;
    }
  } else {
    if (sx >= sy) {
      s = sx - sy;
    } else {
      s = sy - sx;
      sign = y.sign;
    }
    if (s == 0) return f.zero(env.mode == Rounding::Down);
    const uint32_t lz = count_leading_zeros(s) - 1;
    s <<= lz;
    e -= static_cast<int32_t>(lz);
  }
  return round_pack(f, sign, e, s, env);
}

}  // namespace

uint64_t add(Format fm, uint64_t a, uint64_t b, Env& env) { return add_sub(fmt_of(fm), a, b, false, env); }
uint64_t sub(Format fm, uint64_t a, uint64_t b, Env& env) { return add_sub(fmt_of(fm), a, b, true, env); }

uint64_t mul(Format fm, uint64_t a, uint64_t b, Env& env) {
  const Fmt& f = fmt_of(fm);
  const Unpacked ua = unpack(f, a, env), ub = unpack(f, b, env);
  if (is_nan(ua.cls) || is_nan(ub.cls)) return propagate_nan(f, a, ua.cls, b, ub.cls, env);
  const bool sign = ua.sign != ub.sign;
  if (ua.cls == kInf || ub.cls == kInf) {
    if (ua.cls == kZero || ub.cls == kZero) return invalid(f, env);
    return f.inf(sign);
  }
  if (ua.cls == kZero || ub.cls == kZero) return f.zero(sign);
  uint64_t hi, lo;
  multiply_unsigned(ua.sig, ub.sig, &hi, &lo);
  int32_t e = ua.exp + ub.exp;
  uint64_t sig;
  if (hi >> 63) {
    ++e;
    sig = (hi >> 1) | ((hi & 1) | (lo != 0 ? 1 : 0));
  } else {
    sig = hi | (lo != 0 ? 1 : 0);
  }
  return round_pack(f, sign, e, sig, env);
}

uint64_t div(Format fm, uint64_t a, uint64_t b, Env& env) {
  const Fmt& f = fmt_of(fm);
  const Unpacked ua = unpack(f, a, env), ub = unpack(f, b, env);
  if (is_nan(ua.cls) || is_nan(ub.cls)) return propagate_nan(f, a, ua.cls, b, ub.cls, env);
  const bool sign = ua.sign != ub.sign;
  if (ua.cls == kInf) return ub.cls == kInf ? invalid(f, env) : f.inf(sign);
  if (ub.cls == kInf) return f.zero(sign);
  if (ub.cls == kZero) {
    if (ua.cls == kZero) return invalid(f, env);
    env.flags |= kDivideByZero;
    return f.inf(sign);
  }
  if (ua.cls == kZero) return f.zero(sign);
  uint64_t q = 0, r = 0;
  divide_unsigned(ua.sig >> 2, ua.sig << 62, ub.sig, &q, &r);      // (A * 2^62) / B, A < 2^64 so the high half is below B
  int32_t e = ua.exp - ub.exp;
  uint64_t sig = q | (r != 0 ? 1 : 0);
  if (!((q >> 62) & 1)) {
    sig = (q << 1) | (r != 0 ? 1 : 0);
    --e;
  }
  return round_pack(f, sign, e, sig, env);
}

uint64_t sqrt(Format fm, uint64_t a, Env& env) {
  const Fmt& f = fmt_of(fm);
  a = flush_denormal(f, a, env);
  const Unpacked ua = unpack(f, a, env);
  if (is_nan(ua.cls)) return propagate_nan(f, a, ua.cls, a, ua.cls, env);
  if (ua.cls == kZero) return a;
  if (ua.sign) return invalid(f, env);
  if (ua.cls == kInf) return a;
  uint64_t m = ua.sig >> 1;      // bit 62
  int32_t e = ua.exp;
  if (e & 1) {      // make the exponent even
    m <<= 1;
    --e;
  }
  // R = floor(sqrt(m * 2^62)), bit by bit.
  u128 num = static_cast<u128>(m) << 62, res = 0, bit = static_cast<u128>(1) << 126;
  while (bit > num) bit >>= 2;
  while (bit != 0) {
    if (num >= res + bit) {
      num -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  const uint64_t root = static_cast<uint64_t>(res);
  return round_pack(f, false, e / 2, root | (num != 0 ? 1 : 0), env);
}

uint64_t convert(Format from, Format to, uint64_t a, Env& env) {
  const Fmt& ff = fmt_of(from);
  const Fmt& ft = fmt_of(to);
  const Unpacked u = unpack(ff, a, env);
  switch (u.cls) {
    case kZero: return ft.zero(u.sign);
    case kInf: return ft.inf(u.sign);
    case kQuietNan: case kSignalingNan: {
      if (u.cls == kSignalingNan) env.flags |= kInvalid;
      const uint64_t payload = (a & ff.man_mask());
      const uint64_t man = ff.man_bits < ft.man_bits ? payload << (ft.man_bits - ff.man_bits) : payload >> (ff.man_bits - ft.man_bits);
      return (u.sign ? ft.sign_bit() : 0) | (uint64_t{ft.exp_max()} << ft.man_bits) | man | ft.quiet_bit();
    }
    default: return round_pack(ft, u.sign, u.exp, u.sig >> 1 | (u.sig & 1), env);
  }
}

uint64_t from_int(Format fm, int64_t value, Env& env) {
  const Fmt& f = fmt_of(fm);
  if (value == 0) return f.zero(false);
  const bool sign = value < 0;
  const uint64_t mag = sign ? ~static_cast<uint64_t>(value) + 1 : static_cast<uint64_t>(value);
  const uint32_t lz = count_leading_zeros(mag);
  const uint64_t sig = mag << lz;      // bit 63 set
  return round_pack(f, sign, static_cast<int32_t>(63 - lz), shift_right_jam(sig, 1), env);
}

namespace {

// The integer part of a finite nonzero value times 2^fbits, with the two bits that decide rounding. False if it is certainly too big for 64 bits.
bool split_integer(const Unpacked& u, uint32_t fbits, uint64_t* mag, bool* round_bit, bool* sticky) {
  *mag = 0;
  *round_bit = *sticky = false;
  const int64_t exp = int64_t{u.exp} + fbits;
  if (exp > 63) return false;
  if (exp == 63) {
    *mag = u.sig;
  } else if (exp >= 0) {
    const uint32_t shift = static_cast<uint32_t>(63 - exp);      // 1 .. 63
    *mag = u.sig >> shift;
    const uint64_t rest = u.sig << (64 - shift);
    *round_bit = (rest >> 63) != 0;
    *sticky = (rest << 1) != 0;
  } else if (exp == -1) {
    *round_bit = true;
    *sticky = (u.sig << 1) != 0;
  } else {
    *sticky = true;
  }
  return true;
}

bool round_up_integer(Rounding mode, bool sign, uint64_t mag, bool round_bit, bool sticky) {
  const bool inexact = round_bit || sticky;
  switch (mode) {
    case Rounding::NearestEven: return round_bit && (sticky || (mag & 1));
    case Rounding::TieAway: return round_bit;
    case Rounding::Down: return sign && inexact;
    case Rounding::Up: return !sign && inexact;
    default: return false;
  }
}

}  // namespace

int64_t to_int(Format fm, uint64_t a, uint32_t bits, bool truncate, Env& env) {
  const Fmt& f = fmt_of(fm);
  const int64_t indefinite = bits == 16 ? static_cast<int64_t>(INT16_MIN) : bits == 32 ? static_cast<int64_t>(INT32_MIN) : INT64_MIN;
  const Unpacked u = unpack(f, a, env);
  if (u.cls == kQuietNan || u.cls == kSignalingNan || u.cls == kInf) {
    env.flags |= kInvalid;
    return indefinite;
  }
  if (u.cls == kZero) return 0;
  uint64_t mag;
  bool round_bit, sticky;
  if (!split_integer(u, 0, &mag, &round_bit, &sticky)) { env.flags |= kInvalid; return indefinite; }
  const bool inexact = round_bit || sticky;
  if (!truncate && round_up_integer(env.mode, u.sign, mag, round_bit, sticky)) {
    if (mag == ~uint64_t{0}) { env.flags |= kInvalid; return indefinite; }
    ++mag;
  }
  const uint64_t limit = uint64_t{1} << (bits - 1);      // |most negative|
  if (u.sign ? mag > limit : mag >= limit) {
    env.flags |= kInvalid;
    return indefinite;
  }
  if (inexact) env.flags |= kInexact;
  return u.sign ? static_cast<int64_t>(~mag + 1) : static_cast<int64_t>(mag);
}

uint64_t to_integer_saturating(Format fm, uint64_t a, Rounding mode, uint32_t bits, bool is_unsigned, uint32_t fbits, Env& env) {
  const Fmt& f = fmt_of(fm);
  const Unpacked u = unpack(f, a, env);
  const uint64_t max_unsigned = bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
  const uint64_t max_signed = bits == 64 ? ~uint64_t{0} >> 1 : (uint64_t{1} << (bits - 1)) - 1;
  auto saturate = [&](bool negative) -> uint64_t {
    env.flags |= kInvalid;
    if (is_unsigned) return negative ? 0 : max_unsigned;
    return bits == 64 ? (negative ? ~max_signed : max_signed) : (negative ? ~max_signed & max_unsigned : max_signed);
  };
  if (u.cls == kQuietNan || u.cls == kSignalingNan) {
    env.flags |= kInvalid;
    return 0;
  }
  if (u.cls == kInf) return saturate(u.sign);
  if (u.cls == kZero) return 0;
  uint64_t mag;
  bool round_bit, sticky;
  if (!split_integer(u, fbits, &mag, &round_bit, &sticky)) return saturate(u.sign);
  const bool inexact = round_bit || sticky;
  if (round_up_integer(mode, u.sign, mag, round_bit, sticky)) {
    if (mag == ~uint64_t{0}) return saturate(u.sign);
    ++mag;
  }
  if (is_unsigned) {
    if (u.sign) {
      if (mag != 0) return saturate(true);
      if (inexact) env.flags |= kInexact;
      return 0;
    }
    if (mag > max_unsigned) return saturate(false);
  } else if (u.sign ? mag > max_signed + 1 : mag > max_signed) {
    return saturate(u.sign);
  }
  if (inexact) env.flags |= kInexact;
  const uint64_t value = u.sign ? ~mag + 1 : mag;
  return bits == 64 ? value : value & max_unsigned;
}

uint64_t from_integer(Format fm, uint64_t magnitude, bool negative, uint32_t fbits, Env& env) {
  const Fmt& f = fmt_of(fm);
  if (magnitude == 0) return f.zero(false);
  const uint32_t lz = count_leading_zeros(magnitude);
  const uint64_t sig = magnitude << lz;
  return round_pack(f, negative, static_cast<int32_t>(63 - lz) - static_cast<int32_t>(fbits), shift_right_jam(sig, 1), env);
}

uint64_t round_to_integral(Format fm, uint64_t a, Rounding mode, bool exact, Env& env) {
  const Fmt& f = fmt_of(fm);
  a = flush_denormal(f, a, env);
  const Unpacked u = unpack(f, a, env);
  if (u.cls == kSignalingNan) {
    env.flags |= kInvalid;
    return a | f.quiet_bit();
  }
  if (u.cls != kFinite) return a;      // zero, infinity, quiet NaN
  if (u.exp >= static_cast<int32_t>(f.man_bits)) return a;      // already integral
  uint64_t mag;
  bool round_bit, sticky;
  split_integer(u, 0, &mag, &round_bit, &sticky);
  if (round_up_integer(mode, u.sign, mag, round_bit, sticky)) ++mag;
  if (exact && (round_bit || sticky)) env.flags |= kInexact;
  if (mag == 0) return f.zero(u.sign);
  Env quiet;
  quiet.mode = Rounding::NearestEven;
  uint64_t r = from_integer(fm, mag, u.sign, 0, quiet);      // (mag is small enough to be exact)
  return r;
}

Order compare(Format fm, uint64_t a, uint64_t b, bool signaling, Env& env) {
  const Fmt& f = fmt_of(fm);
  const Unpacked ua = unpack(f, a, env), ub = unpack(f, b, env);
  if (is_nan(ua.cls) || is_nan(ub.cls)) {
    if (signaling || ua.cls == kSignalingNan || ub.cls == kSignalingNan) env.flags |= kInvalid;
    return Order::Unordered;
  }
  if (ua.cls == kZero && ub.cls == kZero) return Order::Equal;
  const uint64_t ma = a & ~f.sign_bit(), mb = b & ~f.sign_bit();
  const bool sa = ua.sign, sb = ub.sign;
  if (sa != sb) return sa ? Order::Less : Order::Greater;
  if (ma == mb) return Order::Equal;
  const bool a_smaller = ma < mb;
  return (a_smaller != sa) ? Order::Less : Order::Greater;
}

uint64_t minimum(Format fm, uint64_t a, uint64_t b, Env& env) {
  const Fmt& f = fmt_of(fm);
  a = flush_denormal(f, a, env);
  b = flush_denormal(f, b, env);
  return compare(fm, a, b, true, env) == Order::Less ? a : b;
}
uint64_t maximum(Format fm, uint64_t a, uint64_t b, Env& env) {
  const Fmt& f = fmt_of(fm);
  a = flush_denormal(f, a, env);
  b = flush_denormal(f, b, env);
  return compare(fm, a, b, true, env) == Order::Greater ? a : b;
}

}  // namespace softfloat
}  // namespace emulation

namespace emulation {
namespace softfloat {

namespace {
int clz128(unsigned __int128 v) {
  const uint64_t hi = static_cast<uint64_t>(v >> 64), lo = static_cast<uint64_t>(v);
  auto clz64 = [](uint64_t x) { int n = 0; while (!(x >> 63)) { x <<= 1; ++n; } return n; };
  return hi ? clz64(hi) : 64 + clz64(lo);
}
unsigned __int128 jam128(unsigned __int128 v, uint32_t n) {
  if (n == 0) return v;
  if (n >= 128) return v != 0 ? 1 : 0;
  return (v >> n) | ((v << (128 - n)) != 0 ? 1 : 0);
}
}  // namespace

bool is_nan(Format fm, uint64_t a) {
  const Fmt& f = fmt_of(fm);
  return ((a >> f.man_bits) & f.exp_max()) == f.exp_max() && (a & f.man_mask()) != 0;
}
uint64_t negate(Format fm, uint64_t a) { return a ^ fmt_of(fm).sign_bit(); }
uint64_t absolute(Format fm, uint64_t a) { return a & ~fmt_of(fm).sign_bit(); }

uint64_t fused_multiply_add(Format fm, uint64_t a, uint64_t b, uint64_t c, Env& env) {
  const Fmt& f = fmt_of(fm);
  const Unpacked ua = unpack(f, a, env), ub = unpack(f, b, env), uc = unpack(f, c, env);
  const bool any_signaling = ua.cls == kSignalingNan || ub.cls == kSignalingNan || uc.cls == kSignalingNan;
  const bool any_nan = is_nan(ua.cls) || is_nan(ub.cls) || is_nan(uc.cls);
  const bool product_invalid = (ua.cls == kInf && ub.cls == kZero) || (ua.cls == kZero && ub.cls == kInf);
  if (any_nan) {
    if (env.arm && product_invalid && uc.cls == kQuietNan && !any_signaling) return invalid(f, env);
    if (any_signaling) env.flags |= kInvalid;
    // AArch64 looks at the addend first, then the multiplicands; a signaling NaN beats a quiet one.
    const uint64_t vals[3] = {c, a, b};
    const Class cls[3] = {uc.cls, ua.cls, ub.cls};
    for (int i = 0; i < 3; ++i)
      if (cls[i] == kSignalingNan) return vals[i] | f.quiet_bit();
    for (int i = 0; i < 3; ++i)
      if (cls[i] == kQuietNan) return vals[i];
  }
  if (product_invalid) return invalid(f, env);
  const bool sp = ua.sign != ub.sign;
  const bool product_inf = ua.cls == kInf || ub.cls == kInf;
  if (product_inf || uc.cls == kInf) {
    if (product_inf && uc.cls == kInf && sp != uc.sign) return invalid(f, env);
    return product_inf ? f.inf(sp) : f.inf(uc.sign);
  }
  const bool product_zero = ua.cls == kZero || ub.cls == kZero;
  if (product_zero) {
    if (uc.cls == kZero) return f.zero(sp == uc.sign ? sp : env.mode == Rounding::Down);
    if (env.ftz && ((c >> f.man_bits) & f.exp_max()) == 0) {      // (an exact denormal sum is tiny too)
      env.flags |= kUnderflow | kInexact;
      return c & f.sign_bit();
    }
    return c;
  }
  unsigned __int128 x;
  int32_t ex;
  {
    uint64_t hi, lo;
    multiply_unsigned(ua.sig, ub.sig, &hi, &lo);
    unsigned __int128 p = (static_cast<unsigned __int128>(hi) << 64) | lo;
    ex = ua.exp + ub.exp;
    if (hi >> 63) {
      p = jam128(p, 1);
      ++ex;
    }
    x = p;      // leading bit at 126; value = x * 2^(ex - 126)
  }
  if (uc.cls == kZero) {
    const uint32_t lz = static_cast<uint32_t>(clz128(x)) - 1;      // 0
    (void)lz;
    return round_pack(f, sp, ex, static_cast<uint64_t>(jam128(x, 64)), env);
  }
  unsigned __int128 y = static_cast<unsigned __int128>(uc.sig) << 63;
  int32_t ey = uc.exp;
  bool sx = sp, sy = uc.sign;
  if (ey > ex) {      // x is the larger exponent
    const unsigned __int128 t = x; x = y; y = t;
    const int32_t te = ex; ex = ey; ey = te;
    const bool ts = sx; sx = sy; sy = ts;
  }
  y = jam128(y, static_cast<uint32_t>(ex - ey));
  unsigned __int128 s;
  bool sign = sx;
  if (sx == sy) {
    s = x + y;
  } else if (x >= y) {
    s = x - y;
  } else {
    s = y - x;
    sign = sy;
  }
  if (s == 0) return f.zero(env.mode == Rounding::Down);
  const int top = 127 - clz128(s);      // position of the leading bit
  const int32_t e = ex + (top - 126);
  const uint64_t sig = top >= 62 ? static_cast<uint64_t>(jam128(s, static_cast<uint32_t>(top - 62))) : static_cast<uint64_t>(s << (62 - top));
  return round_pack(f, sign, e, sig, env);
}

uint64_t arm_minmax(Format fm, uint64_t a, uint64_t b, bool is_max, bool number, Env& env) {
  const Fmt& f = fmt_of(fm);
  const Unpacked ua = unpack(f, a, env), ub = unpack(f, b, env);
  if (is_nan(ua.cls) || is_nan(ub.cls)) {
    if (number && !(ua.cls == kSignalingNan || ub.cls == kSignalingNan)) {      // a quiet NaN loses to a number
      if (ua.cls == kQuietNan && !is_nan(ub.cls)) return b;
      if (ub.cls == kQuietNan && !is_nan(ua.cls)) return a;
    }
    return propagate_nan(f, a, ua.cls, b, ub.cls, env);
  }
  if (ua.cls == kZero && ub.cls == kZero) {
    const bool pick_a = is_max ? !ua.sign : ua.sign;      // max: +0 wins; min: -0 wins
    return pick_a ? a : b;
  }
  const Order o = compare(fm, a, b, false, env);
  if (o == Order::Equal) return a;
  return (is_max ? o == Order::Greater : o == Order::Less) ? a : b;
}

uint64_t flush_denormal(Format fm, uint64_t bits, const Env& env) { return flush_denormal(fmt_of(fm), bits, env); }

}  // namespace softfloat
}  // namespace emulation
