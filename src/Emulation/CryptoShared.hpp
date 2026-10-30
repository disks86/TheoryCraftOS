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

// The block-cipher building blocks the x86-64 (X86_64Crypto.cpp) and AArch64 (AArch64Crypto.cpp) guests share. A 128-bit block is two little-endian quadwords.
namespace emulation {

// SubBytes and ShiftRows together (or their inverses), and MixColumns / InvMixColumns, on one AES block.
void crypto_aes_sub_shift(uint64_t* lo, uint64_t* hi, bool inverse);
void crypto_aes_mix(uint64_t* lo, uint64_t* hi, bool inverse);
// Four SM4 rounds (or four steps of the key schedule): `state` holds X[0..3] (dword 0 first), `keys` four round keys or constants, the result X[4..7] goes to `out`.
void crypto_sm4_rounds(const uint64_t state[2], const uint64_t keys[2], bool key_schedule, uint64_t out[2]);
// Carry-less 64 x 64 -> 128-bit multiplication.
void crypto_clmul(uint64_t a, uint64_t b, uint64_t* lo, uint64_t* hi);

}  // namespace emulation
