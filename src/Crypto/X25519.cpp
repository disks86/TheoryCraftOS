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

}  // namespace crypto
