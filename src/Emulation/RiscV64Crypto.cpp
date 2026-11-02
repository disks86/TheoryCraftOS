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
#include "Emulation/RiscV64Crypto.hpp"

// The scalar bit-manipulation and cryptography extensions that are not in B: Zbkb (pack, brev8), Zbkx (xperm), Zknd / Zkne (AES), Zknh (SHA-2), Zksed (SM4) and Zksh (SM3).
namespace emulation {
namespace rvcrypto {

namespace {

constexpr uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
constexpr uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1) p ^= a;
    a = xtime(a);
    b >>= 1;
  }
  return p;
}
constexpr uint8_t rotl8(uint8_t x, int n) { return static_cast<uint8_t>((x << n) | (x >> (8 - n))); }

// The AES S-box from its definition: the multiplicative inverse in GF(2^8), then the affine map.
struct AesTables {
  uint8_t sbox[256], inv[256];
  constexpr AesTables() : sbox(), inv() {
    for (int i = 0; i < 256; ++i) {
      uint8_t inverse = 0;
      if (i != 0)
        for (int j = 1; j < 256; ++j)
          if (gmul(static_cast<uint8_t>(i), static_cast<uint8_t>(j)) == 1) { inverse = static_cast<uint8_t>(j); break; }
      const uint8_t x = inverse;
      const uint8_t s = static_cast<uint8_t>(x ^ rotl8(x, 1) ^ rotl8(x, 2) ^ rotl8(x, 3) ^ rotl8(x, 4) ^ 0x63);
      sbox[i] = s;
      inv[s] = static_cast<uint8_t>(i);
    }
  }
};
constexpr AesTables kAes;

constexpr uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> (n & 31)) | (x << ((32 - n) & 31)); }
constexpr uint32_t rotl32(uint32_t x, uint32_t n) { return (x << (n & 31)) | (x >> ((32 - n) & 31)); }
constexpr uint64_t rotr64(uint64_t x, uint32_t n) { return (x >> (n & 63)) | (x << ((64 - n) & 63)); }
constexpr uint64_t sext32(uint32_t x) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(x))); }

// SM4's S-box (read out of qemu through SM4KS, whose low byte is the S-box output).
constexpr uint8_t kSm4[256] = {
    0xD6, 0x90, 0xE9, 0xFE, 0xCC, 0xE1, 0x3D, 0xB7, 0x16, 0xB6, 0x14, 0xC2, 0x28, 0xFB, 0x2C, 0x05,
    0x2B, 0x67, 0x9A, 0x76, 0x2A, 0xBE, 0x04, 0xC3, 0xAA, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99,
    0x9C, 0x42, 0x50, 0xF4, 0x91, 0xEF, 0x98, 0x7A, 0x33, 0x54, 0x0B, 0x43, 0xED, 0xCF, 0xAC, 0x62,
    0xE4, 0xB3, 0x1C, 0xA9, 0xC9, 0x08, 0xE8, 0x95, 0x80, 0xDF, 0x94, 0xFA, 0x75, 0x8F, 0x3F, 0xA6,
    0x47, 0x07, 0xA7, 0xFC, 0xF3, 0x73, 0x17, 0xBA, 0x83, 0x59, 0x3C, 0x19, 0xE6, 0x85, 0x4F, 0xA8,
    0x68, 0x6B, 0x81, 0xB2, 0x71, 0x64, 0xDA, 0x8B, 0xF8, 0xEB, 0x0F, 0x4B, 0x70, 0x56, 0x9D, 0x35,
    0x1E, 0x24, 0x0E, 0x5E, 0x63, 0x58, 0xD1, 0xA2, 0x25, 0x22, 0x7C, 0x3B, 0x01, 0x21, 0x78, 0x87,
    0xD4, 0x00, 0x46, 0x57, 0x9F, 0xD3, 0x27, 0x52, 0x4C, 0x36, 0x02, 0xE7, 0xA0, 0xC4, 0xC8, 0x9E,
    0xEA, 0xBF, 0x8A, 0xD2, 0x40, 0xC7, 0x38, 0xB5, 0xA3, 0xF7, 0xF2, 0xCE, 0xF9, 0x61, 0x15, 0xA1,
    0xE0, 0xAE, 0x5D, 0xA4, 0x9B, 0x34, 0x1A, 0x55, 0xAD, 0x93, 0x32, 0x30, 0xF5, 0x8C, 0xB1, 0xE3,
    0x1D, 0xF6, 0xE2, 0x2E, 0x82, 0x66, 0xCA, 0x60, 0xC0, 0x29, 0x23, 0xAB, 0x0D, 0x53, 0x4E, 0x6F,
    0xD5, 0xDB, 0x37, 0x45, 0xDE, 0xFD, 0x8E, 0x2F, 0x03, 0xFF, 0x6A, 0x72, 0x6D, 0x6C, 0x5B, 0x51,
    0x8D, 0x1B, 0xAF, 0x92, 0xBB, 0xDD, 0xBC, 0x7F, 0x11, 0xD9, 0x5C, 0x41, 0x1F, 0x10, 0x5A, 0xD8,
    0x0A, 0xC1, 0x31, 0x88, 0xA5, 0xCD, 0x7B, 0xBD, 0x2D, 0x74, 0xD0, 0x12, 0xB8, 0xE5, 0xB4, 0xB0,
    0x89, 0x69, 0x97, 0x4A, 0x0C, 0x96, 0x77, 0x7E, 0x65, 0xB9, 0xF1, 0x09, 0xC5, 0x6E, 0xC6, 0x84,
    0x18, 0xF0, 0x7D, 0xEC, 0x3A, 0xDC, 0x4D, 0x20, 0x79, 0xEE, 0x5F, 0x3E, 0xD7, 0xCB, 0x39, 0x48,
};

