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
#include "Emulation/X86_64Cpu.hpp"
#include "Emulation/X86_64EvexInternal.hpp"

namespace emulation {

namespace {

// --- AES -----------------------------------------------------------------------------------------------------------------------------------------

constexpr uint8_t gf_multiply(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1) r ^= a;
    const bool carry = a & 0x80;
    a = static_cast<uint8_t>(a << 1);
    if (carry) a ^= 0x1B;
    b >>= 1;
  }
  return r;
}

struct SBoxes {
  uint8_t forward[256] = {};
  uint8_t inverse[256] = {};
  constexpr SBoxes() {
    uint8_t p = 1, q = 1;
    do {      // p walks the multiplicative group by 3, q is its inverse (the usual generator loop)
      p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
      q ^= static_cast<uint8_t>(q << 1);
      q ^= static_cast<uint8_t>(q << 2);
      q ^= static_cast<uint8_t>(q << 4);
      if (q & 0x80) q ^= 0x09;
      const uint8_t rotated = static_cast<uint8_t>(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
      forward[p] = static_cast<uint8_t>(rotated ^ 0x63);
    } while (p != 1);
    forward[0] = 0x63;
    for (int i = 0; i < 256; ++i) inverse[forward[i]] = static_cast<uint8_t>(i);
  }
};
constexpr SBoxes kSBox;

using Block = uint8_t[16];

void to_block(uint64_t lo, uint64_t hi, Block out) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>(lo >> (8 * i));
    out[8 + i] = static_cast<uint8_t>(hi >> (8 * i));
  }
}

void from_block(const Block in, uint64_t* lo, uint64_t* hi) {
  *lo = *hi = 0;
  for (int i = 0; i < 8; ++i) {
    *lo |= uint64_t{in[i]} << (8 * i);
    *hi |= uint64_t{in[8 + i]} << (8 * i);
  }
}

void shift_rows(Block b, bool inverse) {
  Block t;
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) t[c * 4 + r] = b[((inverse ? c - r + 4 : c + r) % 4) * 4 + r];
  }
  for (int i = 0; i < 16; ++i) b[i] = t[i];
}

void sub_bytes(Block b, bool inverse) {
  for (int i = 0; i < 16; ++i) b[i] = inverse ? kSBox.inverse[b[i]] : kSBox.forward[b[i]];
}

void mix_columns(Block b, bool inverse) {
  for (int c = 0; c < 4; ++c) {
    const uint8_t a0 = b[c * 4], a1 = b[c * 4 + 1], a2 = b[c * 4 + 2], a3 = b[c * 4 + 3];
    if (!inverse) {
      b[c * 4] = static_cast<uint8_t>(gf_multiply(a0, 2) ^ gf_multiply(a1, 3) ^ a2 ^ a3);
      b[c * 4 + 1] = static_cast<uint8_t>(a0 ^ gf_multiply(a1, 2) ^ gf_multiply(a2, 3) ^ a3);
      b[c * 4 + 2] = static_cast<uint8_t>(a0 ^ a1 ^ gf_multiply(a2, 2) ^ gf_multiply(a3, 3));
      b[c * 4 + 3] = static_cast<uint8_t>(gf_multiply(a0, 3) ^ a1 ^ a2 ^ gf_multiply(a3, 2));
    } else {
      b[c * 4] = static_cast<uint8_t>(gf_multiply(a0, 14) ^ gf_multiply(a1, 11) ^ gf_multiply(a2, 13) ^ gf_multiply(a3, 9));
      b[c * 4 + 1] = static_cast<uint8_t>(gf_multiply(a0, 9) ^ gf_multiply(a1, 14) ^ gf_multiply(a2, 11) ^ gf_multiply(a3, 13));
      b[c * 4 + 2] = static_cast<uint8_t>(gf_multiply(a0, 13) ^ gf_multiply(a1, 9) ^ gf_multiply(a2, 14) ^ gf_multiply(a3, 11));
      b[c * 4 + 3] = static_cast<uint8_t>(gf_multiply(a0, 11) ^ gf_multiply(a1, 13) ^ gf_multiply(a2, 9) ^ gf_multiply(a3, 14));
    }
  }
}

