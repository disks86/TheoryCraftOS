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

}  // namespace

// AES-NI (66 0F 38 DB..DF, 66 0F 3A DF), PCLMULQDQ (66 0F 3A 44), their VEX forms (VAES and VPCLMULQDQ also work on 256 bits), and the SHA extensions (0F 38 C8..CD, 0F 3A CC; no
// VEX form). `vvvv` is the first source of the VEX forms; the legacy forms pass the destination register, which is also their first source.
X86_64Cpu::Step X86_64Cpu::crypto_map(GuestMemory& m, uint32_t map, uint8_t op, bool, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  const bool sha = (map == 2 && op >= 0xC8 && op <= 0xCD) || (map == 3 && op == 0xCC);
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
      case 0x344: out[h] = carryless_multiply((imm & 1) ? (a[h].hi) : (a[h].lo), (imm & 0x10) ? b[h].hi : b[h].lo); break;
      default: return fail(6);
    }
  }
  s_.xmm[reg] = out[0];
  s_.ymm_high[reg] = wide ? out[1] : Xmm();
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
