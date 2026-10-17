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
inline uint32_t rol32(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }
inline uint64_t load_be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
inline void store_be64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}
inline uint32_t load_le32(const uint8_t* p) { return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline void store_le32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// GHASH: multiplication in GF(2^128) in GCM's bit-reflected convention, bit by bit with masks (no data-dependent branches or lookups).
struct Block128 {
  uint64_t hi, lo;
};
Block128 gf_mul(Block128 x, Block128 h) {
  Block128 z = {0, 0}, v = h;
  for (int i = 0; i < 128; ++i) {
    const uint64_t bit = i < 64 ? (x.hi >> (63 - i)) & 1 : (x.lo >> (127 - i)) & 1;
    const uint64_t mask = 0 - bit;
    z.hi ^= v.hi & mask;
    z.lo ^= v.lo & mask;
    const uint64_t carry = 0 - (v.lo & 1);
    v.lo = (v.lo >> 1) | (v.hi << 63);
    v.hi = (v.hi >> 1) ^ (0xe100000000000000ull & carry);
  }
  return z;
}

struct Ghash {
  Block128 h, y = {0, 0};
  void absorb(ConstByteView data) {   // zero-padded to a whole block at the end
    for (uint64_t pos = 0; pos < data.size(); pos += 16) {
      uint8_t block[16] = {0};
      const uint64_t n = data.size() - pos < 16 ? data.size() - pos : 16;
      for (uint64_t i = 0; i < n; ++i) block[i] = data[pos + i];
      y.hi ^= load_be64(block);
      y.lo ^= load_be64(block + 8);
      y = gf_mul(y, h);
    }
  }
};

void chacha_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
  uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (int i = 0; i < 8; ++i) s[4 + i] = load_le32(key + 4 * i);
  s[12] = counter;
  for (int i = 0; i < 3; ++i) s[13 + i] = load_le32(nonce + 4 * i);
  uint32_t x[16];
  for (int i = 0; i < 16; ++i) x[i] = s[i];
  auto qr = [&](int a, int b, int c, int d) {
    x[a] += x[b]; x[d] = rol32(x[d] ^ x[a], 16);
    x[c] += x[d]; x[b] = rol32(x[b] ^ x[c], 12);
    x[a] += x[b]; x[d] = rol32(x[d] ^ x[a], 8);
    x[c] += x[d]; x[b] = rol32(x[b] ^ x[c], 7);
  };
  for (int round = 0; round < 10; ++round) {
    qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
    qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
  }
  for (int i = 0; i < 16; ++i) store_le32(out + 4 * i, x[i] + s[i]);
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------- AES-GCM

bool AesGcm::set_key(ConstByteView key) {
  if (key.size() != 16 && key.size() != 32) return false;
  if (!aes_.set_key(key)) return false;
  const uint8_t zero[16] = {0};
  uint8_t h[16];
  aes_.encrypt_block(zero, h);
  h_hi_ = load_be64(h);
  h_lo_ = load_be64(h + 8);
  return true;
}

void AesGcm::ctr(const uint8_t nonce[12], ByteView data) const {
  uint8_t counter_block[16], keystream[16];
  for (int i = 0; i < 12; ++i) counter_block[i] = nonce[i];
  uint32_t counter = 2;
  for (uint64_t pos = 0; pos < data.size(); pos += 16, ++counter) {
    for (int i = 0; i < 4; ++i) counter_block[12 + i] = static_cast<uint8_t>(counter >> (24 - 8 * i));
    aes_.encrypt_block(counter_block, keystream);
    const uint64_t n = data.size() - pos < 16 ? data.size() - pos : 16;
    for (uint64_t i = 0; i < n; ++i) data[pos + i] = static_cast<uint8_t>(data[pos + i] ^ keystream[i]);
  }
}

void AesGcm::tag_for(const uint8_t nonce[12], ConstByteView aad, ConstByteView cipher, uint8_t tag[16]) const {
  Ghash g;
  g.h = {h_hi_, h_lo_};
  g.absorb(aad);
  g.absorb(cipher);
  uint8_t lengths[16];
  store_be64(lengths, aad.size() * 8);
  store_be64(lengths + 8, cipher.size() * 8);
  g.absorb(ConstByteView(lengths, 16));
  uint8_t j0[16], mask[16], y[16];
  for (int i = 0; i < 12; ++i) j0[i] = nonce[i];
  j0[12] = j0[13] = j0[14] = 0;
  j0[15] = 1;
  aes_.encrypt_block(j0, mask);
  store_be64(y, g.y.hi);
  store_be64(y + 8, g.y.lo);
  for (int i = 0; i < 16; ++i) tag[i] = static_cast<uint8_t>(y[i] ^ mask[i]);
}

void AesGcm::seal(const uint8_t nonce[12], ConstByteView aad, ByteView data, uint8_t tag[16]) const {
  ctr(nonce, data);
  tag_for(nonce, aad, ConstByteView(data), tag);
}

bool AesGcm::open(const uint8_t nonce[12], ConstByteView aad, ByteView data, const uint8_t tag[16]) const {
  uint8_t expected[16];
  tag_for(nonce, aad, ConstByteView(data), expected);
  if (!equal_constant_time(expected, tag, 16)) return false;
  ctr(nonce, data);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- ChaCha20 / Poly1305

void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], ByteView data) {
  uint8_t block[64];
  for (uint64_t pos = 0; pos < data.size(); pos += 64, ++counter) {
    chacha_block(key, counter, nonce, block);
    const uint64_t n = data.size() - pos < 64 ? data.size() - pos : 64;
    for (uint64_t i = 0; i < n; ++i) data[pos + i] = static_cast<uint8_t>(data[pos + i] ^ block[i]);
  }
}