// AESENC, AESENCLAST, AESDEC, AESDECLAST on one 128-bit lane
X86_64Cpu::Xmm aes_round(const X86_64Cpu::Xmm& state, const X86_64Cpu::Xmm& key, bool inverse, bool last) {
  Block b;
  to_block(state.lo, state.hi, b);
  shift_rows(b, inverse);      // (the two steps commute for the encryption and the decryption alike)
  sub_bytes(b, inverse);
  if (!last && !inverse) mix_columns(b, false);
  Block k;
  to_block(key.lo, key.hi, k);
  if (!last && inverse) {      // AESDEC: InvMixColumns comes before the key is added
    mix_columns(b, true);
  }
  for (int i = 0; i < 16; ++i) b[i] ^= k[i];
  X86_64Cpu::Xmm out;
  from_block(b, &out.lo, &out.hi);
  return out;
}

X86_64Cpu::Xmm aes_inverse_mix(const X86_64Cpu::Xmm& v) {
  Block b;
  to_block(v.lo, v.hi, b);
  mix_columns(b, true);
  X86_64Cpu::Xmm out;
  from_block(b, &out.lo, &out.hi);
  return out;
}

uint32_t sub_word(uint32_t w) {
  uint32_t r = 0;
  for (int i = 0; i < 4; ++i) r |= uint32_t{kSBox.forward[(w >> (8 * i)) & 0xFF]} << (8 * i);
  return r;
}

uint32_t rotate_right(uint32_t v, uint32_t n) { return (v >> n) | (v << (32 - n)); }
uint32_t rotate_left(uint32_t v, uint32_t n) { return (v << n) | (v >> (32 - n)); }

X86_64Cpu::Xmm aes_key_generation_assist(const X86_64Cpu::Xmm& src, uint8_t rcon) {
  const uint32_t x1 = static_cast<uint32_t>(src.lo >> 32), x3 = static_cast<uint32_t>(src.hi >> 32);
  const uint32_t s1 = sub_word(x1), s3 = sub_word(x3);
  X86_64Cpu::Xmm out;
  out.lo = uint64_t{s1} | (uint64_t{rotate_right(s1, 8) ^ rcon} << 32);
  out.hi = uint64_t{s3} | (uint64_t{rotate_right(s3, 8) ^ rcon} << 32);
  return out;
}

// --- PCLMULQDQ -----------------------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Xmm carryless_multiply(uint64_t a, uint64_t b) {
  X86_64Cpu::Xmm out;
  for (uint32_t i = 0; i < 64; ++i) {
    if (!((b >> i) & 1)) continue;
    out.lo ^= a << i;
    if (i != 0) out.hi ^= a >> (64 - i);
  }
  return out;
}

// --- SHA -----------------------------------------------------------------------------------------------------------------------------------------

uint32_t dword(const X86_64Cpu::Xmm& x, uint32_t i) { return static_cast<uint32_t>((i < 2 ? x.lo : x.hi) >> (32 * (i & 1))); }

// the four dwords, most significant first (as the instruction descriptions list them)
X86_64Cpu::Xmm pack(uint32_t d3, uint32_t d2, uint32_t d1, uint32_t d0) {
  X86_64Cpu::Xmm out;
  out.lo = uint64_t{d0} | (uint64_t{d1} << 32);
  out.hi = uint64_t{d2} | (uint64_t{d3} << 32);
  return out;
}

