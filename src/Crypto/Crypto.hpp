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
#include "Utility/ArrayView.hpp"

namespace crypto {

// Hash functions and HMAC (RFC 2104), block ciphers and the modes SNMPv3 uses. Plain portable code with no tables that depend on
// the data (the HMAC comparison helper is constant time); keys and state are on the caller's stack and cleared when done.
// MD5 and SHA-1 are here because the protocols that need them (SNMPv3 USM, RFC 3414) do: do not use them for anything new.

enum class HashKind : uint8_t { Md5, Sha1, Sha224, Sha256 };
constexpr uint32_t hash_size(HashKind k) { return k == HashKind::Md5 ? 16 : k == HashKind::Sha1 ? 20 : k == HashKind::Sha224 ? 28 : 32; }
constexpr uint32_t hash_block_size(HashKind) { return 64; }
constexpr uint32_t kMaxHashSize = 32;

class Hasher {
 public:
  explicit Hasher(HashKind kind);
  void update(ConstByteView data);
  // Writes hash_size(kind) bytes and resets the hasher for reuse.
  void finish(uint8_t* out);
  HashKind kind() const { return kind_; }

 private:
  void block(const uint8_t* p);
  void reset();
  HashKind kind_;
  uint32_t h_[8];
  uint64_t total_ = 0;
  uint8_t buffer_[64];
  uint32_t buffered_ = 0;
};

// One-shot hash of `data`.
void hash(HashKind kind, ConstByteView data, uint8_t* out);
// HMAC of `data` under `key` into `out` (hash_size(kind) bytes).
void hmac(HashKind kind, ConstByteView key, ConstByteView data, uint8_t* out);
// True if the first `n` bytes of a and b are equal; takes the same time wherever they differ.
bool equal_constant_time(const uint8_t* a, const uint8_t* b, uint32_t n);

// AES with 128-, 192- or 256-bit keys: encryption of one block, and CFB-128 (NIST SP 800-38A) which needs nothing else.
class Aes {
 public:
  // Key of 16, 24 or 32 bytes; false for any other length.
  bool set_key(ConstByteView key);
  void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
  // Encrypts or decrypts `data` in place in CFB-128 mode with the 16-byte `iv`; any length.
  void cfb128(const uint8_t iv[16], ByteView data, bool decrypt) const;

 private:
  uint32_t round_keys_[60];
  uint32_t rounds_ = 0;
};

// Single DES (FIPS 46-3) and CBC mode: RFC 3414's privacy protocol. The 8-byte key's parity bits are ignored.
class Des {
 public:
  void set_key(const uint8_t key[8]);
  void encrypt_block(const uint8_t in[8], uint8_t out[8]) const { block(in, out, false); }
  void decrypt_block(const uint8_t in[8], uint8_t out[8]) const { block(in, out, true); }
  // CBC over whole 8-byte blocks, in place; `data.size()` must be a multiple of 8.
  void cbc(const uint8_t iv[8], ByteView data, bool decrypt) const;

 private:
  void block(const uint8_t in[8], uint8_t out[8], bool decrypt) const;
  uint64_t subkeys_[16];
};

}  // namespace crypto
