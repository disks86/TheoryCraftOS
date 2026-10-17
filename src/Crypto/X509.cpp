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

#include "Crypto/X509.hpp"
#include "Utility/AllocatedView.hpp"

namespace crypto {

namespace {

// ---- DER

struct Tlv {
  uint8_t tag = 0;
  ConstByteView content;
  ConstByteView whole;   // tag, length and content
};

// Reads one TLV from the front of `*in` (definite lengths, tags below 31, contents under 16 MiB).
bool read_tlv(ConstByteView* in, Tlv* out) {
  const uint64_t size = in->size();
  if (size < 2) return false;
  const uint8_t tag = (*in)[0];
  if ((tag & 0x1f) == 0x1f) return false;
  uint64_t length = (*in)[1], header = 2;
  if (length & 0x80) {
    const uint32_t bytes = length & 0x7f;
    if (bytes == 0 || bytes > 3 || size < 2 + bytes) return false;
    length = 0;
    for (uint32_t i = 0; i < bytes; ++i) length = (length << 8) | (*in)[2 + i];
    header = 2 + bytes;
    if (length < 128) return false;   // not minimal
  }
  if (header + length > size) return false;
  out->tag = tag;
  out->content = in->subview(header, length);
  out->whole = in->first(header + length);
  *in = in->subview(header + length);
  return true;
}

bool expect(ConstByteView* in, uint8_t tag, Tlv* out) { return read_tlv(in, out) && out->tag == tag; }

constexpr uint8_t kInteger = 0x02, kBitString = 0x03, kOctetString = 0x04, kNull = 0x05, kOid = 0x06, kUtcTime = 0x17, kGeneralizedTime = 0x18, kSequence = 0x30, kSet = 0x31, kBoolean = 0x01;

bool bytes_equal(ConstByteView a, ConstByteView b) {
  if (a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) return false;
  return true;
}
template <uint32_t N>
bool oid_is(ConstByteView oid, const uint8_t (&known)[N]) { return bytes_equal(oid, ConstByteView(known, N)); }

// OID contents
const uint8_t kOidRsaEncryption[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01};
const uint8_t kOidSha1Rsa[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x05};
const uint8_t kOidSha256Rsa[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b};
const uint8_t kOidSha384Rsa[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c};
const uint8_t kOidSha512Rsa[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d};
const uint8_t kOidRsaPss[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a};
const uint8_t kOidMgf1[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x08};
const uint8_t kOidEcPublicKey[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
const uint8_t kOidP256[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
const uint8_t kOidP384[] = {0x2b, 0x81, 0x04, 0x00, 0x22};
const uint8_t kOidEcdsaSha1[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x01};
const uint8_t kOidEcdsaSha256[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02};
const uint8_t kOidEcdsaSha384[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03};
const uint8_t kOidEcdsaSha512[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04};
const uint8_t kOidSha256[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01};
const uint8_t kOidSha384[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02};
const uint8_t kOidSha512[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03};
// extensions (2.5.29.x)
const uint8_t kExtSubjectKeyId[] = {0x55, 0x1d, 0x0e};
const uint8_t kExtKeyUsage[] = {0x55, 0x1d, 0x0f};
const uint8_t kExtSan[] = {0x55, 0x1d, 0x11};
const uint8_t kExtIssuerAlt[] = {0x55, 0x1d, 0x12};
const uint8_t kExtBasicConstraints[] = {0x55, 0x1d, 0x13};
const uint8_t kExtCrlDp[] = {0x55, 0x1d, 0x1f};
const uint8_t kExtPolicies[] = {0x55, 0x1d, 0x20};
const uint8_t kExtAuthorityKeyId[] = {0x55, 0x1d, 0x23};
const uint8_t kExtPolicyConstraints[] = {0x55, 0x1d, 0x24};
const uint8_t kExtExtKeyUsage[] = {0x55, 0x1d, 0x25};
const uint8_t kExtFreshestCrl[] = {0x55, 0x1d, 0x2e};
const uint8_t kExtInhibitAny[] = {0x55, 0x1d, 0x36};
const uint8_t kExtAia[] = {0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x01};
const uint8_t kExtSia[] = {0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x0b};
const uint8_t kEkuServerAuth[] = {0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
const uint8_t kEkuAny[] = {0x55, 0x1d, 0x25, 0x00};

HashKind hash_of(SignatureAlgorithm a) {
  switch (a) {
    case SignatureAlgorithm::RsaSha1: case SignatureAlgorithm::EcdsaSha1: return HashKind::Sha1;
    case SignatureAlgorithm::RsaSha256: case SignatureAlgorithm::RsaPssSha256: case SignatureAlgorithm::EcdsaSha256: return HashKind::Sha256;
    case SignatureAlgorithm::RsaSha384: case SignatureAlgorithm::RsaPssSha384: case SignatureAlgorithm::EcdsaSha384: return HashKind::Sha384;
    default: return HashKind::Sha512;
  }
}
bool is_ecdsa(SignatureAlgorithm a) { return a >= SignatureAlgorithm::EcdsaSha1; }
bool is_pss(SignatureAlgorithm a) { return a >= SignatureAlgorithm::RsaPssSha256 && a <= SignatureAlgorithm::RsaPssSha512; }
bool is_sha1(SignatureAlgorithm a) { return a == SignatureAlgorithm::RsaSha1 || a == SignatureAlgorithm::EcdsaSha1; }

// The hash of a PSS parameter set's AlgorithmIdentifier (OID content), or Unknown.
bool hash_from_oid(ConstByteView oid, SignatureAlgorithm* rsa_pss_alg) {
  if (oid_is(oid, kOidSha256)) *rsa_pss_alg = SignatureAlgorithm::RsaPssSha256;
  else if (oid_is(oid, kOidSha384)) *rsa_pss_alg = SignatureAlgorithm::RsaPssSha384;
  else if (oid_is(oid, kOidSha512)) *rsa_pss_alg = SignatureAlgorithm::RsaPssSha512;
  else return false;
  return true;
}

// RSASSA-PSS-params (RFC 4055): only the combinations with the same hash for the digest and MGF1 and a salt as long as the hash.
SignatureAlgorithm parse_pss_params(ConstByteView params) {
  ConstByteView in = params;
  Tlv seq;
  if (!expect(&in, kSequence, &seq)) return SignatureAlgorithm::Unknown;
  ConstByteView body = seq.content;
  SignatureAlgorithm alg = SignatureAlgorithm::Unknown, mgf_alg = SignatureAlgorithm::Unknown;
  bool have_hash = false, have_mgf = false;
  uint32_t salt = 20;
  while (!body.empty()) {
    Tlv field;
    if (!read_tlv(&body, &field)) return SignatureAlgorithm::Unknown;
    ConstByteView inner = field.content;
    Tlv value;
    if (field.tag == 0xa0) {   // hashAlgorithm
      if (!expect(&inner, kSequence, &value)) return SignatureAlgorithm::Unknown;
      ConstByteView alg_id = value.content;
      Tlv oid;
      if (!expect(&alg_id, kOid, &oid) || !hash_from_oid(oid.content, &alg)) return SignatureAlgorithm::Unknown;
      have_hash = true;
    } else if (field.tag == 0xa1) {   // maskGenAlgorithm
      if (!expect(&inner, kSequence, &value)) return SignatureAlgorithm::Unknown;
      ConstByteView alg_id = value.content;
      Tlv oid, hash_seq;
      if (!expect(&alg_id, kOid, &oid) || !oid_is(oid.content, kOidMgf1) || !expect(&alg_id, kSequence, &hash_seq)) return SignatureAlgorithm::Unknown;
      ConstByteView hash_id = hash_seq.content;
      Tlv hoid;
      if (!expect(&hash_id, kOid, &hoid) || !hash_from_oid(hoid.content, &mgf_alg)) return SignatureAlgorithm::Unknown;
      have_mgf = true;
    } else if (field.tag == 0xa2) {   // saltLength
      if (!expect(&inner, kInteger, &value) || value.content.size() != 1) return SignatureAlgorithm::Unknown;
      salt = value.content[0];
    } else if (field.tag != 0xa3) {   // trailerField (must be the default, 1)
      return SignatureAlgorithm::Unknown;
    }
  }
  if (!have_hash || !have_mgf || alg != mgf_alg || salt != hash_size(hash_of(alg))) return SignatureAlgorithm::Unknown;
  return alg;
}

SignatureAlgorithm parse_signature_algorithm(ConstByteView alg_id_content) {
  ConstByteView in = alg_id_content;
  Tlv oid;
  if (!expect(&in, kOid, &oid)) return SignatureAlgorithm::Unknown;
  if (oid_is(oid.content, kOidSha256Rsa)) return SignatureAlgorithm::RsaSha256;
  if (oid_is(oid.content, kOidSha384Rsa)) return SignatureAlgorithm::RsaSha384;
  if (oid_is(oid.content, kOidSha512Rsa)) return SignatureAlgorithm::RsaSha512;
  if (oid_is(oid.content, kOidSha1Rsa)) return SignatureAlgorithm::RsaSha1;
  if (oid_is(oid.content, kOidEcdsaSha256)) return SignatureAlgorithm::EcdsaSha256;
  if (oid_is(oid.content, kOidEcdsaSha384)) return SignatureAlgorithm::EcdsaSha384;
  if (oid_is(oid.content, kOidEcdsaSha512)) return SignatureAlgorithm::EcdsaSha512;
  if (oid_is(oid.content, kOidEcdsaSha1)) return SignatureAlgorithm::EcdsaSha1;
  if (oid_is(oid.content, kOidRsaPss)) return parse_pss_params(in);
  return SignatureAlgorithm::Unknown;
}

bool parse_time(const Tlv& t, uint64_t* out) {
  const ConstByteView c = t.content;
  const bool utc = t.tag == kUtcTime;
  if (!utc && t.tag != kGeneralizedTime) return false;
  const uint64_t digits = utc ? 12 : 14;
  if (c.size() != digits + 1 || c[digits] != 'Z') return false;
  uint32_t v[7];
  uint32_t pos = 0;
  auto num = [&](uint32_t n, uint32_t* result) {
    *result = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t d = c[pos++];
      if (d < '0' || d > '9') return false;
      *result = *result * 10 + (d - '0');
    }
    return true;
  };
  if (utc) {
    if (!num(2, &v[0])) return false;
    v[0] += v[0] >= 50 ? 1900 : 2000;
  } else if (!num(4, &v[0])) {
    return false;
  }
  for (int i = 1; i < 6; ++i)
    if (!num(2, &v[i])) return false;
  const uint64_t t64 = unix_time(v[0], v[1], v[2], v[3], v[4], v[5]);
  if (t64 == 0) return false;
  *out = t64;
  return true;
}

// A name's CN attribute value for diagnostics is not needed; names are compared as DER.

bool parse_extensions(ConstByteView content, Certificate* c) {
  ConstByteView list = content;
  Tlv seq;
  if (!expect(&list, kSequence, &seq) || !list.empty()) return false;
  ConstByteView exts = seq.content;
  while (!exts.empty()) {
    Tlv ext;
    if (!expect(&exts, kSequence, &ext)) return false;
    ConstByteView fields = ext.content;
    Tlv oid, value;
    if (!expect(&fields, kOid, &oid)) return false;
    bool critical = false;
    Tlv next;
    if (!read_tlv(&fields, &next)) return false;
    if (next.tag == kBoolean) {
      if (next.content.size() != 1) return false;
      critical = next.content[0] != 0;
      if (!read_tlv(&fields, &next)) return false;
    }
    if (next.tag != kOctetString || !fields.empty()) return false;
    value = next;
    ConstByteView body = value.content;
    if (oid_is(oid.content, kExtBasicConstraints)) {
      Tlv s;
      if (!expect(&body, kSequence, &s) || !body.empty()) return false;
      c->has_basic_constraints = true;
      ConstByteView in = s.content;
      Tlv f;
      bool have = false;
      if (!in.empty()) {
        if (!read_tlv(&in, &f)) return false;
        have = true;
      }
      if (have && f.tag == kBoolean) {
        if (f.content.size() != 1) return false;
        c->is_ca = f.content[0] != 0;
        have = false;
        if (!in.empty()) {
          if (!read_tlv(&in, &f)) return false;
          have = true;
        }
      }
      if (have) {
        if (f.tag != kInteger || f.content.empty() || f.content.size() > 2 || (f.content[0] & 0x80) || !in.empty()) return false;
        c->path_length = f.content.size() == 1 ? f.content[0] : (f.content[0] << 8 | f.content[1]);
      }
    } else if (oid_is(oid.content, kExtKeyUsage)) {
      Tlv bits;
      if (!expect(&body, kBitString, &bits) || bits.content.size() < 2 || bits.content[0] > 7) return false;
      c->has_key_usage = true;
      c->key_usage = 0;
      for (uint32_t i = 0; i < 8; ++i)
        if (bits.content[1] & (0x80 >> i)) c->key_usage |= static_cast<uint16_t>(1 << i);
      if (bits.content.size() > 2 && (bits.content[2] & 0x80)) c->key_usage |= 1 << 8;
    } else if (oid_is(oid.content, kExtExtKeyUsage)) {
      Tlv s;
      if (!expect(&body, kSequence, &s) || !body.empty()) return false;
      c->has_ext_key_usage = true;
      ConstByteView in = s.content;
      while (!in.empty()) {
        Tlv o;
        if (!expect(&in, kOid, &o)) return false;
        if (oid_is(o.content, kEkuServerAuth) || oid_is(o.content, kEkuAny)) c->server_auth = true;
      }
    } else if (oid_is(oid.content, kExtSan)) {
      Tlv s;
      if (!expect(&body, kSequence, &s) || !body.empty()) return false;
      c->san = s.content;
    } else if (oid_is(oid.content, kExtSubjectKeyId)) {
      Tlv s;
      if (!expect(&body, kOctetString, &s)) return false;
      c->subject_key_id = s.content;
    } else if (oid_is(oid.content, kExtAuthorityKeyId)) {
      Tlv s;
      if (!expect(&body, kSequence, &s)) return false;
      ConstByteView in = s.content;
      while (!in.empty()) {
        Tlv f;
        if (!read_tlv(&in, &f)) return false;
        if (f.tag == 0x80) c->authority_key_id = f.content;   // keyIdentifier [0]
      }
    } else if (oid_is(oid.content, kExtIssuerAlt) || oid_is(oid.content, kExtCrlDp) || oid_is(oid.content, kExtPolicies) || oid_is(oid.content, kExtFreshestCrl) ||
               oid_is(oid.content, kExtAia) || oid_is(oid.content, kExtSia) || oid_is(oid.content, kExtInhibitAny) || oid_is(oid.content, kExtPolicyConstraints)) {
      // understood well enough to ignore
    } else if (critical) {
      return false;   // unknown critical extension (name constraints included)
    }
  }
  return true;
}

}  // namespace

bool Certificate::self_issued() const { return bytes_equal(issuer, subject); }

const char* cert_status_name(CertStatus s) {
  switch (s) {
    case CertStatus::Ok: return "ok";
    case CertStatus::Malformed: return "malformed certificate";
    case CertStatus::UnsupportedAlgorithm: return "unsupported algorithm";
    case CertStatus::UnknownCriticalExtension: return "unknown critical extension";
    case CertStatus::NoCertificates: return "no certificates";
    case CertStatus::UntrustedRoot: return "no path to a trusted certificate";
    case CertStatus::BadSignature: return "bad signature";
    case CertStatus::NotYetValid: return "not yet valid";
    case CertStatus::Expired: return "expired";
    case CertStatus::NotCa: return "issuer is not a CA";
    case CertStatus::PathLengthExceeded: return "path length exceeded";
    case CertStatus::BadLeafUsage: return "certificate not for server authentication";
    case CertStatus::HostnameMismatch: return "host name does not match";
    case CertStatus::WeakKey: return "key too weak";
    case CertStatus::ChainTooLong: return "chain too long";
  }
  return "?";
}

uint64_t unix_time(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute, uint32_t second) {
  if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 60) return 0;
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (day > static_cast<uint32_t>(kDays[month - 1] + (month == 2 && leap ? 1 : 0))) return 0;
  // days from civil (Howard Hinnant)
  const int64_t y = static_cast<int64_t>(year) - (month <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const uint64_t yoe = static_cast<uint64_t>(y - era * 400);
  const uint64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
  return static_cast<uint64_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

CertStatus parse_certificate(ConstByteView der, Certificate* out) {
  *out = Certificate();
  ConstByteView in = der;
  Tlv cert;
  if (!expect(&in, kSequence, &cert) || !in.empty()) return CertStatus::Malformed;
  out->der = cert.whole;
  ConstByteView body = cert.content;
  Tlv tbs, sig_alg, sig;
  if (!expect(&body, kSequence, &tbs)) return CertStatus::Malformed;
  if (!expect(&body, kSequence, &sig_alg)) return CertStatus::Malformed;
  if (!expect(&body, kBitString, &sig) || !body.empty() || sig.content.size() < 1 || sig.content[0] != 0) return CertStatus::Malformed;
  out->tbs = tbs.whole;
  out->signature_algorithm = sig_alg.whole;
  out->signature = sig.content.subview(1);
  out->sig_alg = parse_signature_algorithm(sig_alg.content);

  ConstByteView t = tbs.content;
  Tlv field;
  if (!read_tlv(&t, &field)) return CertStatus::Malformed;
  uint32_t version = 1;
  if (field.tag == 0xa0) {   // [0] version
    ConstByteView v = field.content;
    Tlv vi;
    if (!expect(&v, kInteger, &vi) || vi.content.size() != 1 || vi.content[0] > 2) return CertStatus::Malformed;
    version = vi.content[0] + 1u;
    if (!read_tlv(&t, &field)) return CertStatus::Malformed;
  }
  if (field.tag != kInteger || field.content.empty()) return CertStatus::Malformed;
  out->serial = field.content;
  Tlv inner_alg;
  if (!expect(&t, kSequence, &inner_alg) || !bytes_equal(inner_alg.whole, sig_alg.whole)) return CertStatus::Malformed;
  Tlv issuer, validity, subject, spki;
  if (!expect(&t, kSequence, &issuer) || !expect(&t, kSequence, &validity) || !expect(&t, kSequence, &subject) || !expect(&t, kSequence, &spki)) return CertStatus::Malformed;
  out->issuer = issuer.whole;
  out->subject = subject.whole;
  ConstByteView vt = validity.content;
  Tlv nb, na;
  if (!read_tlv(&vt, &nb) || !read_tlv(&vt, &na) || !vt.empty() || !parse_time(nb, &out->not_before) || !parse_time(na, &out->not_after)) return CertStatus::Malformed;

  // SubjectPublicKeyInfo
  ConstByteView sp = spki.content;
  Tlv key_alg, key_bits;
  if (!expect(&sp, kSequence, &key_alg) || !expect(&sp, kBitString, &key_bits) || !sp.empty() || key_bits.content.empty() || key_bits.content[0] != 0) return CertStatus::Malformed;
  ConstByteView ka = key_alg.content;
  Tlv key_oid;
  if (!expect(&ka, kOid, &key_oid)) return CertStatus::Malformed;
  const ConstByteView key = key_bits.content.subview(1);
  if (oid_is(key_oid.content, kOidRsaEncryption)) {
    ConstByteView k = key;
    Tlv seq, n, e;
    if (!expect(&k, kSequence, &seq) || !k.empty()) return CertStatus::Malformed;
    ConstByteView kb = seq.content;
    if (!expect(&kb, kInteger, &n) || !expect(&kb, kInteger, &e) || !kb.empty() || n.content.empty() || e.content.empty()) return CertStatus::Malformed;
    out->rsa_modulus = n.content;
    out->rsa_exponent = e.content;
    out->key_type = KeyType::Rsa;
  } else if (oid_is(key_oid.content, kOidEcPublicKey)) {
    Tlv curve;
    if (!expect(&ka, kOid, &curve)) return CertStatus::Malformed;
    if (oid_is(curve.content, kOidP256) && key.size() == 65) out->key_type = KeyType::EcP256;
    else if (oid_is(curve.content, kOidP384) && key.size() == 97) out->key_type = KeyType::EcP384;
    out->ec_point = key;
  }

  // optional fields: issuerUniqueID [1], subjectUniqueID [2], extensions [3]
  while (!t.empty()) {
    Tlv f;
    if (!read_tlv(&t, &f)) return CertStatus::Malformed;
    if (f.tag == 0xa3) {
      if (version != 3 || !t.empty()) return CertStatus::Malformed;
      if (!parse_extensions(f.content, out)) return CertStatus::UnknownCriticalExtension;
    } else if (f.tag != 0x81 && f.tag != 0x82) {
      return CertStatus::Malformed;
    }
  }
  return CertStatus::Ok;
}

bool parse_ecdsa_signature(ConstByteView der, ConstByteView* r, ConstByteView* s) {
  ConstByteView in = der;
  Tlv seq, ri, si;
  if (!expect(&in, kSequence, &seq) || !in.empty()) return false;
  ConstByteView body = seq.content;
  if (!expect(&body, kInteger, &ri) || !expect(&body, kInteger, &si) || !body.empty()) return false;
  if (ri.content.empty() || si.content.empty() || (ri.content[0] & 0x80) || (si.content[0] & 0x80)) return false;
  *r = ri.content;
  *s = si.content;
  return true;
}

bool verify_digest_signature(const Certificate& signer, bool ecdsa, RsaPadding padding, HashKind hash, const uint8_t* digest, ConstByteView signature) {
  if (ecdsa) {
    if (signer.key_type != KeyType::EcP256 && signer.key_type != KeyType::EcP384) return false;
    ConstByteView r, s;
    if (!parse_ecdsa_signature(signature, &r, &s)) return false;
    return ecdsa_verify(signer.key_type == KeyType::EcP256 ? EcCurve::P256 : EcCurve::P384, signer.ec_point, digest, hash_size(hash), r, s);
  }
  if (signer.key_type != KeyType::Rsa) return false;
  return rsa_verify(padding, hash, signer.rsa_modulus, signer.rsa_exponent, digest, signature);
}

CertStatus verify_signature(const Certificate& cert, const Certificate& issuer) {
  if (cert.sig_alg == SignatureAlgorithm::Unknown || issuer.key_type == KeyType::Unsupported) return CertStatus::UnsupportedAlgorithm;
  const HashKind hash = hash_of(cert.sig_alg);
  uint8_t digest[kMaxHashSize];
  crypto::hash(hash, cert.tbs, digest);
  const bool ecdsa = is_ecdsa(cert.sig_alg);
  if (ecdsa != (issuer.key_type != KeyType::Rsa)) return CertStatus::BadSignature;
  return verify_digest_signature(issuer, ecdsa, is_pss(cert.sig_alg) ? RsaPadding::Pss : RsaPadding::Pkcs1V15, hash, digest, cert.signature) ? CertStatus::Ok : CertStatus::BadSignature;
}

// ---- host names

namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// Does the DNS name `pattern` (from a certificate, possibly "*.example.com") match `host`?
bool dns_match(ConstByteView pattern, const char* host) {
  uint64_t hn = 0;
  while (host[hn]) ++hn;
  if (hn > 0 && host[hn - 1] == '.') --hn;
  uint64_t pn = pattern.size();
  if (pn > 0 && pattern[pn - 1] == '.') --pn;
  if (hn == 0 || pn == 0) return false;
  uint64_t p_start = 0, h_start = 0;
  if (pn >= 2 && pattern[0] == '*' && pattern[1] == '.') {
    // the wildcard stands for exactly one non-empty label, and what follows must have at least two labels
    uint64_t dots = 0;
    for (uint64_t i = 2; i < pn; ++i)
      if (pattern[i] == '.') ++dots;
    if (dots < 1) return false;
    uint64_t first_dot = 0;
    while (first_dot < hn && host[first_dot] != '.') ++first_dot;
    if (first_dot == 0 || first_dot >= hn) return false;
    p_start = 1;
    h_start = first_dot;
  }
  if (pn - p_start != hn - h_start) return false;
  for (uint64_t i = 0; i < pn - p_start; ++i)
    if (lower(static_cast<char>(pattern[p_start + i])) != lower(host[h_start + i])) return false;
  return true;
}
}  // namespace

bool certificate_matches_host(const Certificate& cert, const char* name) {
  ConstByteView in = cert.san;
  while (!in.empty()) {
    Tlv g;
    if (!read_tlv(&in, &g)) return false;
    if (g.tag == 0x82 && dns_match(g.content, name)) return true;   // dNSName
  }
  return false;
}

bool certificate_matches_ipv4(const Certificate& cert, uint32_t address) {
  ConstByteView in = cert.san;
  while (!in.empty()) {
    Tlv g;
    if (!read_tlv(&in, &g)) return false;
    if (g.tag == 0x87 && g.content.size() == 4) {   // iPAddress
      const uint32_t v = static_cast<uint32_t>(g.content[0]) << 24 | static_cast<uint32_t>(g.content[1]) << 16 | static_cast<uint32_t>(g.content[2]) << 8 | g.content[3];
      if (v == address) return true;
    }
  }
  return false;
}

// ---- trust store

TrustStore::~TrustStore() {
  for (uint32_t i = 0; i < count_; ++i)
    if (entries_[i].owned) allocator_.deallocate(const_cast<uint8_t*>(entries_[i].der.data()));
  allocator_.deallocate(entries_);
}

bool TrustStore::grow() {
  if (count_ < capacity_) return true;
  const uint32_t capacity = capacity_ ? capacity_ * 2 : 16;
  Entry* bigger = static_cast<Entry*>(allocator_.allocate(sizeof(Entry) * capacity));
  if (!bigger) return false;
  for (uint32_t i = 0; i < count_; ++i) bigger[i] = entries_[i];   // Entry is trivially copyable
  allocator_.deallocate(entries_);
  entries_ = bigger;
  capacity_ = capacity;
  return true;
}

bool TrustStore::add_borrowed(ConstByteView der) {
  Certificate c;
  if (parse_certificate(der, &c) == CertStatus::Malformed || !grow()) return false;
  entries_[count_] = Entry{der, false};
  ++count_;
  return true;
}

bool TrustStore::add(ConstByteView der) {
  Certificate c;
  if (parse_certificate(der, &c) == CertStatus::Malformed || !grow()) return false;
  uint8_t* copy = static_cast<uint8_t*>(allocator_.allocate(der.size()));
  if (!copy) return false;
  for (uint64_t i = 0; i < der.size(); ++i) copy[i] = der[i];
  entries_[count_] = Entry{ConstByteView(copy, der.size()), true};
  ++count_;
  return true;
}

int64_t base64_decode(const char* text, uint64_t length, uint8_t* out, uint64_t capacity) {
  uint64_t n = 0;
  uint32_t bits = 0, have = 0;
  uint32_t pad = 0;
  for (uint64_t i = 0; i < length; ++i) {
    const char c = text[i];
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=') { ++pad; continue; }
    else if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    else return -1;
    if (pad) return -1;   // data after the padding
    bits = (bits << 6) | static_cast<uint32_t>(v);
    have += 6;
    if (have >= 8) {
      have -= 8;
      if (n >= capacity) return -1;
      out[n++] = static_cast<uint8_t>(bits >> have);
      bits &= (1u << have) - 1;
    }
  }
  if (pad > 2 || have >= 6) return -1;
  return static_cast<int64_t>(n);
}

uint32_t TrustStore::add_pem(const char* text, uint64_t length) {
  static const char kBegin[] = "-----BEGIN CERTIFICATE-----";
  static const char kEnd[] = "-----END CERTIFICATE-----";
  uint32_t added = 0;
  uint64_t pos = 0;
  auto find = [&](const char* needle, uint64_t from) -> int64_t {
    uint64_t nl = 0;
    while (needle[nl]) ++nl;
    for (uint64_t i = from; i + nl <= length; ++i) {
      uint64_t k = 0;
      while (k < nl && text[i + k] == needle[k]) ++k;
      if (k == nl) return static_cast<int64_t>(i);
    }
    return -1;
  };
  while (true) {
    const int64_t b = find(kBegin, pos);
    if (b < 0) break;
    const uint64_t body = static_cast<uint64_t>(b) + sizeof(kBegin) - 1;
    const int64_t e = find(kEnd, body);
    if (e < 0) break;
    const uint64_t body_length = static_cast<uint64_t>(e) - body;
    uint8_t* der = static_cast<uint8_t*>(allocator_.allocate(body_length));   // base64 text is longer than what it encodes
    if (!der) break;
    const int64_t n = base64_decode(text + body, body_length, der, body_length);
    if (n > 0 && grow()) {
      Certificate c;
      if (parse_certificate(ConstByteView(der, static_cast<uint64_t>(n)), &c) != CertStatus::Malformed) {
        entries_[count_] = Entry{ConstByteView(der, static_cast<uint64_t>(n)), true};
        ++count_;
        ++added;
        der = nullptr;
      }
    }
    allocator_.deallocate(der);
    pos = static_cast<uint64_t>(e) + sizeof(kEnd) - 1;
  }
  return added;
}

// ---- chain validation

namespace {

CertStatus check_time(const Certificate& c, uint64_t now) {
  if (now == 0) return CertStatus::Ok;
  if (now < c.not_before) return CertStatus::NotYetValid;
  if (now > c.not_after) return CertStatus::Expired;
  return CertStatus::Ok;
}

CertStatus check_key_strength(const Certificate& c, const VerifyOptions& o) {
  if (c.key_type == KeyType::Rsa && rsa_modulus_bits(c.rsa_modulus) < o.min_rsa_bits) return CertStatus::WeakKey;
  return CertStatus::Ok;
}

bool names_match(const Certificate& cert, const Certificate& issuer) { return bytes_equal(cert.issuer, issuer.subject); }

// May `issuer` issue certificates? (basic constraints, key usage); `below` is the number of intermediate CAs between it and the leaf.
CertStatus check_issuer(const Certificate& issuer, uint32_t below) {
  if (!issuer.has_basic_constraints || !issuer.is_ca) return CertStatus::NotCa;
  if (issuer.has_key_usage && !(issuer.key_usage & Certificate::kKeyCertSign)) return CertStatus::NotCa;
  if (issuer.path_length >= 0 && below > static_cast<uint32_t>(issuer.path_length)) return CertStatus::PathLengthExceeded;
  return CertStatus::Ok;
}

}  // namespace

CertStatus verify_chain(const ConstByteView* certs, uint32_t count, const TrustStore& trust, const VerifyOptions& options, Certificate* leaf_out) {
  constexpr uint32_t kMaxChain = 12;
  if (count == 0) return CertStatus::NoCertificates;
  if (count > kMaxChain) return CertStatus::ChainTooLong;
  Certificate parsed[kMaxChain];
  for (uint32_t i = 0; i < count; ++i) {
    const CertStatus s = parse_certificate(certs[i], &parsed[i]);
    if (s != CertStatus::Ok) return i == 0 ? s : (s == CertStatus::Malformed ? CertStatus::Malformed : s);
  }
  const Certificate& leaf = parsed[0];
  if (leaf_out) *leaf_out = leaf;

  // the leaf: usage, host name
  if (options.require_server_auth) {
    if (leaf.has_ext_key_usage && !leaf.server_auth) return CertStatus::BadLeafUsage;
  }
  if (options.hostname) {
    if (!certificate_matches_host(leaf, options.hostname)) return CertStatus::HostnameMismatch;
  } else if (options.ipv4 != 0) {
    if (!certificate_matches_ipv4(leaf, options.ipv4)) return CertStatus::HostnameMismatch;
  }

  // walk up: at each step the current certificate is either a trust anchor itself, or is signed by an anchor, or by a supplied intermediate
  bool used[kMaxChain] = {};
  used[0] = true;
  const Certificate* current = &leaf;
  CertStatus failure = CertStatus::UntrustedRoot;
  for (uint32_t depth = 0; depth <= options.max_depth; ++depth) {
    CertStatus s = check_time(*current, options.now);
    if (s != CertStatus::Ok) return s;
    s = check_key_strength(*current, options);
    if (s != CertStatus::Ok) return s;
    // is the current certificate a trust anchor as it stands (identical DER)?
    for (uint32_t i = 0; i < trust.count(); ++i)
      if (bytes_equal(trust.at(i), current->der)) return CertStatus::Ok;
    if (is_sha1(current->sig_alg) && !options.allow_sha1) return CertStatus::UnsupportedAlgorithm;
    // signed by an anchor?
    for (uint32_t i = 0; i < trust.count(); ++i) {
      Certificate anchor;
      if (parse_certificate(trust.at(i), &anchor) != CertStatus::Ok || !names_match(*current, anchor)) continue;
      s = check_issuer(anchor, depth);
      if (s != CertStatus::Ok) { failure = s; continue; }
      if (verify_signature(*current, anchor) != CertStatus::Ok) { failure = CertStatus::BadSignature; continue; }
      s = check_time(anchor, options.now);
      if (s != CertStatus::Ok) return s;
      return CertStatus::Ok;
    }
    // or by one of the supplied intermediates
    const Certificate* next = nullptr;
    uint32_t next_index = 0;
    for (uint32_t i = 1; i < count; ++i) {
      if (used[i] || !names_match(*current, parsed[i])) continue;
      const CertStatus issuer_status = check_issuer(parsed[i], depth);
      if (issuer_status != CertStatus::Ok) { failure = issuer_status; continue; }
      if (verify_signature(*current, parsed[i]) != CertStatus::Ok) { failure = CertStatus::BadSignature; continue; }
      next = &parsed[i];
      next_index = i;
      break;
    }
    if (!next) return failure;
    used[next_index] = true;
    current = next;
  }
  return CertStatus::ChainTooLong;
}

}  // namespace crypto