X86_64Cpu::Xmm sha1_rounds4(const X86_64Cpu::Xmm& a_in, const X86_64Cpu::Xmm& w_in, uint32_t func) {
  uint32_t a = dword(a_in, 3), b = dword(a_in, 2), c = dword(a_in, 1), d = dword(a_in, 0), e = 0;
  const uint32_t w[4] = {dword(w_in, 3), dword(w_in, 2), dword(w_in, 1), dword(w_in, 0)};
  static const uint32_t k[4] = {0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xCA62C1D6u};
  for (int i = 0; i < 4; ++i) {
    uint32_t f;
    switch (func) {
      case 0: f = (b & c) ^ (~b & d); break;
      case 2: f = (b & c) ^ (b & d) ^ (c & d); break;
      default: f = b ^ c ^ d; break;
    }
    const uint32_t next = f + rotate_left(a, 5) + w[i] + e + k[func];      // (the first word already holds W[0] + E)
    e = d;
    d = c;
    c = rotate_left(b, 30);
    b = a;
    a = next;
  }
  return pack(a, b, c, d);
}

uint32_t sigma0_big(uint32_t x) { return rotate_right(x, 2) ^ rotate_right(x, 13) ^ rotate_right(x, 22); }
uint32_t sigma1_big(uint32_t x) { return rotate_right(x, 6) ^ rotate_right(x, 11) ^ rotate_right(x, 25); }
uint32_t sigma0_small(uint32_t x) { return rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3); }
uint32_t sigma1_small(uint32_t x) { return rotate_right(x, 17) ^ rotate_right(x, 19) ^ (x >> 10); }

X86_64Cpu::Xmm sha256_rounds2(const X86_64Cpu::Xmm& cdgh, const X86_64Cpu::Xmm& abef, const X86_64Cpu::Xmm& wk) {
  uint32_t a = dword(abef, 3), b = dword(abef, 2), e = dword(abef, 1), f = dword(abef, 0);
  uint32_t c = dword(cdgh, 3), d = dword(cdgh, 2), g = dword(cdgh, 1), h = dword(cdgh, 0);
  for (int i = 0; i < 2; ++i) {
    const uint32_t ch = (e & f) ^ (~e & g), maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t = ch + sigma1_big(e) + dword(wk, i) + h;
    const uint32_t new_a = t + maj + sigma0_big(a), new_e = t + d;
    h = g;
    g = f;
    f = e;
    e = new_e;
    d = c;
    c = b;
    b = a;
    a = new_a;
  }
  return pack(a, b, e, f);
}

// --- SHA-512 and SM4 (the VEX-encoded extensions) -------------------------------------------------------------------------------------------------

const uint8_t kSm4SBox[256] = {
    0xd6, 0x90, 0xe9, 0xfe, 0xcc, 0xe1, 0x3d, 0xb7, 0x16, 0xb6, 0x14, 0xc2, 0x28, 0xfb, 0x2c, 0x05,
    0x2b, 0x67, 0x9a, 0x76, 0x2a, 0xbe, 0x04, 0xc3, 0xaa, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99,
    0x9c, 0x42, 0x50, 0xf4, 0x91, 0xef, 0x98, 0x7a, 0x33, 0x54, 0x0b, 0x43, 0xed, 0xcf, 0xac, 0x62,
    0xe4, 0xb3, 0x1c, 0xa9, 0xc9, 0x08, 0xe8, 0x95, 0x80, 0xdf, 0x94, 0xfa, 0x75, 0x8f, 0x3f, 0xa6,
    0x47, 0x07, 0xa7, 0xfc, 0xf3, 0x73, 0x17, 0xba, 0x83, 0x59, 0x3c, 0x19, 0xe6, 0x85, 0x4f, 0xa8,
    0x68, 0x6b, 0x81, 0xb2, 0x71, 0x64, 0xda, 0x8b, 0xf8, 0xeb, 0x0f, 0x4b, 0x70, 0x56, 0x9d, 0x35,
    0x1e, 0x24, 0x0e, 0x5e, 0x63, 0x58, 0xd1, 0xa2, 0x25, 0x22, 0x7c, 0x3b, 0x01, 0x21, 0x78, 0x87,
    0xd4, 0x00, 0x46, 0x57, 0x9f, 0xd3, 0x27, 0x52, 0x4c, 0x36, 0x02, 0xe7, 0xa0, 0xc4, 0xc8, 0x9e,
    0xea, 0xbf, 0x8a, 0xd2, 0x40, 0xc7, 0x38, 0xb5, 0xa3, 0xf7, 0xf2, 0xce, 0xf9, 0x61, 0x15, 0xa1,
    0xe0, 0xae, 0x5d, 0xa4, 0x9b, 0x34, 0x1a, 0x55, 0xad, 0x93, 0x32, 0x30, 0xf5, 0x8c, 0xb1, 0xe3,
    0x1d, 0xf6, 0xe2, 0x2e, 0x82, 0x66, 0xca, 0x60, 0xc0, 0x29, 0x23, 0xab, 0x0d, 0x53, 0x4e, 0x6f,
    0xd5, 0xdb, 0x37, 0x45, 0xde, 0xfd, 0x8e, 0x2f, 0x03, 0xff, 0x6a, 0x72, 0x6d, 0x6c, 0x5b, 0x51,
    0x8d, 0x1b, 0xaf, 0x92, 0xbb, 0xdd, 0xbc, 0x7f, 0x11, 0xd9, 0x5c, 0x41, 0x1f, 0x10, 0x5a, 0xd8,
    0x0a, 0xc1, 0x31, 0x88, 0xa5, 0xcd, 0x7b, 0xbd, 0x2d, 0x74, 0xd0, 0x12, 0xb8, 0xe5, 0xb4, 0xb0,
    0x89, 0x69, 0x97, 0x4a, 0x0c, 0x96, 0x77, 0x7e, 0x65, 0xb9, 0xf1, 0x09, 0xc5, 0x6e, 0xc6, 0x84,
    0x18, 0xf0, 0x7d, 0xec, 0x3a, 0xdc, 0x4d, 0x20, 0x79, 0xee, 0x5f, 0x3e, 0xd7, 0xcb, 0x39, 0x48,
};

