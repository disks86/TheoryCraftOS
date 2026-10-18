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

#include "Crypto/Aead.hpp"

namespace crypto {

namespace {

// Field arithmetic modulo 2^255 - 19 in sixteen 16-bit limbs held in int64, after TweetNaCl: simple and constant time, not fast.
typedef int64_t Gf[16];

void car25519(Gf o) {
  for (int i = 0; i < 16; ++i) {
    o[i] += (1ll << 16);
    const int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c << 16;
  }
}

void sel25519(Gf p, Gf q, int64_t b) {
  const int64_t c = ~(b - 1);
  for (int i = 0; i < 16; ++i) {
    const int64_t t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

void pack25519(uint8_t* o, const Gf n) {
  Gf m, t;
  for (int i = 0; i < 16; ++i) t[i] = n[i];
  car25519(t);
  car25519(t);
  car25519(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    const int64_t b = (m[15] >> 16) & 1;
    m[14] &= 0xffff;
    sel25519(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = static_cast<uint8_t>(t[i] & 0xff);
    o[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
  }
}

void unpack25519(Gf o, const uint8_t* n) {
  for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (static_cast<int64_t>(n[2 * i + 1]) << 8);
  o[15] &= 0x7fff;
}

void A(Gf o, const Gf a, const Gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
void Z(Gf o, const Gf a, const Gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
void M(Gf o, const Gf a, const Gf b) {
  int64_t t[31];
  for (int i = 0; i < 31; ++i) t[i] = 0;
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; ++i) o[i] = t[i];
  car25519(o);
  car25519(o);
}
void S(Gf o, const Gf a) { M(o, a, a); }

void inv25519(Gf o, const Gf i) {
  Gf c;
  for (int a = 0; a < 16; ++a) c[a] = i[a];
  for (int a = 253; a >= 0; --a) {
    S(c, c);
    if (a != 2 && a != 4) M(c, c, i);
  }
  for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void scalarmult(uint8_t* q, const uint8_t* n, const uint8_t* p) {
  uint8_t z[32];
  Gf x, a, b, c, d, e, f;
  static const Gf k121665 = {0xdb41, 1};
  for (int i = 0; i < 31; ++i) z[i] = n[i];
  z[31] = static_cast<uint8_t>((n[31] & 127) | 64);
  z[0] &= 248;
  unpack25519(x, p);
  for (int i = 0; i < 16; ++i) {
    b[i] = x[i];
    d[i] = a[i] = c[i] = 0;
  }
  a[0] = d[0] = 1;
  for (int i = 254; i >= 0; --i) {
    const int64_t r = (z[i >> 3] >> (i & 7)) & 1;
    sel25519(a, b, r);
    sel25519(c, d, r);
    A(e, a, c);
    Z(a, a, c);
    A(c, b, d);
    Z(b, b, d);
    S(d, e);
    S(f, a);
    M(a, c, a);
    M(c, b, e);
    A(e, a, c);
    Z(a, a, c);
    S(b, a);
    Z(c, d, f);
    M(a, c, k121665);
    A(a, a, d);
    M(c, c, a);
    M(a, d, f);
    M(d, b, x);
    S(b, e);
    sel25519(a, b, r);
    sel25519(c, d, r);
  }
  inv25519(c, c);
  M(a, a, c);
  pack25519(q, a);
}


// ---- Ed25519 signature verification (RFC 8032), after TweetNaCl's crypto_sign_open.

const Gf gf0 = {0}, gf1 = {1};
const Gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const Gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const Gf kBaseX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const Gf kBaseY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const Gf kSqrtM1 = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
const int64_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

void set_gf(Gf r, const Gf a) { for (int i = 0; i < 16; ++i) r[i] = a[i]; }

int parity(const Gf a) {
  uint8_t d[32];
  pack25519(d, a);
  return d[0] & 1;
}

bool differ(const Gf a, const Gf b) {
  uint8_t c[32], d[32];
  pack25519(c, a);
  pack25519(d, b);
  uint8_t acc = 0;
  for (int i = 0; i < 32; ++i) acc |= c[i] ^ d[i];
  return acc != 0;
}

void pow2523(Gf o, const Gf i) {
  Gf c;
  set_gf(c, i);
  for (int a = 250; a >= 0; --a) {
    S(c, c);
    if (a != 1) M(c, c, i);
  }
  set_gf(o, c);
}

void point_add(Gf p[4], Gf q[4]) {
  Gf a, b, c, d, t, e, f, g, h;
  Z(a, p[1], p[0]);
  Z(t, q[1], q[0]);
  M(a, a, t);
  A(b, p[0], p[1]);
  A(t, q[0], q[1]);
  M(b, b, t);
  M(c, p[3], q[3]);
  M(c, c, kD2);
  M(d, p[2], q[2]);
  A(d, d, d);
  Z(e, b, a);
  Z(f, d, c);
  A(g, d, c);
  A(h, b, a);
  M(p[0], e, f);
  M(p[1], h, g);
  M(p[2], g, f);
  M(p[3], e, h);
}

void point_swap(Gf p[4], Gf q[4], int64_t b) {
  for (int i = 0; i < 4; ++i) sel25519(p[i], q[i], b);
}

void point_pack(uint8_t* r, Gf p[4]) {
  Gf tx, ty, zi;
  inv25519(zi, p[2]);
  M(tx, p[0], zi);
  M(ty, p[1], zi);
  pack25519(r, ty);
  r[31] ^= static_cast<uint8_t>(parity(tx) << 7);
}

void point_mult(Gf p[4], Gf q[4], const uint8_t* s) {
  set_gf(p[0], gf0);
  set_gf(p[1], gf1);
  set_gf(p[2], gf1);
  set_gf(p[3], gf0);
  for (int i = 255; i >= 0; --i) {
    const int64_t b = (s[i / 8] >> (i & 7)) & 1;
    point_swap(p, q, b);
    point_add(q, p);
    point_add(p, p);
    point_swap(p, q, b);
  }
}

// Unpacks a public key into the negated point -A; false if it is not a point on the curve.
bool point_unpack_negated(Gf r[4], const uint8_t p[32]) {
  Gf t, chk, num, den, den2, den4, den6;
  set_gf(r[2], gf1);
  unpack25519(r[1], p);
  S(num, r[1]);
  M(den, num, kD);
  Z(num, num, r[2]);
  A(den, r[2], den);
  S(den2, den);
  S(den4, den2);
  M(den6, den4, den2);
  M(t, den6, num);
  M(t, t, den);
  pow2523(t, t);
  M(t, t, num);
  M(t, t, den);
  M(t, t, den);
  M(r[0], t, den);
  S(chk, r[0]);
  M(chk, chk, den);
  if (differ(chk, num)) M(r[0], r[0], kSqrtM1);
  S(chk, r[0]);
  M(chk, chk, den);
  if (differ(chk, num)) return false;
  if (parity(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
  M(r[3], r[0], r[1]);
  return true;
}

// Reduces a 64-byte little endian number modulo the group order L, to 32 bytes.
void reduce_mod_l(uint8_t* r, int64_t x[64]) {
  int64_t carry;
  int i, j;
  for (i = 63; i >= 32; --i) {
    carry = 0;
    for (j = i - 32; j < i - 12; ++j) {
      x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
      carry = (x[j] + 128) >> 8;
      x[j] -= carry * 256;
    }
    x[j] += carry;
    x[i] = 0;
  }
  carry = 0;
  for (j = 0; j < 32; ++j) {
    x[j] += carry - (x[31] >> 4) * kL[j];
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  for (j = 0; j < 32; ++j) x[j] -= carry * kL[j];
  for (i = 0; i < 32; ++i) {
    x[i + 1] += x[i] >> 8;
    r[i] = static_cast<uint8_t>(x[i] & 255);
  }
}

}  // namespace

void x25519_public_key(const uint8_t private_key[32], uint8_t public_key[32]) {
  uint8_t base[32] = {9};
  scalarmult(public_key, private_key, base);
}

bool x25519_shared_secret(const uint8_t private_key[32], const uint8_t peer_public[32], uint8_t secret[32]) {
  scalarmult(secret, private_key, peer_public);
  uint8_t acc = 0;
  for (int i = 0; i < 32; ++i) acc |= secret[i];
  return acc != 0;
}

bool ed25519_verify(const uint8_t public_key[32], ConstByteView message, const uint8_t signature[64]) {
  Gf p[4], q[4];
  if (!point_unpack_negated(q, public_key)) return false;
  uint8_t digest[64];
  Hasher h(HashKind::Sha512);
  h.update(ConstByteView(signature, 32));
  h.update(ConstByteView(public_key, 32));
  h.update(message);
  h.finish(digest);
  int64_t x[64];
  for (int i = 0; i < 64; ++i) x[i] = digest[i];
  uint8_t k[32];
  reduce_mod_l(k, x);
  point_mult(p, q, k);   // k * (-A)
  Gf base[4], sb[4];
  set_gf(base[0], kBaseX);
  set_gf(base[1], kBaseY);
  set_gf(base[2], gf1);
  M(base[3], kBaseX, kBaseY);
  point_mult(sb, base, signature + 32);   // S * B
  point_add(p, sb);
  uint8_t packed[32];
  point_pack(packed, p);
  return equal_constant_time(packed, signature, 32);
}

}  // namespace crypto