uint32_t sm4_input(uint64_t b, uint32_t bs) { return static_cast<uint32_t>(b >> (8 * bs)) & 0xFF; }

}  // namespace

bool scalar_crypto(uint32_t insn, uint64_t a, uint64_t b, uint64_t* out) {
  const uint32_t opcode = insn & 0x7F, funct3 = (insn >> 12) & 7, funct7 = insn >> 25, top = insn >> 20;
  if (opcode == 0x33) {
    switch ((funct7 << 3) | funct3) {
      case (0x04 << 3) | 4: *out = ((b & 0xFFFFFFFFull) << 32) | (a & 0xFFFFFFFFull); return true;      // pack
      case (0x04 << 3) | 7: *out = ((b & 0xFF) << 8) | (a & 0xFF); return true;      // packh
      case (0x14 << 3) | 2: {      // xperm4: a table of 16 nibbles indexed by the nibbles of b
        uint64_t r = 0;
        for (uint32_t i = 0; i < 16; ++i) {
          const uint32_t index = static_cast<uint32_t>(b >> (4 * i)) & 15;
          r |= ((a >> (4 * index)) & 15) << (4 * i);
        }
        *out = r;
        return true;
      }
      case (0x14 << 3) | 4: {      // xperm8: a table of 8 bytes indexed by the bytes of b
        uint64_t r = 0;
        for (uint32_t i = 0; i < 8; ++i) {
          const uint32_t index = static_cast<uint32_t>(b >> (8 * i)) & 0xFF;
          if (index < 8) r |= ((a >> (8 * index)) & 0xFF) << (8 * i);
        }
        *out = r;
        return true;
      }
      case (0x19 << 3) | 0: case (0x1B << 3) | 0: case (0x1D << 3) | 0: case (0x1F << 3) | 0: {      // aes64es, aes64esm, aes64ds, aes64dsm
        uint8_t state[16];
        for (uint32_t i = 0; i < 8; ++i) { state[i] = static_cast<uint8_t>(a >> (8 * i)); state[8 + i] = static_cast<uint8_t>(b >> (8 * i)); }
        const bool decrypt = funct7 >= 0x1D, mix = (funct7 & 2) != 0;
        static const uint8_t kForward[8] = {0, 5, 10, 15, 4, 9, 14, 3}, kInverse[8] = {0, 13, 10, 7, 4, 1, 14, 11};
        uint8_t col[8];
        for (uint32_t i = 0; i < 8; ++i) col[i] = decrypt ? kAes.inv[state[kInverse[i]]] : kAes.sbox[state[kForward[i]]];
        if (mix) {
          for (uint32_t c = 0; c < 2; ++c) {
            const uint8_t s0 = col[4 * c], s1 = col[4 * c + 1], s2 = col[4 * c + 2], s3 = col[4 * c + 3];
            if (!decrypt) {
              col[4 * c] = static_cast<uint8_t>(gmul(s0, 2) ^ gmul(s1, 3) ^ s2 ^ s3);
              col[4 * c + 1] = static_cast<uint8_t>(s0 ^ gmul(s1, 2) ^ gmul(s2, 3) ^ s3);
              col[4 * c + 2] = static_cast<uint8_t>(s0 ^ s1 ^ gmul(s2, 2) ^ gmul(s3, 3));
              col[4 * c + 3] = static_cast<uint8_t>(gmul(s0, 3) ^ s1 ^ s2 ^ gmul(s3, 2));
            } else {
              col[4 * c] = static_cast<uint8_t>(gmul(s0, 14) ^ gmul(s1, 11) ^ gmul(s2, 13) ^ gmul(s3, 9));
              col[4 * c + 1] = static_cast<uint8_t>(gmul(s0, 9) ^ gmul(s1, 14) ^ gmul(s2, 11) ^ gmul(s3, 13));
              col[4 * c + 2] = static_cast<uint8_t>(gmul(s0, 13) ^ gmul(s1, 9) ^ gmul(s2, 14) ^ gmul(s3, 11));
              col[4 * c + 3] = static_cast<uint8_t>(gmul(s0, 11) ^ gmul(s1, 13) ^ gmul(s2, 9) ^ gmul(s3, 14));
            }
          }
        }
        uint64_t r = 0;
        for (uint32_t i = 0; i < 8; ++i) r |= static_cast<uint64_t>(col[i]) << (8 * i);
        *out = r;
        return true;
      }
      case (0x3F << 3) | 0: {      // aes64ks2
        const uint32_t high = static_cast<uint32_t>(a >> 32), low = high ^ static_cast<uint32_t>(b);
        *out = (static_cast<uint64_t>(low ^ static_cast<uint32_t>(b >> 32)) << 32) | low;
        return true;
      }
      default: break;
    }
    // SM4: the byte select is in bits 31:30
    if (funct3 == 0 && ((funct7 & 0x1F) == 0x18 || (funct7 & 0x1F) == 0x1A)) {
      const uint32_t bs = insn >> 30;      // (bits 31:30 select the byte)
      const uint32_t sb = kSm4[sm4_input(b, bs)];
      uint32_t y;
      if ((funct7 & 0x1F) == 0x18) y = sb ^ (sb << 8) ^ (sb << 2) ^ (sb << 18) ^ ((sb & 0x3F) << 26) ^ ((sb & 0xC0) << 10);      // sm4ed
      else y = sb ^ ((sb & 7) << 29) ^ ((sb & 0xFE) << 7) ^ ((sb & 1) << 23) ^ ((sb & 0xF8) << 13);      // sm4ks
      *out = sext32(rotl32(y, 8 * bs) ^ static_cast<uint32_t>(a));
      return true;
    }
    return false;
  }
  if (opcode == 0x3B && funct7 == 0x04 && funct3 == 4) {      // packw
    *out = sext32(static_cast<uint32_t>(((b & 0xFFFF) << 16) | (a & 0xFFFF)));
    return true;
  }
  if (opcode == 0x13 && funct3 == 5 && top == 0x687) {      // brev8: the bits of each byte reversed
    uint64_t r = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      uint8_t byte = static_cast<uint8_t>(a >> (8 * i)), reversed = 0;
      for (uint32_t j = 0; j < 8; ++j) reversed = static_cast<uint8_t>(reversed | (((byte >> j) & 1) << (7 - j)));
      r |= static_cast<uint64_t>(reversed) << (8 * i);
    }
    *out = r;
    return true;
  }
  if (opcode == 0x13 && funct3 == 1) {
    const uint32_t x = static_cast<uint32_t>(a);
    switch (top) {
      case 0x100: *out = sext32(rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22)); return true;      // sha256sum0
      case 0x101: *out = sext32(rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25)); return true;      // sha256sum1
      case 0x102: *out = sext32(rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3)); return true;      // sha256sig0
      case 0x103: *out = sext32(rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10)); return true;      // sha256sig1
      case 0x104: *out = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39); return true;      // sha512sum0
      case 0x105: *out = rotr64(a, 14) ^ rotr64(a, 18) ^ rotr64(a, 41); return true;      // sha512sum1
      case 0x106: *out = rotr64(a, 1) ^ rotr64(a, 8) ^ (a >> 7); return true;      // sha512sig0
      case 0x107: *out = rotr64(a, 19) ^ rotr64(a, 61) ^ (a >> 6); return true;      // sha512sig1
      case 0x108: *out = sext32(x ^ rotl32(x, 9) ^ rotl32(x, 17)); return true;      // sm3p0
      case 0x109: *out = sext32(x ^ rotl32(x, 15) ^ rotl32(x, 23)); return true;      // sm3p1
      case 0x300: {      // aes64im: InvMixColumns of the two columns in rs1
        uint64_t r = 0;
        for (uint32_t c = 0; c < 2; ++c) {
          const uint8_t s0 = static_cast<uint8_t>(a >> (32 * c)), s1 = static_cast<uint8_t>(a >> (32 * c + 8)), s2 = static_cast<uint8_t>(a >> (32 * c + 16)), s3 = static_cast<uint8_t>(a >> (32 * c + 24));
          const uint32_t w = static_cast<uint32_t>(gmul(s0, 14) ^ gmul(s1, 11) ^ gmul(s2, 13) ^ gmul(s3, 9)) | (static_cast<uint32_t>(gmul(s0, 9) ^ gmul(s1, 14) ^ gmul(s2, 11) ^ gmul(s3, 13)) << 8) |
                                (static_cast<uint32_t>(gmul(s0, 13) ^ gmul(s1, 9) ^ gmul(s2, 14) ^ gmul(s3, 11)) << 16) | (static_cast<uint32_t>(gmul(s0, 11) ^ gmul(s1, 13) ^ gmul(s2, 9) ^ gmul(s3, 14)) << 24);
          r |= static_cast<uint64_t>(w) << (32 * c);
        }
        *out = r;
        return true;
      }
      default: break;
    }
    if ((top >> 4) == 0x31 && (top & 15) <= 10) {      // aes64ks1i: the key schedule's round constant step
      static const uint8_t kRcon[11] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36, 0x00};
      const uint32_t rnum = top & 15;
      uint32_t t = static_cast<uint32_t>(a >> 32);
      if (rnum != 10) t = rotr32(t, 8);
      uint32_t sub = 0;
      for (uint32_t i = 0; i < 4; ++i) sub |= static_cast<uint32_t>(kAes.sbox[(t >> (8 * i)) & 0xFF]) << (8 * i);
      sub ^= kRcon[rnum];
      *out = (static_cast<uint64_t>(sub) << 32) | sub;
      return true;
    }
  }
  return false;
}