uint64_t rotate_right64(uint64_t v, uint32_t n) { return (v >> n) | (v << (64 - n)); }
uint64_t big_sigma0(uint64_t x) { return rotate_right64(x, 28) ^ rotate_right64(x, 34) ^ rotate_right64(x, 39); }
uint64_t big_sigma1(uint64_t x) { return rotate_right64(x, 14) ^ rotate_right64(x, 18) ^ rotate_right64(x, 41); }
uint64_t small_sigma0(uint64_t x) { return rotate_right64(x, 1) ^ rotate_right64(x, 8) ^ (x >> 7); }
uint64_t small_sigma1(uint64_t x) { return rotate_right64(x, 19) ^ rotate_right64(x, 61) ^ (x >> 6); }

// the four qwords of a 256-bit register, lowest first
void qwords(const X86_64Cpu::Xmm& lo, const X86_64Cpu::Xmm& hi, uint64_t q[4]) {
  q[0] = lo.lo;
  q[1] = lo.hi;
  q[2] = hi.lo;
  q[3] = hi.hi;
}

// Two rounds of SHA-512: `abef` and `cdgh` hold the state (A highest), `wk` the two precomputed W + K sums; the result is the new A, B, E, F.
void sha512_rounds2(const uint64_t abef[4], const uint64_t cdgh[4], const uint64_t wk[2], uint64_t out[4]) {
  uint64_t a = abef[3], b = abef[2], e = abef[1], f = abef[0];
  uint64_t c = cdgh[3], d = cdgh[2], g = cdgh[1], h = cdgh[0];
  for (int i = 0; i < 2; ++i) {
    const uint64_t ch = (e & f) ^ (~e & g), maj = (a & b) ^ (a & c) ^ (b & c);
    const uint64_t t = ch + big_sigma1(e) + wk[i] + h;
    const uint64_t new_a = t + maj + big_sigma0(a), new_e = t + d;
    h = g;
    g = f;
    f = e;
    e = new_e;
    d = c;
    c = b;
    b = a;
    a = new_a;
  }
  out[3] = a;
  out[2] = b;
  out[1] = e;
  out[0] = f;
}

