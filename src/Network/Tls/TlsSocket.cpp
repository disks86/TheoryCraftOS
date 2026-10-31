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

#include "Network/Tls/TlsSocket.hpp"
#include "Network/Tls/TlsSession.hpp"
#include "Crypto/Ecc.hpp"
#include "Network/Tls/TlsCertificate.hpp"
#include "Network/Tls/TlsWire.hpp"

namespace network {

using namespace crypto;
using namespace tls;

using namespace tlswire;

struct TlsSocket::Handshake {
  struct Share {
    uint16_t group = 0;
    bool have = false;
    uint8_t priv[48] = {};
    uint8_t pub[97] = {};
    uint32_t pub_length = 0;
  };
  Handshake(allocators::IAllocator& a) : t256(HashKind::Sha256), t384(HashKind::Sha384), leaf(a) {}
  ~Handshake() {
    // wipe the secrets
    for (Share& s : shares)
      for (uint8_t& b : s.priv) b = 0;
    for (uint8_t& b : handshake_secret) b = 0;
    for (uint8_t& b : master_secret) b = 0;
    for (uint8_t& b : client_hs_secret) b = 0;
    for (uint8_t& b : server_hs_secret) b = 0;
  }
  Hasher t256, t384;
  uint8_t client_random[32] = {}, server_random[32] = {}, session_id[32] = {};
  Share shares[3];
  uint32_t share_count = 0;
  uint8_t cookie[256] = {};
  uint32_t cookie_length = 0;
  bool retried = false;
  uint16_t selected_group = 0;
  bool cert_requested = false;
  bool saw_certificate = false;
  bool sent_ccs = false;
  // resumption: the ticket taken from the cache, whether it was offered and whether the server took it
  TlsTicket ticket;
  bool have_ticket = false, psk_offered = false, psk_accepted = false;
  // TLS 1.3
  uint8_t handshake_secret[48] = {}, master_secret[48] = {}, client_hs_secret[48] = {}, server_hs_secret[48] = {};
  // TLS 1.2
  uint8_t ske_pub[97] = {};
  uint32_t ske_pub_length = 0;
  uint16_t ske_group = 0;
  uint16_t ske_scheme = 0;
  uint8_t ske_params[2 + 1 + 97] = {};
  uint32_t ske_params_length = 0;
  uint8_t ske_signature[600] = {};
  uint32_t ske_signature_length = 0;
  ByteBuffer leaf;   // the server's certificate
};

TlsSocket::TlsSocket(INetworkStack& stack, allocators::IAllocator& allocator, const TlsConfig& config, SharedPointer<ITcpSocket> inner)
    : stack_(stack), allocator_(allocator), config_(config), inner_(static_cast<SharedPointer<ITcpSocket>&&>(inner)), server_certificate_(allocator), handshake_data_(allocator), in_(allocator), out_(allocator) {
  uint32_t n = 0;
  for (; config.server_name && config.server_name[n] && n + 1 < sizeof server_name_; ++n) server_name_[n] = config.server_name[n];
  server_name_[n] = 0;
  n = 0;
  for (; config.alpn && config.alpn[n] && n + 1 < sizeof alpn_; ++n) alpn_[n] = config.alpn[n];
  alpn_[n] = 0;
  config_.server_name = server_name_[0] ? server_name_ : nullptr;
  config_.alpn = alpn_[0] ? alpn_ : nullptr;
  if (inner_) {
    state_ = St::Connecting;
    started_ms_ = stack_.now_ms();
  }
}

TlsSocket::~TlsSocket() {
  if (state_ == St::Open && !sent_close_) close();
  for (uint8_t& b : resumption_master_) b = 0;
  for (uint8_t& b : client_traffic_secret_) b = 0;
  for (uint8_t& b : server_traffic_secret_) b = 0;
}

// ------------------------------------------------------------------------------------------------ ITcpSocket

bool TlsSocket::connect(Ipv4Address address, uint16_t port) {
  if (state_ != St::Idle || inner_) return false;
  inner_ = stack_.tcp_open();
  if (!inner_ || !inner_->connect(address, port)) {
    inner_ = SharedPointer<ITcpSocket>();
    return false;
  }
  if (config_.server_ipv4 == 0) config_.server_ipv4 = address.value();
  state_ = St::Connecting;
  started_ms_ = stack_.now_ms();
  return true;
}

TcpState TlsSocket::state() const {
  const_cast<TlsSocket*>(this)->pump();
  if (!inner_) return TcpState::Closed;
  if (state_ == St::Failed) return TcpState::Closed;
  const TcpState s = inner_->state();
  if (!handshake_complete() && s == TcpState::Established) return TcpState::SynSent;   // not usable yet
  return s;
}

bool TlsSocket::is_connected() const {
  const_cast<TlsSocket*>(this)->pump();
  return state_ == St::Open;
}

bool TlsSocket::failed() const {
  const_cast<TlsSocket*>(this)->pump();
  return state_ == St::Failed || (inner_ && inner_->failed());
}

bool TlsSocket::peer_closed() const {
  const_cast<TlsSocket*>(this)->pump();
  return peer_closed_ && in_.size() == 0;
}

uint32_t TlsSocket::available() const {
  const_cast<TlsSocket*>(this)->pump();
  return in_.size();
}

uint32_t TlsSocket::unsent_or_unacknowledged() const {
  const_cast<TlsSocket*>(this)->pump();
  return out_.size() + (inner_ ? inner_->unsent_or_unacknowledged() : 0);
}

uint32_t TlsSocket::receive(ByteView buffer) {
  pump();
  const uint32_t n = in_.read(buffer);
  if (n) pump();   // room for more
  return n;
}

uint32_t TlsSocket::send(ConstByteView data) {
  pump();
  if (state_ != St::Open || sent_close_) return 0;
  uint32_t done = 0;
  while (done < data.size()) {
    // room in the output queue for a whole record (header, explicit nonce, tag, inner type)
    const uint32_t chunk = static_cast<uint32_t>(data.size() - done < kMaxPlaintext ? data.size() - done : kMaxPlaintext);
    if (out_.free_space() < chunk + 64) {
      flush();
      if (out_.free_space() < chunk + 64) break;
    }
    if (!send_record(kApplicationData, data.subview(done, chunk))) break;
    done += chunk;
  }
  flush();
  return done;
}

void TlsSocket::close() {
  if (!inner_) return;
  pump();
  if (state_ == St::Open && !sent_close_) {
    send_alert(1, kAlertCloseNotify);
    sent_close_ = true;
    state_ = St::Closed;
  }
  flush();
  inner_->close();
}

void TlsSocket::abort() {
  if (inner_) inner_->abort();
  if (state_ != St::Failed) state_ = St::Closed;
}

// ------------------------------------------------------------------------------------------------ plumbing

void TlsSocket::flush() {
  if (!inner_ || out_.size() == 0) return;
  uint8_t chunk[1024];
  while (out_.size() > 0) {
    const uint32_t n = out_.peek(0, ByteView(chunk, sizeof chunk));
    const uint32_t sent = inner_->send(ConstByteView(chunk, n));
    out_.discard(sent);
    if (sent < n) break;
  }
}

void TlsSocket::fail(TlsError error, uint8_t alert) {
  if (state_ == St::Failed) return;
  error_ = error;
  if (alert != 0 && inner_ && inner_->is_connected() && (state_ != St::Idle && state_ != St::Connecting)) {
    send_alert(2, alert);
    flush();
  }
  state_ = St::Failed;
  hs_.reset();
  if (inner_) inner_->close();
}

void TlsSocket::send_alert(uint8_t level, uint8_t description) {
  const uint8_t body[2] = {level, description};
  send_record(kAlert, ConstByteView(body, 2));
}

bool TlsSocket::send_record(uint8_t type, ConstByteView data) {
  const uint32_t n = write_.seal(type, data, tx_, sizeof tx_);
  if (n == 0 || out_.free_space() < n) return false;
  out_.write(ConstByteView(tx_, n));
  return true;
}

bool TlsSocket::send_handshake(const uint8_t* message, uint32_t length) {
  if (hs_) transcript_add(ConstByteView(message, length));
  uint32_t done = 0;
  while (done < length) {   // a handshake message may need several records
    const uint32_t chunk = length - done < kMaxPlaintext ? length - done : kMaxPlaintext;
    if (!send_record(kHandshake, ConstByteView(message + done, chunk))) return false;
    done += chunk;
  }
  return true;
}

void TlsSocket::transcript_add(ConstByteView message) {
  hs_->t256.update(message);
  hs_->t384.update(message);
}

void TlsSocket::transcript_hash(uint8_t* out) {
  if (suite_ && suite_->hash == HashKind::Sha384) {
    Hasher copy = hs_->t384;
    copy.finish(out);
  } else {
    Hasher copy = hs_->t256;
    copy.finish(out);
  }
}

// ------------------------------------------------------------------------------------------------ the pump

void TlsSocket::pump() {
  if (pumping_ || !inner_ || state_ == St::Failed || state_ == St::Idle) return;
  pumping_ = true;
  if (state_ == St::Connecting) {
    if (inner_->failed()) fail(TlsError::Transport, 0);
    else if (inner_->is_connected()) start_handshake();
    else if (stack_.now_ms() - started_ms_ > config_.handshake_timeout_ms) fail(TlsError::Timeout, 0);
  }
  if (state_ != St::Connecting && state_ != St::Failed) {
    flush();
    read_records();
    flush();
    if (state_ != St::Failed && !handshake_complete() && stack_.now_ms() - started_ms_ > config_.handshake_timeout_ms) fail(TlsError::Timeout, kAlertInternal);
  }
  pumping_ = false;
}

bool TlsSocket::read_records() {
  for (int guard = 0; guard < 64; ++guard) {
    if (state_ == St::Failed) return false;
    if ((state_ == St::Open || state_ == St::Closed) && in_.free_space() < kMaxPlaintext) return true;   // the reader has to catch up first
    // header
    while (rx_length_ < kRecordHeader) {
      const uint32_t n = inner_->receive(ByteView(rx_ + rx_length_, kRecordHeader - rx_length_));
      if (n == 0) goto no_more;
      rx_length_ += n;
    }
    {
      const uint32_t length = static_cast<uint32_t>(rx_[3] << 8 | rx_[4]);
      if (rx_[1] != 3 || (rx_[2] > 3)) { fail(TlsError::Protocol, kAlertProtocolVersion); return false; }
      if (length > kMaxRecordPayload) { fail(TlsError::BadRecord, kAlertOverflow); return false; }
      if (tls13_ && read_.active() && length > kMaxPlaintext + 256) { fail(TlsError::BadRecord, kAlertOverflow); return false; }
      while (rx_length_ < kRecordHeader + length) {
        const uint32_t n = inner_->receive(ByteView(rx_ + rx_length_, kRecordHeader + length - rx_length_));
        if (n == 0) goto no_more;
        rx_length_ += n;
      }
      const uint8_t type = rx_[0];
      rx_length_ = 0;
      if (!handle_record(type, rx_ + kRecordHeader, length)) return false;
    }
  }
  return true;
no_more:
  if (inner_->peer_closed() && inner_->available() == 0 && !peer_closed_) {
    if (!handshake_complete()) {
      fail(TlsError::Transport, 0);
      return false;
    }
    peer_closed_ = true;
    if (error_ == TlsError::None) error_ = TlsError::Truncated;   // no close_notify came first
  }
  if (inner_->failed() && !handshake_complete()) fail(TlsError::Transport, 0);
  return true;
}

bool TlsSocket::handle_record(uint8_t type, uint8_t* payload, uint32_t length) {
  if (type == kChangeCipherSpec) {
    // never encrypted
    if (length != 1 || payload[0] != 1) { fail(TlsError::Protocol, kAlertUnexpected); return false; }
    if (tls13_) {
      if (state_ == St::Open) { fail(TlsError::Protocol, kAlertUnexpected); return false; }
      return true;   // the middlebox compatibility record: ignored
    }
    if (state_ != St::ChangeCipherSpec12 || handshake_pos_ != handshake_data_.size()) { fail(TlsError::Protocol, kAlertUnexpected); return false; }
    // switch to the server's keys
    if (!set_keys_12(read_, false)) { fail(TlsError::Internal, kAlertInternal); return false; }
    state_ = St::Finished12;
    return true;
  }
  ConstByteView plain;
  uint8_t inner_type = 0;
  if (!read_.open(type, payload, length, &plain, &inner_type)) {
    fail(TlsError::BadRecord, kAlertBadMac);
    return false;
  }
  if (plain.size() > kMaxPlaintext) { fail(TlsError::BadRecord, kAlertOverflow); return false; }
  switch (inner_type) {
    case kHandshake:
      if (plain.empty()) { fail(TlsError::Protocol, kAlertUnexpected); return false; }
      return handle_handshake_data(plain);
    case kAlert:
      return handle_alert(plain);
    case kApplicationData:
      if (state_ != St::Open && state_ != St::Closed) { fail(TlsError::Protocol, kAlertUnexpected); return false; }
      if (!plain.empty()) in_.write(plain);
      return true;
    default:
      fail(TlsError::Protocol, kAlertUnexpected);
      return false;
  }
}

bool TlsSocket::handle_alert(ConstByteView data) {
  if (data.size() != 2) { fail(TlsError::Protocol, kAlertDecode); return false; }
  const uint8_t level = data[0], description = data[1];
  if (description == kAlertCloseNotify) {
    peer_closed_ = true;
    if (state_ == St::Open) {
      // answer with our own close_notify, as TLS asks
      if (!sent_close_) {
        send_alert(1, kAlertCloseNotify);
        sent_close_ = true;
        flush();
      }
    }
    return true;
  }
  if (level == 1 && !tls13_ && description != kAlertCloseNotify) return true;   // a TLS 1.2 warning
  if (tls13_ && description == 90) return true;   // user_canceled
  alert_received_ = description;
  state_ = St::Failed;
  error_ = TlsError::Alert;
  hs_.reset();
  if (inner_) inner_->close();
  return false;
}

bool TlsSocket::handle_handshake_data(ConstByteView data) {
  if (handshake_data_.size() + data.size() > 128 * 1024) { fail(TlsError::Protocol, kAlertInternal); return false; }
  if (!handshake_data_.append(data)) { fail(TlsError::Internal, kAlertInternal); return false; }
  while (handshake_data_.size() - handshake_pos_ >= 4) {
    const ConstByteView all = handshake_data_.view().subview(handshake_pos_);
    const uint32_t body_length = static_cast<uint32_t>(all[1]) << 16 | static_cast<uint32_t>(all[2]) << 8 | all[3];
    if (body_length > 120 * 1024) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
    if (all.size() < 4 + body_length) break;
    const uint8_t type = all[0];
    const ConstByteView whole = all.first(4 + body_length), body = all.subview(4, body_length);
    // The message must not move while it is handled: the buffer only grows in handle_handshake_data, never inside the handlers.
    if (!handle_message(type, body, whole)) return false;
    handshake_pos_ += 4 + body_length;
  }
  if (handshake_pos_ == handshake_data_.size()) {
    handshake_data_.clear();
    handshake_pos_ = 0;
  }
  return state_ != St::Failed;
}

// ------------------------------------------------------------------------------------------------ the handshake

bool TlsSocket::start_handshake() {
  if (!config_.random || (!config_.allow_tls12 && !config_.allow_tls13)) { fail(TlsError::Internal, 0); return false; }
  hs_ = UniquePointer<Handshake>::make(allocator_, allocator_);
  if (!hs_ || !in_.reserve(32 * 1024) || !out_.reserve(40 * 1024)) { fail(TlsError::Internal, 0); return false; }
  if (!config_.random->fill(ByteView(hs_->client_random, 32)) || !config_.random->fill(ByteView(hs_->session_id, 32))) { fail(TlsError::Internal, 0); return false; }
  if (config_.allow_tls13 && config_.sessions && config_.server_name && config_.server_name[0]) {
    hs_->have_ticket = config_.sessions->take(config_.server_name, false, stack_.now_ms(), &hs_->ticket) && tls::find_suite(hs_->ticket.suite_id) != nullptr;
  }
  // key shares: X25519 and P-256
  if (config_.allow_tls13) {
    Handshake::Share& x = hs_->shares[0];
    x.group = kGroupX25519;
    x.pub_length = 32;
    if (!config_.random->fill(ByteView(x.priv, 32))) { fail(TlsError::Internal, 0); return false; }
    x25519_public_key(x.priv, x.pub);
    x.have = true;
    Handshake::Share& p = hs_->shares[1];
    p.group = kGroupP256;
    p.pub_length = 65;
    if (!ec_generate_private(EcCurve::P256, *config_.random, p.priv) || !ec_public_key(EcCurve::P256, p.priv, p.pub)) { fail(TlsError::Internal, 0); return false; }
    p.have = true;
    hs_->share_count = 2;
  }
  state_ = St::ServerHello;
  return build_client_hello(false);
}

bool TlsSocket::build_client_hello(bool retry) {
  uint8_t buffer[1100 + kMaxTicketBytes + 160];
  Writer w(buffer, sizeof buffer);
  w.u8(kHsClientHello);
  const uint32_t length_mark = w.mark24();
  w.u16(0x0303);
  w.bytes(hs_->client_random, 32);
  w.u8(32);
  w.bytes(hs_->session_id, 32);
  const uint32_t suites_mark = w.mark16();
  if (config_.allow_tls13) {
    w.u16(0x1303);
    w.u16(0x1301);
    w.u16(0x1302);
  }
  if (config_.allow_tls12) {
    w.u16(0xcca9);
    w.u16(0xcca8);
    w.u16(0xc02b);
    w.u16(0xc02f);
    w.u16(0xc02c);
    w.u16(0xc030);
    if (config_.allow_cbc_suites) {   // (last: only for servers with nothing better)
      w.u16(0xc027);
      w.u16(0xc023);
      w.u16(0xc028);
      w.u16(0xc024);
      w.u16(0xc013);
      w.u16(0xc009);
      w.u16(0xc014);
      w.u16(0xc00a);
    }
  }
  w.end16(suites_mark);
  w.u8(1);
  w.u8(0);
  const uint32_t ext_mark = w.mark16();
  if (config_.server_name) {
    const uint32_t n = length_of(config_.server_name);
    w.u16(kExtServerName);
    w.u16(n + 5);
    w.u16(n + 3);
    w.u8(0);
    w.u16(n);
    w.bytes(reinterpret_cast<const uint8_t*>(config_.server_name), n);
  }
  w.u16(kExtGroups);
  w.u16(8);
  w.u16(6);
  w.u16(kGroupX25519);
  w.u16(kGroupP256);
  w.u16(kGroupP384);
  if (config_.allow_tls12) {
    w.u16(kExtPointFormats);
    w.u16(2);
    w.u8(1);
    w.u8(0);
  }
  w.u16(kExtSigAlgs);
  w.u16(18);
  w.u16(16);
  const uint16_t schemes[8] = {kSigEcdsaP256, kSigEcdsaP384, kSigPssSha256, kSigPssSha384, kSigPssSha512, kSigPkcs1Sha256, kSigPkcs1Sha384, kSigPkcs1Sha512};
  for (uint16_t s : schemes) w.u16(s);
  if (config_.allow_tls12) {
    w.u16(kExtEms);
    w.u16(0);
    if (config_.allow_cbc_suites) {
      w.u16(kExtEncryptThenMac);
      w.u16(0);
    }
    w.u16(kExtRenegotiation);
    w.u16(1);
    w.u8(0);
  }
  if (config_.alpn) {
    const uint32_t n = length_of(config_.alpn);
    w.u16(kExtAlpn);
    w.u16(n + 3);
    w.u16(n + 1);
    w.u8(n);
    w.bytes(reinterpret_cast<const uint8_t*>(config_.alpn), n);
  }
  w.u16(kExtVersions);
  w.u16((config_.allow_tls13 ? 2 : 0) + (config_.allow_tls12 ? 2 : 0) + 1);
  w.u8((config_.allow_tls13 ? 2 : 0) + (config_.allow_tls12 ? 2 : 0));
  if (config_.allow_tls13) w.u16(0x0304);
  if (config_.allow_tls12) w.u16(0x0303);
  if (config_.allow_tls13) {
    w.u16(kExtKeyShare);
    const uint32_t outer = w.mark16();
    const uint32_t inner = w.mark16();
    for (uint32_t i = 0; i < hs_->share_count; ++i) {
      const Handshake::Share& s = hs_->shares[i];
      if (!s.have) continue;
      if (retry && s.group != hs_->selected_group) continue;
      w.u16(s.group);
      w.u16(s.pub_length);
      w.bytes(s.pub, s.pub_length);
    }
    w.end16(inner);
    w.end16(outer);
    if (retry && hs_->cookie_length) {
      w.u16(kExtCookie);
      w.u16(hs_->cookie_length + 2);
      w.u16(hs_->cookie_length);
      w.bytes(hs_->cookie, hs_->cookie_length);
    }
  }
  // resumption: the pre_shared_key extension, which has to be the last one; its binder is made once the message is complete. (After a HelloRetryRequest the
  // ticket is not offered again.)
  uint32_t binder_length = 0;
  hs_->psk_offered = false;
  if (config_.allow_tls13 && config_.sessions) w.n += tls::write_psk_modes(w.p + w.n, w.cap - w.n);   // (so that the server sends tickets)
  if (config_.allow_tls13 && !retry && hs_->have_ticket) {
    const CipherSuite* ticket_suite = tls::find_suite(hs_->ticket.suite_id);
    const uint32_t n = tls::write_pre_shared_key(hs_->ticket, ticket_suite->hash, stack_.now_ms(), w.p + w.n, w.cap - w.n);
    if (n) {
      w.n += n;
      hs_->psk_offered = true;
      binder_length = hash_size(ticket_suite->hash);
    }
  }
  w.end16(ext_mark);
  w.end24(length_mark);
  if (!w.ok) { fail(TlsError::Internal, kAlertInternal); return false; }
  if (binder_length) {   // the binder covers the message up to the binders list (2 bytes of length, 1 of binder length, the binder)
    const CipherSuite* ticket_suite = tls::find_suite(hs_->ticket.suite_id);
    if (!tls::psk_binder(ticket_suite->hash, ConstByteView(hs_->ticket.psk, hs_->ticket.psk_length), ConstByteView(buffer, w.n - binder_length - 3), buffer + w.n - binder_length)) {
      fail(TlsError::Internal, kAlertInternal);
      return false;
    }
  }
  if (!send_handshake(buffer, w.n)) { fail(TlsError::Internal, kAlertInternal); return false; }
  flush();
  return true;
}

bool TlsSocket::handle_message(uint8_t type, ConstByteView body, ConstByteView whole) {
  switch (state_) {
    case St::ServerHello:
      if (type != kHsServerHello) break;
      return on_server_hello(body, whole);
    case St::EncryptedExtensions:
      if (type != kHsEncryptedExtensions) break;
      if (!on_encrypted_extensions(body)) return false;
      transcript_add(whole);
      state_ = hs_->psk_accepted ? St::Finished13 : St::CertificateOrRequest13;   // (a resumed handshake has no certificate)
      return true;
    case St::CertificateOrRequest13:
      if (type == kHsCertificateRequest) {
        hs_->cert_requested = true;
        transcript_add(whole);
        state_ = St::Certificate13;
        return true;
      }
      if (type != kHsCertificate) break;
      if (!on_certificate(body, true)) return false;
      transcript_add(whole);
      state_ = St::CertificateVerify13;
      return true;
    case St::Certificate13:
      if (type != kHsCertificate) break;
      if (!on_certificate(body, true)) return false;
      transcript_add(whole);
      state_ = St::CertificateVerify13;
      return true;
    case St::CertificateVerify13:
      if (type != kHsCertificateVerify) break;
      if (!on_certificate_verify(body)) return false;
      transcript_add(whole);
      state_ = St::Finished13;
      return true;
    case St::Finished13:
      if (type != kHsFinished) break;
      return on_finished_13(body, whole);
    case St::Certificate12:
      if (type != kHsCertificate) break;
      if (!on_certificate(body, false)) return false;
      transcript_add(whole);
      state_ = St::KeyExchange12;
      return true;
    case St::KeyExchange12:
      if (type != kHsServerKeyExchange) break;
      if (!on_server_key_exchange(body)) return false;
      transcript_add(whole);
      state_ = St::RequestOrDone12;
      return true;
    case St::RequestOrDone12:
      if (type == kHsCertificateRequest) {
        hs_->cert_requested = true;
        transcript_add(whole);
        state_ = St::Done12;
        return true;
      }
      if (type != kHsServerHelloDone) break;
      transcript_add(whole);
      return body.empty() ? on_server_hello_done() : (fail(TlsError::Protocol, kAlertDecode), false);
    case St::Done12:
      if (type != kHsServerHelloDone) break;
      transcript_add(whole);
      return body.empty() ? on_server_hello_done() : (fail(TlsError::Protocol, kAlertDecode), false);
    case St::Finished12:
      if (type != kHsFinished) break;
      return on_finished_12(body);
    case St::Open:
      return on_post_handshake(type, body);
    default:
      break;
  }
  fail(TlsError::Protocol, kAlertUnexpected);
  return false;
}

// ---- ServerHello

bool TlsSocket::on_server_hello(ConstByteView body, ConstByteView whole) {
  Reader r(body);
  const uint16_t legacy_version = r.u16();
  const ConstByteView random = r.take(32);
  const uint8_t sid_length = r.u8();
  const ConstByteView sid = r.take(sid_length);
  const uint16_t suite_id = r.u16();
  const uint8_t compression = r.u8();
  if (!r.ok || sid_length > 32) { fail(TlsError::Protocol, kAlertDecode); return false; }
  uint16_t selected_version = 0, key_share_group = 0;
  ConstByteView key_share_data, cookie;
  bool have_key_share = false, have_ems = false, have_reneg = false, have_psk = false, have_etm = false;
  uint16_t psk_selected = 0;
  char alpn_chosen[32] = {};
  if (r.n > 0) {
    const uint16_t ext_length = r.u16();
    Reader e(r.take(ext_length));
    if (!r.ok || !r.done()) { fail(TlsError::Protocol, kAlertDecode); return false; }
    while (e.n > 0) {
      const uint16_t type = e.u16();
      const uint16_t length = e.u16();
      Reader d(e.take(length));
      if (!e.ok) { fail(TlsError::Protocol, kAlertDecode); return false; }
      switch (type) {
        case kExtVersions: selected_version = d.u16(); if (!d.done()) { fail(TlsError::Protocol, kAlertDecode); return false; } break;
        case kExtKeyShare: {
          key_share_group = d.u16();
          have_key_share = true;
          if (d.n > 0) {   // not in a HelloRetryRequest
            const uint16_t kl = d.u16();
            key_share_data = d.take(kl);
          }
          if (!d.done()) { fail(TlsError::Protocol, kAlertDecode); return false; }
          break;
        }
        case kExtCookie: {
          const uint16_t cl = d.u16();
          cookie = d.take(cl);
          if (!d.done()) { fail(TlsError::Protocol, kAlertDecode); return false; }
          break;
        }
        case kExtPreSharedKey: psk_selected = d.u16(); have_psk = true; if (!d.done()) { fail(TlsError::Protocol, kAlertDecode); return false; } break;
        case kExtEms: have_ems = true; break;
        case kExtEncryptThenMac: have_etm = true; if (d.n != 0) { fail(TlsError::Protocol, kAlertDecode); return false; } break;
        case kExtRenegotiation: have_reneg = true; if (!(d.n == 1 && d.u8() == 0)) { fail(TlsError::Protocol, kAlertHandshakeFailure); return false; } break;
        case kExtAlpn: {
          d.u16();
          const uint8_t pl = d.u8();
          const ConstByteView proto = d.take(pl);
          if (!d.done() || pl >= sizeof alpn_chosen || !config_.alpn || pl != length_of(config_.alpn)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
          for (uint32_t i = 0; i < pl; ++i) alpn_chosen[i] = static_cast<char>(proto[i]);
          if (!equal_constant_time(reinterpret_cast<const uint8_t*>(alpn_chosen), reinterpret_cast<const uint8_t*>(config_.alpn), pl)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
          break;
        }
        default: break;   // server_name, ec_point_formats, session_ticket and unknown ones are ignored in TLS 1.2
      }
    }
  }
  (void)have_reneg;
  const bool hello_retry = random.size() == 32 && equal_constant_time(random.data(), kHelloRetryMagic, 32);
  const CipherSuite* suite = find_suite(suite_id);
  if (compression != 0 || !suite) { fail(TlsError::Unsupported, kAlertIllegalParameter); return false; }
  if (selected_version == 0x0304) {
    if (!config_.allow_tls13 || !suite->tls13 || legacy_version != 0x0303 || sid_length != 32 || !equal_constant_time(sid.data(), hs_->session_id, 32)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
    if (hello_retry) {
      if (!have_key_share || key_share_data.size() != 0) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
      return on_hello_retry_request(body, whole, suite_id, key_share_group, cookie);
    }
    if (!have_key_share || !hs_->share_count) { fail(TlsError::Protocol, kAlertMissingExtension); return false; }
    if (have_psk) {   // the server took the ticket: it must be the one offered, and for a suite with the same hash
      if (!hs_->psk_offered || psk_selected != 0 || tls::find_suite(hs_->ticket.suite_id)->hash != suite->hash) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
      hs_->psk_accepted = true;
    }
    suite_ = suite;
    tls13_ = true;
    hs_->selected_group = key_share_group;
    for (uint32_t i = 0; i < 32; ++i) hs_->server_random[i] = random[i];
    transcript_add(whole);
    // the shared secret
    const Handshake::Share* share = nullptr;
    for (uint32_t i = 0; i < hs_->share_count; ++i)
      if (hs_->shares[i].have && hs_->shares[i].group == key_share_group) share = &hs_->shares[i];
    if (!share) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
    uint8_t shared[48];
    uint32_t shared_length = 32;
    if (key_share_group == kGroupX25519) {
      if (key_share_data.size() != 32 || !x25519_shared_secret(share->priv, key_share_data.data(), shared)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
    } else {
      const EcCurve curve = curve_of(key_share_group);
      shared_length = ec_field_size(curve);
      if (key_share_data.size() != ec_point_size(curve) || !ec_ecdh(curve, share->priv, key_share_data, shared)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
    }
    if (!derive_handshake_keys_13(ConstByteView(shared, shared_length))) return false;
    for (uint8_t& b : shared) b = 0;
    // middlebox compatibility: one change_cipher_spec record, before anything encrypted is sent
    if (!hs_->sent_ccs) {
      const uint8_t ccs = 1;
      send_record(kChangeCipherSpec, ConstByteView(&ccs, 1));
      hs_->sent_ccs = true;
    }
    state_ = St::EncryptedExtensions;
    return true;
  }
  // TLS 1.2
  if (selected_version != 0 || !config_.allow_tls12 || legacy_version != 0x0303 || suite->tls13 || hello_retry) {
    fail(selected_version == 0 && legacy_version < 0x0303 ? TlsError::Unsupported : TlsError::Protocol, kAlertProtocolVersion);
    return false;
  }
  if (config_.allow_tls13) {   // downgrade protection (RFC 8446 4.1.3)
    static const uint8_t kDowngrade[7] = {'D', 'O', 'W', 'N', 'G', 'R', 'D'};
    if (equal_constant_time(random.data() + 24, kDowngrade, 7) && (random[31] == 0 || random[31] == 1)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
  }
  if (config_.require_extended_master_secret && !have_ems) { fail(TlsError::Unsupported, kAlertHandshakeFailure); return false; }
  if (suite->cbc() && !config_.allow_cbc_suites) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }   // (it was not offered)
  suite_ = suite;
  tls13_ = false;
  ems_ = have_ems;
  etm_ = have_etm && suite->cbc();
  for (uint32_t i = 0; i < 32; ++i) hs_->server_random[i] = random[i];
  for (uint32_t i = 0; i < sizeof alpn_selected_; ++i) alpn_selected_[i] = alpn_chosen[i];
  transcript_add(whole);
  state_ = St::Certificate12;
  return true;
}

bool TlsSocket::on_hello_retry_request(ConstByteView body, ConstByteView whole, uint16_t suite_id, uint16_t group, ConstByteView cookie) {
  (void)body;
  if (hs_->retried || !group_supported(group)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
  for (uint32_t i = 0; i < hs_->share_count; ++i)
    if (hs_->shares[i].have && hs_->shares[i].group == group) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }   // it would not ask for what we sent
  if (cookie.size() > sizeof hs_->cookie) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
  suite_ = find_suite(suite_id);
  // the transcript so far (ClientHello) is replaced by a message_hash message
  uint8_t digest[kMaxHashSize];
  transcript_hash(digest);
  const uint32_t hl = hash_size(suite_->hash);
  hs_->t256 = Hasher(HashKind::Sha256);
  hs_->t384 = Hasher(HashKind::Sha384);
  uint8_t synthetic[4 + kMaxHashSize] = {kHsMessageHash, 0, 0, static_cast<uint8_t>(hl)};
  for (uint32_t i = 0; i < hl; ++i) synthetic[4 + i] = digest[i];
  transcript_add(ConstByteView(synthetic, 4 + hl));
  transcript_add(whole);
  hs_->retried = true;
  hs_->selected_group = group;
  hs_->cookie_length = static_cast<uint32_t>(cookie.size());
  for (uint32_t i = 0; i < cookie.size(); ++i) hs_->cookie[i] = cookie[i];
  // a share for the group it wants
  Handshake::Share& s = hs_->shares[hs_->share_count++];
  s.group = group;
  if (group == kGroupX25519) {
    s.pub_length = 32;
    if (!config_.random->fill(ByteView(s.priv, 32))) { fail(TlsError::Internal, kAlertInternal); return false; }
    x25519_public_key(s.priv, s.pub);
  } else {
    const EcCurve curve = curve_of(group);
    s.pub_length = ec_point_size(curve);
    if (!ec_generate_private(curve, *config_.random, s.priv) || !ec_public_key(curve, s.priv, s.pub)) { fail(TlsError::Internal, kAlertInternal); return false; }
  }
  s.have = true;
  tls13_ = true;
  if (!hs_->sent_ccs) {
    const uint8_t ccs = 1;
    send_record(kChangeCipherSpec, ConstByteView(&ccs, 1));
    hs_->sent_ccs = true;
  }
  return build_client_hello(true);
}

// ---- TLS 1.3 key schedule and the server's flight

bool TlsSocket::set_traffic_keys(RecordProtection& side, const uint8_t* secret) {
  const HashKind h = suite_->hash;
  uint8_t key[32], iv[12];
  const ConstByteView s(secret, hash_size(h));
  if (!hkdf_expand_label(h, s, "key", ConstByteView(), ByteView(key, aead_key_size(suite_->aead))) || !hkdf_expand_label(h, s, "iv", ConstByteView(), ByteView(iv, 12))) return false;
  const bool ok = side.set_keys(suite_->aead, true, key, iv, 12);
  for (uint8_t& b : key) b = 0;
  return ok;
}

bool TlsSocket::derive_handshake_keys_13(ConstByteView shared) {
  const HashKind h = suite_->hash;
  const uint32_t hl = hash_size(h);
  uint8_t zeros[kMaxHashSize] = {0}, early[kMaxHashSize], derived[kMaxHashSize], empty[kMaxHashSize], th[kMaxHashSize];
  hash(h, ConstByteView(), empty);
  if (hs_->psk_accepted) tls::early_secret(h, ConstByteView(hs_->ticket.psk, hs_->ticket.psk_length), early);
  else hkdf_extract(h, ConstByteView(), ConstByteView(zeros, hl), early);
  if (!derive_secret(h, ConstByteView(early, hl), "derived", ConstByteView(empty, hl), derived)) { fail(TlsError::Internal, kAlertInternal); return false; }
  hkdf_extract(h, ConstByteView(derived, hl), shared, hs_->handshake_secret);
  transcript_hash(th);
  if (!derive_secret(h, ConstByteView(hs_->handshake_secret, hl), "c hs traffic", ConstByteView(th, hl), hs_->client_hs_secret) ||
      !derive_secret(h, ConstByteView(hs_->handshake_secret, hl), "s hs traffic", ConstByteView(th, hl), hs_->server_hs_secret) ||
      !derive_secret(h, ConstByteView(hs_->handshake_secret, hl), "derived", ConstByteView(empty, hl), derived)) { fail(TlsError::Internal, kAlertInternal); return false; }
  hkdf_extract(h, ConstByteView(derived, hl), ConstByteView(zeros, hl), hs_->master_secret);
  if (!set_traffic_keys(read_, hs_->server_hs_secret)) { fail(TlsError::Internal, kAlertInternal); return false; }
  return true;
}

bool TlsSocket::on_encrypted_extensions(ConstByteView body) {
  Reader r(body);
  const uint16_t length = r.u16();
  Reader e(r.take(length));
  if (!r.done()) { fail(TlsError::Protocol, kAlertDecode); return false; }
  while (e.n > 0) {
    const uint16_t type = e.u16();
    const uint16_t l = e.u16();
    Reader d(e.take(l));
    if (!e.ok) { fail(TlsError::Protocol, kAlertDecode); return false; }
    if (type == kExtAlpn) {
      d.u16();
      const uint8_t pl = d.u8();
      const ConstByteView proto = d.take(pl);
      if (!d.done() || !config_.alpn || pl != length_of(config_.alpn) || pl >= sizeof alpn_selected_ ||
          !equal_constant_time(proto.data(), reinterpret_cast<const uint8_t*>(config_.alpn), pl)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
      for (uint32_t i = 0; i < pl; ++i) alpn_selected_[i] = static_cast<char>(proto[i]);
      alpn_selected_[pl] = 0;
    }
  }
  return true;
}

bool TlsSocket::on_certificate(ConstByteView body, bool tls13) {
  CertificateProblem problem;
  if (!check_server_certificate(allocator_, config_, inner_->remote_address().value(), body, tls13, suite_ ? suite_->auth : SuiteAuth::Any, &hs_->leaf, &problem)) {
    cert_status_ = problem.status;
    fail(problem.error, problem.alert);
    return false;
  }
  cert_status_ = CertStatus::Ok;
  hs_->saw_certificate = true;
  return true;
}

bool TlsSocket::verify_signature_with_leaf(uint16_t scheme, const uint8_t* content, uint32_t content_length, ConstByteView signature, bool tls13) {
  return verify_leaf_signature(hs_->leaf.view(), scheme, ConstByteView(content, content_length), signature, tls13);
}

bool TlsSocket::on_certificate_verify(ConstByteView body) {
  Reader r(body);
  const uint16_t scheme = r.u16();
  const uint16_t sl = r.u16();
  const ConstByteView signature = r.take(sl);
  if (!r.done()) { fail(TlsError::Protocol, kAlertDecode); return false; }
  uint8_t content[64 + 34 + kMaxHashSize];
  uint32_t n = 0;
  for (int i = 0; i < 64; ++i) content[n++] = 0x20;
  for (const char* c = "TLS 1.3, server CertificateVerify"; *c; ++c) content[n++] = static_cast<uint8_t>(*c);
  content[n++] = 0;
  transcript_hash(content + n);
  n += hash_size(suite_->hash);
  if (!verify_signature_with_leaf(scheme, content, n, signature, true)) { fail(TlsError::Certificate, kAlertDecrypt); cert_status_ = CertStatus::BadSignature; return false; }
  return true;
}

bool TlsSocket::on_finished_13(ConstByteView body, ConstByteView whole) {
  const HashKind h = suite_->hash;
  const uint32_t hl = hash_size(h);
  uint8_t th[kMaxHashSize], key[kMaxHashSize], expected[kMaxHashSize];
  if (body.size() != hl) { fail(TlsError::Protocol, kAlertDecode); return false; }
  transcript_hash(th);
  if (!hkdf_expand_label(h, ConstByteView(hs_->server_hs_secret, hl), "finished", ConstByteView(), ByteView(key, hl))) { fail(TlsError::Internal, kAlertInternal); return false; }
  hmac(h, ConstByteView(key, hl), ConstByteView(th, hl), expected);
  if (!equal_constant_time(expected, body.data(), hl)) { fail(TlsError::Protocol, kAlertDecrypt); return false; }
  transcript_add(whole);
  // application secrets cover everything up to the server's Finished
  transcript_hash(th);
  if (!derive_secret(h, ConstByteView(hs_->master_secret, hl), "c ap traffic", ConstByteView(th, hl), client_traffic_secret_) ||
      !derive_secret(h, ConstByteView(hs_->master_secret, hl), "s ap traffic", ConstByteView(th, hl), server_traffic_secret_)) { fail(TlsError::Internal, kAlertInternal); return false; }
  // our flight under the handshake keys: an empty Certificate if one was asked for, then Finished
  if (!set_traffic_keys(write_, hs_->client_hs_secret)) { fail(TlsError::Internal, kAlertInternal); return false; }
  if (hs_->cert_requested) {
    const uint8_t empty_certificate[8] = {kHsCertificate, 0, 0, 4, 0, 0, 0, 0};
    if (!send_handshake(empty_certificate, 8)) { fail(TlsError::Internal, kAlertInternal); return false; }
  }
  transcript_hash(th);
  if (!hkdf_expand_label(h, ConstByteView(hs_->client_hs_secret, hl), "finished", ConstByteView(), ByteView(key, hl))) { fail(TlsError::Internal, kAlertInternal); return false; }
  uint8_t finished[4 + kMaxHashSize] = {kHsFinished, 0, 0, static_cast<uint8_t>(hl)};
  hmac(h, ConstByteView(key, hl), ConstByteView(th, hl), finished + 4);
  if (!send_handshake(finished, 4 + hl)) { fail(TlsError::Internal, kAlertInternal); return false; }
  // the resumption master secret covers everything up to our Finished; tickets sent later are made from it
  transcript_hash(th);
  have_resumption_ = tls::resumption_master_secret(h, ConstByteView(hs_->master_secret, hl), ConstByteView(th, hl), resumption_master_);
  resumed_ = hs_->psk_accepted;
  // switch to the application keys
  if (!set_traffic_keys(write_, client_traffic_secret_) || !set_traffic_keys(read_, server_traffic_secret_)) { fail(TlsError::Internal, kAlertInternal); return false; }
  finish_handshake();
  return true;
}

// ---- TLS 1.2

bool TlsSocket::on_server_key_exchange(ConstByteView body) {
  Reader r(body);
  const uint8_t curve_type = r.u8();
  const uint16_t group = r.u16();
  const uint8_t pl = r.u8();
  const ConstByteView pub = r.take(pl);
  if (!r.ok || curve_type != 3 || !group_supported(group)) { fail(TlsError::Unsupported, kAlertHandshakeFailure); return false; }
  const uint32_t expected = group == kGroupX25519 ? 32 : ec_point_size(curve_of(group));
  if (pl != expected || pl > sizeof hs_->ske_pub) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
  const uint32_t params_length = static_cast<uint32_t>(body.size() - r.n);
  const uint16_t scheme = r.u16();
  const uint16_t sl = r.u16();
  const ConstByteView signature = r.take(sl);
  if (!r.done()) { fail(TlsError::Protocol, kAlertDecode); return false; }
  if (!hs_->saw_certificate) { fail(TlsError::Protocol, kAlertUnexpected); return false; }
  uint8_t content[64 + 4 + 97];
  uint32_t n = 0;
  for (int i = 0; i < 32; ++i) content[n++] = hs_->client_random[i];
  for (int i = 0; i < 32; ++i) content[n++] = hs_->server_random[i];
  for (uint32_t i = 0; i < params_length; ++i) content[n++] = body[i];
  if (!verify_signature_with_leaf(scheme, content, n, signature, false)) { cert_status_ = CertStatus::BadSignature; fail(TlsError::Certificate, kAlertDecrypt); return false; }
  hs_->ske_group = group;
  hs_->ske_pub_length = pl;
  for (uint32_t i = 0; i < pl; ++i) hs_->ske_pub[i] = pub[i];
  return true;
}

bool TlsSocket::on_server_hello_done() {
  const CipherSuite* s = suite_;
  const uint32_t hl = hash_size(s->hash);
  // our ephemeral key and the pre-master secret
  uint8_t priv[48], pub[97], shared[48];
  uint32_t pub_length, shared_length;
  if (hs_->ske_group == kGroupX25519) {
    pub_length = 32;
    shared_length = 32;
    if (!config_.random->fill(ByteView(priv, 32))) { fail(TlsError::Internal, kAlertInternal); return false; }
    x25519_public_key(priv, pub);
    if (!x25519_shared_secret(priv, hs_->ske_pub, shared)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
  } else {
    const EcCurve curve = curve_of(hs_->ske_group);
    pub_length = ec_point_size(curve);
    shared_length = ec_field_size(curve);
    if (!ec_generate_private(curve, *config_.random, priv) || !ec_public_key(curve, priv, pub)) { fail(TlsError::Internal, kAlertInternal); return false; }
    if (!ec_ecdh(curve, priv, ConstByteView(hs_->ske_pub, hs_->ske_pub_length), shared)) { fail(TlsError::Protocol, kAlertIllegalParameter); return false; }
  }
  for (uint8_t& b : priv) b = 0;
  if (hs_->cert_requested) {   // we have no certificate to offer
    const uint8_t empty_certificate[7] = {kHsCertificate, 0, 0, 3, 0, 0, 0};
    if (!send_handshake(empty_certificate, 7)) { fail(TlsError::Internal, kAlertInternal); return false; }
  }
  uint8_t cke[4 + 1 + 97] = {kHsClientKeyExchange, 0, 0, static_cast<uint8_t>(1 + pub_length), static_cast<uint8_t>(pub_length)};
  for (uint32_t i = 0; i < pub_length; ++i) cke[5 + i] = pub[i];
  if (!send_handshake(cke, 5 + pub_length)) { fail(TlsError::Internal, kAlertInternal); return false; }
  // master secret
  if (ems_) {
    uint8_t session_hash[kMaxHashSize];
    transcript_hash(session_hash);
    tls12_prf(s->hash, ConstByteView(shared, shared_length), "extended master secret", ConstByteView(session_hash, hl), ConstByteView(), ByteView(hs_->master_secret, 48));
  } else {
    tls12_prf(s->hash, ConstByteView(shared, shared_length), "master secret", ConstByteView(hs_->client_random, 32), ConstByteView(hs_->server_random, 32), ByteView(hs_->master_secret, 48));
  }
  for (uint8_t& b : shared) b = 0;
  // keys
  const uint8_t ccs = 1;
  if (!send_record(kChangeCipherSpec, ConstByteView(&ccs, 1)) || !set_keys_12(write_, true)) { fail(TlsError::Internal, kAlertInternal); return false; }
  uint8_t th[kMaxHashSize];
  transcript_hash(th);
  uint8_t finished[4 + 12] = {kHsFinished, 0, 0, 12};
  tls12_prf(s->hash, ConstByteView(hs_->master_secret, 48), "client finished", ConstByteView(th, hl), ConstByteView(), ByteView(finished + 4, 12));
  if (!send_handshake(finished, 16)) { fail(TlsError::Internal, kAlertInternal); return false; }
  state_ = St::ChangeCipherSpec12;
  flush();
  return true;
}

// The key block of the connection (RFC 5246 6.3): for the AEAD suites the two keys and the two implicit IVs, for CBC the two MAC keys and the two keys.
bool TlsSocket::set_keys_12(RecordProtection& side, bool client_write) {
  const CipherSuite* s = suite_;
  uint8_t block[2 * 48 + 2 * 32 + 2 * 12];
  const uint32_t key_len = aead_key_size(s->aead);
  if (s->cbc()) {
    const uint32_t mac_len = hash_size(s->mac);
    tls12_prf(s->hash, ConstByteView(hs_->master_secret, 48), "key expansion", ConstByteView(hs_->server_random, 32), ConstByteView(hs_->client_random, 32), ByteView(block, 2 * mac_len + 2 * key_len));
    const uint8_t* mac_key = block + (client_write ? 0 : mac_len);
    const uint8_t* key = block + 2 * mac_len + (client_write ? 0 : key_len);
    const bool ok = side.set_cbc_keys(s->aead, s->mac, key, mac_key, etm_, config_.random);
    for (uint8_t& b : block) b = 0;
    return ok;
  }
  const uint32_t iv_len = s->aead == AeadKind::ChaCha20Poly1305 ? 12 : 4;
  tls12_prf(s->hash, ConstByteView(hs_->master_secret, 48), "key expansion", ConstByteView(hs_->server_random, 32), ConstByteView(hs_->client_random, 32), ByteView(block, 2 * key_len + 2 * iv_len));
  const bool ok = client_write ? side.set_keys(s->aead, false, block, block + 2 * key_len, iv_len) : side.set_keys(s->aead, false, block + key_len, block + 2 * key_len + iv_len, iv_len);
  for (uint8_t& b : block) b = 0;
  return ok;
}

bool TlsSocket::on_finished_12(ConstByteView body) {
  const uint32_t hl = hash_size(suite_->hash);
  uint8_t th[kMaxHashSize], expected[12];
  transcript_hash(th);
  tls12_prf(suite_->hash, ConstByteView(hs_->master_secret, 48), "server finished", ConstByteView(th, hl), ConstByteView(), ByteView(expected, 12));
  if (body.size() != 12 || !equal_constant_time(expected, body.data(), 12)) { fail(TlsError::Protocol, kAlertDecrypt); return false; }
  finish_handshake();
  return true;
}

void TlsSocket::finish_handshake() {
  if (hs_) {
    server_certificate_.clear();
    server_certificate_.append(hs_->leaf.view());
  }
  hs_.reset();
  state_ = St::Open;
}

// ---- after the handshake

bool TlsSocket::update_keys_13(bool send_side) {
  const HashKind h = suite_->hash;
  const uint32_t hl = hash_size(h);
  uint8_t* secret = send_side ? client_traffic_secret_ : server_traffic_secret_;
  uint8_t next[kMaxHashSize];
  if (!hkdf_expand_label(h, ConstByteView(secret, hl), "traffic upd", ConstByteView(), ByteView(next, hl))) return false;
  for (uint32_t i = 0; i < hl; ++i) secret[i] = next[i];
  return set_traffic_keys(send_side ? write_ : read_, secret);
}

bool TlsSocket::on_post_handshake(uint8_t type, ConstByteView body) {
  if (!tls13_) {
    if (type == 0) {   // HelloRequest: renegotiation is not supported
      send_alert(1, kAlertNoRenegotiation);
      return true;
    }
    fail(TlsError::Protocol, kAlertUnexpected);
    return false;
  }
  if (type == kHsNewSessionTicket) {
    if (config_.sessions && have_resumption_ && config_.server_name && suite_) {
      UniquePointer<TlsTicket> ticket = UniquePointer<TlsTicket>::make(allocator_);
      if (ticket && tls::make_ticket(suite_->hash, suite_->id, ConstByteView(resumption_master_, hash_size(suite_->hash)), body, config_.server_name, false, stack_.now_ms(), ticket.get()))
        config_.sessions->store(*ticket);
    }
    return true;
  }
  if (type == kHsKeyUpdate) {
    if (body.size() != 1 || body[0] > 1) { fail(TlsError::Protocol, kAlertDecode); return false; }
    if (!update_keys_13(false)) { fail(TlsError::Internal, kAlertInternal); return false; }
    if (body[0] == 1) {   // asked to do the same for our direction
      const uint8_t message[5] = {kHsKeyUpdate, 0, 0, 1, 0};
      if (!send_handshake(message, 5) || !update_keys_13(true)) { fail(TlsError::Internal, kAlertInternal); return false; }
      flush();
    }
    return true;
  }
  fail(TlsError::Protocol, kAlertUnexpected);
  return false;
}

}  // namespace network