namespace {

void aes_shift_rows(const uint8_t in[16], bool inverse, uint8_t out[16]) {
  for (uint32_t c = 0; c < 4; ++c)
    for (uint32_t r = 0; r < 4; ++r) out[r + 4 * c] = in[r + 4 * (inverse ? (c + 4 - r) % 4 : (c + r) % 4)];
}

void aes_mix_columns(uint8_t s[16], bool inverse) {
  for (uint32_t c = 0; c < 4; ++c) {
    const uint8_t s0 = s[4 * c], s1 = s[4 * c + 1], s2 = s[4 * c + 2], s3 = s[4 * c + 3];
    if (!inverse) {
      s[4 * c] = static_cast<uint8_t>(gmul(s0, 2) ^ gmul(s1, 3) ^ s2 ^ s3);
      s[4 * c + 1] = static_cast<uint8_t>(s0 ^ gmul(s1, 2) ^ gmul(s2, 3) ^ s3);
      s[4 * c + 2] = static_cast<uint8_t>(s0 ^ s1 ^ gmul(s2, 2) ^ gmul(s3, 3));
      s[4 * c + 3] = static_cast<uint8_t>(gmul(s0, 3) ^ s1 ^ s2 ^ gmul(s3, 2));
    } else {
      s[4 * c] = static_cast<uint8_t>(gmul(s0, 14) ^ gmul(s1, 11) ^ gmul(s2, 13) ^ gmul(s3, 9));
      s[4 * c + 1] = static_cast<uint8_t>(gmul(s0, 9) ^ gmul(s1, 14) ^ gmul(s2, 11) ^ gmul(s3, 13));
      s[4 * c + 2] = static_cast<uint8_t>(gmul(s0, 13) ^ gmul(s1, 9) ^ gmul(s2, 14) ^ gmul(s3, 11));
      s[4 * c + 3] = static_cast<uint8_t>(gmul(s0, 11) ^ gmul(s1, 13) ^ gmul(s2, 9) ^ gmul(s3, 14));
    }
  }
}

uint8_t rcon_for(uint32_t round) {
  uint8_t r = 1;
  for (uint32_t i = 1; i < round; ++i) r = xtime(r);
  return r;
}

}  // namespace

