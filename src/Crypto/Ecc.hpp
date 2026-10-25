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
#include "Crypto/Crypto.hpp"
#include "Crypto/Random.hpp"

namespace crypto {

// Elliptic curve Diffie-Hellman and ECDSA verification on NIST P-256 and P-384 (FIPS 186-4 / SEC 2). Points are the uncompressed SEC1
// form 04 | X | Y. The private-key operations (ecdh, public_key) use a fixed-length Montgomery ladder on a blinded scalar and constant-time field
// arithmetic; the exceptional-case branches of the group law are not hidden. Verification works on public values only.
enum class EcCurve : uint8_t { P256, P384 };

constexpr uint32_t ec_field_size(EcCurve c) { return c == EcCurve::P256 ? 32 : 48; }
constexpr uint32_t ec_point_size(EcCurve c) { return 1 + 2 * ec_field_size(c); }
constexpr uint32_t kMaxEcPoint = 97;

// A random private scalar in [1, n-1] (ec_field_size bytes, big-endian). False if the random source fails.
bool ec_generate_private(EcCurve curve, IRandom& random, uint8_t* private_key);
// The public point for a private scalar.
bool ec_public_key(EcCurve curve, const uint8_t* private_key, uint8_t* point);
// The X coordinate of private * peer (the ECDH shared secret). False if the peer's point is not on the curve (or is malformed) or the scalar is out of range.
bool ec_ecdh(EcCurve curve, const uint8_t* private_key, ConstByteView peer_point, uint8_t* shared_x);
// True if `point` is a valid, finite point on the curve.
bool ec_point_valid(EcCurve curve, ConstByteView point);
// ECDSA signature check (r and s as unsigned big-endian integers) over `digest`, truncated per FIPS 186-4 if longer than the curve order.
bool ecdsa_verify(EcCurve curve, ConstByteView public_point, const uint8_t* digest, uint32_t digest_length, ConstByteView r, ConstByteView s);

// ECDSA signature over `digest` (truncated per FIPS 186-4 like verification does) with a fresh random nonce from `random`: r | s, each ec_field_size bytes,
// big-endian, into `signature`. False if the key is out of range or the random source fails.
bool ecdsa_sign(EcCurve curve, const uint8_t* private_key, const uint8_t* digest, uint32_t digest_length, IRandom& random, uint8_t* signature);

}  // namespace crypto
