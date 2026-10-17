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

#include "Crypto/Ecc.hpp"
#include "Crypto/BigInt.hpp"

namespace crypto {

namespace {

using Num = MontEc::Num;

struct CurveParams {
  const char* p;
  const char* b;
  const char* gx;
  const char* gy;
  const char* n;
};
const CurveParams kP256 = {
    "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
    "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"};
const CurveParams kP384 = {
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973"};

uint32_t hex_bytes(const char* hex, uint8_t* out) {
  uint32_t n = 0;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (uint32_t i = 0; hex[i] && hex[i + 1]; i += 2) out[n++] = static_cast<uint8_t>(nib(hex[i]) * 16 + nib(hex[i + 1]));
  return n;
}

struct Point {   // Jacobian, Montgomery domain; z = 0 is the point at infinity
  Num x, y, z;
};

struct Curve {
  MontEc f;       // the field
  MontEc order;   // the group order
  Num b;          // curve constant, Montgomery
  Point g;
  uint32_t size = 0;   // bytes per coordinate
  bool ready = false;

  bool init(const CurveParams& params) {
    uint8_t buf[48];
    uint32_t n = hex_bytes(params.p, buf);
    if (!f.init(ConstByteView(buf, n))) return false;
    size = n;
    n = hex_bytes(params.n, buf);
    if (!order.init(ConstByteView(buf, n))) return false;
    Num t;
    n = hex_bytes(params.b, buf);
    f.load(ConstByteView(buf, n), &t);
    f.to_mont(t, &b);
    n = hex_bytes(params.gx, buf);
    f.load(ConstByteView(buf, n), &t);
    f.to_mont(t, &g.x);
    n = hex_bytes(params.gy, buf);
    f.load(ConstByteView(buf, n), &t);
    f.to_mont(t, &g.y);
    g.z = f.one();
    ready = true;
    return true;
  }

  bool is_infinity(const Point& p) const { return p.z.is_zero(); }
  Point infinity() const {
    Point p;
    p.x = f.one();
    p.y = f.one();
    return p;
  }

  void dbl(const Point& p, Point* out) const {
    if (is_infinity(p) || p.y.is_zero()) {
      *out = infinity();
      return;
    }
    Num delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    f.sqr(p.z, &delta);
    f.sqr(p.y, &gamma);
    f.mul(p.x, gamma, &beta);
    f.sub(p.x, delta, &t1);
    f.add(p.x, delta, &t2);
    f.mul(t1, t2, &alpha);
    f.add(alpha, alpha, &t1);
    f.add(t1, alpha, &alpha);   // 3 (x - delta)(x + delta)
    f.sqr(alpha, &x3);
    f.add(beta, beta, &t1);
    f.add(t1, t1, &t1);         // 4 beta
    f.add(t1, t1, &t2);         // 8 beta
    f.sub(x3, t2, &x3);
    f.add(p.y, p.z, &t2);
    f.sqr(t2, &z3);
    f.sub(z3, gamma, &z3);
    f.sub(z3, delta, &z3);
    f.sub(t1, x3, &t1);
    f.mul(alpha, t1, &y3);
    f.sqr(gamma, &t2);
    f.add(t2, t2, &t2);
    f.add(t2, t2, &t2);
    f.add(t2, t2, &t2);         // 8 gamma^2
    f.sub(y3, t2, &y3);
    out->x = x3;
    out->y = y3;
    out->z = z3;
  }

  void add(const Point& p, const Point& q, Point* out) const {
    if (is_infinity(p)) { *out = q; return; }
    if (is_infinity(q)) { *out = p; return; }
    Num z1z1, z2z2, u1, u2, s1, s2, h, r, t;
    f.sqr(p.z, &z1z1);
    f.sqr(q.z, &z2z2);
    f.mul(p.x, z2z2, &u1);
    f.mul(q.x, z1z1, &u2);
    f.mul(q.z, z2z2, &t);
    f.mul(p.y, t, &s1);
    f.mul(p.z, z1z1, &t);
    f.mul(q.y, t, &s2);
    f.sub(u2, u1, &h);
    f.sub(s2, s1, &r);
    if (h.is_zero()) {
      if (r.is_zero()) dbl(p, out);
      else *out = infinity();
      return;
    }
    Num h2, h3, v, x3, y3, z3;
    f.sqr(h, &h2);
    f.mul(h, h2, &h3);
    f.mul(u1, h2, &v);
    f.sqr(r, &x3);
    f.sub(x3, h3, &x3);
    f.sub(x3, v, &x3);
    f.sub(x3, v, &x3);
    f.sub(v, x3, &t);
    f.mul(r, t, &y3);
    f.mul(s1, h3, &t);
    f.sub(y3, t, &y3);
    f.mul(p.z, q.z, &t);
    f.mul(h, t, &z3);
    out->x = x3;
    out->y = y3;
    out->z = z3;
  }

  // Affine coordinates in the plain domain; false for infinity.
  bool to_affine(const Point& p, Num* x, Num* y) const {
    if (is_infinity(p)) return false;
    Num zi, zi2, zi3, ax, ay;
    f.invert_prime(p.z, &zi);
    f.sqr(zi, &zi2);
    f.mul(zi2, zi, &zi3);
    f.mul(p.x, zi2, &ax);
    f.mul(p.y, zi3, &ay);
    f.from_mont(ax, x);
    f.from_mont(ay, y);
    return true;
  }

  bool on_curve(const Num& x, const Num& y) const {   // Montgomery domain, affine
    Num lhs, rhs, t;
    f.sqr(y, &lhs);
    f.sqr(x, &t);
    f.mul(t, x, &rhs);
    f.sub(rhs, x, &rhs);
    f.sub(rhs, x, &rhs);
    f.sub(rhs, x, &rhs);   // x^3 - 3x
    f.add(rhs, b, &rhs);
    return lhs == rhs;
  }

  bool parse_point(ConstByteView bytes, Point* out) const {
    if (bytes.size() != 1 + 2 * size || bytes[0] != 4) return false;
    Num x, y;
    if (!f.load(bytes.subview(1, size), &x) || !f.load(bytes.subview(1 + size, size), &y)) return false;
    if (!f.below_modulus(x) || !f.below_modulus(y)) return false;
    f.to_mont(x, &out->x);
    f.to_mont(y, &out->y);
    out->z = f.one();
    return on_curve(out->x, out->y);
  }

  bool write_point(const Point& p, uint8_t* out) const {
    Num x, y;
    if (!to_affine(p, &x, &y)) return false;
    out[0] = 4;
    return f.store(x, ByteView(out + 1, size)) && f.store(y, ByteView(out + 1 + size, size));
  }

  // k * p for a public scalar (big-endian bytes): plain double and add.
  void mul_public(const Point& p, ConstByteView k, Point* out) const {
    Point acc = infinity();
    for (uint64_t i = 0; i < k.size(); ++i)
      for (int bit = 7; bit >= 0; --bit) {
        dbl(acc, &acc);
        if ((k[i] >> bit) & 1) add(acc, p, &acc);
      }
    *out = acc;
  }

  static void cswap(Point* a, Point* b, uint32_t swap) {
    const uint32_t mask = 0 - swap;
    for (uint32_t i = 0; i < 12; ++i) {
      uint32_t t = mask & (a->x.w[i] ^ b->x.w[i]);
      a->x.w[i] ^= t; b->x.w[i] ^= t;
      t = mask & (a->y.w[i] ^ b->y.w[i]);
      a->y.w[i] ^= t; b->y.w[i] ^= t;
      t = mask & (a->z.w[i] ^ b->z.w[i]);
      a->z.w[i] ^= t; b->z.w[i] ^= t;
    }
  }

  // k * p for a secret scalar in [1, n-1]: the scalar is blinded to d + n or d + 2n (always bit_length(n) + 1 bits) and run through a ladder.
  void mul_secret(const Point& p, const Num& d, Point* out) const {
    const uint32_t len = order.limbs(), nbits = order.bits();
    uint32_t t1[13] = {}, t2[13] = {};
    uint64_t carry = 0;
    for (uint32_t i = 0; i < 13; ++i) {
      const uint64_t s = static_cast<uint64_t>(i < len ? d.w[i] : 0) + (i < len ? order.modulus().w[i] : 0) + carry;
      t1[i] = static_cast<uint32_t>(s);
      carry = s >> 32;
    }
    carry = 0;
    for (uint32_t i = 0; i < 13; ++i) {
      const uint64_t s = static_cast<uint64_t>(t1[i]) + (i < len ? order.modulus().w[i] : 0) + carry;
      t2[i] = static_cast<uint32_t>(s);
      carry = s >> 32;
    }
    const uint32_t top = (t1[nbits >> 5] >> (nbits & 31)) & 1;   // does d + n reach bit nbits?
    const uint32_t mask = 0 - top;
    uint32_t k[13];
    for (uint32_t i = 0; i < 13; ++i) k[i] = (t1[i] & mask) | (t2[i] & ~mask);
    Point r0 = infinity(), r1 = p;
    uint32_t previous = 0;
    for (int32_t i = static_cast<int32_t>(nbits); i >= 0; --i) {
      const uint32_t bit = (k[i >> 5] >> (i & 31)) & 1;
      cswap(&r0, &r1, bit ^ previous);
      previous = bit;
      Point sum, twice;
      add(r0, r1, &sum);
      dbl(r0, &twice);
      r1 = sum;
      r0 = twice;
    }
    cswap(&r0, &r1, previous);
    *out = r0;
  }
};

Curve g_p256, g_p384;

const Curve* curve_for(EcCurve c) {
  Curve& cv = c == EcCurve::P256 ? g_p256 : g_p384;
  if (!cv.ready && !cv.init(c == EcCurve::P256 ? kP256 : kP384)) return nullptr;
  return &cv;
}

}  // namespace

bool ec_generate_private(EcCurve curve, IRandom& random, uint8_t* private_key) {
  const Curve* c = curve_for(curve);
  if (!c) return false;
  const uint32_t size = c->size;
  for (int attempt = 0; attempt < 64; ++attempt) {
    if (!random.fill(ByteView(private_key, size))) return false;
    Num d;
    if (!c->order.load(ConstByteView(private_key, size), &d)) return false;
    if (!d.is_zero() && c->order.below_modulus(d)) return true;
  }
  return false;
}

static bool scalar_from_bytes(const Curve& c, const uint8_t* bytes, Num* d) {
  if (!c.order.load(ConstByteView(bytes, c.size), d)) return false;
  return !d->is_zero() && c.order.below_modulus(*d);
}

bool ec_public_key(EcCurve curve, const uint8_t* private_key, uint8_t* point) {
  const Curve* c = curve_for(curve);
  Num d;
  if (!c || !scalar_from_bytes(*c, private_key, &d)) return false;
  Point r;
  c->mul_secret(c->g, d, &r);
  return c->write_point(r, point);
}

bool ec_point_valid(EcCurve curve, ConstByteView point) {
  const Curve* c = curve_for(curve);
  Point p;
  return c && c->parse_point(point, &p);
}

bool ec_ecdh(EcCurve curve, const uint8_t* private_key, ConstByteView peer_point, uint8_t* shared_x) {
  const Curve* c = curve_for(curve);
  Num d;
  Point peer, r;
  if (!c || !scalar_from_bytes(*c, private_key, &d) || !c->parse_point(peer_point, &peer)) return false;
  c->mul_secret(peer, d, &r);
  Num x, y;
  if (!c->to_affine(r, &x, &y)) return false;
  return c->f.store(x, ByteView(shared_x, c->size));
}

bool ecdsa_verify(EcCurve curve, ConstByteView public_point, const uint8_t* digest, uint32_t digest_length, ConstByteView r_bytes, ConstByteView s_bytes) {
  const Curve* c = curve_for(curve);
  Point q;
  if (!c || !c->parse_point(public_point, &q)) return false;
  Num r, s;
  if (!c->order.load(r_bytes, &r) || !c->order.load(s_bytes, &s)) return false;
  if (r.is_zero() || s.is_zero() || !c->order.below_modulus(r) || !c->order.below_modulus(s)) return false;
  // e: the leftmost bits of the digest, as many as the order has
  const uint32_t nbits = c->order.bits();
  uint32_t use = digest_length;
  if (use * 8 > nbits) use = (nbits + 7) / 8;
  uint8_t e_bytes[64];
  for (uint32_t i = 0; i < use; ++i) e_bytes[i] = digest[i];
  if (digest_length * 8 > nbits) {
    const uint32_t shift = use * 8 - nbits;
    if (shift) {
      for (uint32_t i = use; i-- > 0;) e_bytes[i] = static_cast<uint8_t>((e_bytes[i] >> shift) | (i ? e_bytes[i - 1] << (8 - shift) : 0));
    }
  }
  Num e;
  if (!c->order.load(ConstByteView(e_bytes, use), &e)) return false;
  c->order.reduce_once(&e);
  Num em, sm, rm, w, u1, u2;
  c->order.to_mont(e, &em);
  c->order.to_mont(s, &sm);
  c->order.to_mont(r, &rm);
  c->order.invert_prime(sm, &w);
  c->order.mul(em, w, &u1);
  c->order.mul(rm, w, &u2);
  c->order.from_mont(u1, &u1);
  c->order.from_mont(u2, &u2);
  uint8_t k1[48], k2[48];
  if (!c->order.store(u1, ByteView(k1, c->size)) || !c->order.store(u2, ByteView(k2, c->size))) return false;
  Point a, b, sum;
  c->mul_public(c->g, ConstByteView(k1, c->size), &a);
  c->mul_public(q, ConstByteView(k2, c->size), &b);
  c->add(a, b, &sum);
  Num x, y;
  if (!c->to_affine(sum, &x, &y)) return false;
  // x mod n == r (p > n, so x may exceed n once)
  c->order.reduce_once(&x);
  return x == r;
}

}  // namespace crypto
