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

#include "Crypto/Crypto.hpp"

namespace crypto {

namespace {

inline uint32_t rol(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const uint8_t kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

const uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- hashes

Hasher::Hasher(HashKind kind) : kind_(kind) { reset(); }

void Hasher::reset() {
  total_ = 0;
  buffered_ = 0;
  switch (kind_) {
    case HashKind::Md5: {
      const uint32_t iv[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
      for (int i = 0; i < 4; ++i) h_[i] = iv[i];
      break;
    }
    case HashKind::Sha1: {
      const uint32_t iv[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
      for (int i = 0; i < 5; ++i) h_[i] = iv[i];
      break;
    }
    case HashKind::Sha224: {
      const uint32_t iv[8] = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4};
      for (int i = 0; i < 8; ++i) h_[i] = iv[i];
      break;
    }
    case HashKind::Sha256: {
      const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
      for (int i = 0; i < 8; ++i) h_[i] = iv[i];
      break;
    }
  }
}

void Hasher::block(const uint8_t* p) {
  if (kind_ == HashKind::Md5) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    for (int i = 0; i < 64; ++i) {
      uint32_t f;
      int g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (b | ~d); g = (7 * i) & 15; }
      const uint32_t t = d;
      d = c;
      c = b;
      b = b + rol(a + f + kMd5K[i] + m[g], kMd5S[i]);
      a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
  } else if (kind_ == HashKind::Sha1) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
      else { f = b ^ c ^ d; k = 0xca62c1d6; }
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
  } else {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    for (int i = 0; i < 8; ++i) v[i] = h_[i];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25);
      const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const uint32_t t1 = v[7] + s1 + ch + kSha256K[i] + w[i];
      const uint32_t s0 = ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22);
      const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const uint32_t t2 = s0 + maj;
      v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) h_[i] += v[i];
  }
}

void Hasher::update(ConstByteView data) {
  total_ += data.size();
  uint64_t i = 0;
  while (i < data.size()) {
    if (buffered_ == 0 && data.size() - i >= 64) {
      block(data.data() + i);
      i += 64;
      continue;
    }
    const uint32_t take = static_cast<uint32_t>(data.size() - i < 64 - buffered_ ? data.size() - i : 64 - buffered_);
    for (uint32_t k = 0; k < take; ++k) buffer_[buffered_ + k] = data[i + k];
    buffered_ += take;
    i += take;
    if (buffered_ == 64) {
      block(buffer_);
      buffered_ = 0;
    }
  }
}

void Hasher::finish(uint8_t* out) {
  const uint64_t bits = total_ * 8;
  const bool little = kind_ == HashKind::Md5;
  uint8_t pad[72] = {0x80};
  const uint32_t padding = (buffered_ < 56 ? 56 - buffered_ : 120 - buffered_);
  for (int i = 0; i < 8; ++i) pad[padding + i] = static_cast<uint8_t>(little ? bits >> (8 * i) : bits >> (8 * (7 - i)));
  update(ConstByteView(pad, padding + 8));
  const uint32_t words = hash_size(kind_) / 4;
  for (uint32_t i = 0; i < words; ++i)
    for (int k = 0; k < 4; ++k) out[i * 4 + k] = static_cast<uint8_t>(little ? h_[i] >> (8 * k) : h_[i] >> (8 * (3 - k)));
  reset();
}

void hash(HashKind kind, ConstByteView data, uint8_t* out) {
  Hasher h(kind);
  h.update(data);
  h.finish(out);
}

void hmac(HashKind kind, ConstByteView key, ConstByteView data, uint8_t* out) {
  const uint32_t block = hash_block_size(kind), size = hash_size(kind);
  uint8_t k[64] = {0};
  if (key.size() > block) hash(kind, key, k);
  else
    for (uint64_t i = 0; i < key.size(); ++i) k[i] = key[i];
  uint8_t pad[64];
  for (uint32_t i = 0; i < block; ++i) pad[i] = k[i] ^ 0x36;
  Hasher inner(kind);
  inner.update(ConstByteView(pad, block));
  inner.update(data);
  uint8_t digest[kMaxHashSize];
  inner.finish(digest);
  for (uint32_t i = 0; i < block; ++i) pad[i] = k[i] ^ 0x5c;
  Hasher outer(kind);
  outer.update(ConstByteView(pad, block));
  outer.update(ConstByteView(digest, size));
  outer.finish(out);
  for (uint32_t i = 0; i < block; ++i) k[i] = pad[i] = 0;
}

