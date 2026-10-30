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

// The AArch64 cryptographic extensions: AES (aese, aesd, aesmc, aesimc), SHA-1 and SHA-256, SHA-512, SHA-3 (eor3, bcax, rax1, xar), SM3, SM4, and the polynomial multiplies pmull / pmull2 / pmul.
#include "Emulation/AArch64Cpu.hpp"
#include "Emulation/CryptoShared.hpp"

namespace emulation {

namespace {

using Vec = AArch64Cpu::Vec;

constexpr uint32_t bits_of(uint32_t insn, uint32_t hi, uint32_t lo) { return (insn >> lo) & ((1u << (hi - lo + 1)) - 1); }

uint32_t rol32(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }
uint32_t ror32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
uint64_t ror64(uint64_t x, uint32_t n) { return n == 0 ? x : (x >> n) | (x << (64 - n)); }
uint64_t rol64(uint64_t x, uint32_t n) { return n == 0 ? x : (x << n) | (x >> (64 - n)); }

uint32_t word(const Vec& v, uint32_t i) { return static_cast<uint32_t>((i < 2 ? v.lo : v.hi) >> (32 * (i & 1))); }
void set_word(Vec* v, uint32_t i, uint32_t x) {
  uint64_t& half = i < 2 ? v->lo : v->hi;
  const uint32_t shift = 32 * (i & 1);
  half = (half & ~(uint64_t{0xFFFFFFFFu} << shift)) | (uint64_t{x} << shift);
}
Vec from_words(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  Vec v;
  v.lo = w0 | (uint64_t{w1} << 32);
  v.hi = w2 | (uint64_t{w3} << 32);
  return v;
}

uint32_t sha_choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
uint32_t sha_majority(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }
uint32_t sha_parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
uint32_t hash_sigma0(uint32_t x) { return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22); }
uint32_t hash_sigma1(uint32_t x) { return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25); }
uint32_t schedule_sigma0(uint32_t x) { return ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3); }
uint32_t schedule_sigma1(uint32_t x) { return ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10); }

// SHA256hash of the Arm pseudocode: four rounds; part 1 returns the new ABCD half (X), part 2 the new EFGH half (Y). <Y, X> = ROL(Y : X, 32) moves Y's top word into X's bottom and X's top into Y's.
Vec sha256_hash(Vec x, Vec y, const Vec& w, bool part1) {
  for (uint32_t e = 0; e < 4; ++e) {
    const uint32_t chs = sha_choose(word(y, 0), word(y, 1), word(y, 2));
    const uint32_t maj = sha_majority(word(x, 0), word(x, 1), word(x, 2));
    const uint32_t t = word(y, 3) + hash_sigma1(word(y, 0)) + chs + word(w, e);
    set_word(&x, 3, t + word(x, 3));
    set_word(&y, 3, t + hash_sigma0(word(x, 0)) + maj);
    const Vec nx = from_words(word(y, 3), word(x, 0), word(x, 1), word(x, 2));
    const Vec ny = from_words(word(x, 3), word(y, 0), word(y, 1), word(y, 2));
    x = nx;
    y = ny;
  }
  return part1 ? x : y;
}

// SHA-1 hash update: four rounds of the function chosen by `kind` (0 choose, 1 parity, 2 majority); x = ABCD, y = E, w = the four message words plus constants.
Vec sha1_update(Vec x, uint32_t y, const Vec& w, uint32_t kind) {
  for (uint32_t e = 0; e < 4; ++e) {
    const uint32_t a = word(x, 0), b = word(x, 1), c = word(x, 2), d = word(x, 3);
    const uint32_t t = kind == 0 ? sha_choose(b, c, d) : kind == 1 ? sha_parity(b, c, d) : sha_majority(b, c, d);
    const uint32_t next = y + rol32(a, 5) + t + word(w, e);
    x = from_words(next, a, rol32(b, 30), c);
    y = d;
  }
  return x;
}