void aes_round(uint8_t state[16], const uint8_t key[16], uint32_t kind) {
  const bool decrypt = kind == 0 || kind == 1, mix = kind == 0 || kind == 2;
  uint8_t t[16];
  aes_shift_rows(state, decrypt, t);
  for (uint32_t i = 0; i < 16; ++i) t[i] = decrypt ? kAes.inv[t[i]] : kAes.sbox[t[i]];
  if (decrypt) {      // decryption adds the round key before InvMixColumns
    for (uint32_t i = 0; i < 16; ++i) t[i] ^= key[i];
    if (mix) aes_mix_columns(t, true);
  } else {
    if (mix) aes_mix_columns(t, false);
    for (uint32_t i = 0; i < 16; ++i) t[i] ^= key[i];
  }
  for (uint32_t i = 0; i < 16; ++i) state[i] = t[i];
}

void aes_key_128(const uint8_t key[16], uint32_t round, uint8_t out[16]) {
  if (round == 0 || round > 10) round ^= 8;
  uint8_t temp[4] = {kAes.sbox[key[13]], kAes.sbox[key[14]], kAes.sbox[key[15]], kAes.sbox[key[12]]};
  temp[0] ^= rcon_for(round);
  for (uint32_t i = 0; i < 4; ++i) out[i] = key[i] ^ temp[i];
  for (uint32_t i = 4; i < 16; ++i) out[i] = key[i] ^ out[i - 4];
}