bool equal_constant_time(const uint8_t* a, const uint8_t* b, uint32_t n) {
  uint8_t diff = 0;
  for (uint32_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

// ---------------------------------------------------------------------------------------------------------------- AES

namespace {

struct AesTables {
  uint8_t sbox[256];
  AesTables() {
    // The S-box: multiplicative inverse in GF(2^8) followed by the affine transformation.
    uint8_t p = 1, q = 1;
    do {
      p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));  // p *= 3
      q = static_cast<uint8_t>(q ^ (q << 1));                              // q /= 3
      q = static_cast<uint8_t>(q ^ (q << 2));
      q = static_cast<uint8_t>(q ^ (q << 4));
      if (q & 0x80) q ^= 0x09;
      const uint8_t x = static_cast<uint8_t>(q ^ rol8(q, 1) ^ rol8(q, 2) ^ rol8(q, 3) ^ rol8(q, 4));
      sbox[p] = static_cast<uint8_t>(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
  }
  static uint8_t rol8(uint8_t v, int n) { return static_cast<uint8_t>((v << n) | (v >> (8 - n))); }
};

const AesTables& aes_tables() {
  static const AesTables t;
  return t;
}

inline uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

}  // namespace

bool Aes::set_key(ConstByteView key) {
  if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;
  const AesTables& t = aes_tables();
  const uint32_t nk = static_cast<uint32_t>(key.size() / 4);
  rounds_ = nk + 6;
  const uint32_t total = 4 * (rounds_ + 1);
  uint8_t rcon = 1;
  for (uint32_t i = 0; i < total; ++i) {
    if (i < nk) {
      round_keys_[i] = (static_cast<uint32_t>(key[4 * i]) << 24) | (key[4 * i + 1] << 16) | (key[4 * i + 2] << 8) | key[4 * i + 3];
      continue;
    }
    uint32_t temp = round_keys_[i - 1];
    if (i % nk == 0) {
      temp = rol(temp, 8);
      temp = (static_cast<uint32_t>(t.sbox[temp >> 24]) << 24) | (t.sbox[(temp >> 16) & 255] << 16) | (t.sbox[(temp >> 8) & 255] << 8) | t.sbox[temp & 255];
      temp ^= static_cast<uint32_t>(rcon) << 24;
      rcon = xtime(rcon);
    } else if (nk > 6 && i % nk == 4) {
      temp = (static_cast<uint32_t>(t.sbox[temp >> 24]) << 24) | (t.sbox[(temp >> 16) & 255] << 16) | (t.sbox[(temp >> 8) & 255] << 8) | t.sbox[temp & 255];
    }
    round_keys_[i] = round_keys_[i - nk] ^ temp;
  }
  return true;
}

void Aes::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
  const AesTables& t = aes_tables();
  uint8_t s[16];
  auto add_round_key = [&](uint32_t round) {
    for (int c = 0; c < 4; ++c) {
      const uint32_t k = round_keys_[round * 4 + c];
      s[c * 4] ^= static_cast<uint8_t>(k >> 24);
      s[c * 4 + 1] ^= static_cast<uint8_t>(k >> 16);
      s[c * 4 + 2] ^= static_cast<uint8_t>(k >> 8);
      s[c * 4 + 3] ^= static_cast<uint8_t>(k);
    }
  };
  for (int i = 0; i < 16; ++i) s[i] = in[i];
  add_round_key(0);
  for (uint32_t round = 1; round <= rounds_; ++round) {
    uint8_t b[16];
    for (int i = 0; i < 16; ++i) b[i] = t.sbox[s[i]];
    // ShiftRows (the state is column-major: s[4 * column + row])
    uint8_t r[16];
    for (int c = 0; c < 4; ++c)
      for (int row = 0; row < 4; ++row) r[c * 4 + row] = b[((c + row) & 3) * 4 + row];
    if (round < rounds_) {
      for (int c = 0; c < 4; ++c) {
        const uint8_t a0 = r[c * 4], a1 = r[c * 4 + 1], a2 = r[c * 4 + 2], a3 = r[c * 4 + 3];
        s[c * 4] = static_cast<uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        s[c * 4 + 1] = static_cast<uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        s[c * 4 + 2] = static_cast<uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        s[c * 4 + 3] = static_cast<uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
      }
    } else {
      for (int i = 0; i < 16; ++i) s[i] = r[i];
    }
    add_round_key(round);
  }
  for (int i = 0; i < 16; ++i) out[i] = s[i];
}

void Aes::cfb128(const uint8_t iv[16], ByteView data, bool decrypt) const {
  uint8_t feedback[16], keystream[16];
  for (int i = 0; i < 16; ++i) feedback[i] = iv[i];
  for (uint64_t pos = 0; pos < data.size(); pos += 16) {
    encrypt_block(feedback, keystream);
    const uint64_t n = data.size() - pos < 16 ? data.size() - pos : 16;
    for (uint64_t i = 0; i < n; ++i) {
      const uint8_t in = data[pos + i];
      data[pos + i] = static_cast<uint8_t>(in ^ keystream[i]);
      feedback[i] = decrypt ? in : data[pos + i];  // the ciphertext feeds the next block
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------- DES

namespace {
#include "Crypto/DesTables.inc"

uint64_t permute(uint64_t in, uint32_t in_bits, const uint8_t* table, uint32_t out_bits) {
  uint64_t out = 0;
  for (uint32_t i = 0; i < out_bits; ++i) out = (out << 1) | ((in >> (in_bits - 1 - table[i])) & 1);
  return out;
}
}  // namespace

void Des::set_key(const uint8_t key[8]) {
  uint64_t k = 0;
  for (int i = 0; i < 8; ++i) k = (k << 8) | key[i];
  const uint64_t cd = permute(k, 64, kPc1, 56);
  uint32_t c = static_cast<uint32_t>(cd >> 28) & 0x0FFFFFFF, d = static_cast<uint32_t>(cd) & 0x0FFFFFFF;
  for (int round = 0; round < 16; ++round) {
    for (int r = 0; r < kRotations[round]; ++r) {
      c = ((c << 1) | (c >> 27)) & 0x0FFFFFFF;
      d = ((d << 1) | (d >> 27)) & 0x0FFFFFFF;
    }
    subkeys_[round] = permute((static_cast<uint64_t>(c) << 28) | d, 56, kPc2, 48);
  }
}

void Des::block(const uint8_t in[8], uint8_t out[8], bool decrypt) const {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | in[i];
  v = permute(v, 64, kIp, 64);
  uint32_t l = static_cast<uint32_t>(v >> 32), r = static_cast<uint32_t>(v);
  for (int round = 0; round < 16; ++round) {
    const uint64_t key = subkeys_[decrypt ? 15 - round : round];
    const uint64_t e = permute(r, 32, kExpansion, 48) ^ key;
    uint32_t s = 0;
    for (int box = 0; box < 8; ++box) {
      const uint32_t six = static_cast<uint32_t>(e >> (42 - 6 * box)) & 0x3F;
      const uint32_t row = ((six >> 4) & 2) | (six & 1), col = (six >> 1) & 15;
      s = (s << 4) | kSBox[box * 64 + row * 16 + col];
    }
    const uint32_t f = static_cast<uint32_t>(permute(s, 32, kP, 32));
    const uint32_t next = l ^ f;
    l = r;
    r = next;
  }
  const uint64_t pre = (static_cast<uint64_t>(r) << 32) | l;  // the halves are swapped before the final permutation
  const uint64_t result = permute(pre, 64, kFp, 64);
  for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(result >> (56 - 8 * i));
}

void Des::cbc(const uint8_t iv[8], ByteView data, bool decrypt) const {
  uint8_t chain[8];
  for (int i = 0; i < 8; ++i) chain[i] = iv[i];
  for (uint64_t pos = 0; pos + 8 <= data.size(); pos += 8) {
    uint8_t* p = data.data() + pos;
    uint8_t in[8], out[8];
    for (int i = 0; i < 8; ++i) in[i] = p[i];
    if (decrypt) {
      block(in, out, true);
      for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(out[i] ^ chain[i]);
        chain[i] = in[i];
      }
    } else {
      for (int i = 0; i < 8; ++i) in[i] = static_cast<uint8_t>(in[i] ^ chain[i]);
      block(in, out, false);
      for (int i = 0; i < 8; ++i) p[i] = chain[i] = out[i];
    }
  }
}

}  // namespace crypto
