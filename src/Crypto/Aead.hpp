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

namespace crypto {

// Authenticated encryption as TLS uses it (RFC 5116 / 8439): AES-GCM and ChaCha20-Poly1305, both with a 12-byte nonce and a 16-byte tag.
// seal() encrypts `data` in place and writes the tag; open() checks the tag first and decrypts in place only if it is right.
constexpr uint32_t kAeadNonceSize = 12, kAeadTagSize = 16;

class AesGcm {
 public:
  // 16 or 32 byte key (AES-128 / AES-256).
  bool set_key(ConstByteView key);
  void seal(const uint8_t nonce[12], ConstByteView aad, ByteView data, uint8_t tag[16]) const;
  bool open(const uint8_t nonce[12], ConstByteView aad, ByteView data, const uint8_t tag[16]) const;

 private:
  void ctr(const uint8_t nonce[12], ByteView data) const;  // from counter 2
  void tag_for(const uint8_t nonce[12], ConstByteView aad, ConstByteView cipher, uint8_t tag[16]) const;
  Aes aes_;
  uint64_t h_hi_ = 0, h_lo_ = 0;
};

class ChaCha20Poly1305 {
 public:
  bool set_key(ConstByteView key);  // 32 bytes
  void seal(const uint8_t nonce[12], ConstByteView aad, ByteView data, uint8_t tag[16]) const;
  bool open(const uint8_t nonce[12], ConstByteView aad, ByteView data, const uint8_t tag[16]) const;

 private:
  uint8_t key_[32] = {};
};

// The ChaCha20 stream cipher alone (RFC 8439 2.4): XOR the keystream starting at block `counter` into `data`.
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], ByteView data);

// Poly1305 (RFC 8439 2.5) under a 32-byte one-time key, fed in pieces.
class Poly1305 {
 public:
  explicit Poly1305(const uint8_t key[32]);
  void update(ConstByteView data);
  void finish(uint8_t tag[16]);

 private:
  void blocks(const uint8_t* p, uint32_t count, uint32_t hibit);
  uint32_t r_[5], h_[5], pad_[4];
  uint8_t buffer_[16];
  uint32_t buffered_ = 0;
};
void poly1305(const uint8_t key[32], ConstByteView data, uint8_t tag[16]);

// X25519 (RFC 7748). shared_secret() is false if the result is all zero (a peer that sent a low-order point).
void x25519_public_key(const uint8_t private_key[32], uint8_t public_key[32]);
bool x25519_shared_secret(const uint8_t private_key[32], const uint8_t peer_public[32], uint8_t secret[32]);

}  // namespace crypto