void aes_key_256(const uint8_t previous[16], const uint8_t last[16], uint32_t round, uint8_t out[16]) {
  if (round == 0 || round > 14) round ^= 8;
  uint8_t temp[4];
  if (round & 1) { for (uint32_t i = 0; i < 4; ++i) temp[i] = kAes.sbox[last[12 + i]]; }
  else {
    temp[0] = static_cast<uint8_t>(kAes.sbox[last[13]] ^ rcon_for(round / 2));
    temp[1] = kAes.sbox[last[14]];
    temp[2] = kAes.sbox[last[15]];
    temp[3] = kAes.sbox[last[12]];
  }
  for (uint32_t i = 0; i < 4; ++i) out[i] = previous[i] ^ temp[i];
  for (uint32_t i = 4; i < 16; ++i) out[i] = previous[i] ^ out[i - 4];
}

namespace {

uint64_t ror_w(uint64_t x, uint32_t n, uint32_t sew) {
  const uint64_t mask = sew == 64 ? ~uint64_t{0} : 0xFFFFFFFFull;
  x &= mask;
  return ((x >> n) | (x << (sew - n))) & mask;
}

}  // namespace

void sha2_message(uint64_t vd[4], const uint64_t vs2[4], const uint64_t vs1[4], uint32_t sew) {
  const uint64_t mask = sew == 64 ? ~uint64_t{0} : 0xFFFFFFFFull;
  uint64_t w[20];
  w[0] = vd[0]; w[1] = vd[1]; w[2] = vd[2]; w[3] = vd[3];
  w[4] = vs2[0]; w[9] = vs2[1]; w[10] = vs2[2]; w[11] = vs2[3];
  w[12] = vs1[0]; w[13] = vs1[1]; w[14] = vs1[2]; w[15] = vs1[3];
  auto s0 = [&](uint64_t x) { return sew == 64 ? ror_w(x, 1, 64) ^ ror_w(x, 8, 64) ^ (x >> 7) : ror_w(x, 7, 32) ^ ror_w(x, 18, 32) ^ ((x & mask) >> 3); };
  auto s1 = [&](uint64_t x) { return sew == 64 ? ror_w(x, 19, 64) ^ ror_w(x, 61, 64) ^ (x >> 6) : ror_w(x, 17, 32) ^ ror_w(x, 19, 32) ^ ((x & mask) >> 10); };
  for (uint32_t t = 16; t < 20; ++t) w[t] = (s1(w[t - 2]) + w[t - 7] + s0(w[t - 15]) + w[t - 16]) & mask;
  for (uint32_t i = 0; i < 4; ++i) vd[i] = w[16 + i];
}