// 26-bit limbs, the way poly1305-donna does it for 32-bit machines.
Poly1305::Poly1305(const uint8_t key[32]) {
  r_[0] = load_le32(key) & 0x3ffffff;
  r_[1] = (load_le32(key + 3) >> 2) & 0x3ffff03;
  r_[2] = (load_le32(key + 6) >> 4) & 0x3ffc0ff;
  r_[3] = (load_le32(key + 9) >> 6) & 0x3f03fff;
  r_[4] = (load_le32(key + 12) >> 8) & 0x00fffff;
  for (int i = 0; i < 5; ++i) h_[i] = 0;
  for (int i = 0; i < 4; ++i) pad_[i] = load_le32(key + 16 + 4 * i);
}

void Poly1305::blocks(const uint8_t* p, uint32_t count, uint32_t hibit) {
  const uint32_t r0 = r_[0], r1 = r_[1], r2 = r_[2], r3 = r_[3], r4 = r_[4];
  const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
  uint32_t h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3], h4 = h_[4];
  for (; count; --count, p += 16) {
    h0 += load_le32(p) & 0x3ffffff;
    h1 += (load_le32(p + 3) >> 2) & 0x3ffffff;
    h2 += (load_le32(p + 6) >> 4) & 0x3ffffff;
    h3 += (load_le32(p + 9) >> 6) & 0x3ffffff;
    h4 += (load_le32(p + 12) >> 8) | hibit;
    const uint64_t d0 = static_cast<uint64_t>(h0) * r0 + static_cast<uint64_t>(h1) * s4 + static_cast<uint64_t>(h2) * s3 + static_cast<uint64_t>(h3) * s2 + static_cast<uint64_t>(h4) * s1;
    uint64_t d1 = static_cast<uint64_t>(h0) * r1 + static_cast<uint64_t>(h1) * r0 + static_cast<uint64_t>(h2) * s4 + static_cast<uint64_t>(h3) * s3 + static_cast<uint64_t>(h4) * s2;
    uint64_t d2 = static_cast<uint64_t>(h0) * r2 + static_cast<uint64_t>(h1) * r1 + static_cast<uint64_t>(h2) * r0 + static_cast<uint64_t>(h3) * s4 + static_cast<uint64_t>(h4) * s3;
    uint64_t d3 = static_cast<uint64_t>(h0) * r3 + static_cast<uint64_t>(h1) * r2 + static_cast<uint64_t>(h2) * r1 + static_cast<uint64_t>(h3) * r0 + static_cast<uint64_t>(h4) * s4;
    uint64_t d4 = static_cast<uint64_t>(h0) * r4 + static_cast<uint64_t>(h1) * r3 + static_cast<uint64_t>(h2) * r2 + static_cast<uint64_t>(h3) * r1 + static_cast<uint64_t>(h4) * r0;
    uint32_t c = static_cast<uint32_t>(d0 >> 26);
    h0 = static_cast<uint32_t>(d0) & 0x3ffffff;
    d1 += c; c = static_cast<uint32_t>(d1 >> 26); h1 = static_cast<uint32_t>(d1) & 0x3ffffff;
    d2 += c; c = static_cast<uint32_t>(d2 >> 26); h2 = static_cast<uint32_t>(d2) & 0x3ffffff;
    d3 += c; c = static_cast<uint32_t>(d3 >> 26); h3 = static_cast<uint32_t>(d3) & 0x3ffffff;
    d4 += c; c = static_cast<uint32_t>(d4 >> 26); h4 = static_cast<uint32_t>(d4) & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;
  }
  h_[0] = h0; h_[1] = h1; h_[2] = h2; h_[3] = h3; h_[4] = h4;
}

