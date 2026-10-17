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
#include "Allocators/IAllocator.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/Ecc.hpp"
#include "Crypto/Rsa.hpp"
#include "Utility/ArrayView.hpp"

namespace crypto {

// X.509 certificates (RFC 5280): parsing of the fields TLS needs, signature checks, and chain validation against a set of trust anchors.
// Parsed certificates only point into the DER bytes they were parsed from, which must stay valid.
//
// What is checked: the signatures along the chain (RSA PKCS#1 v1.5 and PSS, ECDSA on P-256 / P-384; SHA-256/384/512, SHA-1 only if the
// options allow it), validity dates, that issuers are CAs with the keyCertSign usage and a path length that is not exceeded, the leaf's
// extended key usage, and the host name or IP address against the subject alternative names. Critical extensions that are not understood
// make a certificate invalid. Not checked: revocation (CRL, OCSP), name constraints (a certificate with that extension marked critical is
// refused), certificate policies, Ed25519 / RSA-PSS keys.

enum class KeyType : uint8_t { Unsupported, Rsa, EcP256, EcP384 };
enum class SignatureAlgorithm : uint8_t {
  Unknown,
  RsaSha1, RsaSha256, RsaSha384, RsaSha512,
  RsaPssSha256, RsaPssSha384, RsaPssSha512,
  EcdsaSha1, EcdsaSha256, EcdsaSha384, EcdsaSha512,
};

struct Certificate {
  ConstByteView der, tbs, issuer, subject, serial;
  ConstByteView signature_algorithm;   // the AlgorithmIdentifier as encoded
  ConstByteView signature;             // the BIT STRING's bytes
  SignatureAlgorithm sig_alg = SignatureAlgorithm::Unknown;
  uint64_t not_before = 0, not_after = 0;   // seconds since 1970
  KeyType key_type = KeyType::Unsupported;
  ConstByteView rsa_modulus, rsa_exponent;
  ConstByteView ec_point;
  bool has_basic_constraints = false, is_ca = false;
  int32_t path_length = -1;            // -1: no limit
  bool has_key_usage = false;
  uint16_t key_usage = 0;              // bit n set when the KeyUsage bit n is (digitalSignature is bit 0, keyCertSign bit 5)
  bool has_ext_key_usage = false, server_auth = false;
  ConstByteView san;                   // the GeneralNames SEQUENCE's content, empty if none
  ConstByteView subject_key_id, authority_key_id;

  static constexpr uint16_t kDigitalSignature = 1 << 0, kKeyEncipherment = 1 << 2, kKeyAgreement = 1 << 4, kKeyCertSign = 1 << 5;
  bool self_issued() const;            // issuer name == subject name
};

enum class CertStatus : uint8_t {
  Ok,
  Malformed,
  UnsupportedAlgorithm,
  UnknownCriticalExtension,
  NoCertificates,
  UntrustedRoot,         // no chain to a trust anchor was found
  BadSignature,
  NotYetValid,
  Expired,
  NotCa,                 // an issuer that is not a CA, or lacks keyCertSign
  PathLengthExceeded,
  BadLeafUsage,          // extended key usage without serverAuth
  HostnameMismatch,
  WeakKey,
  ChainTooLong,
};
const char* cert_status_name(CertStatus s);

// Parses one DER certificate. Malformed, UnknownCriticalExtension or Ok (unsupported key or signature algorithms parse but are
// flagged in key_type / sig_alg).
CertStatus parse_certificate(ConstByteView der, Certificate* out);

// Seconds since 1970 for a date (UTC); 0 on invalid fields.
uint64_t unix_time(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute, uint32_t second);

// Checks that `cert` was signed by the key in `issuer`.
CertStatus verify_signature(const Certificate& cert, const Certificate& issuer);
// Checks a signature made with a certificate's key over a digest already computed (what TLS does with the handshake signature).
// `ecdsa_der` is a DER ECDSA-Sig-Value. For RSA, `padding` selects PKCS#1 v1.5 or PSS.
bool verify_digest_signature(const Certificate& signer, bool ecdsa, RsaPadding padding, HashKind hash, const uint8_t* digest, ConstByteView signature);
// Splits a DER ECDSA-Sig-Value into the integers' bytes (without the leading zero a positive INTEGER may carry).
bool parse_ecdsa_signature(ConstByteView der, ConstByteView* r, ConstByteView* s);

// Does the certificate's subject alternative name cover this DNS name (case-insensitive; "*.example.com" matches one label)? Or this IPv4 address?
bool certificate_matches_host(const Certificate& cert, const char* name);
bool certificate_matches_ipv4(const Certificate& cert, uint32_t address);

// Certificates the machine trusts. Entries are DER; add() copies, add_borrowed() keeps pointing at the caller's bytes (a certificate
// bundle compiled into the program).
class TrustStore {
 public:
  explicit TrustStore(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~TrustStore();
  TrustStore(const TrustStore&) = delete;
  TrustStore& operator=(const TrustStore&) = delete;

  bool add(ConstByteView der);
  bool add_borrowed(ConstByteView der);
  // Adds every "-----BEGIN CERTIFICATE-----" block of a PEM text; returns how many were added (0 if there were none or memory ran out).
  uint32_t add_pem(const char* text, uint64_t length);
  uint32_t count() const { return count_; }
  ConstByteView at(uint32_t index) const { return entries_[index].der; }

 private:
  struct Entry {
    ConstByteView der;
    bool owned = false;
  };
  bool grow();
  allocators::IAllocator& allocator_;
  Entry* entries_ = nullptr;
  uint32_t count_ = 0, capacity_ = 0;
};

struct VerifyOptions {
  uint64_t now = 0;                 // seconds since 1970; 0 skips the validity check (the machine has no trusted clock)
  const char* hostname = nullptr;   // checked against the leaf's SAN if set
  uint32_t ipv4 = 0;                // or this address, if nonzero and no hostname
  bool allow_sha1 = false;
  uint32_t min_rsa_bits = 2048;
  uint32_t max_depth = 8;
  bool require_server_auth = true;
};

// Validates a chain as a TLS server sends it: certs[0] is the leaf, the others are intermediates in any order. The chain must end in a
// certificate that is in `trust` (a self-signed server certificate that was added to the store as it is is accepted on its own).
CertStatus verify_chain(const ConstByteView* certs, uint32_t count, const TrustStore& trust, const VerifyOptions& options, Certificate* leaf = nullptr);

// Base64 (standard alphabet, whitespace skipped). Returns the decoded length, or -1 if the text is not valid base64 or `out` is too small.
int64_t base64_decode(const char* text, uint64_t length, uint8_t* out, uint64_t capacity);

}  // namespace crypto