// Four rounds of SM4 on one 128-bit lane: `state` holds X[0..3] (dword 0 first), `keys` four round keys (or constants for the key schedule); the result is X[4..7].
X86_64Cpu::Xmm sm4_four_rounds(const X86_64Cpu::Xmm& state, const X86_64Cpu::Xmm& keys, bool key_schedule) {
  uint32_t x[8] = {dword(state, 0), dword(state, 1), dword(state, 2), dword(state, 3), 0, 0, 0, 0};
  for (int j = 0; j < 4; ++j) {
    const uint32_t input = x[j + 1] ^ x[j + 2] ^ x[j + 3] ^ dword(keys, static_cast<uint32_t>(j));
    uint32_t t = 0;
    for (int i = 0; i < 4; ++i) t |= uint32_t{kSm4SBox[(input >> (8 * i)) & 0xFF]} << (8 * i);
    x[j + 4] = x[j] ^ (key_schedule ? (t ^ rotate_left(t, 13) ^ rotate_left(t, 23)) : (t ^ rotate_left(t, 2) ^ rotate_left(t, 10) ^ rotate_left(t, 18) ^ rotate_left(t, 24)));
  }
  return pack(x[7], x[6], x[5], x[4]);
}

}  // namespace

// AES-NI (66 0F 38 DB..DF, 66 0F 3A DF), PCLMULQDQ (66 0F 3A 44), their VEX forms (VAES and VPCLMULQDQ also work on 256 bits), and the SHA extensions (0F 38 C8..CD, 0F 3A CC; no
// VEX form). `vvvv` is the first source of the VEX forms; the legacy forms pass the destination register, which is also their first source.
X86_64Cpu::Step X86_64Cpu::crypto_map(GuestMemory& m, uint32_t map, uint8_t op, bool, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  const bool vex = vex_.active;
  if (vex && map == 2 && (op == 0xCB || op == 0xCC || op == 0xCD)) return sha512_instruction(m, op, vvvv);
  if (vex && map == 2 && op == 0xDA) return sm4_instruction(m, vvvv);
  const bool sha = (map == 2 && op >= 0xC8 && op <= 0xCD) || (map == 3 && op == 0xCC);
  if (sha && vex) return fail(6);      // (the SHA-1 and SHA-256 instructions have no VEX form)
  if (sha ? pp != 0 : pp != 1) return fail(6);
  const bool wide = vex_.wide && !sha;
  if (!decode_modrm()) return bad_decode();
  const bool has_imm = map == 3;
  const uint32_t imm = has_imm ? static_cast<uint32_t>(next_imm(1, false)) : 0;
  if (!finish_decode()) return bad_decode();
  const uint32_t reg = d_.reg;
  const bool unary = (map == 2 && op == 0xDB) || (map == 3 && op == 0xDF);
  if (unary && wide) return fail(6);      // aesimc and aeskeygenassist have no 256-bit form
  Xmm b[2];
  if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
  Xmm out[2];
  const uint32_t halves = wide ? 2 : 1;
  const Xmm a[2] = {s_.xmm[sha ? reg : vvvv], s_.ymm_high[(sha ? reg : vvvv) & 15]};
  if (sha) {
    switch (map << 8 | op) {
      case 0x2C8: out[0] = pack(dword(b[0], 3) + rotate_left(dword(s_.xmm[reg], 3), 30), dword(b[0], 2), dword(b[0], 1), dword(b[0], 0)); break;      // sha1nexte
      case 0x2C9: {      // sha1msg1
        const Xmm x = s_.xmm[reg];
        out[0] = pack(dword(x, 1) ^ dword(x, 3), dword(x, 0) ^ dword(x, 2), dword(b[0], 3) ^ dword(x, 1), dword(b[0], 2) ^ dword(x, 0));
        break;
      }
      case 0x2CA: {      // sha1msg2
        const Xmm x = s_.xmm[reg];
        const uint32_t w16 = rotate_left(dword(x, 3) ^ dword(b[0], 2), 1), w17 = rotate_left(dword(x, 2) ^ dword(b[0], 1), 1), w18 = rotate_left(dword(x, 1) ^ dword(b[0], 0), 1);
        const uint32_t w19 = rotate_left(dword(x, 0) ^ w16, 1);
        out[0] = pack(w16, w17, w18, w19);
        break;
      }
      case 0x2CB: out[0] = sha256_rounds2(s_.xmm[reg], b[0], s_.xmm[0]); break;      // sha256rnds2: the implicit xmm0 holds W + K
      case 0x2CC: {      // sha256msg1
        const Xmm x = s_.xmm[reg];
        out[0] = pack(dword(x, 3) + sigma0_small(dword(b[0], 0)), dword(x, 2) + sigma0_small(dword(x, 3)), dword(x, 1) + sigma0_small(dword(x, 2)), dword(x, 0) + sigma0_small(dword(x, 1)));
        break;
      }
      case 0x2CD: {      // sha256msg2
        const Xmm x = s_.xmm[reg];
        const uint32_t w16 = dword(x, 0) + sigma1_small(dword(b[0], 2)), w17 = dword(x, 1) + sigma1_small(dword(b[0], 3));
        const uint32_t w18 = dword(x, 2) + sigma1_small(w16), w19 = dword(x, 3) + sigma1_small(w17);
        out[0] = pack(w19, w18, w17, w16);
        break;
      }
      default: out[0] = sha1_rounds4(s_.xmm[reg], b[0], imm & 3); break;      // sha1rnds4
    }
    s_.xmm[reg] = out[0];
    s_.rip += d_.position;
    return Step::Ok;
  }
  for (uint32_t h = 0; h < halves; ++h) {
    switch (map << 8 | op) {
      case 0x2DB: out[h] = aes_inverse_mix(b[h]); break;
      case 0x2DC: out[h] = aes_round(a[h], b[h], false, false); break;
      case 0x2DD: out[h] = aes_round(a[h], b[h], false, true); break;
      case 0x2DE: out[h] = aes_round(a[h], b[h], true, false); break;
      case 0x2DF: out[h] = aes_round(a[h], b[h], true, true); break;
      case 0x3DF: out[h] = aes_key_generation_assist(b[h], static_cast<uint8_t>(imm)); break;
      case 0x2CF: {      // gf2p8mulb
        Xmm r;
        for (uint32_t i = 0; i < 16; ++i) {
          const uint8_t x = static_cast<uint8_t>((i < 8 ? a[h].lo : a[h].hi) >> (8 * (i & 7))), y = static_cast<uint8_t>((i < 8 ? b[h].lo : b[h].hi) >> (8 * (i & 7)));
          (i < 8 ? r.lo : r.hi) |= uint64_t{gf_multiply(x, y)} << (8 * (i & 7));
        }
        out[h] = r;
        break;
      }
      case 0x3CE: case 0x3CF: {      // gf2p8affineqb, gf2p8affineinvqb
        Xmm r;
        for (uint32_t i = 0; i < 16; ++i) {
          const uint8_t x = static_cast<uint8_t>((i < 8 ? a[h].lo : a[h].hi) >> (8 * (i & 7)));
          const uint64_t matrix = i < 8 ? b[h].lo : b[h].hi;
          uint8_t input = x;
          if (op == 0xCF) {      // multiplicative inverse in the field (0 stays 0)
            input = 0;
            if (x) {
              input = 1;
              for (int k = 0; k < 254; ++k) input = gf_multiply(input, x);
            }
          }
          uint8_t result = 0;
          for (uint32_t k = 0; k < 8; ++k) {
            uint8_t t = static_cast<uint8_t>(matrix >> (8 * (7 - k))) & input;
            t ^= t >> 4; t ^= t >> 2; t ^= t >> 1;
            result |= static_cast<uint8_t>(((t & 1) ^ ((imm >> k) & 1)) << k);
          }
          (i < 8 ? r.lo : r.hi) |= uint64_t{result} << (8 * (i & 7));
        }
        out[h] = r;
        break;
      }
      case 0x344: out[h] = carryless_multiply((imm & 1) ? (a[h].hi) : (a[h].lo), (imm & 0x10) ? b[h].hi : b[h].lo); break;
      default: return fail(6);
    }
  }
  s_.xmm[reg] = out[0];
  s_.ymm_high[reg] = wide ? out[1] : Xmm();
  s_.rip += d_.position;
  return Step::Ok;
}