void sha2_rounds(uint64_t vd[4], const uint64_t vs2[4], const uint64_t vs1[4], uint32_t sew, bool high) {
  const uint64_t mask = sew == 64 ? ~uint64_t{0} : 0xFFFFFFFFull;
  uint64_t a = vs2[3], b = vs2[2], e = vs2[1], f = vs2[0], c = vd[3], d = vd[2], g = vd[1], h = vd[0];      // (vs2 holds {a, b, e, f} and vd {c, d, g, h}, most significant element first)
  auto big0 = [&](uint64_t x) { return sew == 64 ? ror_w(x, 28, 64) ^ ror_w(x, 34, 64) ^ ror_w(x, 39, 64) : ror_w(x, 2, 32) ^ ror_w(x, 13, 32) ^ ror_w(x, 22, 32); };
  auto big1 = [&](uint64_t x) { return sew == 64 ? ror_w(x, 14, 64) ^ ror_w(x, 18, 64) ^ ror_w(x, 41, 64) : ror_w(x, 6, 32) ^ ror_w(x, 11, 32) ^ ror_w(x, 25, 32); };
  for (uint32_t r = 0; r < 2; ++r) {
    const uint64_t wk = vs1[(high ? 2 : 0) + r];
    const uint64_t t1 = (h + big1(e) + ((e & f) ^ (~e & g)) + wk) & mask;
    const uint64_t t2 = (big0(a) + ((a & b) ^ (a & c) ^ (b & c))) & mask;
    h = g; g = f; f = e; e = (d + t1) & mask; d = c; c = b; b = a; a = (t1 + t2) & mask;
  }
  vd[3] = a; vd[2] = b; vd[1] = e; vd[0] = f;
}

namespace {

uint32_t sm4_sub(uint32_t x) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < 4; ++i) r |= static_cast<uint32_t>(kSm4[(x >> (8 * i)) & 0xFF]) << (8 * i);
  return r;
}
uint32_t bswap32(uint32_t x) { return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24); }
uint32_t sm3_p0(uint32_t x) { return x ^ rotl32(x, 9) ^ rotl32(x, 17); }
uint32_t sm3_p1(uint32_t x) { return x ^ rotl32(x, 15) ^ rotl32(x, 23); }

}  // namespace

