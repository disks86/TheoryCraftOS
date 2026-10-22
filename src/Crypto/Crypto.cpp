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

const uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};
const uint64_t kSha512Iv[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};
const uint64_t kSha384Iv[8] = {
    0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
    0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull,
};

inline uint64_t ror64(uint64_t x, uint32_t n) { return (x >> n) | (x << (64 - n)); }

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
    case HashKind::Sha384:
      for (int i = 0; i < 8; ++i) h64_[i] = kSha384Iv[i];
      break;
    case HashKind::Sha512:
      for (int i = 0; i < 8; ++i) h64_[i] = kSha512Iv[i];
      break;
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
  } else if (kind_ == HashKind::Sha384 || kind_ == HashKind::Sha512) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = 0;
      for (int k = 0; k < 8; ++k) w[i] = (w[i] << 8) | p[i * 8 + k];
    }
    for (int i = 16; i < 80; ++i) {
      const uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
      const uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t v[8];
    for (int i = 0; i < 8; ++i) v[i] = h64_[i];
    for (int i = 0; i < 80; ++i) {
      const uint64_t s1 = ror64(v[4], 14) ^ ror64(v[4], 18) ^ ror64(v[4], 41);
      const uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const uint64_t t1 = v[7] + s1 + ch + kSha512K[i] + w[i];
      const uint64_t s0 = ror64(v[0], 28) ^ ror64(v[0], 34) ^ ror64(v[0], 39);
      const uint64_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const uint64_t t2 = s0 + maj;
      v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) h64_[i] += v[i];
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
  const uint32_t size = hash_block_size(kind_);
  uint64_t i = 0;
  while (i < data.size()) {
    if (buffered_ == 0 && data.size() - i >= size) {
      block(data.data() + i);
      i += size;
      continue;
    }
    const uint32_t take = static_cast<uint32_t>(data.size() - i < size - buffered_ ? data.size() - i : size - buffered_);
    for (uint32_t k = 0; k < take; ++k) buffer_[buffered_ + k] = data[i + k];
    buffered_ += take;
    i += take;
    if (buffered_ == size) {
      block(buffer_);
      buffered_ = 0;
    }
  }
}

void Hasher::finish(uint8_t* out) {
  const uint64_t bits = total_ * 8;
  const bool little = kind_ == HashKind::Md5;
  const bool wide = kind_ == HashKind::Sha384 || kind_ == HashKind::Sha512;   // 128-byte blocks, a 128-bit length
  const uint32_t block_size = wide ? 128 : 64, length_size = wide ? 16 : 8;
  uint8_t pad[144] = {0x80};
  const uint32_t padding = (buffered_ < block_size - length_size ? block_size - length_size - buffered_ : 2 * block_size - length_size - buffered_);
  for (uint32_t i = 0; i < length_size; ++i) {
    const uint32_t shift = little ? 8 * i : 8 * (length_size - 1 - i);
    pad[padding + i] = shift < 64 ? static_cast<uint8_t>(bits >> shift) : 0;
  }
  update(ConstByteView(pad, padding + length_size));
  if (wide) {
    for (uint32_t i = 0; i < hash_size(kind_) / 8; ++i)
      for (int k = 0; k < 8; ++k) out[i * 8 + k] = static_cast<uint8_t>(h64_[i] >> (8 * (7 - k)));
  } else {
    const uint32_t words = hash_size(kind_) / 4;
    for (uint32_t i = 0; i < words; ++i)
      for (int k = 0; k < 4; ++k) out[i * 4 + k] = static_cast<uint8_t>(little ? h_[i] >> (8 * k) : h_[i] >> (8 * (3 - k)));
  }
  reset();
}

void hash(HashKind kind, ConstByteView data, uint8_t* out) {
  Hasher h(kind);
  h.update(data);
  h.finish(out);
}

