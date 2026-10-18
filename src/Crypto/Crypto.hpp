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

// Hash functions and HMAC (RFC 2104), block ciphers and the modes SNMPv3 and TLS use. Plain portable code. The hashes, HMAC, ChaCha20,
// Poly1305, GHASH and the comparison helper are constant time; AES and DES look up S-boxes by data, so their timing can leak through the
// cache (the usual trade-off of table-based AES on machines without AES instructions). Keys and state are on the caller's stack.
// MD5 and SHA-1 are here because the protocols that need them (SNMPv3 USM, RFC 3414) do: do not use them for anything new.

enum class HashKind : uint8_t { Md5, Sha1, Sha224, Sha256, Sha384, Sha512 };
constexpr uint32_t hash_size(HashKind k) {
  return k == HashKind::Md5 ? 16 : k == HashKind::Sha1 ? 20 : k == HashKind::Sha224 ? 28 : k == HashKind::Sha256 ? 32 : k == HashKind::Sha384 ? 48 : 64;
}
constexpr uint32_t hash_block_size(HashKind k) { return k == HashKind::Sha384 || k == HashKind::Sha512 ? 128 : 64; }
constexpr uint32_t kMaxHashSize = 64, kMaxHashBlock = 128;

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
  uint64_t h64_[8];  // SHA-384 / SHA-512
  uint64_t total_ = 0;
  uint8_t buffer_[kMaxHashBlock];
  uint32_t buffered_ = 0;
};

// One-shot hash of `data`.
void hash(HashKind kind, ConstByteView data, uint8_t* out);
// HMAC of `data` under `key` into `out` (hash_size(kind) bytes).
void hmac(HashKind kind, ConstByteView key, ConstByteView data, uint8_t* out);
// True if the first `n` bytes of a and b are equal; takes the same time wherever they differ.
bool equal_constant_time(const uint8_t* a, const uint8_t* b, uint32_t n);

// HKDF (RFC 5869). hkdf_expand() fills `out` (at most 255 hash lengths) and is false if it asks for more.
void hkdf_extract(HashKind kind, ConstByteView salt, ConstByteView ikm, uint8_t* prk);
bool hkdf_expand(HashKind kind, ConstByteView prk, ConstByteView info, ByteView out);

// MD4 (RFC 1320), 16 bytes into `out`. Broken as a hash: it is here because NTLM's password hash is defined with it. Nothing new may use it.
void md4(ConstByteView data, uint8_t out[16]);

// The counter mode key derivation of NIST SP 800-108 with HMAC-SHA-256 as the PRF (what SMB 3 derives its keys with): `out.size()` bytes
// (at most 32) from `key`, `label` and `context` (both used as given, a zero byte between them, the length in bits at the end).
bool kdf_hmac_sha256(ConstByteView key, ConstByteView label, ConstByteView context, ByteView out);

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

// AES-CMAC (RFC 4493) of `data` under a 128-bit key: 16 bytes into `mac`. False for a key that is not 16 bytes.
bool aes_cmac(ConstByteView key, ConstByteView data, uint8_t mac[16]);

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
