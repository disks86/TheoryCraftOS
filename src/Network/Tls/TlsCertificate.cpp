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

#include "Network/Tls/TlsCertificate.hpp"
#include "Network/Tls/TlsWire.hpp"

namespace network::tls {

using namespace crypto;
using namespace tlswire;

bool check_server_certificate(allocators::IAllocator& allocator, const TlsConfig& config, uint32_t peer_ipv4, ConstByteView body, bool tls13, SuiteAuth suite_auth, ByteBuffer* leaf_der,
                              CertificateProblem* problem) {
  auto refuse = [&](CertStatus status, uint8_t alert, TlsError error) {
    problem->status = status;
    problem->alert = alert;
    problem->error = error;
    return false;
  };
  Reader r(body);
  if (tls13) {
    const uint8_t context = r.u8();
    r.take(context);
  }
  const uint32_t list_length = r.u24();
  Reader list(r.take(list_length));
  if (!r.done()) return refuse(CertStatus::Malformed, kAlertDecode, TlsError::Protocol);
  ConstByteView certs[10];
  uint32_t count = 0;
  while (list.n > 0) {
    const uint32_t cl = list.u24();
    const ConstByteView cert = list.take(cl);
    if (tls13) list.take(list.u16());
    if (!list.ok || cl == 0) return refuse(CertStatus::Malformed, kAlertDecode, TlsError::Protocol);
    if (count == 10) return refuse(CertStatus::ChainTooLong, kAlertBadCertificate, TlsError::Certificate);
    certs[count++] = cert;
  }
  if (count == 0) return refuse(CertStatus::NoCertificates, tls13 ? kAlertCertificateUnknown : kAlertHandshakeFailure, TlsError::Certificate);
  Certificate leaf;
  if (config.verify_certificates) {
    TrustStore empty(allocator);
    VerifyOptions o;
    o.now = config.clock ? config.clock(config.clock_context) : 0;
    o.hostname = config.server_name;
    if (!o.hostname) o.ipv4 = config.server_ipv4 ? config.server_ipv4 : peer_ipv4;
    if (!o.hostname && o.ipv4 == 0 && !config.skip_hostname_check)   // nothing to check the certificate against: refuse rather than trust any host
      return refuse(CertStatus::HostnameMismatch, kAlertBadCertificate, TlsError::Certificate);
    if (config.skip_hostname_check) {
      o.hostname = nullptr;
      o.ipv4 = 0;
    }
    o.min_rsa_bits = config.min_rsa_bits;
    o.allow_sha1 = config.allow_sha1_certificates;
    const CertStatus status = verify_chain(certs, count, config.trust ? *config.trust : empty, o, &leaf);
    if (status != CertStatus::Ok) {
      return refuse(status,
                    status == CertStatus::Expired || status == CertStatus::NotYetValid ? kAlertCertificateExpired
                    : status == CertStatus::UntrustedRoot                              ? kAlertUnknownCa
                    : status == CertStatus::UnsupportedAlgorithm || status == CertStatus::UnknownCriticalExtension ? kAlertUnsupportedCertificate
                    : status == CertStatus::WeakKey                                    ? kAlertInsufficientSecurity
                                                                                       : kAlertBadCertificate,
                    TlsError::Certificate);
    }
  } else if (parse_certificate(certs[0], &leaf) != CertStatus::Ok) {
    return refuse(CertStatus::Malformed, kAlertBadCertificate, TlsError::Certificate);
  }
  // the key must suit the connection
  if (leaf.key_type == KeyType::Unsupported || (leaf.has_key_usage && !(leaf.key_usage & Certificate::kDigitalSignature)))
    return refuse(CertStatus::UnsupportedAlgorithm, kAlertUnsupportedCertificate, TlsError::Certificate);
  if (!tls13 && ((suite_auth == SuiteAuth::Rsa) != (leaf.key_type == KeyType::Rsa))) return refuse(CertStatus::UnsupportedAlgorithm, kAlertUnsupportedCertificate, TlsError::Certificate);
  leaf_der->clear();
  if (!leaf_der->append(certs[0])) return refuse(CertStatus::Ok, kAlertInternal, TlsError::Internal);
  problem->status = CertStatus::Ok;
  return true;
}

bool verify_leaf_signature(ConstByteView leaf_der, uint16_t scheme, ConstByteView content, ConstByteView signature, bool tls13) {
  Certificate leaf;
  if (parse_certificate(leaf_der, &leaf) != CertStatus::Ok) return false;
  bool ecdsa = false, pss = false;
  switch (scheme) {
    // TLS 1.3 ties the ECDSA scheme to the curve; TLS 1.2 only names the hash, so any EC key will do
    case kSigEcdsaP256: ecdsa = true; if (tls13 ? leaf.key_type != KeyType::EcP256 : (leaf.key_type != KeyType::EcP256 && leaf.key_type != KeyType::EcP384)) return false; break;
    case kSigEcdsaP384: ecdsa = true; if (tls13 ? leaf.key_type != KeyType::EcP384 : (leaf.key_type != KeyType::EcP256 && leaf.key_type != KeyType::EcP384)) return false; break;
    case kSigPssSha256: case kSigPssSha384: case kSigPssSha512: pss = true; if (leaf.key_type != KeyType::Rsa) return false; break;
    case kSigPkcs1Sha256: case kSigPkcs1Sha384: case kSigPkcs1Sha512: if (tls13 || leaf.key_type != KeyType::Rsa) return false; break;
    default: return false;
  }
  const HashKind h = scheme_hash(scheme);
  uint8_t digest[kMaxHashSize];
  hash(h, content, digest);
  return verify_digest_signature(leaf, ecdsa, pss ? RsaPadding::Pss : RsaPadding::Pkcs1V15, h, digest, signature);
}

}  // namespace network::tls
