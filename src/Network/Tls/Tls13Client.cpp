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

#include "Network/Tls/Tls13Client.hpp"
#include "Crypto/Ecc.hpp"
#include "Network/Tls/TlsCertificate.hpp"
#include "Network/Tls/TlsWire.hpp"
#include "Utility/ByteStream.hpp"

namespace network {

using namespace crypto;
using namespace tls;
using namespace tlswire;

namespace {

constexpr uint16_t kExtQuicParameters = 0x39;
constexpr uint32_t kMaxMessage = 128 * 1024;

uint32_t read24(ByteReader& r) {
  const uint32_t hi = r.u8();
  return (hi << 16) | r.be16();
}
void write24(ByteWriter& w, uint32_t v) {
  w.u8(static_cast<uint8_t>(v >> 16));
  w.be16(static_cast<uint16_t>(v));
}
void patch24(ByteWriter& w, uint64_t at, uint32_t v) {
  const uint8_t b[3] = {static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
  w.patch_bytes(at, ConstByteView(b));
}
// A 16-bit length that is filled in when the vector it covers is done.
struct Vector16 {
  explicit Vector16(ByteWriter& writer) : w(writer), at(writer.position()) { w.be16(0); }
  void end() { w.patch_be16(at, static_cast<uint16_t>(w.position() - at - 2)); }
  ByteWriter& w;
  uint64_t at;
};

}  // namespace

Tls13Client::Tls13Client(allocators::IAllocator& allocator, const TlsConfig& config, uint32_t server_ipv4, ConstByteView quic_parameters)
    : allocator_(allocator), config_(config), alpn_list_(allocator), server_ipv4_(server_ipv4), quic_parameters_(allocator), peer_parameters_(allocator), leaf_(allocator),
      in_{ByteBuffer(allocator), ByteBuffer(allocator), ByteBuffer(allocator)}, out_{ByteBuffer(allocator), ByteBuffer(allocator), ByteBuffer(allocator)},
      t256_(HashKind::Sha256), t384_(HashKind::Sha384) {
  if (config.server_name) {
    uint32_t n = 0;
    for (; config.server_name[n] && n + 1 < sizeof server_name_; ++n) server_name_[n] = config.server_name[n];
    server_name_[n] = 0;
    config_.server_name = server_name_;
  }
  if (config.alpn) alpn_list_.assign(StringView(config.alpn));
  config_.alpn = nullptr;
  quic_parameters_.append(quic_parameters);
}

Tls13Client::~Tls13Client() {
  for (uint8_t& b : resumption_master_) b = 0;
  for (uint8_t& b : handshake_secret_) b = 0;
  for (uint8_t& b : master_secret_) b = 0;
  for (auto& level : client_secret_)
    for (uint8_t& b : level) b = 0;
  for (auto& level : server_secret_)
    for (uint8_t& b : level) b = 0;
  for (Share& s : shares_)
    for (uint8_t& b : s.priv) b = 0;
}

bool Tls13Client::fail(TlsError error, uint8_t alert) {
  if (state_ != St::Failed) {
    state_ = St::Failed;
    error_ = error;
    alert_ = alert;
  }
  return false;
}

void Tls13Client::consume_output(TlsLevel level, uint64_t bytes) {
  const uint32_t i = static_cast<uint32_t>(level);
  out_taken_[i] += bytes;
  if (out_taken_[i] >= out_[i].size()) {
    out_[i].clear();
    out_taken_[i] = 0;
  }
}

void Tls13Client::transcript_add(ConstByteView message) {
  t256_.update(message);
  t384_.update(message);
}

void Tls13Client::transcript_hash(uint8_t* out) {
  Hasher copy = (suite_ && suite_->hash == HashKind::Sha384) ? t384_ : t256_;
  copy.finish(out);
}

void Tls13Client::send(TlsLevel level, ConstByteView message) {
  transcript_add(message);
  if (!out_[static_cast<uint32_t>(level)].append(message)) fail(TlsError::Internal, kAlertInternal);
}

// ---- ClientHello

bool Tls13Client::make_share(uint16_t group, Share* share) {
  share->group = group;
  if (group == kGroupX25519) {
    share->pub_length = 32;
    if (!config_.random->fill(ByteView(share->priv, 32))) return false;
    x25519_public_key(share->priv, share->pub);
  } else {
    const EcCurve curve = curve_of(group);
    share->pub_length = ec_point_size(curve);
    if (!ec_generate_private(curve, *config_.random, share->priv) || !ec_public_key(curve, share->priv, share->pub)) return false;
  }
  share->have = true;
  return true;
}

bool Tls13Client::start() {
  if (state_ != St::Idle) return false;
  if (!config_.random || !config_.random->fill(ByteView(client_random_, 32)) || !make_share(kGroupX25519, &shares_[0])) return fail(TlsError::Internal, kAlertInternal);
  share_count_ = 1;
  if (config_.sessions && config_.server_name && config_.server_name[0] && now_fn_)
    have_ticket_ = config_.sessions->take(config_.server_name, true, now_fn_(now_context_), &ticket_) && find_suite(ticket_.suite_id) != nullptr;
  state_ = St::ServerHello;
  return build_client_hello(false);
}

bool Tls13Client::build_client_hello(bool retry) {
  uint8_t storage[2048 + kMaxTicketBytes + 160];
  ByteWriter w{ByteView(storage)};
  w.u8(kHsClientHello);
  const uint64_t length_at = w.position();
  write24(w, 0);
  w.be16(0x0303);
  w.bytes(ConstByteView(client_random_));
  w.u8(0);   // no legacy session id: QUIC has no middlebox compatibility mode
  {
    Vector16 suites(w);
    w.be16(0x1301);
    w.be16(0x1303);
    w.be16(0x1302);
    suites.end();
  }
  w.u8(1);
  w.u8(0);
  Vector16 extensions(w);
  if (config_.server_name) {
    const StringView name(config_.server_name);
    w.be16(kExtServerName);
    w.be16(static_cast<uint16_t>(name.size() + 5));
    w.be16(static_cast<uint16_t>(name.size() + 3));
    w.u8(0);
    w.be16(static_cast<uint16_t>(name.size()));
    w.bytes(ConstByteView(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
  }
  w.be16(kExtGroups);
  w.be16(8);
  w.be16(6);
  w.be16(kGroupX25519);
  w.be16(kGroupP256);
  w.be16(kGroupP384);
  w.be16(kExtSigAlgs);
  w.be16(18);
  w.be16(16);
  const uint16_t schemes[8] = {kSigEcdsaP256, kSigEcdsaP384, kSigPssSha256, kSigPssSha384, kSigPssSha512, kSigPkcs1Sha256, kSigPkcs1Sha384, kSigPkcs1Sha512};
  for (uint16_t s : schemes) w.be16(s);
  if (!alpn_list_.empty()) {   // "h3" or "h3,h3-29": each protocol a length-prefixed name
    w.be16(kExtAlpn);
    const uint64_t outer = w.position();
    w.be16(0);
    const uint64_t inner = w.position();
    w.be16(0);
    const StringView list = alpn_list_.view();
    uint64_t at = 0;
    while (at <= list.size()) {
      uint64_t end = list.find(',', at);
      if (end == StringView::npos) end = list.size();
      const StringView name = list.substr(at, end - at);
      if (!name.empty() && name.size() < 256) {
        w.u8(static_cast<uint8_t>(name.size()));
        w.bytes(ConstByteView(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
      }
      at = end + 1;
    }
    w.patch_be16(inner, static_cast<uint16_t>(w.position() - inner - 2));
    w.patch_be16(outer, static_cast<uint16_t>(w.position() - outer - 2));
  }
  w.be16(kExtVersions);
  w.be16(3);
  w.u8(2);
  w.be16(0x0304);
  {
    w.be16(kExtKeyShare);
    const uint64_t outer = w.position();
    w.be16(0);
    const uint64_t inner = w.position();
    w.be16(0);
    for (uint32_t i = 0; i < share_count_; ++i) {
      const Share& s = shares_[i];
      if (!s.have || (retry && s.group != selected_group_)) continue;
      w.be16(s.group);
      w.be16(static_cast<uint16_t>(s.pub_length));
      w.bytes(ConstByteView(s.pub, s.pub_length));
    }
    w.patch_be16(inner, static_cast<uint16_t>(w.position() - inner - 2));
    w.patch_be16(outer, static_cast<uint16_t>(w.position() - outer - 2));
  }
  if (retry && cookie_length_) {
    w.be16(kExtCookie);
    w.be16(static_cast<uint16_t>(cookie_length_ + 2));
    w.be16(static_cast<uint16_t>(cookie_length_));
    w.bytes(ConstByteView(cookie_, cookie_length_));
  }
  if (quic_parameters_.size() > 0) {
    w.be16(kExtQuicParameters);
    w.be16(static_cast<uint16_t>(quic_parameters_.size()));
    w.bytes(quic_parameters_.view());
  }
  // resumption: the pre_shared_key extension has to be the last, and its binder is made once the message is complete (a retried hello does not offer it again)
  uint32_t binder_length = 0;
  psk_offered_ = false;
  if (config_.sessions && now_fn_) {   // (so that the server sends tickets)
    uint8_t modes[8];
    w.bytes(ConstByteView(modes, tls::write_psk_modes(modes, sizeof modes)));
  }
  if (!retry && have_ticket_) {
    const CipherSuite* ticket_suite = find_suite(ticket_.suite_id);
    uint8_t ext[kMaxTicketBytes + 160];
    const uint32_t n = tls::write_pre_shared_key(ticket_, ticket_suite->hash, now_fn_(now_context_), ext, sizeof ext);
    if (n) {
      w.bytes(ConstByteView(ext, n));
      psk_offered_ = true;
      binder_length = hash_size(ticket_suite->hash);
    }
  }
  extensions.end();
  patch24(w, length_at, static_cast<uint32_t>(w.position() - length_at - 3));
  if (!w.ok()) return fail(TlsError::Internal, kAlertInternal);
  if (binder_length) {   // it covers the message up to the binders list (2 bytes of length, 1 of binder length, the binder)
    uint8_t binder[kMaxHashSize];
    const uint64_t end = w.position();
    if (!tls::psk_binder(find_suite(ticket_.suite_id)->hash, ConstByteView(ticket_.psk, ticket_.psk_length), ConstByteView(storage, end - binder_length - 3), binder)) return fail(TlsError::Internal, kAlertInternal);
    w.patch_bytes(end - binder_length, ConstByteView(binder, binder_length));
  }
  send(TlsLevel::Initial, w.written());
  return state_ != St::Failed;
}

// ---- Receiving

bool Tls13Client::receive(TlsLevel level, ConstByteView data) {
  if (state_ == St::Failed || state_ == St::Idle) return false;
  if (!in_[static_cast<uint32_t>(level)].append(data)) return fail(TlsError::Internal, kAlertInternal);
  return process(level);
}

bool Tls13Client::process(TlsLevel level) {
  ByteBuffer& in = in_[static_cast<uint32_t>(level)];
  uint64_t used = 0;
  while (in.size() - used >= 4) {
    const ConstByteView rest = in.view().subview(used);
    ByteReader r(rest);
    const uint8_t type = r.u8();
    const uint32_t length = read24(r);
    if (length > kMaxMessage) return fail(TlsError::Protocol, kAlertIllegalParameter);
    if (rest.size() < 4 + length) break;
    const ConstByteView whole = rest.first(4 + length), body = rest.subview(4, length);
    // the message is read before the buffer changes (secrets that come out of it move the next messages to another level)
    if (!handle_message(level, type, body, whole)) return false;
    used += 4 + length;
    // key changes happen between messages: nothing may follow a ServerHello or a Finished at the same level
    if ((type == kHsServerHello && level == TlsLevel::Initial) || (type == kHsFinished && level == TlsLevel::Handshake)) {
      if (in.size() != used) return fail(TlsError::Protocol, kAlertUnexpected);
    }
  }
  if (used > 0) {
    ByteBuffer rest(allocator_);
    if (used < in.size() && !rest.append(in.view().subview(used))) return fail(TlsError::Internal, kAlertInternal);
    in.clear();
    in.append(rest.view());
  }
  return true;
}

bool Tls13Client::handle_message(TlsLevel level, uint8_t type, ConstByteView body, ConstByteView whole) {
  switch (state_) {
    case St::ServerHello:
      if (type != kHsServerHello || level != TlsLevel::Initial) break;
      return on_server_hello(body, whole);
    case St::EncryptedExtensions:
      if (type != kHsEncryptedExtensions || level != TlsLevel::Handshake) break;
      if (!on_encrypted_extensions(body)) return false;
      transcript_add(whole);
      state_ = psk_accepted_ ? St::Finished : St::CertificateOrRequest;   // (a resumed handshake has no certificate)
      return true;
    case St::CertificateOrRequest:
      if (level != TlsLevel::Handshake) break;
      if (type == kHsCertificateRequest) {   // client certificates are not supported: when the time comes an empty Certificate message is the answer
        certificate_requested_ = true;
        transcript_add(whole);
        state_ = St::Certificate;
        return true;
      }
      if (type != kHsCertificate) break;
      if (!on_certificate(body)) return false;
      transcript_add(whole);
      state_ = St::CertificateVerify;
      return true;
    case St::Certificate:
      if (type != kHsCertificate || level != TlsLevel::Handshake) break;
      if (!on_certificate(body)) return false;
      transcript_add(whole);
      state_ = St::CertificateVerify;
      return true;
    case St::CertificateVerify:
      if (type != kHsCertificateVerify || level != TlsLevel::Handshake) break;
      if (!on_certificate_verify(body)) return false;
      transcript_add(whole);
      state_ = St::Finished;
      return true;
    case St::Finished:
      if (type != kHsFinished || level != TlsLevel::Handshake) break;
      return on_finished(body, whole);
    case St::Connected:
      if (type == kHsNewSessionTicket && level == TlsLevel::Application) {
        if (config_.sessions && have_resumption_ && config_.server_name && now_fn_) {
          UniquePointer<TlsTicket> ticket = UniquePointer<TlsTicket>::make(allocator_);
          if (ticket && tls::make_ticket(suite_->hash, suite_->id, ConstByteView(resumption_master_, hash_length_), body, config_.server_name, true, now_fn_(now_context_), ticket.get()))
            config_.sessions->store(*ticket);
        }
        return true;
      }
      if (type == kHsKeyUpdate) return fail(TlsError::Protocol, kAlertUnexpected);       // QUIC has its own key update
      break;
    default:
      break;
  }
  return fail(TlsError::Protocol, kAlertUnexpected);
}

// ---- ServerHello

bool Tls13Client::on_server_hello(ConstByteView body, ConstByteView whole) {
  ByteReader r(body);
  const uint16_t legacy_version = r.be16();
  const ConstByteView random = r.take(32);
  const uint8_t sid_length = r.u8();
  r.take(sid_length);
  const uint16_t suite_id = r.be16();
  const uint8_t compression = r.u8();
  if (!r.ok() || sid_length != 0 || legacy_version != 0x0303 || compression != 0) return fail(TlsError::Protocol, kAlertIllegalParameter);
  uint16_t selected_version = 0, key_share_group = 0;
  ConstByteView key_share_data, cookie;
  bool have_key_share = false, have_psk = false;
  uint16_t psk_selected = 0;
  if (r.remaining() > 0) {
    const uint16_t ext_length = r.be16();
    ByteReader e(r.take(ext_length));
    if (!r.ok() || !r.at_end()) return fail(TlsError::Protocol, kAlertDecode);
    while (e.remaining() > 0) {
      const uint16_t type = e.be16();
      const uint16_t length = e.be16();
      ByteReader d(e.take(length));
      if (!e.ok()) return fail(TlsError::Protocol, kAlertDecode);
      if (type == kExtVersions) {
        selected_version = d.be16();
      } else if (type == kExtKeyShare) {
        key_share_group = d.be16();
        have_key_share = true;
        if (d.remaining() > 0) {   // (not in a HelloRetryRequest)
          const uint16_t kl = d.be16();
          key_share_data = d.take(kl);
        }
      } else if (type == kExtCookie) {
        const uint16_t cl = d.be16();
        cookie = d.take(cl);
      } else if (type == kExtPreSharedKey) {
        psk_selected = d.be16();
        have_psk = true;
      } else {
        return fail(TlsError::Protocol, kAlertUnsupportedExtension);   // nothing else is allowed in a ServerHello
      }
      if (!d.ok() || !d.at_end()) return fail(TlsError::Protocol, kAlertDecode);
    }
  }
  const CipherSuite* suite = find_suite(suite_id);
  if (selected_version != 0x0304 || !suite || !suite->tls13) return fail(TlsError::Unsupported, kAlertProtocolVersion);
  const bool hello_retry = equal_constant_time(random.data(), kHelloRetryMagic, 32);
  if (hello_retry) {
    if (!have_key_share || !key_share_data.empty()) return fail(TlsError::Protocol, kAlertIllegalParameter);
    return on_hello_retry_request(whole, suite_id, key_share_group, cookie);
  }
  if (!have_key_share || share_count_ == 0) return fail(TlsError::Protocol, kAlertMissingExtension);
  if (suite_ && suite_ != suite) return fail(TlsError::Protocol, kAlertIllegalParameter);   // (it must keep the choice it made in the HelloRetryRequest)
  if (have_psk) {   // the server took the ticket: it must be the one offered, and for a suite with the same hash
    if (!psk_offered_ || psk_selected != 0 || find_suite(ticket_.suite_id)->hash != suite->hash) return fail(TlsError::Protocol, kAlertIllegalParameter);
    psk_accepted_ = true;
  }
  suite_ = suite;
  hash_length_ = hash_size(suite->hash);
  selected_group_ = key_share_group;
  transcript_add(whole);
  const Share* share = nullptr;
  for (uint32_t i = 0; i < share_count_; ++i)
    if (shares_[i].have && shares_[i].group == key_share_group) share = &shares_[i];
  if (!share) return fail(TlsError::Protocol, kAlertIllegalParameter);
  uint8_t shared[48];
  uint32_t shared_length = 32;
  if (key_share_group == kGroupX25519) {
    if (key_share_data.size() != 32 || !x25519_shared_secret(share->priv, key_share_data.data(), shared)) return fail(TlsError::Protocol, kAlertIllegalParameter);
  } else {
    const EcCurve curve = curve_of(key_share_group);
    shared_length = ec_field_size(curve);
    if (key_share_data.size() != ec_point_size(curve) || !ec_ecdh(curve, share->priv, key_share_data, shared)) return fail(TlsError::Protocol, kAlertIllegalParameter);
  }
  const bool ok = derive_handshake_secrets(ConstByteView(shared, shared_length));
  for (uint8_t& b : shared) b = 0;
  if (!ok) return false;
  state_ = St::EncryptedExtensions;
  return true;
}

bool Tls13Client::on_hello_retry_request(ConstByteView whole, uint16_t suite_id, uint16_t group, ConstByteView cookie) {
  if (retried_ || !group_supported(group) || cookie.size() > sizeof cookie_) return fail(TlsError::Protocol, kAlertIllegalParameter);
  for (uint32_t i = 0; i < share_count_; ++i)
    if (shares_[i].have && shares_[i].group == group) return fail(TlsError::Protocol, kAlertIllegalParameter);   // it would not ask for what we sent
  suite_ = find_suite(suite_id);
  // the transcript so far (the ClientHello) is replaced by a message_hash message
  uint8_t digest[kMaxHashSize];
  transcript_hash(digest);
  const uint32_t hl = hash_size(suite_->hash);
  t256_ = Hasher(HashKind::Sha256);
  t384_ = Hasher(HashKind::Sha384);
  uint8_t synthetic[4 + kMaxHashSize] = {kHsMessageHash, 0, 0, static_cast<uint8_t>(hl)};
  for (uint32_t i = 0; i < hl; ++i) synthetic[4 + i] = digest[i];
  transcript_add(ConstByteView(synthetic, 4 + hl));
  transcript_add(whole);
  retried_ = true;
  selected_group_ = group;
  cookie_length_ = static_cast<uint32_t>(cookie.size());
  for (uint32_t i = 0; i < cookie.size(); ++i) cookie_[i] = cookie[i];
  if (!make_share(group, &shares_[share_count_++])) return fail(TlsError::Internal, kAlertInternal);
  return build_client_hello(true);
}

// ---- The key schedule

bool Tls13Client::derive_handshake_secrets(ConstByteView shared) {
  const HashKind h = suite_->hash;
  const uint32_t hl = hash_length_;
  uint8_t zeros[kMaxHashSize] = {0}, early[kMaxHashSize], derived[kMaxHashSize], empty[kMaxHashSize], th[kMaxHashSize];
  hash(h, ConstByteView(), empty);
  if (psk_accepted_) tls::early_secret(h, ConstByteView(ticket_.psk, ticket_.psk_length), early);
  else hkdf_extract(h, ConstByteView(), ConstByteView(zeros, hl), early);
  if (!derive_secret(h, ConstByteView(early, hl), "derived", ConstByteView(empty, hl), derived)) return fail(TlsError::Internal, kAlertInternal);
  hkdf_extract(h, ConstByteView(derived, hl), shared, handshake_secret_);
  transcript_hash(th);
  const uint32_t hs = static_cast<uint32_t>(TlsLevel::Handshake);
  if (!derive_secret(h, ConstByteView(handshake_secret_, hl), "c hs traffic", ConstByteView(th, hl), client_secret_[hs]) ||
      !derive_secret(h, ConstByteView(handshake_secret_, hl), "s hs traffic", ConstByteView(th, hl), server_secret_[hs]) ||
      !derive_secret(h, ConstByteView(handshake_secret_, hl), "derived", ConstByteView(empty, hl), derived))
    return fail(TlsError::Internal, kAlertInternal);
  hkdf_extract(h, ConstByteView(derived, hl), ConstByteView(zeros, hl), master_secret_);
  have_secrets_[hs] = true;
  return true;
}

// ---- The server's flight

bool Tls13Client::on_encrypted_extensions(ConstByteView body) {
  ByteReader r(body);
  const uint16_t length = r.be16();
  ByteReader e(r.take(length));
  if (!r.ok() || !r.at_end()) return fail(TlsError::Protocol, kAlertDecode);
  bool have_parameters = false;
  while (e.remaining() > 0) {
    const uint16_t type = e.be16();
    const uint16_t l = e.be16();
    ByteReader d(e.take(l));
    if (!e.ok()) return fail(TlsError::Protocol, kAlertDecode);
    if (type == kExtAlpn) {
      d.be16();
      const uint8_t pl = d.u8();
      const ConstByteView proto = d.take(pl);
      if (!d.ok() || !d.at_end() || pl == 0 || pl >= sizeof alpn_selected_) return fail(TlsError::Protocol, kAlertIllegalParameter);
      // it must be one that was offered
      bool offered = false;
      const StringView list = alpn_list_.view();
      uint64_t at = 0;
      while (!offered && at <= list.size()) {
        uint64_t end = list.find(',', at);
        if (end == StringView::npos) end = list.size();
        const StringView name = list.substr(at, end - at);
        offered = name.size() == pl && equal_constant_time(reinterpret_cast<const uint8_t*>(name.data()), proto.data(), pl);
        at = end + 1;
      }
      if (!offered) return fail(TlsError::Protocol, kAlertIllegalParameter);
      for (uint32_t i = 0; i < pl; ++i) alpn_selected_[i] = static_cast<char>(proto[i]);
      alpn_length_ = pl;
    } else if (type == kExtQuicParameters) {
      have_parameters = true;
      peer_parameters_.clear();
      if (!peer_parameters_.append(d.rest())) return fail(TlsError::Internal, kAlertInternal);
    }
  }
  if (quic_parameters_.size() > 0 && !have_parameters) return fail(TlsError::Protocol, kAlertMissingExtension);
  if (!alpn_list_.empty() && alpn_length_ == 0) return fail(TlsError::Protocol, 120);   // no_application_protocol (QUIC requires ALPN)
  return true;
}

bool Tls13Client::on_certificate(ConstByteView body) {
  CertificateProblem problem;
  if (!check_server_certificate(allocator_, config_, server_ipv4_, body, true, SuiteAuth::Any, &leaf_, &problem)) {
    cert_status_ = problem.status;
    return fail(problem.error, problem.alert);
  }
  saw_certificate_ = true;
  return true;
}

bool Tls13Client::on_certificate_verify(ConstByteView body) {
  ByteReader r(body);
  const uint16_t scheme = r.be16();
  const uint16_t sl = r.be16();
  const ConstByteView signature = r.take(sl);
  if (!r.ok() || !r.at_end() || !saw_certificate_) return fail(TlsError::Protocol, kAlertDecode);
  uint8_t content[64 + 34 + kMaxHashSize];
  uint32_t n = 0;
  for (int i = 0; i < 64; ++i) content[n++] = 0x20;
  for (const char* c = "TLS 1.3, server CertificateVerify"; *c; ++c) content[n++] = static_cast<uint8_t>(*c);
  content[n++] = 0;
  transcript_hash(content + n);
  n += hash_length_;
  if (!verify_leaf_signature(leaf_.view(), scheme, ConstByteView(content, n), signature, true)) {
    cert_status_ = CertStatus::BadSignature;
    return fail(TlsError::Certificate, kAlertDecrypt);
  }
  return true;
}

bool Tls13Client::on_finished(ConstByteView body, ConstByteView whole) {
  const HashKind h = suite_->hash;
  const uint32_t hl = hash_length_;
  uint8_t th[kMaxHashSize], key[kMaxHashSize], expected[kMaxHashSize];
  const uint32_t hs = static_cast<uint32_t>(TlsLevel::Handshake), ap = static_cast<uint32_t>(TlsLevel::Application);
  if (body.size() != hl) return fail(TlsError::Protocol, kAlertDecode);
  transcript_hash(th);
  if (!hkdf_expand_label(h, ConstByteView(server_secret_[hs], hl), "finished", ConstByteView(), ByteView(key, hl))) return fail(TlsError::Internal, kAlertInternal);
  hmac(h, ConstByteView(key, hl), ConstByteView(th, hl), expected);
  if (!equal_constant_time(expected, body.data(), hl)) return fail(TlsError::Protocol, kAlertDecrypt);
  transcript_add(whole);
  // the application secrets cover everything up to the server's Finished
  transcript_hash(th);
  if (!derive_secret(h, ConstByteView(master_secret_, hl), "c ap traffic", ConstByteView(th, hl), client_secret_[ap]) ||
      !derive_secret(h, ConstByteView(master_secret_, hl), "s ap traffic", ConstByteView(th, hl), server_secret_[ap]))
    return fail(TlsError::Internal, kAlertInternal);
  have_secrets_[ap] = true;
  // our flight, under the handshake keys: an empty Certificate if one was asked for, then Finished
  if (certificate_requested_) {
    const uint8_t empty_certificate[8] = {kHsCertificate, 0, 0, 4, 0, 0, 0, 0};
    send(TlsLevel::Handshake, ConstByteView(empty_certificate));
    if (state_ == St::Failed) return false;
  }
  transcript_hash(th);
  if (!hkdf_expand_label(h, ConstByteView(client_secret_[hs], hl), "finished", ConstByteView(), ByteView(key, hl))) return fail(TlsError::Internal, kAlertInternal);
  uint8_t finished[4 + kMaxHashSize] = {kHsFinished, 0, 0, static_cast<uint8_t>(hl)};
  hmac(h, ConstByteView(key, hl), ConstByteView(th, hl), finished + 4);
  send(TlsLevel::Handshake, ConstByteView(finished, 4 + hl));
  if (state_ == St::Failed) return false;
  // the resumption master secret covers everything up to our Finished; tickets sent later are made from it
  transcript_hash(th);
  have_resumption_ = tls::resumption_master_secret(h, ConstByteView(master_secret_, hl), ConstByteView(th, hl), resumption_master_);
  state_ = St::Connected;
  return true;
}

}  // namespace network
