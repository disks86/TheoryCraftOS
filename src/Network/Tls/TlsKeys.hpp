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
#include "Crypto/Aead.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/Random.hpp"

namespace network::tls {

// TLS 1.2 (RFC 5246, with RFC 5289 / 7905 suites) and TLS 1.3 (RFC 8446) building blocks: the cipher suite table, key derivation and the
// record protection (AEAD) layer. The handshake itself is in TlsSocket.

// The record protection of a suite: the AEAD ones (TLS 1.2 and 1.3), and AES-CBC with an HMAC (TLS 1.2 only, RFC 5246 6.2.3.2; encrypt-then-MAC when the
// server agrees to RFC 7366).
enum class AeadKind : uint8_t { Aes128Gcm, Aes256Gcm, ChaCha20Poly1305, Aes128Cbc, Aes256Cbc };
enum class SuiteAuth : uint8_t { Any, Rsa, Ecdsa };   // the kind of server certificate a TLS 1.2 suite needs

struct CipherSuite {
  uint16_t id;
  const char* name;
  AeadKind aead;
  crypto::HashKind hash;   // the PRF hash (TLS 1.2) or the HKDF hash (TLS 1.3)
  bool tls13;
  SuiteAuth auth;
  crypto::HashKind mac = crypto::HashKind::Sha256;   // the HMAC of the records of a CBC suite
  bool cbc() const { return aead == AeadKind::Aes128Cbc || aead == AeadKind::Aes256Cbc; }
};
const CipherSuite* find_suite(uint16_t id);
constexpr uint32_t aead_key_size(AeadKind k) { return k == AeadKind::Aes128Gcm || k == AeadKind::Aes128Cbc ? 16 : 32; }

// Content types
constexpr uint8_t kChangeCipherSpec = 20, kAlert = 21, kHandshake = 22, kApplicationData = 23;
constexpr uint32_t kMaxPlaintext = 16384;
constexpr uint32_t kMaxRecordPayload = kMaxPlaintext + 2048;   // what a TLS 1.2 peer may send; TLS 1.3 allows 256 bytes of expansion
constexpr uint32_t kRecordHeader = 5;

// Protects (seal) or unprotects (open) records in one direction with one key set. Before keys are set the layer is inactive and records pass
// as they are. `seal` and `open` keep the record sequence number.
class RecordProtection {
 public:
  bool active() const { return active_; }
  void reset();
  // TLS 1.3: `iv` is the 12-byte write_iv. TLS 1.2 AES-GCM: `iv` is the 4-byte implicit salt (the explicit nonce is the sequence number);
  // TLS 1.2 ChaCha20-Poly1305: the 12-byte IV XORed with the sequence number. `iv_length` says which.
  bool set_keys(AeadKind kind, bool tls13, const uint8_t* key, const uint8_t* iv, uint32_t iv_length);
  // TLS 1.2 AES-CBC: the encryption key, the MAC key (as long as the MAC) and whether the MAC covers the ciphertext (encrypt-then-MAC). `random` makes the
  // IV of each record we send.
  bool set_cbc_keys(AeadKind kind, crypto::HashKind mac, const uint8_t* key, const uint8_t* mac_key, bool encrypt_then_mac, crypto::IRandom* random);
  // Builds the record for `plain` (header included) into `out`; returns its length, 0 if `capacity` is too small. Inactive: a plain record.
  uint32_t seal(uint8_t type, ConstByteView plain, uint8_t* out, uint32_t capacity);
  // Decrypts `payload` (the record without its header) in place. On success `*plain` is the plaintext inside the payload and `*type` the
  // content type (the real one for TLS 1.3). False if the record does not authenticate or is malformed. Inactive: the payload as it is.
  bool open(uint8_t header_type, uint8_t* payload, uint32_t length, ConstByteView* plain, uint8_t* type);
  uint64_t sequence() const { return sequence_; }

 private:
  bool active_ = false, tls13_ = false;
  AeadKind kind_ = AeadKind::Aes128Gcm;
  crypto::AesGcm gcm_;
  crypto::ChaCha20Poly1305 chacha_;
  // AES-CBC
  uint32_t seal_cbc(uint8_t type, ConstByteView plain, uint8_t* out, uint32_t capacity);
  bool open_cbc(uint8_t header_type, uint8_t* payload, uint32_t length, ConstByteView* plain);
  void cbc_mac(uint8_t type, uint32_t length_field, ConstByteView data, uint8_t* out) const;
  bool cbc() const { return kind_ == AeadKind::Aes128Cbc || kind_ == AeadKind::Aes256Cbc; }
  crypto::Aes aes_;
  crypto::HashKind mac_hash_ = crypto::HashKind::Sha256;
  uint8_t mac_key_[48] = {};
  uint32_t mac_size_ = 0;
  bool etm_ = false;
  crypto::IRandom* random_ = nullptr;
  uint8_t iv_[12] = {};
  uint32_t iv_length_ = 0;
  uint64_t sequence_ = 0;
};

// TLS 1.2 PRF (RFC 5246 5): P_hash(secret, label | seed1 | seed2). `out` of any length.
void tls12_prf(crypto::HashKind hash, ConstByteView secret, const char* label, ConstByteView seed1, ConstByteView seed2, ByteView out);

// TLS 1.3: HKDF-Expand-Label (RFC 8446 7.1) and Derive-Secret. `secret` is hash-length bytes; derive_secret's `transcript_hash` is hash-length too.
bool hkdf_expand_label(crypto::HashKind hash, ConstByteView secret, const char* label, ConstByteView context, ByteView out);
bool derive_secret(crypto::HashKind hash, ConstByteView secret, const char* label, ConstByteView transcript_hash, uint8_t* out);

}  // namespace network::tls
