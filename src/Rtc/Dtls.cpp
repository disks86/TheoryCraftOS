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

#include "Rtc/Dtls.hpp"
#include "Crypto/X509.hpp"
#include "Network/Tls/TlsKeys.hpp"
#include "Utility/Move.hpp"

namespace rtc {

namespace {

using crypto::EcCurve;
using crypto::HashKind;
using crypto::Certificate;
using crypto::CertStatus;
using crypto::KeyType;
using crypto::parse_certificate;
using crypto::parse_ecdsa_signature;
using network::tls::tls12_prf;

constexpr uint16_t kVersion = 0xFEFD, kSuite = 0xC02B, kCurveP256 = 23, kSchemeEcdsaSha256 = 0x0403;
constexpr uint8_t kTypeChangeCipherSpec = 20, kTypeAlert = 21, kTypeHandshake = 22, kTypeApplication = 23;
constexpr uint8_t kClientHello = 1, kServerHello = 2, kHelloVerify = 3, kCertificate = 11, kServerKeyExchange = 12, kCertificateRequest = 13, kServerHelloDone = 14,
                  kCertificateVerify = 15, kClientKeyExchange = 16, kFinished = 20;
constexpr uint64_t kMaxMessage = 16384, kFragmentBody = 1000;
constexpr uint32_t kRecordHeader = 13, kMaxRetransmits = 7;

bool tlv(ByteBuffer* out, uint8_t tag, ConstByteView content) {
  const uint64_t n = content.size();
  if (!out->put8(tag)) return false;
  if (n < 128) {
    if (!out->put8(static_cast<uint8_t>(n))) return false;
  } else if (n < 256) {
    if (!out->put8(0x81) || !out->put8(static_cast<uint8_t>(n))) return false;
  } else if (n < 65536) {
    if (!out->put8(0x82) || !out->put8(static_cast<uint8_t>(n >> 8)) || !out->put8(static_cast<uint8_t>(n))) return false;
  } else {
    return false;
  }
  return out->append(content);
}

// A positive INTEGER from big-endian bytes: leading zeros dropped, one added when the top bit is set.
bool der_integer(allocators::IAllocator& a, ByteBuffer* out, ConstByteView be) {
  uint64_t skip = 0;
  while (skip + 1 < be.size() && be[skip] == 0) ++skip;
  ByteBuffer body(a);
  if ((be[skip] & 0x80) && !body.put8(0)) return false;
  return body.append(be.subview(skip)) && tlv(out, 0x02, body.view());
}

bool der_signature(allocators::IAllocator& a, const uint8_t raw[64], ByteBuffer* out) {
  ByteBuffer inner(a);
  out->clear();
  return der_integer(a, &inner, ConstByteView(raw, 32)) && der_integer(a, &inner, ConstByteView(raw + 32, 32)) && tlv(out, 0x30, inner.view());
}

bool name_cn(allocators::IAllocator& a, StringView cn, ByteBuffer* out) {
  ByteBuffer oid(a), value(a), attr(a), set(a);
  static const uint8_t kCn[] = {0x55, 0x04, 0x03};
  return tlv(&oid, 0x06, ConstByteView(kCn, 3)) && tlv(&value, 0x0C, ConstByteView(reinterpret_cast<const uint8_t*>(cn.data()), cn.size())) && attr.append(oid.view()) && attr.append(value.view()) &&
         tlv(&set, 0x30, attr.view()) && tlv(out, 0x31, set.view()) ? true : false;
}

ConstByteView bytes(StringView s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

}  // namespace

bool fingerprint_bytes(StringView text, uint8_t out[32]) {
  if (text.size() != 32 * 3 - 1) return false;
  for (uint32_t i = 0; i < 32; ++i) {
    uint32_t v = 0;
    for (uint32_t j = 0; j < 2; ++j) {
      const char c = text[i * 3 + j];
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (d < 0) return false;
      v = v * 16 + static_cast<uint32_t>(d);
    }
    if (i + 1 < 32 && text[i * 3 + 2] != ':') return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

bool DtlsIdentity::generate(crypto::IRandom& random, StringView cn) {
  ready_ = false;
  uint8_t point[crypto::kMaxEcPoint], serial[8];
  if (!crypto::ec_generate_private(EcCurve::P256, random, private_key_) || !crypto::ec_public_key(EcCurve::P256, private_key_, point) || !random.fill(ByteView(serial, 8))) return false;
  serial[0] &= 0x7F;
  serial[7] |= 1;
  static const uint8_t kEcdsaSha256[] = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
  static const uint8_t kEcPublicKey[] = {0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01}, kP256[] = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
  static const char kNotBefore[] = "240101000000Z", kNotAfter[] = "491231235959Z";
  allocators::IAllocator& a = allocator_;
  ByteBuffer tbs(a), version(a), serial_int(a), alg(a), alg_seq(a), name(a), name_seq(a), validity(a), validity_seq(a), alg_id(a), spki_alg(a), key_bits(a), spki(a), body(a);
  ByteBuffer v2(a);
  static const uint8_t kV3[] = {0x02, 0x01, 0x02};
  if (!tlv(&version, 0xA0, ConstByteView(kV3, 3)) || !der_integer(a, &serial_int, ConstByteView(serial, 8)) || !alg.append(ConstByteView(kEcdsaSha256, sizeof(kEcdsaSha256))) || !tlv(&alg_seq, 0x30, alg.view())) return false;
  ByteBuffer cn_attr(a);
  if (!name_cn(a, cn, &cn_attr) || !tlv(&name_seq, 0x30, cn_attr.view())) return false;
  ByteBuffer times(a);
  if (!tlv(&times, 0x17, bytes(kNotBefore)) || !tlv(&times, 0x17, bytes(kNotAfter)) || !tlv(&validity_seq, 0x30, times.view())) return false;
  ByteBuffer spki_inner(a), alg_pair(a);
  if (!alg_pair.append(ConstByteView(kEcPublicKey, sizeof(kEcPublicKey))) || !alg_pair.append(ConstByteView(kP256, sizeof(kP256))) || !tlv(&spki_inner, 0x30, alg_pair.view()) ||
      !key_bits.put8(0) || !key_bits.append(ConstByteView(point, 65)) || !tlv(&spki_inner, 0x03, key_bits.view()) || !tlv(&spki, 0x30, spki_inner.view()))
    return false;
  if (!body.append(version.view()) || !body.append(serial_int.view()) || !body.append(alg_seq.view()) || !body.append(name_seq.view()) || !body.append(validity_seq.view()) ||
      !body.append(name_seq.view()) || !body.append(spki.view()) || !tlv(&tbs, 0x30, body.view()))
    return false;
  uint8_t digest[32], raw[64];
  crypto::hash(HashKind::Sha256, tbs.view(), digest);
  if (!crypto::ecdsa_sign(EcCurve::P256, private_key_, digest, 32, random, raw)) return false;
  ByteBuffer sig_der(a), sig_bits(a), cert_body(a);
  if (!der_signature(a, raw, &sig_der) || !sig_bits.put8(0) || !sig_bits.append(sig_der.view())) return false;
  if (!cert_body.append(tbs.view()) || !cert_body.append(alg_seq.view()) || !tlv(&cert_body, 0x03, sig_bits.view())) return false;
  certificate_.clear();
  if (!tlv(&certificate_, 0x30, cert_body.view())) return false;
  ready_ = true;
  return true;
}

void DtlsIdentity::fingerprint(String* out) const {
  uint8_t digest[32];
  static const char kHex[] = "0123456789ABCDEF";
  crypto::hash(HashKind::Sha256, certificate_.view(), digest);
  out->clear();
  for (uint32_t i = 0; i < 32; ++i) {
    if (i) out->push_back(':');
    out->push_back(kHex[digest[i] >> 4]);
    out->push_back(kHex[digest[i] & 15]);
  }
}

DtlsTransport::DtlsTransport(allocators::IAllocator& allocator, const DtlsIdentity& identity, crypto::IRandom& random)
    : allocator_(allocator), identity_(identity), random_(random), transcript_(allocator), fragment_(allocator), fragment_have_(allocator), outbox_(allocator), flight_(allocator), app_in_(allocator) {}

bool DtlsTransport::set_expected_fingerprint(StringView algorithm, StringView hex) {
  has_expected_ = false;
  if (!algorithm.equals_ignore_case("sha-256") || !fingerprint_bytes(hex, expected_fp_)) return false;
  has_expected_ = true;
  return true;
}

bool DtlsTransport::srtp_keys(SrtpProfile* profile, uint8_t ck[32], uint8_t sk[32], uint8_t cs[14], uint8_t ss[14]) const {
  if (state_ != State::Connected || !srtp_profile_known(srtp_profile_)) return false;
  const SrtpProfile p = static_cast<SrtpProfile>(srtp_profile_);
  const uint32_t kl = srtp_key_size(p), sl = srtp_salt_size(p);
  uint8_t material[2 * (32 + 14)], seed_pad = 0;
  (void)seed_pad;
  uint8_t seeds[64];
  for (int i = 0; i < 32; ++i) {
    seeds[i] = client_random_[i];
    seeds[32 + i] = server_random_[i];
  }
  tls12_prf(HashKind::Sha256, ConstByteView(master_, 48), "EXTRACTOR-dtls_srtp", ConstByteView(seeds, 32), ConstByteView(seeds + 32, 32), ByteView(material, 2 * (kl + sl)));
  for (uint32_t i = 0; i < kl; ++i) {
    ck[i] = material[i];
    sk[i] = material[kl + i];
  }
  for (uint32_t i = 0; i < sl; ++i) {
    cs[i] = material[2 * kl + i];
    ss[i] = material[2 * kl + sl + i];
  }
  *profile = p;
  return true;
}

// --- Output ------------------------------------------------------------------------------------------------------------------------

bool DtlsTransport::fail(const char* why, uint8_t description) {
  if (state_ == State::Failed || state_ == State::Closed) return false;
  failure_ = why;
  alert(2, description);
  state_ = State::Failed;
  return false;
}

void DtlsTransport::alert(uint8_t level, uint8_t description) {
  const uint8_t body[2] = {level, description};
  send_record(kTypeAlert, ConstByteView(body, 2), false);
}

void DtlsTransport::close() {
  if (state_ == State::Closed) return;
  if (state_ == State::Connected) alert(1, 0);   // close_notify
  state_ = State::Closed;
}

bool DtlsTransport::send_record(uint8_t type, ConstByteView payload, bool flight, int epoch_override) {
  const uint16_t epoch = epoch_override >= 0 ? static_cast<uint16_t>(epoch_override) : write_epoch_;
  const uint8_t slot = epoch > 1 ? 1 : static_cast<uint8_t>(epoch);
  const uint64_t sequence = write_seq_[slot]++;
  UniquePointer<ByteBuffer> rec = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
  if (!rec) return false;
  ByteBuffer& r = *rec;
  uint8_t seq6[8];
  store_be16(ByteView(seq6, 8), 0, epoch);
  for (int i = 0; i < 6; ++i) seq6[2 + i] = static_cast<uint8_t>(sequence >> (8 * (5 - i)));
  const bool encrypt = epoch >= 1 && keys_ready_;
  const uint64_t body_len = encrypt ? 8 + payload.size() + 16 : payload.size();
  if (body_len > 1300 || !r.put8(type) || !put_be16(&r, kVersion) || !r.append(ConstByteView(seq6, 8)) || !put_be16(&r, static_cast<uint32_t>(body_len))) return false;
  if (!encrypt) {
    if (!r.append(payload)) return false;
  } else {
    uint8_t nonce[12], aad[13], tag[16];
    for (int i = 0; i < 4; ++i) nonce[i] = write_keys_.iv[i];
    for (int i = 0; i < 8; ++i) nonce[4 + i] = seq6[i];
    for (int i = 0; i < 8; ++i) aad[i] = seq6[i];
    aad[8] = type;
    store_be16(ByteView(aad, 13), 9, kVersion);
    store_be16(ByteView(aad, 13), 11, static_cast<uint32_t>(payload.size()));
    if (!r.append(ConstByteView(seq6, 8))) return false;
    const uint64_t start = r.size();
    if (!r.append(payload) || !r.put_zeros(16)) return false;
    write_keys_.gcm.seal(nonce, ConstByteView(aad, 13), r.writable_view().subview(start, payload.size()), tag);
    r.writable_view().subview(start + payload.size(), 16).copy_from(ConstByteView(tag, 16));
  }
  if (flight) {   // (kept as type, epoch and payload: sent again it gets new record numbers, or the peer would take it for a replay)
    UniquePointer<ByteBuffer> copy = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
    if (copy && copy->put8(type) && put_be16(copy.get(), epoch) && copy->append(payload)) flight_.push_back(util::move(copy));
  }
  return outbox_.push_back(util::move(rec));
}

void DtlsTransport::add_transcript(uint8_t type, uint16_t sequence, ConstByteView body) {
  transcript_.put8(type);
  transcript_.put8(static_cast<uint8_t>(body.size() >> 16));
  put_be16(&transcript_, static_cast<uint32_t>(body.size()));
  put_be16(&transcript_, sequence);
  put_be24(&transcript_, 0);
  put_be24(&transcript_, static_cast<uint32_t>(body.size()));
  transcript_.append(body);
}

void DtlsTransport::transcript_hash(uint8_t out[32]) const { crypto::hash(HashKind::Sha256, transcript_.view(), out); }

bool DtlsTransport::send_handshake(uint8_t type, ConstByteView body) {
  const uint16_t seq = send_message_seq_++;
  add_transcript(type, seq, body);
  uint64_t offset = 0;
  do {
    const uint64_t n = body.size() - offset < kFragmentBody ? body.size() - offset : kFragmentBody;
    ByteBuffer m(allocator_);
    if (!m.put8(type) || !put_be24(&m, static_cast<uint32_t>(body.size())) || !put_be16(&m, seq) || !put_be24(&m, static_cast<uint32_t>(offset)) || !put_be24(&m, static_cast<uint32_t>(n)) ||
        !m.append(body.subview(offset, n)) || !send_record(kTypeHandshake, m.view(), true))
      return false;
    offset += n;
  } while (offset < body.size());
  return true;
}

void DtlsTransport::begin_flight() {
  flight_.clear();
  retransmits_ = 0;
  timeout_ms_ = 1000;
  retransmit_at_ = now_ms_ + timeout_ms_;
}

void DtlsTransport::resend_flight() {
  for (const UniquePointer<ByteBuffer>& f : flight_) {
    const ConstByteView v = f->view();
    if (v.size() >= 3) send_record(v[0], v.subview(3), false, load_be16(v, 1));
  }
  last_resend_ms_ = now_ms_;
}

bool DtlsTransport::next_datagram(ByteBuffer* out) {
  if (outbox_.count() == 0) return false;
  out->clear();
  out->append(outbox_[0]->view());
  outbox_.remove_at(0);
  return true;
}

void DtlsTransport::tick(uint64_t now) {
  now_ms_ = now;
  if (state_ != State::Connecting || flight_.count() == 0 || now < retransmit_at_) return;
  if (++retransmits_ > kMaxRetransmits) {
    fail("the handshake timed out", 80);
    return;
  }
  resend_flight();
  timeout_ms_ = timeout_ms_ * 2 > 16000 ? 16000 : timeout_ms_ * 2;
  retransmit_at_ = now + timeout_ms_;
}

bool DtlsTransport::send_application(ConstByteView data) {
  if (state_ != State::Connected || data.size() > 1100) return false;
  return send_record(kTypeApplication, data, false);
}

bool DtlsTransport::receive_application(ByteBuffer* out) {
  if (app_in_.count() == 0) return false;
  out->clear();
  out->append(app_in_[0]->view());
  app_in_.remove_at(0);
  return true;
}

// --- Handshake pieces -----------------------------------------------------------------------------------------------------------------

void DtlsTransport::hello_extensions(ByteBuffer* out, bool server) {
  ByteBuffer e(allocator_);
  if (!server) {
    static const uint8_t kGroups[] = {0x00, 0x0A, 0x00, 0x04, 0x00, 0x02, 0x00, 0x17}, kFormats[] = {0x00, 0x0B, 0x00, 0x02, 0x01, 0x00},
                         kSigs[] = {0x00, 0x0D, 0x00, 0x06, 0x00, 0x04, 0x04, 0x03, 0x04, 0x01},
                         kSrtp[] = {0x00, 0x0E, 0x00, 0x0B, 0x00, 0x08, 0x00, 0x07, 0x00, 0x01, 0x00, 0x02, 0x00, 0x08, 0x00};
    e.append(ConstByteView(kGroups, sizeof(kGroups)));
    e.append(ConstByteView(kFormats, sizeof(kFormats)));
    e.append(ConstByteView(kSigs, sizeof(kSigs)));
    e.append(ConstByteView(kSrtp, sizeof(kSrtp)));
  } else if (srtp_profile_ != 0) {
    const uint8_t srtp[] = {0x00, 0x0E, 0x00, 0x05, 0x00, 0x02, static_cast<uint8_t>(srtp_profile_ >> 8), static_cast<uint8_t>(srtp_profile_), 0x00};
    e.append(ConstByteView(srtp, sizeof(srtp)));
  }
  static const uint8_t kEms[] = {0x00, 0x17, 0x00, 0x00}, kReneg[] = {0xFF, 0x01, 0x00, 0x01, 0x00};
  if (!server || peer_ems_) e.append(ConstByteView(kEms, sizeof(kEms)));
  e.append(ConstByteView(kReneg, sizeof(kReneg)));
  put_be16(out, static_cast<uint32_t>(e.size()));
  out->append(e.view());
}

bool DtlsTransport::parse_hello_extensions(ConstByteView ext, bool server) {
  uint64_t at = 0;
  while (at < ext.size()) {
    if (ext.size() - at < 4) return false;
    const uint16_t type = load_be16(ext, at);
    const uint64_t len = load_be16(ext, at + 2);
    if (len > ext.size() - at - 4) return false;
    const ConstByteView body = ext.subview(at + 4, len);
    if (type == 23) {
      peer_ems_ = true;
    } else if (type == 14) {   // use_srtp: profiles<2..>, mki<0..255>
      if (body.size() < 3) return false;
      const uint64_t list = load_be16(body, 0);
      if (list + 3 > body.size() || (list & 1)) return false;
      if (server) {   // we are the server: our preference among the client's offers
        static const uint16_t kPreference[] = {7, 1, 2, 8};
        for (const uint16_t want : kPreference)
          for (uint64_t i = 0; i < list && srtp_profile_ == 0; i += 2)
            if (load_be16(body, 2 + i) == want) srtp_profile_ = want;
      } else {
        if (list != 2) return false;
        const uint16_t chosen = load_be16(body, 2);
        if (chosen != 7 && chosen != 1 && chosen != 2 && chosen != 8) return false;
        srtp_profile_ = chosen;
      }
    }
    at += 4 + len;
  }
  return true;
}

void DtlsTransport::start(uint64_t now) {
  now_ms_ = now;
  if (state_ != State::New) return;
  state_ = State::Connecting;
  if (role_ == Role::Server) {
    step_ = Step::ClientHello;
    return;
  }
  if (cookie_size_ == 0) random_.fill(ByteView(client_random_, 32));   // (the hello sent again with a cookie keeps its random)
  step_ = Step::ServerHello;
  begin_flight();
  ByteBuffer m(allocator_);
  put_be16(&m, kVersion);
  m.append(ConstByteView(client_random_, 32));
  m.put8(0);                       // session id
  m.put8(static_cast<uint8_t>(cookie_size_));
  m.append(ConstByteView(cookie_, cookie_size_));
  put_be16(&m, 2);
  put_be16(&m, kSuite);
  m.put8(1);
  m.put8(0);
  hello_extensions(&m, false);
  send_handshake(kClientHello, m.view());
}

bool DtlsTransport::derive_keys(ConstByteView premaster) {
  uint8_t seeds[64], block[40];
  if (use_ems_) {
    uint8_t h[32];
    transcript_hash(h);
    tls12_prf(HashKind::Sha256, premaster, "extended master secret", ConstByteView(h, 32), ConstByteView(), ByteView(master_, 48));
  } else {
    tls12_prf(HashKind::Sha256, premaster, "master secret", ConstByteView(client_random_, 32), ConstByteView(server_random_, 32), ByteView(master_, 48));
  }
  for (int i = 0; i < 32; ++i) {
    seeds[i] = server_random_[i];
    seeds[32 + i] = client_random_[i];
  }
  tls12_prf(HashKind::Sha256, ConstByteView(master_, 48), "key expansion", ConstByteView(seeds, 32), ConstByteView(seeds + 32, 32), ByteView(block, 40));
  const bool client = role_ == Role::Client;
  if (!(client ? write_keys_ : read_keys_).gcm.set_key(ConstByteView(block, 16)) || !(client ? read_keys_ : write_keys_).gcm.set_key(ConstByteView(block + 16, 16))) return false;
  for (int i = 0; i < 4; ++i) {
    (client ? write_keys_ : read_keys_).iv[i] = block[32 + i];
    (client ? read_keys_ : write_keys_).iv[i] = block[36 + i];
  }
  keys_ready_ = true;
  return true;
}

bool DtlsTransport::sign(const uint8_t digest[32], ByteBuffer* der) {
  uint8_t raw[64];
  return crypto::ecdsa_sign(EcCurve::P256, identity_.private_key(), digest, 32, random_, raw) && der_signature(allocator_, raw, der);
}

bool DtlsTransport::verify(const uint8_t digest[32], uint16_t scheme, ConstByteView signature) {
  if (scheme != kSchemeEcdsaSha256 || !have_peer_key_) return false;
  ConstByteView r, s;
  return parse_ecdsa_signature(signature, &r, &s) && crypto::ecdsa_verify(EcCurve::P256, ConstByteView(peer_public_, 65), digest, 32, r, s);
}

bool DtlsTransport::take_certificate(ConstByteView body, bool*) {
  if (body.size() < 6) return false;
  const uint64_t total = (uint64_t{body[0]} << 16) | load_be16(body, 1);
  if (total + 3 != body.size()) return false;
  const uint64_t len = (uint64_t{body[3]} << 16) | load_be16(body, 4);
  if (len == 0 || len + 6 > body.size()) return false;
  const ConstByteView der = body.subview(6, len);
  uint8_t digest[32];
  crypto::hash(HashKind::Sha256, der, digest);
  if (!has_expected_ || !crypto::equal_constant_time(digest, expected_fp_, 32)) return fail("the certificate does not match the fingerprint", 42);
  Certificate cert;
  if (parse_certificate(der, &cert) != CertStatus::Ok || cert.key_type != KeyType::EcP256 || cert.ec_point.size() != 65) return fail("unsupported certificate", 43);
  for (int i = 0; i < 65; ++i) peer_public_[i] = cert.ec_point[i];
  have_peer_key_ = true;
  return true;
}

bool DtlsTransport::send_finished() {
  uint8_t h[32], verify_data[12];
  transcript_hash(h);
  tls12_prf(HashKind::Sha256, ConstByteView(master_, 48), role_ == Role::Client ? "client finished" : "server finished", ConstByteView(h, 32), ConstByteView(), ByteView(verify_data, 12));
  return send_handshake(kFinished, ConstByteView(verify_data, 12));
}

bool DtlsTransport::check_finished(ConstByteView body) {
  uint8_t h[32], expected[12];
  transcript_hash(h);
  tls12_prf(HashKind::Sha256, ConstByteView(master_, 48), role_ == Role::Client ? "server finished" : "client finished", ConstByteView(h, 32), ConstByteView(), ByteView(expected, 12));
  return body.size() == 12 && crypto::equal_constant_time(expected, body.data(), 12);
}

bool DtlsTransport::send_client_flight() {
  begin_flight();
  if (cert_requested_) {
    ByteBuffer m(allocator_);
    const ConstByteView der = identity_.certificate();
    put_be24(&m, static_cast<uint32_t>(der.size() + 3));
    put_be24(&m, static_cast<uint32_t>(der.size()));
    m.append(der);
    if (!send_handshake(kCertificate, m.view())) return false;
  }
  uint8_t shared[32];
  if (!crypto::ec_generate_private(EcCurve::P256, random_, ephemeral_private_) || !crypto::ec_public_key(EcCurve::P256, ephemeral_private_, own_point_) ||
      !crypto::ec_ecdh(EcCurve::P256, ephemeral_private_, ConstByteView(peer_point_, 65), shared))
    return fail("key agreement failed", 40);
  ByteBuffer cke(allocator_);
  cke.put8(65);
  cke.append(ConstByteView(own_point_, 65));
  if (!send_handshake(kClientKeyExchange, cke.view()) || !derive_keys(ConstByteView(shared, 32))) return fail("key derivation failed", 80);
  if (cert_requested_) {
    uint8_t h[32];
    ByteBuffer der(allocator_), m(allocator_);
    transcript_hash(h);
    if (!sign(h, &der)) return fail("signing failed", 80);
    put_be16(&m, kSchemeEcdsaSha256);
    put_be16(&m, static_cast<uint32_t>(der.size()));
    m.append(der.view());
    if (!send_handshake(kCertificateVerify, m.view())) return false;
  }
  const uint8_t ccs = 1;
  if (!send_record(kTypeChangeCipherSpec, ConstByteView(&ccs, 1), true)) return false;
  write_epoch_ = 1;
  if (!send_finished()) return false;
  step_ = Step::ChangeCipherSpec;
  return true;
}

bool DtlsTransport::send_server_flight() {
  begin_flight();
  random_.fill(ByteView(server_random_, 32));
  {
    ByteBuffer m(allocator_);
    put_be16(&m, kVersion);
    m.append(ConstByteView(server_random_, 32));
    m.put8(0);
    put_be16(&m, kSuite);
    m.put8(0);
    hello_extensions(&m, true);
    if (!send_handshake(kServerHello, m.view())) return false;
  }
  {
    ByteBuffer m(allocator_);
    const ConstByteView der = identity_.certificate();
    put_be24(&m, static_cast<uint32_t>(der.size() + 3));
    put_be24(&m, static_cast<uint32_t>(der.size()));
    m.append(der);
    if (!send_handshake(kCertificate, m.view())) return false;
  }
  {
    if (!crypto::ec_generate_private(EcCurve::P256, random_, ephemeral_private_) || !crypto::ec_public_key(EcCurve::P256, ephemeral_private_, own_point_)) return fail("key generation failed", 80);
    ByteBuffer params(allocator_), signed_data(allocator_), der(allocator_), m(allocator_);
    params.put8(3);
    put_be16(&params, kCurveP256);
    params.put8(65);
    params.append(ConstByteView(own_point_, 65));
    signed_data.append(ConstByteView(client_random_, 32));
    signed_data.append(ConstByteView(server_random_, 32));
    signed_data.append(params.view());
    uint8_t digest[32];
    crypto::hash(HashKind::Sha256, signed_data.view(), digest);
    if (!sign(digest, &der)) return fail("signing failed", 80);
    m.append(params.view());
    put_be16(&m, kSchemeEcdsaSha256);
    put_be16(&m, static_cast<uint32_t>(der.size()));
    m.append(der.view());
    if (!send_handshake(kServerKeyExchange, m.view())) return false;
  }
  static const uint8_t kRequest[] = {0x01, 0x40, 0x00, 0x02, 0x04, 0x03, 0x00, 0x00};   // ecdsa_sign, ecdsa_secp256r1_sha256, no authorities
  if (!send_handshake(kCertificateRequest, ConstByteView(kRequest, sizeof(kRequest))) || !send_handshake(kServerHelloDone, ConstByteView())) return false;
  step_ = Step::ClientCertificate;
  return true;
}

// --- Input ---------------------------------------------------------------------------------------------------------------------------

void DtlsTransport::receive(ConstByteView d, uint64_t now) {
  now_ms_ = now;
  if (state_ == State::Failed || state_ == State::Closed) return;
  uint64_t at = 0;
  while (d.size() - at >= kRecordHeader) {
    const uint8_t type = d[at];
    const uint16_t epoch = load_be16(d, at + 3);
    const uint64_t seq = (uint64_t{load_be16(d, at + 5)} << 32) | load_be32(d, at + 7), len = load_be16(d, at + 11);
    if (load_be16(d, at + 1) != kVersion && load_be16(d, at + 1) != 0xFEFF) return;   // (not DTLS 1.2: 1.0 is what a hello may carry)
    if (len > d.size() - at - kRecordHeader) return;
    handle_record(type, epoch, seq, d.subview(at + kRecordHeader, len), now);
    if (state_ == State::Failed || state_ == State::Closed) return;
    at += kRecordHeader + len;
  }
}

bool DtlsTransport::unprotect(uint8_t type, uint16_t epoch, uint64_t sequence, ConstByteView in, ByteBuffer* plain) {
  if (in.size() < 8 + 16 || !keys_ready_) return false;
  const uint64_t n = in.size() - 24;
  uint8_t nonce[12], aad[13], tag[16];
  for (int i = 0; i < 4; ++i) nonce[i] = read_keys_.iv[i];
  for (int i = 0; i < 8; ++i) nonce[4 + i] = in[i];
  store_be16(ByteView(aad, 13), 0, epoch);
  for (int i = 0; i < 6; ++i) aad[2 + i] = static_cast<uint8_t>(sequence >> (8 * (5 - i)));
  aad[8] = type;
  store_be16(ByteView(aad, 13), 9, kVersion);
  store_be16(ByteView(aad, 13), 11, static_cast<uint32_t>(n));
  ByteView(tag, 16).copy_from(in.subview(8 + n, 16));
  plain->clear();
  return plain->append(in.subview(8, n)) && read_keys_.gcm.open(nonce, ConstByteView(aad, 13), plain->writable_view(), tag);
}

void DtlsTransport::handle_record(uint8_t type, uint16_t epoch, uint64_t sequence, ConstByteView body, uint64_t now) {
  if (epoch > 1) return;
  const uint8_t slot = static_cast<uint8_t>(epoch);
  if (read_seen_[slot]) {   // the replay window
    if (sequence <= read_highest_[slot]) {
      const uint64_t dist = read_highest_[slot] - sequence;
      if (dist >= 64 || ((read_window_[slot] >> dist) & 1)) return;
    }
  }
  ByteBuffer plain(allocator_);
  ConstByteView content = body;
  if (epoch == 1) {
    if (read_epoch_ < 1 || !unprotect(type, epoch, sequence, body, &plain)) return;
    content = plain.view();
  } else if (read_epoch_ >= 1 && type != kTypeAlert && type != kTypeHandshake && type != kTypeChangeCipherSpec) {
    return;
  }
  if (read_seen_[slot] && sequence <= read_highest_[slot]) {
    read_window_[slot] |= uint64_t{1} << (read_highest_[slot] - sequence);
  } else {
    const uint64_t shift = read_seen_[slot] ? sequence - read_highest_[slot] : 0;
    read_window_[slot] = (read_seen_[slot] && shift < 64) ? (read_window_[slot] << shift) | 1 : 1;
    read_highest_[slot] = sequence;
    read_seen_[slot] = true;
  }
  switch (type) {
    case kTypeChangeCipherSpec:
      if (epoch == 0 && step_ == Step::ChangeCipherSpec && content.size() == 1 && content[0] == 1 && keys_ready_) {
        read_epoch_ = 1;
        step_ = Step::Finished;
      }
      break;
    case kTypeAlert:
      if (content.size() == 2) {
        if (content[0] == 2 || content[1] == 0) {
          failure_ = content[1] == 0 ? "closed by the peer" : "the peer sent an alert";
          state_ = content[1] == 0 ? State::Closed : State::Failed;
        }
      }
      break;
    case kTypeHandshake: {
      uint64_t at = 0;
      while (content.size() - at >= 12) {
        const uint64_t flen = (uint64_t{content[at + 9]} << 16) | load_be16(content, at + 10);
        if (flen > content.size() - at - 12) return;
        handle_handshake_fragment(content.subview(at, 12 + flen));
        if (state_ == State::Failed || state_ == State::Closed) return;
        at += 12 + flen;
      }
      break;
    }
    case kTypeApplication:
      if (epoch == 1 && state_ == State::Connected) {
        if (app_in_.count() >= 256) app_in_.remove_at(0);
        UniquePointer<ByteBuffer> m = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
        if (m && m->append(content)) app_in_.push_back(util::move(m));
      }
      break;
    default: break;
  }
  (void)now;
}

void DtlsTransport::handle_handshake_fragment(ConstByteView f) {
  const uint8_t type = f[0];
  const uint64_t total = (uint64_t{f[1]} << 16) | load_be16(f, 2), offset = (uint64_t{f[6]} << 16) | load_be16(f, 7), length = (uint64_t{f[9]} << 16) | load_be16(f, 10);
  const uint16_t seq = load_be16(f, 4);
  if (total > kMaxMessage || offset > total || length > total - offset) return;
  if (seq < recv_message_seq_) {   // an old message again: the peer did not get our answer
    if (flight_.count() > 0 && now_ms_ - last_resend_ms_ >= 200 && (state_ == State::Connecting || state_ == State::Connected)) resend_flight();
    return;
  }
  if (seq > recv_message_seq_) return;
  const ConstByteView body = f.subview(12, length);
  if (offset == 0 && length == total) {
    fragment_length_ = 0;
    ByteBuffer whole(allocator_);
    whole.append(body);
    const uint16_t this_seq = recv_message_seq_;
    if (!handle_message(type, whole.view(), ConstByteView())) return;
    if (type != kHelloVerify) {
      (void)this_seq;
    }
    return;
  }
  if (fragment_length_ != total || fragment_type_ != type || fragment_.size() != total) {   // the first fragment of a message
    fragment_.clear();
    fragment_have_.clear();
    if (!fragment_.put_zeros(total) || !fragment_have_.put_zeros(total)) return;
    fragment_length_ = static_cast<uint32_t>(total);
    fragment_type_ = type;
  }
  fragment_.writable_view().subview(offset, length).copy_from(body);
  fragment_have_.writable_view().subview(offset, length).fill(1);
  for (const uint8_t b : fragment_have_.view())
    if (!b) return;
  ByteBuffer whole(allocator_);
  whole.append(fragment_.view());
  fragment_length_ = 0;
  fragment_.clear();
  fragment_have_.clear();
  handle_message(type, whole.view(), ConstByteView());
}

bool DtlsTransport::handle_message(uint8_t type, ConstByteView body, ConstByteView) {
  const uint16_t seq = recv_message_seq_;
  const bool deferred = type == kCertificateVerify || type == kFinished;
  if (!deferred && type != kHelloVerify) add_transcript(type, seq, body);
  bool ok = true;
  if (role_ == Role::Client) {
    switch (type) {
      case kHelloVerify: {   // version(2) cookie<0..32>: ask again with the cookie
        if (step_ != Step::ServerHello || body.size() < 3 || body[2] > 32 || body.size() < 3u + body[2] || cookie_size_ != 0) return fail("unexpected HelloVerifyRequest", 10);
        cookie_size_ = body[2];
        for (uint32_t i = 0; i < cookie_size_; ++i) cookie_[i] = body[3 + i];
        ++recv_message_seq_;
        transcript_.clear();
        send_message_seq_ = 1;
        state_ = State::New;
        start(now_ms_);
        return true;
      }
      case kServerHello: {
        if (step_ != Step::ServerHello || body.size() < 38) return fail("bad ServerHello", 50);
        uint64_t at = 2;
        if (load_be16(body, 0) != kVersion) return fail("not DTLS 1.2", 70);
        for (int i = 0; i < 32; ++i) server_random_[i] = body[at + i];
        at += 32;
        const uint64_t sid = body[at++];
        if (sid > 32 || body.size() < at + sid + 3) return fail("bad ServerHello", 50);
        at += sid;
        if (load_be16(body, at) != kSuite || body[at + 2] != 0) return fail("cipher suite not offered", 47);
        at += 3;
        if (at < body.size()) {
          if (body.size() - at < 2 || load_be16(body, at) != body.size() - at - 2 || !parse_hello_extensions(body.subview(at + 2), false)) return fail("bad ServerHello", 50);
        }
        use_ems_ = peer_ems_;
        step_ = Step::Certificate;
        break;
      }
      case kCertificate:
        if (step_ != Step::Certificate) return fail("unexpected Certificate", 10);
        if (!take_certificate(body, &ok)) return state_ != State::Failed ? fail("bad Certificate", 42) : false;
        step_ = Step::KeyExchange;
        break;
      case kServerKeyExchange: {
        if (step_ != Step::KeyExchange || body.size() < 4 + 65 + 4 || body[0] != 3 || load_be16(body, 1) != kCurveP256 || body[3] != 65) return fail("bad ServerKeyExchange", 40);
        const uint16_t scheme = load_be16(body, 69), siglen = load_be16(body, 71);
        if (siglen + 73u != body.size()) return fail("bad ServerKeyExchange", 40);
        ByteBuffer signed_data(allocator_);
        signed_data.append(ConstByteView(client_random_, 32));
        signed_data.append(ConstByteView(server_random_, 32));
        signed_data.append(body.first(69));
        uint8_t digest[32];
        crypto::hash(HashKind::Sha256, signed_data.view(), digest);
        if (!verify(digest, scheme, body.subview(73, siglen))) return fail("the key exchange signature is wrong", 51);
        for (int i = 0; i < 65; ++i) peer_point_[i] = body[4 + i];
        step_ = Step::HelloDone;
        break;
      }
      case kCertificateRequest:
        if (step_ != Step::HelloDone) return fail("unexpected CertificateRequest", 10);
        cert_requested_ = true;
        break;
      case kServerHelloDone:
        if (step_ != Step::HelloDone) return fail("unexpected ServerHelloDone", 10);
        ++recv_message_seq_;
        return send_client_flight();
      case kFinished:
        if (step_ != Step::Finished) return false;
        if (!check_finished(body)) return fail("the Finished message is wrong", 51);
        add_transcript(type, seq, body);
        flight_.clear();
        state_ = State::Connected;
        step_ = Step::Done;
        break;
      default: return fail("unexpected handshake message", 10);
    }
  } else {
    switch (type) {
      case kClientHello: {
        if (step_ != Step::ClientHello || body.size() < 2 + 32 + 1) return fail("bad ClientHello", 50);
        uint64_t at = 2 + 32;
        for (int i = 0; i < 32; ++i) client_random_[i] = body[2 + i];
        const uint64_t sid = body[at++];
        if (body.size() < at + sid + 1) return fail("bad ClientHello", 50);
        at += sid;
        const uint64_t cookie = body[at++];
        if (body.size() < at + cookie + 2) return fail("bad ClientHello", 50);
        at += cookie;
        const uint64_t suites = load_be16(body, at);
        at += 2;
        if ((suites & 1) || body.size() < at + suites + 1) return fail("bad ClientHello", 50);
        bool found = false;
        for (uint64_t i = 0; i < suites; i += 2) found = found || load_be16(body, at + i) == kSuite;
        at += suites;
        const uint64_t comp = body[at++];
        if (body.size() < at + comp) return fail("bad ClientHello", 50);
        at += comp;
        if (!found) return fail("no common cipher suite", 40);
        peer_ems_ = false;
        srtp_profile_ = 0;
        if (at < body.size()) {
          if (body.size() - at < 2 || load_be16(body, at) != body.size() - at - 2 || !parse_hello_extensions(body.subview(at + 2), true)) return fail("bad ClientHello", 50);
        }
        use_ems_ = peer_ems_;
        ++recv_message_seq_;
        return send_server_flight();
      }
      case kCertificate:
        if (step_ != Step::ClientCertificate) return fail("unexpected Certificate", 10);
        if (!take_certificate(body, &ok)) return state_ != State::Failed ? fail("bad Certificate", 42) : false;
        step_ = Step::ClientKeyExchange;
        break;
      case kClientKeyExchange: {
        uint8_t shared[32];
        if (step_ != Step::ClientKeyExchange || body.size() != 66 || body[0] != 65) return fail("bad ClientKeyExchange", 40);
        for (int i = 0; i < 65; ++i) peer_point_[i] = body[1 + i];
        if (!crypto::ec_ecdh(EcCurve::P256, ephemeral_private_, ConstByteView(peer_point_, 65), shared)) return fail("key agreement failed", 40);
        if (!derive_keys(ConstByteView(shared, 32))) return fail("key derivation failed", 80);
        step_ = Step::CertificateVerify;
        break;
      }
      case kCertificateVerify: {
        if (step_ != Step::CertificateVerify || body.size() < 4 || load_be16(body, 2) + 4u != body.size()) return fail("bad CertificateVerify", 51);
        uint8_t h[32];
        transcript_hash(h);
        if (!verify(h, load_be16(body, 0), body.subview(4))) return fail("the certificate verification is wrong", 51);
        add_transcript(type, seq, body);
        step_ = Step::ChangeCipherSpec;
        break;
      }
      case kFinished: {
        if (step_ != Step::Finished) return false;
        if (!check_finished(body)) return fail("the Finished message is wrong", 51);
        add_transcript(type, seq, body);
        ++recv_message_seq_;
        begin_flight();
        const uint8_t ccs = 1;
        if (!send_record(kTypeChangeCipherSpec, ConstByteView(&ccs, 1), true)) return false;
        write_epoch_ = 1;
        if (!send_finished()) return false;
        state_ = State::Connected;
        step_ = Step::Done;
        return true;
      }
      default: return fail("unexpected handshake message", 10);
    }
  }
  ++recv_message_seq_;
  if (type == kFinished && role_ == Role::Client) return true;
  return true;
}

}  // namespace rtc