// VSHA512RNDS2 ymm1, ymm2, xmm3 (VEX.256.F2.0F38 CB), VSHA512MSG1 ymm1, xmm2 (CC), VSHA512MSG2 ymm1, ymm2 (CD)
X86_64Cpu::Step X86_64Cpu::sha512_instruction(GuestMemory& m, uint8_t op, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!d_.repne || !vex_.wide || d_.rex_w) return fail(6);
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  Xmm b[2];
  if (!read_y(m, op == 0xCD ? 32 : 16, b)) return Step::Fault;
  uint64_t dest[4], src[4], out[4];
  qwords(s_.xmm[d_.reg], s_.ymm_high[d_.reg], dest);
  qwords(b[0], b[1], src);
  switch (op) {
    case 0xCB: {
      uint64_t abef[4];
      qwords(s_.xmm[vvvv], s_.ymm_high[vvvv & 15], abef);
      const uint64_t wk[2] = {b[0].lo, b[0].hi};
      sha512_rounds2(abef, dest, wk, out);
      break;
    }
    case 0xCC:
      out[0] = dest[0] + small_sigma0(dest[1]);
      out[1] = dest[1] + small_sigma0(dest[2]);
      out[2] = dest[2] + small_sigma0(dest[3]);
      out[3] = dest[3] + small_sigma0(b[0].lo);
      break;
    default:
      out[0] = dest[0] + small_sigma1(src[2]);
      out[1] = dest[1] + small_sigma1(src[3]);
      out[2] = dest[2] + small_sigma1(out[0]);
      out[3] = dest[3] + small_sigma1(out[1]);
      break;
  }
  s_.xmm[d_.reg].lo = out[0];
  s_.xmm[d_.reg].hi = out[1];
  s_.ymm_high[d_.reg].lo = out[2];
  s_.ymm_high[d_.reg].hi = out[3];
  s_.rip += d_.position;
  return Step::Ok;
}