uint64_t sigma_s0(uint64_t x) { return ror64(x, 28) ^ ror64(x, 34) ^ ror64(x, 39); }
uint64_t sigma_s1(uint64_t x) { return ror64(x, 14) ^ ror64(x, 18) ^ ror64(x, 41); }
uint64_t small_s0(uint64_t x) { return ror64(x, 1) ^ ror64(x, 8) ^ (x >> 7); }
uint64_t small_s1(uint64_t x) { return ror64(x, 19) ^ ror64(x, 61) ^ (x >> 6); }
uint64_t choose64(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (~x & z); }
uint64_t majority64(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }

// Polynomial (carry-less) multiplication of two bytes, 16-bit result.
uint16_t pmul8(uint8_t a, uint8_t b) {
  uint16_t r = 0;
  for (uint32_t i = 0; i < 8; ++i)
    if ((b >> i) & 1) r ^= static_cast<uint16_t>(a) << i;
  return r;
}

}  // namespace

// Returns true when `insn` is a cryptographic instruction (handled, with the Step in *result), false otherwise.
bool AArch64Cpu::crypto(uint32_t insn, Step* result) {
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  Vec d = s_.v[rd];
  const Vec n = s_.v[rn], m = s_.v[rm];
  auto done = [&](const Vec& v) { s_.v[rd] = v; *result = Step::Ok; return true; };
  auto bad = [&]() { *result = undefined(); return true; };

  if ((insn & 0xFFFE0C00u) == 0x4E280800u) {      // AES single round
    const uint32_t opcode = bits_of(insn, 16, 12);
    if (opcode < 4 || opcode > 7) return bad();
    uint64_t lo = n.lo, hi = n.hi;
    if (opcode <= 5) {      // AESE, AESD: AddRoundKey, then (Inv)SubBytes and (Inv)ShiftRows
      lo ^= d.lo;
      hi ^= d.hi;
      crypto_aes_sub_shift(&lo, &hi, opcode == 5);
    } else {      // AESMC, AESIMC
      crypto_aes_mix(&lo, &hi, opcode == 7);
    }
    Vec v;
    v.lo = lo;
    v.hi = hi;
    return done(v);
  }
  if ((insn & 0xFFE08C00u) == 0x5E000000u) {      // SHA1 / SHA256 three-register
    const uint32_t opcode = bits_of(insn, 14, 12);
    switch (opcode) {
      case 0: case 1: case 2: return done(sha1_update(d, word(n, 0), m, opcode));      // SHA1C, SHA1P, SHA1M
      case 3: {      // SHA1SU0
        Vec t;
        t.lo = d.hi;
        t.hi = n.lo;
        Vec v;
        v.lo = d.lo ^ t.lo ^ m.lo;
        v.hi = d.hi ^ t.hi ^ m.hi;
        return done(v);
      }
      case 4: return done(sha256_hash(d, n, m, true));      // SHA256H
      case 5: return done(sha256_hash(n, d, m, false));      // SHA256H2
      case 6: {      // SHA256SU1
        const Vec t0 = from_words(word(n, 1), word(n, 2), word(n, 3), word(m, 0));
        Vec v;
        for (uint32_t e = 0; e < 2; ++e) set_word(&v, e, schedule_sigma1(word(m, 2 + e)) + word(d, e) + word(t0, e));
        for (uint32_t e = 2; e < 4; ++e) set_word(&v, e, schedule_sigma1(word(v, e - 2)) + word(d, e) + word(t0, e));
        return done(v);
      }
      default: return bad();
    }
  }
  if ((insn & 0xFFFF8C00u) == 0x5E280800u) {      // SHA1H, SHA1SU1, SHA256SU0
    switch (bits_of(insn, 14, 12)) {
      case 0: {      // SHA1H Sd, Sn
        Vec v;
        v.lo = rol32(word(n, 0), 30);
        return done(v);
      }
      case 1: {      // SHA1SU1
        const Vec t = from_words(word(d, 0) ^ word(n, 1), word(d, 1) ^ word(n, 2), word(d, 2) ^ word(n, 3), word(d, 3));
        Vec v = from_words(rol32(word(t, 0), 1), rol32(word(t, 1), 1), rol32(word(t, 2), 1), rol32(word(t, 3), 1) ^ rol32(word(t, 0), 2));
        return done(v);
      }
      case 2: {      // SHA256SU0
        const Vec t = from_words(word(d, 1), word(d, 2), word(d, 3), word(n, 0));
        Vec v;
        for (uint32_t e = 0; e < 4; ++e) set_word(&v, e, schedule_sigma0(word(t, e)) + word(d, e));
        return done(v);
      }
      default: return bad();
    }
  }
  if ((insn & 0xFF000000u) == 0xCE000000u) {
    const uint32_t group = bits_of(insn, 23, 21);
    if (bits_of(insn, 31, 24) == 0xCE && (group == 0 || group == 1 || group == 2) && !((insn >> 15) & 1)) {      // EOR3, BCAX, SM3SS1
      const uint32_t ra = bits_of(insn, 14, 10);
      const Vec a = s_.v[ra];
      if (group == 0) {      // EOR3
        Vec v;
        v.lo = n.lo ^ m.lo ^ a.lo;
        v.hi = n.hi ^ m.hi ^ a.hi;
        return done(v);
      }
      if (group == 1) {      // BCAX
        Vec v;
        v.lo = n.lo ^ (m.lo & ~a.lo);
        v.hi = n.hi ^ (m.hi & ~a.hi);
        return done(v);
      }
      return done(from_words(0, 0, 0, rol32(rol32(word(n, 3), 12) + word(m, 3) + word(a, 3), 7)));      // SM3SS1
    }
    if (group == 4) {      // XAR
      const uint32_t imm6 = bits_of(insn, 15, 10);
      Vec v;
      v.lo = ror64(n.lo ^ m.lo, imm6);
      v.hi = ror64(n.hi ^ m.hi, imm6);
      return done(v);
    }
    if (group == 2 && ((insn >> 15) & 1) && !((insn >> 14) & 1)) {      // SM3TT1A, SM3TT1B, SM3TT2A, SM3TT2B
      const uint32_t imm2 = bits_of(insn, 13, 12), opcode = bits_of(insn, 11, 10);
      uint32_t t;
      const uint32_t d1 = word(d, 1), d2 = word(d, 2), d3 = word(d, 3);
      switch (opcode) {
        case 0: t = d3 ^ d2 ^ d1; break;
        case 1: t = sha_majority(d3, d2, d1); break;
        case 2: t = sha_parity(d3, d2, d1); break;
        default: t = sha_choose(d3, d2, d1); break;
      }
      t += word(d, 0) + word(m, imm2);
      set_word(&d, 0, d1);
      if (opcode < 2) {
        t += word(n, 3) ^ ror32(d3, 20);
        set_word(&d, 1, ror32(d2, 23));
      } else {
        t += word(n, 3);
        t ^= rol32(t, 9) ^ rol32(t, 17);
        set_word(&d, 1, ror32(d2, 13));
      }
      set_word(&d, 2, d3);
      set_word(&d, 3, t);
      return done(d);
    }
    if (group == 3 && ((insn >> 15) & 1)) {      // 011: SHA512 three-register, RAX1, SM3PARTW, SM4EKEY
      const uint32_t op = bits_of(insn, 11, 10);
      if (!((insn >> 14) & 1)) {
        switch (op) {
          case 0: {      // SHA512H Qd, Qn, Vm.2D
            uint64_t d0 = d.lo, d1 = d.hi;
            d1 += sigma_s1(m.hi) + choose64(m.hi, n.lo, n.hi);
            d0 += sigma_s1(d1 + m.lo) + choose64(d1 + m.lo, m.hi, n.lo);
            Vec v;
            v.lo = d0;
            v.hi = d1;
            return done(v);
          }
          case 1: {      // SHA512H2
            uint64_t d0 = d.lo, d1 = d.hi;
            d1 += sigma_s0(m.lo) + majority64(n.lo, m.hi, m.lo);
            d0 += sigma_s0(d1) + majority64(d1, m.lo, m.hi);
            Vec v;
            v.lo = d0;
            v.hi = d1;
            return done(v);
          }
          case 2: {      // SHA512SU1
            Vec v;
            v.lo = d.lo + small_s1(n.lo) + m.lo;
            v.hi = d.hi + small_s1(n.hi) + m.hi;
            return done(v);
          }
          default: {      // RAX1
            Vec v;
            v.lo = n.lo ^ rol64(m.lo, 1);
            v.hi = n.hi ^ rol64(m.hi, 1);
            return done(v);
          }
        }
      }
      switch (op) {
        case 0: {      // SM3PARTW1
          Vec v = d;
          uint32_t t = word(d, 0) ^ word(n, 0) ^ ror32(word(m, 1), 17);
          set_word(&v, 0, t ^ ror32(t, 17) ^ ror32(t, 9));
          t = word(d, 1) ^ word(n, 1) ^ ror32(word(m, 2), 17);
          set_word(&v, 1, t ^ ror32(t, 17) ^ ror32(t, 9));
          t = word(d, 2) ^ word(n, 2) ^ ror32(word(m, 3), 17);
          set_word(&v, 2, t ^ ror32(t, 17) ^ ror32(t, 9));
          t = word(d, 3) ^ word(n, 3) ^ ror32(word(v, 0), 17);
          set_word(&v, 3, t ^ ror32(t, 17) ^ ror32(t, 9));
          return done(v);
        }
        case 1: {      // SM3PARTW2
          Vec v = d;
          const uint32_t t = word(n, 0) ^ ror32(word(m, 0), 25);
          set_word(&v, 0, word(d, 0) ^ t);
          set_word(&v, 1, word(d, 1) ^ word(n, 1) ^ ror32(word(m, 1), 25));
          set_word(&v, 2, word(d, 2) ^ word(n, 2) ^ ror32(word(m, 2), 25));
          set_word(&v, 3, word(d, 3) ^ word(n, 3) ^ ror32(word(m, 3), 25) ^ ror32(t, 17) ^ ror32(t, 2) ^ ror32(t, 26));
          return done(v);
        }
        case 2: {      // SM4EKEY
          const uint64_t state[2] = {n.lo, n.hi}, keys[2] = {m.lo, m.hi};
          uint64_t out[2];
          crypto_sm4_rounds(state, keys, true, out);
          Vec v;
          v.lo = out[0];
          v.hi = out[1];
          return done(v);
        }
        default: return bad();
      }
    }
    if ((insn & 0xFFFFF000u) == 0xCEC08000u) {      // SHA512SU0, SM4E
      const uint32_t op = bits_of(insn, 11, 10);
      if (op == 0) {
        Vec v;
        v.lo = d.lo + small_s0(d.hi);
        v.hi = d.hi + small_s0(n.lo);
        return done(v);
      }
      if (op == 1) {
        const uint64_t state[2] = {d.lo, d.hi}, keys[2] = {n.lo, n.hi};
        uint64_t out[2];
        crypto_sm4_rounds(state, keys, false, out);
        Vec v;
        v.lo = out[0];
        v.hi = out[1];
        return done(v);
      }
      return bad();
    }
    return false;
  }
  if ((insn & 0xBF20FC00u) == 0x0E20E000u) {      // PMULL, PMULL2
    const uint32_t size = bits_of(insn, 23, 22);
    const bool upper = (insn >> 30) & 1;
    if (size == 0) {      // 8 x 8 -> 16 bits
      const uint64_t a = upper ? n.hi : n.lo, b = upper ? m.hi : m.lo;
      Vec v;
      for (uint32_t i = 0; i < 8; ++i) {
        const uint16_t p = pmul8(static_cast<uint8_t>(a >> (8 * i)), static_cast<uint8_t>(b >> (8 * i)));
        (i < 4 ? v.lo : v.hi) |= uint64_t{p} << (16 * (i & 3));
      }
      return done(v);
    }
    if (size == 3) {      // 64 x 64 -> 128 bits
      Vec v;
      crypto_clmul(upper ? n.hi : n.lo, upper ? m.hi : m.lo, &v.lo, &v.hi);
      return done(v);
    }
    return bad();
  }
  return false;
}

}  // namespace emulation
