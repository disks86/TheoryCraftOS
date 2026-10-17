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

// RSA signature verification (public key operations only), PKCS #1 v2.2 (RFC 8017). Moduli of up to 4096 bits; the public exponent must fit
// 32 bits. Everything here works on public values, so nothing is constant time.
enum class RsaPadding : uint8_t { Pkcs1V15, Pss };

// Verifies `signature` over the already computed `digest` (of the message, with `hash`). `modulus` and `exponent` are big-endian.
// For PSS the salt length is the hash length (what TLS 1.3 and X.509 use) and MGF1 uses the same hash.
bool rsa_verify(RsaPadding padding, HashKind hash, ConstByteView modulus, ConstByteView exponent, const uint8_t* digest, ConstByteView signature);

// Bits in a big-endian modulus, ignoring leading zeros.
uint32_t rsa_modulus_bits(ConstByteView modulus);

}  // namespace crypto