// VSM4KEY4 (VEX.F3.0F38 DA) and VSM4RNDS4 (VEX.F2.0F38 DA): xmm1/ymm1 = f(xmm2/ymm2, xmm3/ymm3/m), per 128-bit lane
X86_64Cpu::Step X86_64Cpu::sm4_instruction(GuestMemory& m, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if ((!d_.rep && !d_.repne) || d_.rex_w) return fail(6);
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  const bool wide = vex_.wide;
  Xmm b[2];
  if (!read_y(m, wide ? 32 : 16, b)) return Step::Fault;
  const Xmm a[2] = {s_.xmm[vvvv], s_.ymm_high[vvvv & 15]};
  Xmm out[2];
  for (uint32_t h = 0; h < (wide ? 2u : 1u); ++h) out[h] = sm4_four_rounds(a[h], b[h], d_.rep);
  s_.xmm[d_.reg] = out[0];
  s_.ymm_high[d_.reg] = wide ? out[1] : Xmm();
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation

namespace emulation {

X86_64Cpu::Xmm crypto_aes_round(const X86_64Cpu::Xmm& state, const X86_64Cpu::Xmm& key, bool inverse, bool last) { return aes_round(state, key, inverse, last); }
X86_64Cpu::Xmm crypto_carryless_multiply(uint64_t a, uint64_t b) { return carryless_multiply(a, b); }
uint8_t crypto_gf2p8_multiply(uint8_t a, uint8_t b) { return gf_multiply(a, b); }

uint8_t crypto_gf2p8_inverse(uint8_t a) {
  if (a == 0) return 0;
  uint8_t r = 1;      // a^254 is the inverse
  for (int i = 0; i < 254; ++i) r = gf_multiply(r, a);
  return r;
}

}  // namespace emulation
