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

// The cryptographic functions of the Bluetooth security manager (Core spec vol 3 part H, section 2.2): e, c1, s1, ah for legacy pairing and the encrypted-link keys; f4, f5, f6, g2, h6, h7
// for Secure Connections; and the P-256 key pair and shared secret in the byte order the air uses. All values are as they appear in packets, least significant byte first (the
// specification's examples are most significant first and are reversed here); the AES and AES-CMAC themselves work on the most significant first form, so every function swaps on the way in
// and out.
#pragma once
#include <stdint.h>
#include "Crypto/Random.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace bt {

// The security function e: AES-128 of `data` under `key`, in place.
bool sm_e(const uint8_t key[16], uint8_t data[16]);
// The legacy confirm value c1 and the short term key function s1. `preq` and `pres` are the 7 byte pairing request and response PDUs; `ia`/`ra` the initiator's and responder's addresses.
bool sm_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7], uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16]);
bool sm_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16]);
// The hash of a resolvable private address: ah(irk, prand) is the low 24 bits of e(irk, prand).
bool sm_ah(const uint8_t irk[16], const uint8_t prand[3], uint8_t hash[3]);
// True if the resolvable private address `address` (6 bytes, as on the air) was made from `irk`.
bool sm_irk_matches(const uint8_t irk[16], const uint8_t address[6]);

bool sm_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z, uint8_t out[16]);
bool sm_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16], const uint8_t a1[7], const uint8_t a2[7], uint8_t mackey[16], uint8_t ltk[16]);
bool sm_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16], const uint8_t r[16], const uint8_t io_cap[3], const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16]);
bool sm_g2(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], const uint8_t y[16], uint32_t* value);
bool sm_h6(const uint8_t w[16], const uint8_t key_id[4], uint8_t out[16]);
bool sm_h7(const uint8_t w[16], const uint8_t salt[16], uint8_t out[16]);

// A P-256 key pair: the private scalar and the public point as the air sends it (X then Y, each 32 bytes least significant first).
struct SmKeyPair {
  uint8_t private_key[32] = {};          // least significant first
  uint8_t public_key[64] = {};
};
bool sm_generate_key_pair(crypto::IRandom& random, SmKeyPair* out);
bool sm_public_from_private(const uint8_t private_key[32], uint8_t public_key[64]);
// The DHKey (the shared secret's X coordinate, least significant first). False for a peer key that is not on the curve.
bool sm_dhkey(const uint8_t private_key[32], const uint8_t peer_public[64], uint8_t dhkey[32]);

}  // namespace bt
}  // namespace hardware
