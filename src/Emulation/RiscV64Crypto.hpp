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
#pragma once
#include <stdint.h>

namespace emulation {
namespace rvcrypto {

// Executes the scalar crypto and bit-manipulation instructions of Zbkb, Zbkx, Zknd, Zkne, Zknh, Zksed and Zksh; `a` and `b` are the values of rs1 and rs2. False when the word is not one of them.
bool scalar_crypto(uint32_t insn, uint64_t a, uint64_t b, uint64_t* out);

// --- the vector cryptography extensions work on element groups of 16 (or 32) bytes, given here as byte arrays in element order ---------------------------------------------------------

// Zvkned: one AES round of a 128-bit state with a round key. kind: 0 vaesdm, 1 vaesdf, 2 vaesem, 3 vaesef (the vs1 field of the instruction).
void aes_round(uint8_t state[16], const uint8_t key[16], uint32_t kind);
// vaeskf1: the next AES-128 round key from `key`, for round `round` (1..10, with 0 and 11..15 meaning round ^ 8); vaeskf2: the next AES-256 round key from the previous-but-one key
// in `previous` and the last one in `last`.
void aes_key_128(const uint8_t key[16], uint32_t round, uint8_t out[16]);
void aes_key_256(const uint8_t previous[16], const uint8_t last[16], uint32_t round, uint8_t out[16]);

// Zvknha / Zvknhb: SHA-2 on element groups of four 32-bit (SHA-256) or 64-bit (SHA-512) words. vsha2ms: the next four message words; vsha2ch / vsha2cl: two compression rounds.
void sha2_message(uint64_t vd[4], const uint64_t vs2[4], const uint64_t vs1[4], uint32_t sew);
void sha2_rounds(uint64_t vd[4], const uint64_t vs2[4], const uint64_t vs1[4], uint32_t sew, bool high);

// Zvksed: vsm4k (four round keys from the previous four) and vsm4r (four rounds).
void sm4_keys(const uint32_t previous[4], uint32_t round, uint32_t out[4]);
void sm4_rounds(const uint32_t state[4], const uint32_t keys[4], uint32_t out[4]);
// Zvksh: vsm3me (eight message words from the sixteen before them) and vsm3c (two compression rounds). The words are byte-swapped, as the instructions see them.
void sm3_expand(const uint32_t low[8], const uint32_t high[8], uint32_t out[8]);
void sm3_compress(const uint32_t state[8], const uint32_t message[8], uint32_t round, uint32_t out[8]);
// Zvkg: a product in GF(2^128) in the GHASH convention (the bytes are the GCM block).
void ghash_multiply(const uint8_t x[16], const uint8_t h[16], uint8_t out[16]);

}  // namespace rvcrypto
}  // namespace emulation