void Poly1305::update(ConstByteView data) {
  uint64_t i = 0;
  if (buffered_) {
    while (i < data.size() && buffered_ < 16) buffer_[buffered_++] = data[i++];
    if (buffered_ < 16) return;
    blocks(buffer_, 1, 1u << 24);
    buffered_ = 0;
  }
  const uint64_t whole = (data.size() - i) / 16;
  if (whole) {
    blocks(data.data() + i, static_cast<uint32_t>(whole), 1u << 24);
    i += whole * 16;
  }
  while (i < data.size()) buffer_[buffered_++] = data[i++];
}

void Poly1305::finish(uint8_t tag[16]) {
  if (buffered_) {
    buffer_[buffered_] = 1;   // the marker bit goes inside the block, so no high bit is added
    for (uint32_t i = buffered_ + 1; i < 16; ++i) buffer_[i] = 0;
    blocks(buffer_, 1, 0);
    buffered_ = 0;
  }
  uint32_t h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3], h4 = h_[4];
  uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
  h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
  h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
  h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
  h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
  h1 += c;
  // compute h + -p, and select it if there was no borrow (h >= p)
  uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
  uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
  uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
  uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
  uint32_t g4 = h4 + c - (1u << 26);
  const uint32_t mask = (g4 >> 31) - 1;
  g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
  const uint32_t nmask = ~mask;
  h0 = (h0 & nmask) | g0; h1 = (h1 & nmask) | g1; h2 = (h2 & nmask) | g2; h3 = (h3 & nmask) | g3; h4 = (h4 & nmask) | g4;
  const uint32_t w0 = h0 | (h1 << 26), w1 = (h1 >> 6) | (h2 << 20), w2 = (h2 >> 12) | (h3 << 14), w3 = (h3 >> 18) | (h4 << 8);
  uint64_t f = static_cast<uint64_t>(w0) + pad_[0];
  store_le32(tag, static_cast<uint32_t>(f));
  f = static_cast<uint64_t>(w1) + pad_[1] + (f >> 32);
  store_le32(tag + 4, static_cast<uint32_t>(f));
  f = static_cast<uint64_t>(w2) + pad_[2] + (f >> 32);
  store_le32(tag + 8, static_cast<uint32_t>(f));
  f = static_cast<uint64_t>(w3) + pad_[3] + (f >> 32);
  store_le32(tag + 12, static_cast<uint32_t>(f));
}

void poly1305(const uint8_t key[32], ConstByteView data, uint8_t tag[16]) {
  Poly1305 p(key);
  p.update(data);
  p.finish(tag);
}

namespace {
// RFC 8439 2.8: the tag is Poly1305 over aad | pad | cipher | pad | lengths, with the one-time key from ChaCha20 block 0.
void aead_tag(const uint8_t key[32], const uint8_t nonce[12], ConstByteView aad, ConstByteView cipher, uint8_t tag[16]) {
  uint8_t block[64];
  chacha_block(key, 0, nonce, block);
  Poly1305 poly(block);
  static const uint8_t zeros[16] = {0};
  poly.update(aad);
  poly.update(ConstByteView(zeros, (16 - aad.size() % 16) % 16));
  poly.update(cipher);
  poly.update(ConstByteView(zeros, (16 - cipher.size() % 16) % 16));
  uint8_t lengths[16];
  for (int i = 0; i < 8; ++i) {
    lengths[i] = static_cast<uint8_t>(aad.size() >> (8 * i));
    lengths[8 + i] = static_cast<uint8_t>(cipher.size() >> (8 * i));
  }
  poly.update(ConstByteView(lengths, 16));
  poly.finish(tag);
}
}  // namespace

bool ChaCha20Poly1305::set_key(ConstByteView key) {
  if (key.size() != 32) return false;
  for (int i = 0; i < 32; ++i) key_[i] = key[i];
  return true;
}

void ChaCha20Poly1305::seal(const uint8_t nonce[12], ConstByteView aad, ByteView data, uint8_t tag[16]) const {
  chacha20_xor(key_, 1, nonce, data);
  aead_tag(key_, nonce, aad, ConstByteView(data), tag);
}

bool ChaCha20Poly1305::open(const uint8_t nonce[12], ConstByteView aad, ByteView data, const uint8_t tag[16]) const {
  uint8_t expected[16];
  aead_tag(key_, nonce, aad, ConstByteView(data), expected);
  if (!equal_constant_time(expected, tag, 16)) return false;
  chacha20_xor(key_, 1, nonce, data);
  return true;
}

}  // namespace crypto