void sm4_keys(const uint32_t previous[4], uint32_t round, uint32_t out[4]) {
  uint32_t k[8] = {previous[0], previous[1], previous[2], previous[3]};
  for (uint32_t j = 0; j < 4; ++j) {
    uint32_t ck = 0;
    for (uint32_t b = 0; b < 4; ++b) ck |= static_cast<uint32_t>(((4 * (round & 7) + j) * 4 + b) * 7 & 0xFF) << (24 - 8 * b);
    const uint32_t t = sm4_sub(k[j + 1] ^ k[j + 2] ^ k[j + 3] ^ ck);
    k[j + 4] = k[j] ^ t ^ rotl32(t, 13) ^ rotl32(t, 23);
  }
  for (uint32_t i = 0; i < 4; ++i) out[i] = k[4 + i];
}

void sm4_rounds(const uint32_t state[4], const uint32_t keys[4], uint32_t out[4]) {
  uint32_t x[8] = {state[0], state[1], state[2], state[3]};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t t = sm4_sub(x[i + 1] ^ x[i + 2] ^ x[i + 3] ^ keys[i]);
    x[i + 4] = x[i] ^ t ^ rotl32(t, 2) ^ rotl32(t, 10) ^ rotl32(t, 18) ^ rotl32(t, 24);
  }
  for (uint32_t i = 0; i < 4; ++i) out[i] = x[4 + i];
}

void sm3_expand(const uint32_t low[8], const uint32_t high[8], uint32_t out[8]) {
  uint32_t w[24];
  for (uint32_t i = 0; i < 8; ++i) { w[i] = bswap32(low[i]); w[8 + i] = bswap32(high[i]); }
  for (uint32_t j = 16; j < 24; ++j) w[j] = sm3_p1(w[j - 16] ^ w[j - 9] ^ rotl32(w[j - 3], 15)) ^ rotl32(w[j - 13], 7) ^ w[j - 6];
  for (uint32_t i = 0; i < 8; ++i) out[i] = bswap32(w[16 + i]);
}

void sm3_compress(const uint32_t state[8], const uint32_t message[8], uint32_t round, uint32_t out[8]) {
  uint32_t s[8], m[8];
  for (uint32_t i = 0; i < 8; ++i) { s[i] = bswap32(state[i]); m[i] = bswap32(message[i]); }
  for (uint32_t r = 0; r < 2; ++r) {
    const uint32_t j = 2 * (round & 31) + r;
    const uint32_t w = m[r], wp = m[r] ^ m[4 + r];
    const uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    const uint32_t t = j < 16 ? 0x79CC4519u : 0x7A879D8Au;
    const uint32_t ss1 = rotl32(rotl32(a, 12) + e + rotl32(t, j % 32), 7), ss2 = ss1 ^ rotl32(a, 12);
    const uint32_t ff = j < 16 ? a ^ b ^ c : (a & b) | (a & c) | (b & c), gg = j < 16 ? e ^ f ^ g : (e & f) | (~e & g);
    const uint32_t tt1 = ff + d + ss2 + wp, tt2 = gg + h + ss1 + w;
    s[0] = tt1; s[1] = a; s[2] = rotl32(b, 9); s[3] = c; s[4] = sm3_p0(tt2); s[5] = e; s[6] = rotl32(f, 19); s[7] = g;
  }
  for (uint32_t i = 0; i < 8; ++i) out[i] = bswap32(s[i]);
}

void ghash_multiply(const uint8_t x[16], const uint8_t h[16], uint8_t out[16]) {
  uint64_t vh = 0, vl = 0, zh = 0, zl = 0;
  for (uint32_t i = 0; i < 8; ++i) { vh = (vh << 8) | h[i]; vl = (vl << 8) | h[8 + i]; }
  for (uint32_t i = 0; i < 128; ++i) {
    if ((x[i / 8] >> (7 - i % 8)) & 1) { zh ^= vh; zl ^= vl; }
    const bool lsb = vl & 1;
    vl = (vl >> 1) | (vh << 63);
    vh >>= 1;
    if (lsb) vh ^= 0xE100000000000000ull;
  }
  for (uint32_t i = 0; i < 8; ++i) { out[i] = static_cast<uint8_t>(zh >> (56 - 8 * i)); out[8 + i] = static_cast<uint8_t>(zl >> (56 - 8 * i)); }
}

}  // namespace rvcrypto
}  // namespace emulation