void hmac(HashKind kind, ConstByteView key, ConstByteView data, uint8_t* out) {
  const uint32_t block = hash_block_size(kind), size = hash_size(kind);
  uint8_t k[kMaxHashBlock] = {0};
  if (key.size() > block) hash(kind, key, k);
  else
    for (uint64_t i = 0; i < key.size(); ++i) k[i] = key[i];
  uint8_t pad[kMaxHashBlock];
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

void hkdf_extract(HashKind kind, ConstByteView salt, ConstByteView ikm, uint8_t* prk) {
  uint8_t zeros[kMaxHashSize] = {0};
  hmac(kind, salt.empty() ? ConstByteView(zeros, hash_size(kind)) : salt, ikm, prk);
}

bool hkdf_expand(HashKind kind, ConstByteView prk, ConstByteView info, ByteView out) {
  const uint32_t size = hash_size(kind);
  if (out.size() > 255ull * size) return false;
  uint8_t block[kMaxHashSize];
  uint32_t have = 0;
  uint8_t counter = 1;
  for (uint64_t pos = 0; pos < out.size(); ++counter) {
    // T(n) = HMAC(prk, T(n-1) | info | n)
    uint8_t message[kMaxHashSize + 256 + 1];
    uint32_t n = 0;
    for (uint32_t i = 0; i < have; ++i) message[n++] = block[i];
    if (info.size() > 256) return false;
    for (uint64_t i = 0; i < info.size(); ++i) message[n++] = info[i];
    message[n++] = counter;
    hmac(kind, prk, ConstByteView(message, n), block);
    have = size;
    const uint64_t take = out.size() - pos < size ? out.size() - pos : size;
    for (uint64_t i = 0; i < take; ++i) out[pos + i] = block[i];
    pos += take;
  }
  return true;
}

bool equal_constant_time(const uint8_t* a, const uint8_t* b, uint32_t n) {
  uint8_t diff = 0;
  for (uint32_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

// ---------------------------------------------------------------------------------------------------------------- AES

namespace {

struct AesTables {
  uint8_t sbox[256], inverse[256];
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
    for (int i = 0; i < 256; ++i) inverse[sbox[i]] = static_cast<uint8_t>(i);
  }
  static uint8_t rol8(uint8_t v, int n) { return static_cast<uint8_t>((v << n) | (v >> (8 - n))); }
};

const AesTables& aes_tables() {
  static const AesTables t;
  return t;
}

inline uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

// Multiplication in GF(2^8), for the inverse MixColumns; the multiplier is a constant (9, 11, 13, 14), so the loop does not depend on secret data.
inline uint8_t gmul(uint8_t x, uint8_t y) {
  uint8_t result = 0;
  for (int i = 0; i < 4; ++i) {
    if (y & 1) result ^= x;
    x = xtime(x);
    y >>= 1;
  }
  return result;
}

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

void Aes::decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
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
  add_round_key(rounds_);
  for (uint32_t round = rounds_; round >= 1; --round) {
    // InvShiftRows, then InvSubBytes
    uint8_t b[16];
    for (int c = 0; c < 4; ++c)
      for (int row = 0; row < 4; ++row) b[((c + row) & 3) * 4 + row] = s[c * 4 + row];
    for (int i = 0; i < 16; ++i) s[i] = t.inverse[b[i]];
    add_round_key(round - 1);
    if (round > 1) {   // InvMixColumns (not after the first round key)
      for (int c = 0; c < 4; ++c) {
        const uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
        s[c * 4] = static_cast<uint8_t>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        s[c * 4 + 1] = static_cast<uint8_t>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        s[c * 4 + 2] = static_cast<uint8_t>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        s[c * 4 + 3] = static_cast<uint8_t>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
      }
    }
  }
  for (int i = 0; i < 16; ++i) out[i] = s[i];
}

void Aes::cbc(const uint8_t iv[16], ByteView data, bool decrypt) const {
  uint8_t chain[16], next[16], block[16];
  for (int i = 0; i < 16; ++i) chain[i] = iv[i];
  for (uint64_t pos = 0; pos + 16 <= data.size(); pos += 16) {
    uint8_t* p = data.data() + pos;
    if (decrypt) {
      for (int i = 0; i < 16; ++i) next[i] = p[i];   // (the ciphertext chains into the next block)
      decrypt_block(p, block);
      for (int i = 0; i < 16; ++i) p[i] = static_cast<uint8_t>(block[i] ^ chain[i]);
      for (int i = 0; i < 16; ++i) chain[i] = next[i];
    } else {
      for (int i = 0; i < 16; ++i) block[i] = static_cast<uint8_t>(p[i] ^ chain[i]);
      encrypt_block(block, p);
      for (int i = 0; i < 16; ++i) chain[i] = p[i];
    }
  }
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

void md4(ConstByteView data, uint8_t out[16]) {
  auto rol32 = [](uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); };
  uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
  auto block = [&](const uint8_t* p) {
    uint32_t x[16];
    for (int i = 0; i < 16; ++i) x[i] = uint32_t{p[4 * i]} | (uint32_t{p[4 * i + 1]} << 8) | (uint32_t{p[4 * i + 2]} << 16) | (uint32_t{p[4 * i + 3]} << 24);
    const uint32_t aa = a, bb = b, cc = c, dd = d;
    auto f = [](uint32_t x1, uint32_t y, uint32_t z) { return (x1 & y) | (~x1 & z); };
    auto g = [](uint32_t x1, uint32_t y, uint32_t z) { return (x1 & y) | (x1 & z) | (y & z); };
    auto h = [](uint32_t x1, uint32_t y, uint32_t z) { return x1 ^ y ^ z; };
    static const uint8_t s1[4] = {3, 7, 11, 19}, s2[4] = {3, 5, 9, 13}, s3[4] = {3, 9, 11, 15};
    for (int i = 0; i < 16; ++i) {   // round 1
      uint32_t t = rol32(a + f(b, c, d) + x[i], s1[i % 4]);
      a = d; d = c; c = b; b = t;
    }
    static const uint8_t o2[16] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
    for (int i = 0; i < 16; ++i) {   // round 2
      uint32_t t = rol32(a + g(b, c, d) + x[o2[i]] + 0x5a827999u, s2[i % 4]);
      a = d; d = c; c = b; b = t;
    }
    static const uint8_t o3[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    for (int i = 0; i < 16; ++i) {   // round 3
      uint32_t t = rol32(a + h(b, c, d) + x[o3[i]] + 0x6ed9eba1u, s3[i % 4]);
      a = d; d = c; c = b; b = t;
    }
    a += aa; b += bb; c += cc; d += dd;
  };
  const uint64_t total = data.size();
  uint64_t done = 0;
  for (; done + 64 <= total; done += 64) block(data.data() + done);
  uint8_t tail[128] = {};
  const uint64_t rest = total - done;
  for (uint64_t i = 0; i < rest; ++i) tail[i] = data[done + i];
  tail[rest] = 0x80;
  const uint64_t blocks = rest < 56 ? 1 : 2;
  const uint64_t bits = total * 8;
  for (int i = 0; i < 8; ++i) tail[blocks * 64 - 8 + i] = static_cast<uint8_t>(bits >> (8 * i));
  for (uint64_t k = 0; k < blocks; ++k) block(tail + 64 * k);
  const uint32_t words[4] = {a, b, c, d};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(words[i] >> (8 * j));
}

bool kdf_hmac_sha256(ConstByteView key, ConstByteView label, ConstByteView context, ByteView out) {
  if (out.size() == 0 || out.size() > 32) return false;
  // K(1) = HMAC(key, [1]_32 || label || 0x00 || context || [L]_32), L the output length in bits.
  uint8_t message[256];
  if (label.size() + context.size() + 9 > sizeof message) return false;
  uint32_t n = 0;
  message[n++] = 0; message[n++] = 0; message[n++] = 0; message[n++] = 1;
  for (uint8_t b : label) message[n++] = b;
  message[n++] = 0;
  for (uint8_t b : context) message[n++] = b;
  const uint32_t bits = static_cast<uint32_t>(out.size() * 8);
  message[n++] = static_cast<uint8_t>(bits >> 24); message[n++] = static_cast<uint8_t>(bits >> 16);
  message[n++] = static_cast<uint8_t>(bits >> 8); message[n++] = static_cast<uint8_t>(bits);
  uint8_t mac[32];
  hmac(HashKind::Sha256, key, ConstByteView(message, n), mac);
  for (uint64_t i = 0; i < out.size(); ++i) out[i] = mac[i];
  return true;
}

bool aes_cmac(ConstByteView key, ConstByteView data, uint8_t mac[16]) {
  Aes aes;
  if (key.size() != 16 || !aes.set_key(key)) return false;
  auto shift_left = [](const uint8_t in[16], uint8_t out[16]) {
    uint8_t carry = 0;
    for (int i = 15; i >= 0; --i) {
      out[i] = static_cast<uint8_t>((in[i] << 1) | carry);
      carry = in[i] >> 7;
    }
    return carry;
  };
  uint8_t zero[16] = {}, l[16], k1[16], k2[16];
  aes.encrypt_block(zero, l);
  if (shift_left(l, k1)) k1[15] ^= 0x87;
  if (shift_left(k1, k2)) k2[15] ^= 0x87;
  const uint64_t n = (data.size() + 15) / 16;
  const bool complete = n > 0 && data.size() % 16 == 0;
  uint8_t x[16] = {}, y[16];
  const uint64_t full = n == 0 ? 0 : n - 1;
  for (uint64_t i = 0; i < full; ++i) {
    for (int k = 0; k < 16; ++k) y[k] = x[k] ^ data[i * 16 + k];
    aes.encrypt_block(y, x);
  }
  uint8_t last[16] = {};
  const uint64_t remain = data.size() - full * 16;
  for (uint64_t k = 0; k < remain; ++k) last[k] = data[full * 16 + k];
  if (complete) {
    for (int k = 0; k < 16; ++k) last[k] ^= k1[k];
  } else {
    last[remain] = 0x80;
    for (int k = 0; k < 16; ++k) last[k] ^= k2[k];
  }
  for (int k = 0; k < 16; ++k) y[k] = x[k] ^ last[k];
  aes.encrypt_block(y, mac);
  return true;
}

}  // namespace crypto
