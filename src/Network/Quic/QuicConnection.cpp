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

#include "Network/Quic/QuicConnection.hpp"
#include "Utility/Move.hpp"

namespace network {

using namespace quic;

const char* quic_error_name(QuicError error) {
  switch (error) {
    case QuicError::None: return "none";
    case QuicError::Connect: return "no UDP socket";
    case QuicError::Timeout: return "timed out";
    case QuicError::Version: return "QUIC version not supported";
    case QuicError::Tls: return "TLS failed";
    case QuicError::Protocol: return "QUIC protocol violation";
    case QuicError::PeerClosed: return "closed by the server";
    case QuicError::StatelessReset: return "stateless reset";
    case QuicError::Internal: return "internal error";
    case QuicError::Closed: return "closed";
  }
  return "";
}

namespace {

// QUIC transport error codes (RFC 9000 20.1)
constexpr uint64_t kNoError = 0, kInternalError = 1, kFlowControlError = 3, kStreamLimitError = 4, kStreamStateError = 5, kFinalSizeError = 6, kFrameEncodingError = 7,
                   kTransportParameterError = 8, kProtocolViolation = 10, kCryptoBufferExceeded = 13, kCryptoErrorBase = 0x100;

// transport parameter ids
constexpr uint64_t kParamOriginalDcid = 0x00, kParamIdleTimeout = 0x01, kParamResetToken = 0x02, kParamMaxUdpPayload = 0x03, kParamMaxData = 0x04, kParamBidiLocal = 0x05,
                   kParamBidiRemote = 0x06, kParamUni = 0x07, kParamMaxStreamsBidi = 0x08, kParamMaxStreamsUni = 0x09, kParamAckDelayExponent = 0x0a, kParamMaxAckDelay = 0x0b,
                   kParamDisableMigration = 0x0c, kParamPreferredAddress = 0x0d, kParamActiveCidLimit = 0x0e, kParamInitialScid = 0x0f, kParamRetryScid = 0x10;

void write_parameter(ByteWriter& w, uint64_t id, uint64_t value) {
  write_varint(w, id);
  write_varint(w, varint_size(value));
  write_varint(w, value);
}

}  // namespace

QuicConnection::QuicConnection(INetworkStack& stack, allocators::IAllocator& allocator, const QuicConfig& config)
    : stack_(stack), allocator_(allocator), config_(config), peer_close_reason_(allocator), spaces_{Space(allocator), Space(allocator), Space(allocator)}, retry_token_(allocator),
      peer_cids_(allocator), retire_queue_(allocator), streams_(allocator), stash_(allocator), path_response_(allocator), close_reason_(allocator) {
  if (config_.max_datagram < 1200) config_.max_datagram = 1200;
  if (config_.max_datagram > 1472) config_.max_datagram = 1472;
  idle_timeout_ms_ = config_.idle_timeout_ms;
  const uint64_t md = config_.max_datagram;
  congestion_window_ = 10 * md < (md * 2 > 14720 ? md * 2 : 14720) ? 10 * md : (md * 2 > 14720 ? md * 2 : 14720);
}

QuicConnection::~QuicConnection() {
  if (socket_) socket_->close();
}

void QuicConnection::fail(QuicError error, uint64_t transport_error) {
  if (state_ == QuicState::Failed || state_ == QuicState::Closed) return;
  error_ = error;
  transport_error_ = transport_error;
  // tell the server why when we are the one that found the fault
  if (error == QuicError::Protocol || error == QuicError::Tls) {
    close_pending_ = true;
    close_application_ = false;
    close_code_ = error == QuicError::Tls && tls_ ? kCryptoErrorBase + tls_->alert() : transport_error;
    send_pending();
  }
  state_ = QuicState::Failed;
}

QuicConnection::Stats QuicConnection::stats() const {
  Stats s = stats_;
  s.smoothed_rtt_ms = static_cast<uint32_t>(smoothed_rtt_ms_);
  s.congestion_window = static_cast<uint32_t>(congestion_window_);
  s.bytes_in_flight = bytes_in_flight_;
  return s;
}

// ---- Starting

bool QuicConnection::connect(Ipv4Address address, uint16_t port, StringView server_name) {
  if (state_ != QuicState::Idle) return false;
  crypto::IRandom* random = config_.tls.random;
  if (!random) {
    error_ = QuicError::Internal;
    state_ = QuicState::Failed;
    return false;
  }
  uint8_t id[8];
  if (!random->fill(ByteView(id)) || !original_dcid_.set(ConstByteView(id)) || !random->fill(ByteView(id)) || !scid_.set(ConstByteView(id))) {
    error_ = QuicError::Internal;
    state_ = QuicState::Failed;
    return false;
  }
  dcid_ = original_dcid_;
  socket_ = stack_.udp_open(0);
  if (!socket_) {
    error_ = QuicError::Connect;
    state_ = QuicState::Failed;
    return false;
  }
  peer_address_ = address;
  peer_port_ = port;
  // the keys everyone can compute
  Space& initial = spaces_[0];
  if (!derive_initial_keys(original_dcid_.view(), &initial.send_keys, &initial.receive_keys)) {
    error_ = QuicError::Internal;
    state_ = QuicState::Failed;
    return false;
  }
  initial.keys_ready = true;
  // our transport parameters (RFC 9000 18.2)
  uint8_t storage[256];
  ByteWriter w{ByteView(storage)};
  write_parameter(w, kParamIdleTimeout, config_.idle_timeout_ms);
  write_parameter(w, kParamMaxUdpPayload, config_.max_datagram);
  write_parameter(w, kParamMaxData, config_.connection_window);
  write_parameter(w, kParamBidiLocal, config_.stream_window);
  write_parameter(w, kParamBidiRemote, 0);
  write_parameter(w, kParamUni, 256 * 1024);
  write_parameter(w, kParamMaxStreamsBidi, 0);
  write_parameter(w, kParamMaxStreamsUni, config_.max_incoming_streams);
  write_parameter(w, kParamActiveCidLimit, 4);
  write_varint(w, kParamDisableMigration);
  write_varint(w, 0);
  write_varint(w, kParamInitialScid);
  write_varint(w, scid_.length);
  w.bytes(scid_.view());
  if (!w.ok()) {
    error_ = QuicError::Internal;
    state_ = QuicState::Failed;
    return false;
  }
  recv_max_data_ = config_.connection_window;
  local_max_streams_uni_ = config_.max_incoming_streams;
  // TLS
  TlsConfig tls = config_.tls;
  char name[256];
  uint32_t n = 0;
  for (; n < server_name.size() && n + 1 < sizeof name; ++n) name[n] = server_name[n];
  name[n] = 0;
  tls.server_name = n ? name : nullptr;
  tls.alpn = config_.tls.alpn ? config_.tls.alpn : "h3";
  tls_ = UniquePointer<Tls13Client>::make(allocator_, allocator_, tls, address.value(), w.written());
  if (tls_) tls_->set_time_source([](void* stack) { return static_cast<INetworkStack*>(stack)->now_ms(); }, &stack_);
  if (!tls_ || !tls_->start()) {
    error_ = tls_ && tls_->failed() ? QuicError::Tls : QuicError::Internal;
    state_ = QuicState::Failed;
    return false;
  }
  started_ms_ = last_activity_ms_ = stack_.now_ms();
  state_ = QuicState::Connecting;
  advance_tls();
  send_pending();
  return state_ != QuicState::Failed;
}

// ---- TLS plumbing

void QuicConnection::install_keys(TlsLevel level) {
  const tls::CipherSuite* suite = tls_->suite();
  Space& space = spaces_[static_cast<uint32_t>(level)];
  if (!suite || space.keys_ready || space.discarded) return;
  if (!space.send_keys.set_secret(suite->aead, suite->hash, tls_->client_secret(level)) || !space.receive_keys.set_secret(suite->aead, suite->hash, tls_->server_secret(level))) {
    fail(QuicError::Internal);
    return;
  }
  space.keys_ready = true;
  if (level == TlsLevel::Application) have_next_keys_ = space.receive_keys.next_generation(&next_receive_keys_);
}

void QuicConnection::advance_tls() {
  if (!tls_) return;
  for (uint32_t level = 0; level < kSpaceCount; ++level) {
    const ConstByteView out = tls_->pending_output(static_cast<TlsLevel>(level));
    if (out.empty()) continue;
    spaces_[level].crypto_send.write(out, ~uint64_t{0} >> 2);
    tls_->consume_output(static_cast<TlsLevel>(level), out.size());
  }
  if (tls_->failed()) return fail(QuicError::Tls);
  if (tls_->has_secrets(TlsLevel::Handshake)) install_keys(TlsLevel::Handshake);
  if (tls_->has_secrets(TlsLevel::Application)) install_keys(TlsLevel::Application);
  if (!transport_parameters_applied_ && !tls_->peer_parameters().empty() && !apply_peer_parameters()) return;
  if (tls_->handshake_complete() && spaces_[2].keys_ready && state_ == QuicState::Connecting) {
    if (!transport_parameters_applied_) return fail(QuicError::Protocol, kTransportParameterError);
    state_ = QuicState::Established;
  }
}

bool QuicConnection::apply_peer_parameters() {
  ByteReader r(tls_->peer_parameters());
  bool have_original = false, have_initial_scid = false, have_retry_scid = false;
  while (r.remaining() > 0) {
    const uint64_t id = read_varint(r);
    const uint64_t length = read_varint(r);
    ByteReader v(r.take(length));
    if (!r.ok()) {
      fail(QuicError::Protocol, kTransportParameterError);
      return false;
    }
    auto number = [&]() { return read_varint(v); };
    switch (id) {
      case kParamOriginalDcid: {
        const ConstByteView cid = v.rest();
        have_original = equal_constant_time_bytes(original_dcid_.view(), cid);
        if (!have_original) {
          fail(QuicError::Protocol, kTransportParameterError);
          return false;
        }
        break;
      }
      case kParamIdleTimeout: peer_idle_timeout_ms_ = static_cast<uint32_t>(number()); break;
      case kParamResetToken:
        if (v.remaining() != 16) {
          fail(QuicError::Protocol, kTransportParameterError);
          return false;
        }
        v.read_into(ByteView(peer_reset_token_));
        have_peer_reset_token_ = true;
        break;
      case kParamMaxUdpPayload: {
        const uint64_t n = number();
        if (n < 1200) {
          fail(QuicError::Protocol, kTransportParameterError);
          return false;
        }
        peer_max_udp_payload_ = n > 65527 ? 65527 : static_cast<uint32_t>(n);
        break;
      }
      case kParamMaxData: peer_max_data_ = number(); break;
      case kParamBidiLocal: peer_initial_bidi_local_ = number(); break;
      case kParamBidiRemote: peer_initial_bidi_remote_ = number(); break;
      case kParamUni: peer_initial_uni_ = number(); break;
      case kParamMaxStreamsBidi: peer_max_streams_bidi_ = number(); break;
      case kParamMaxStreamsUni: peer_max_streams_uni_ = number(); break;
      case kParamAckDelayExponent: {
        const uint64_t n = number();
        if (n > 20) {
          fail(QuicError::Protocol, kTransportParameterError);
          return false;
        }
        peer_ack_delay_exponent_ = static_cast<uint32_t>(n);
        break;
      }
      case kParamMaxAckDelay: {
        const uint64_t n = number();
        if (n >= 16384) {
          fail(QuicError::Protocol, kTransportParameterError);
          return false;
        }
        peer_max_ack_delay_ms_ = static_cast<uint32_t>(n);
        break;
      }
      case kParamActiveCidLimit: {
        const uint64_t n = number();
        if (n < 2) {
          fail(QuicError::Protocol, kTransportParameterError);
          return false;
        }
        peer_active_cid_limit_ = n > 8 ? 8 : static_cast<uint32_t>(n);
        break;
      }
      case kParamInitialScid: {
        const ConstByteView cid = v.rest();
        have_initial_scid = equal_constant_time_bytes(dcid_.view(), cid);
        if (!have_initial_scid) {
          fail(QuicError::Protocol, kTransportParameterError);
          return false;
        }
        break;
      }
      case kParamRetryScid: {
        const ConstByteView cid = v.rest();
        have_retry_scid = retry_seen_ && equal_constant_time_bytes(retry_source_cid_.view(), cid);
        if (!have_retry_scid) {
          fail(QuicError::Protocol, kTransportParameterError);
          return false;
        }
        break;
      }
      default: break;   // disable_active_migration, preferred_address, unknown ones
    }
    if (!v.ok()) {
      fail(QuicError::Protocol, kTransportParameterError);
      return false;
    }
  }
  if (!have_original || !have_initial_scid || (retry_seen_ && !have_retry_scid)) {
    fail(QuicError::Protocol, kTransportParameterError);
    return false;
  }
  transport_parameters_applied_ = true;
  // the effective idle timeout is the smaller of the two that are not 0
  if (peer_idle_timeout_ms_ != 0 && (idle_timeout_ms_ == 0 || peer_idle_timeout_ms_ < idle_timeout_ms_)) idle_timeout_ms_ = peer_idle_timeout_ms_;
  // streams opened already (none yet in practice) take the limits
  for (uint64_t i = 0; i < streams_.count(); ++i) {
    Stream& s = *streams_[i];
    if (stream_is_ours(s.id)) s.send_limit = (s.id & 2) ? peer_initial_uni_ : peer_initial_bidi_remote_;
  }
  return true;
}

void QuicConnection::discard_space(uint32_t space_index) {
  Space& space = spaces_[space_index];
  if (space.discarded) return;
  space.discarded = true;
  space.keys_ready = false;
  space.send_keys.wipe();
  space.receive_keys.wipe();
  // what was in flight in it no longer counts
  for (uint64_t i = 0; i < space.sent.count(); ++i)
    if (space.sent[i].in_flight && bytes_in_flight_ >= space.sent[i].size) bytes_in_flight_ -= space.sent[i].size;
  space.sent.clear();
  space.loss_time_ms = 0;
  space.ack_pending = false;
  space.probes = 0;
  set_loss_timer();
}

// ---- Receiving datagrams

void QuicConnection::drain_socket() {
  if (!socket_) return;
  uint8_t buffer[2048];
  for (uint32_t guard = 0; guard < 256 && socket_->pending() > 0 && state_ != QuicState::Failed && state_ != QuicState::Closed; ++guard) {
    Ipv4Address from;
    uint16_t from_port = 0;
    uint32_t length = 0;
    if (!socket_->receive_from(ByteView(buffer), &from, &from_port, &length)) break;
    if (from != peer_address_ || from_port != peer_port_ || length == 0 || length > sizeof buffer) {
      ++stats_.datagrams_dropped;
      continue;
    }
    stats_.bytes_received += length;
    on_datagram(ByteView(buffer, length));
  }
}

void QuicConnection::on_datagram(ByteView datagram) {
  uint64_t at = 0;
  while (at < datagram.size() && state_ != QuicState::Failed && state_ != QuicState::Closed) {
    ByteView packet = datagram.subview(at);
    uint32_t consumed = 0;
    if (!on_packet(packet, &consumed) || consumed == 0) break;
    at += consumed;
  }
}

// One packet at the start of `packet` (a datagram can hold several). `*consumed` is how many bytes it took; false if the rest cannot be used.
bool QuicConnection::on_packet(ByteView packet, uint32_t* consumed) {
  if ((packet[0] & 0x80) != 0) {
    uint32_t length = 0;
    const bool ok = on_long_packet(packet, &length);
    *consumed = length;
    return ok;
  }
  *consumed = static_cast<uint32_t>(packet.size());
  return on_short_packet(packet);
}

bool QuicConnection::on_long_packet(ByteView packet, uint32_t* packet_length) {
  *packet_length = static_cast<uint32_t>(packet.size());
  ByteReader r{ConstByteView(packet)};
  const uint8_t first = r.u8();
  const uint32_t version = r.be32();
  const ConstByteView dcid = r.take(r.u8());
  const ConstByteView scid = r.take(r.u8());
  if (!r.ok()) return false;
  if (version == 0) {   // version negotiation: the server does not speak version 1
    fail(QuicError::Version);
    return false;
  }
  if (version != kVersion1) return false;
  const uint32_t type = (first >> 4) & 3;
  if (type == 3) {
    on_retry(ConstByteView(packet));
    return false;
  }
  if (type == 1) return false;   // 0-RTT: never sent to a client
  if (dcid.size() != scid_.length || !equal_constant_time_bytes(dcid, scid_.view())) return false;
  const uint32_t space_index = type == 0 ? 0 : 1;
  if (type == 0) {   // the token field of an Initial packet (a server sends none)
    const uint64_t token = read_varint(r);
    r.take(token);
  }
  const uint64_t length = read_varint(r);
  if (!r.ok() || length > r.remaining() || length < 4 + 16) return false;
  const uint32_t pn_offset = static_cast<uint32_t>(r.position());
  const uint32_t end = pn_offset + static_cast<uint32_t>(length);
  *packet_length = end;
  Space& space = spaces_[space_index];
  if (!space.keys_ready) return true;   // keys not (yet) had, or dropped: the packet is let go
  ByteView whole = packet.first(end);
  uint32_t pn_length = 0;
  uint64_t truncated = 0;
  if (!remove_header_protection(space.receive_keys, whole, pn_offset, true, &pn_length, &truncated)) return true;
  const uint64_t number = expand_packet_number(space.largest_received, truncated, pn_length);
  if (!decrypt_packet(space.receive_keys, number, whole, pn_offset + pn_length)) {
    ++stats_.datagrams_dropped;
    return true;
  }
  if ((whole[0] & 0x0C) != 0) {
    fail(QuicError::Protocol, kProtocolViolation);
    return false;
  }
  if (!dcid_known_) {   // the server's first packet names the connection ID it wants us to use
    if (!dcid_.set(scid)) return false;
    dcid_known_ = true;
  }
  ++stats_.packets_received;
  last_activity_ms_ = stack_.now_ms();
  if (space.received.contains(number)) return true;   // a packet we already have
  space.received.add(number, number);
  space.received.trim(64);
  if (static_cast<int64_t>(number) > space.largest_received) {
    space.largest_received = static_cast<int64_t>(number);
    space.largest_received_ms = last_activity_ms_;
  }
  bool ack_eliciting = false;
  const ConstByteView payload(whole.data() + pn_offset + pn_length, end - pn_offset - pn_length - 16);
  if (!process_payload(space_index, payload, &ack_eliciting)) return false;
  if (ack_eliciting) {
    space.ack_pending = true;
    ++space.unacked_ack_eliciting;
  }
  return true;
}

bool QuicConnection::on_short_packet(ByteView packet) {
  Space& space = spaces_[2];
  if (!space.keys_ready) return false;
  const uint32_t pn_offset = 1 + scid_.length;
  if (packet.size() < pn_offset + 4 + 16 || !equal_constant_time_bytes(ConstByteView(packet.data() + 1, scid_.length), scid_.view())) return false;
  uint32_t pn_length = 0;
  uint64_t truncated = 0;
  if (!remove_header_protection(space.receive_keys, packet, pn_offset, false, &pn_length, &truncated)) return false;
  const bool phase = (packet[0] & 0x04) != 0;
  const uint64_t number = expand_packet_number(space.largest_received, truncated, pn_length);
  const uint32_t header_length = pn_offset + pn_length;
  bool opened = false, updated = false;
  if (phase == key_phase_) {
    opened = decrypt_packet(space.receive_keys, number, packet, header_length);
  } else if (static_cast<int64_t>(number) > space.largest_received && have_next_keys_) {   // the server has updated its keys
    opened = decrypt_packet(next_receive_keys_, number, packet, header_length);
    updated = opened;
  } else if (have_old_keys_) {   // a packet from before the update that arrived late
    opened = decrypt_packet(old_receive_keys_, number, packet, header_length);
  }
  if (!opened) {
    // a stateless reset looks like a packet that cannot be read and ends with the token the server gave
    if (have_peer_reset_token_ && packet.size() >= 21 && crypto::equal_constant_time(packet.data() + packet.size() - 16, peer_reset_token_, 16)) {
      fail(QuicError::StatelessReset);
      return false;
    }
    ++stats_.datagrams_dropped;
    return false;
  }
  if ((packet[0] & 0x18) != 0) {
    fail(QuicError::Protocol, kProtocolViolation);
    return false;
  }
  if (updated) {
    old_receive_keys_ = space.receive_keys;
    have_old_keys_ = true;
    old_keys_until_ = stack_.now_ms() + 3 * pto_interval_ms(2);
    space.receive_keys = next_receive_keys_;
    have_next_keys_ = space.receive_keys.next_generation(&next_receive_keys_);
    PacketKeys next_send;
    if (!space.send_keys.next_generation(&next_send)) return fail(QuicError::Internal), false;
    space.send_keys = next_send;
    key_phase_ = phase;
    ++stats_.key_updates;
  }
  ++stats_.packets_received;
  last_activity_ms_ = stack_.now_ms();
  if (space.received.contains(number)) return false;
  space.received.add(number, number);
  space.received.trim(64);
  if (static_cast<int64_t>(number) > space.largest_received) {
    space.largest_received = static_cast<int64_t>(number);
    space.largest_received_ms = last_activity_ms_;
  }
  bool ack_eliciting = false;
  const ConstByteView payload(packet.data() + header_length, packet.size() - header_length - 16);
  if (!process_payload(2, payload, &ack_eliciting)) return false;
  if (ack_eliciting) {
    space.ack_pending = true;
    ++space.unacked_ack_eliciting;
  }
  return false;   // (a short header packet takes the rest of the datagram)
}

void QuicConnection::on_retry(ConstByteView packet) {
  if (retry_seen_ || dcid_known_ || packet.size() < 1 + 4 + 1 + 1 + 16) return;
  ByteReader r(packet);
  r.u8();
  r.be32();
  r.take(r.u8());
  const ConstByteView scid = r.take(r.u8());
  if (!r.ok() || scid.size() > kMaxConnectionIdLength || r.remaining() < 16) return;
  const ConstByteView token = r.take(r.remaining() - 16);
  const ConstByteView tag = r.take(16);
  uint8_t expected[16];
  if (!retry_integrity_tag(original_dcid_.view(), packet.first(packet.size() - 16), expected) || !crypto::equal_constant_time(expected, tag.data(), 16)) return;
  if (token.empty()) return;
  retry_seen_ = true;
  stats_.retry_seen = true;
  retry_source_cid_.set(scid);
  dcid_.set(scid);
  retry_token_.clear();
  retry_token_.append(token);
  // new Initial keys from the new connection ID; what was sent in the first Initial packets goes out again
  Space& initial = spaces_[0];
  if (!derive_initial_keys(dcid_.view(), &initial.send_keys, &initial.receive_keys)) return fail(QuicError::Internal);
  for (uint64_t i = 0; i < initial.sent.count(); ++i) {
    if (initial.sent[i].in_flight && bytes_in_flight_ >= initial.sent[i].size) bytes_in_flight_ -= initial.sent[i].size;
    requeue_frames(0, initial.sent[i]);
  }
  initial.sent.clear();
  initial.loss_time_ms = 0;
  pto_count_ = 0;
  set_loss_timer();
}

// ---- Frames

bool QuicConnection::process_payload(uint32_t space_index, ConstByteView payload, bool* ack_eliciting) {
  ByteReader r(payload);
  const bool early = space_index != 2;   // Initial and Handshake packets carry only a few kinds of frame
  while (r.remaining() > 0) {
    const uint64_t type = read_varint(r);
    if (!r.ok()) break;
    if (early && !(type == 0x00 || type == 0x01 || type == 0x02 || type == 0x03 || type == 0x06 || type == 0x1c)) {
      fail(QuicError::Protocol, kProtocolViolation);
      return false;
    }
    if (type != 0x00 && type != 0x02 && type != 0x03 && type != 0x1c && type != 0x1d) *ack_eliciting = true;
    if (type == 0x00) {   // padding: runs of zero bytes
      while (r.remaining() > 0) {
        uint8_t next = 0;
        if (!r.peek_u8(0, &next) || next != 0) break;
        r.u8();
      }
    } else if (type == 0x01) {
      // PING: nothing to do but acknowledge it
    } else if (type == 0x02 || type == 0x03) {
      if (!on_ack(space_index, r, type == 0x03)) return false;
    } else if (type == 0x04) {   // RESET_STREAM
      const uint64_t id = read_varint(r), code = read_varint(r), final_size = read_varint(r);
      if (!r.ok()) break;
      Stream* s = get_stream(id, true);
      if (!s || (stream_is_ours(id) && (id & 2))) {
        fail(QuicError::Protocol, kStreamStateError);
        return false;
      }
      if (!s->reset_received) {
        s->reset_received = true;
        s->reset_code = code;
        s->reset_final_size = final_size;
        // the bytes that will never come still count against the connection's limit
        if (final_size > s->recv.highest()) {
          data_received_ += final_size - s->recv.highest();
          if (data_received_ > recv_max_data_) {
            fail(QuicError::Protocol, kFlowControlError);
            return false;
          }
        }
      }
    } else if (type == 0x05) {   // STOP_SENDING
      const uint64_t id = read_varint(r), code = read_varint(r);
      if (!r.ok()) break;
      Stream* s = find_stream(id);
      if (!s) {
        if (!stream_is_ours(id) || (id & 2)) {
          fail(QuicError::Protocol, kStreamStateError);
          return false;
        }
        continue;
      }
      s->stop_requested = true;
      s->stop_code = code;
      if (!s->reset_sent) {   // answer with a reset of our side
        s->reset_to_send = true;
        s->reset_to_send_code = code;
      }
    } else if (type == 0x06) {
      if (!on_crypto(space_index, r)) return false;
    } else if (type == 0x07) {   // NEW_TOKEN
      const uint64_t n = read_varint(r);
      r.take(n);
    } else if (type >= 0x08 && type <= 0x0f) {
      if (!on_stream(static_cast<uint8_t>(type), r)) return false;
    } else if (type == 0x10) {   // MAX_DATA
      const uint64_t v = read_varint(r);
      if (r.ok() && v > peer_max_data_) peer_max_data_ = v;
    } else if (type == 0x11) {   // MAX_STREAM_DATA
      const uint64_t id = read_varint(r), v = read_varint(r);
      if (!r.ok()) break;
      Stream* s = find_stream(id);
      if (s && v > s->send_limit) s->send_limit = v;
    } else if (type == 0x12) {   // MAX_STREAMS (bidirectional)
      const uint64_t v = read_varint(r);
      if (r.ok() && v > peer_max_streams_bidi_) peer_max_streams_bidi_ = v;
    } else if (type == 0x13) {   // MAX_STREAMS (unidirectional)
      const uint64_t v = read_varint(r);
      if (r.ok() && v > peer_max_streams_uni_) peer_max_streams_uni_ = v;
    } else if (type == 0x14 || type == 0x16 || type == 0x17) {   // DATA_BLOCKED, STREAMS_BLOCKED: nothing to do
      read_varint(r);
    } else if (type == 0x15) {   // STREAM_DATA_BLOCKED
      read_varint(r);
      read_varint(r);
    } else if (type == 0x18) {
      if (!on_new_connection_id(r)) return false;
    } else if (type == 0x19) {   // RETIRE_CONNECTION_ID: the one connection ID we gave is not going away unless the server wants it to
      read_varint(r);
    } else if (type == 0x1a) {   // PATH_CHALLENGE
      const ConstByteView data = r.take(8);
      if (r.ok() && path_response_.size() < 64) path_response_.append(data);
    } else if (type == 0x1b) {   // PATH_RESPONSE
      r.take(8);
    } else if (type == 0x1c || type == 0x1d) {   // CONNECTION_CLOSE
      peer_close_code_ = read_varint(r);
      if (type == 0x1c) read_varint(r);
      const uint64_t n = read_varint(r);
      const ConstByteView reason = r.take(n);
      if (!r.ok()) break;
      peer_close_application_ = type == 0x1d;
      peer_close_reason_.assign(StringView(reinterpret_cast<const char*>(reason.data()), reason.size() > 200 ? 200 : reason.size()));
      error_ = QuicError::PeerClosed;
      state_ = QuicState::Closed;
      return false;
    } else if (type == 0x1e) {   // HANDSHAKE_DONE
      handshake_done_received_ = true;
      handshake_confirmed_ = true;
      discard_space(1);
    } else {
      fail(QuicError::Protocol, kFrameEncodingError);
      return false;
    }
    if (!r.ok()) break;
  }
  if (!r.ok()) {
    fail(QuicError::Protocol, kFrameEncodingError);
    return false;
  }
  return state_ != QuicState::Failed && state_ != QuicState::Closed;
}

bool QuicConnection::on_crypto(uint32_t space_index, ByteReader& r) {
  const uint64_t offset = read_varint(r), length = read_varint(r);
  const ConstByteView data = r.take(length);
  if (!r.ok()) return true;   // (reported by the caller as a malformed frame)
  Space& space = spaces_[space_index];
  if (offset + length > (1u << 20)) {
    fail(QuicError::Protocol, kCryptoBufferExceeded);
    return false;
  }
  if (space.crypto_recv.add(offset, data, false) != RecvBuffer::Result::Ok) {
    fail(QuicError::Protocol, kProtocolViolation);
    return false;
  }
  // what is in order goes to TLS
  while (space.crypto_recv.available() > 0 && tls_) {
    const ConstByteView ready = space.crypto_recv.peek();
    const uint64_t n = ready.size();
    if (!tls_->receive(static_cast<TlsLevel>(space_index), ready)) {
      fail(QuicError::Tls);
      return false;
    }
    space.crypto_recv.discard(n);   // (TLS has taken it)
    advance_tls();
    if (state_ == QuicState::Failed) return false;
  }
  return true;
}

bool QuicConnection::on_new_connection_id(ByteReader& r) {
  const uint64_t sequence = read_varint(r), retire_prior_to = read_varint(r);
  const uint8_t length = r.u8();
  const ConstByteView id = r.take(length);
  const ConstByteView token = r.take(16);
  if (!r.ok()) return true;
  if (length < 1 || length > kMaxConnectionIdLength || retire_prior_to > sequence) {
    fail(QuicError::Protocol, kFrameEncodingError);
    return false;
  }
  for (uint64_t i = 0; i < peer_cids_.count(); ++i)
    if (peer_cids_[i].sequence == sequence) return true;   // (a repeat)
  ConnectionIdEntry entry;
  entry.sequence = sequence;
  entry.id.set(id);
  for (uint32_t i = 0; i < 16; ++i) entry.reset_token[i] = token[i];
  // connection IDs below retire_prior_to are given back
  for (uint64_t i = peer_cids_.count(); i > 0; --i) {
    if (peer_cids_[i - 1].sequence < retire_prior_to) {
      retire_queue_.push_back(peer_cids_[i - 1].sequence);
      peer_cids_.remove_at(i - 1);
    }
  }
  if (sequence >= retire_prior_to) {
    if (peer_cids_.count() >= 8) return true;   // more than we asked for: ignored
    peer_cids_.push_back(entry);
  } else {
    retire_queue_.push_back(sequence);
  }
  return true;
}

QuicConnection::Stream* QuicConnection::find_stream(uint64_t id) const {
  for (uint64_t i = 0; i < streams_.count(); ++i)
    if (streams_[i]->id == id) return streams_[i].get();
  return nullptr;
}

QuicConnection::Stream* QuicConnection::get_stream(uint64_t id, bool create) {
  Stream* s = find_stream(id);
  if (s || !create) return s;
  if (stream_is_ours(id)) return nullptr;   // our streams are made by open_stream()
  // a stream of the server's: only unidirectional ones (we allow no others), within the limit we gave
  if ((id & 2) == 0) return nullptr;
  const uint64_t index = id >> 2;
  if (index >= local_max_streams_uni_) {
    fail(QuicError::Protocol, kStreamLimitError);
    return nullptr;
  }
  auto fresh = UniquePointer<Stream>::make(allocator_, allocator_, id);
  if (!fresh) return nullptr;
  fresh->recv_limit = config_.stream_window < 256 * 1024 ? config_.stream_window : 256 * 1024;
  fresh->send_limit = 0;
  if (!streams_.push_back(util::move(fresh))) return nullptr;
  if (index + 1 > peer_streams_uni_opened_) peer_streams_uni_opened_ = index + 1;
  return streams_[streams_.count() - 1].get();
}

bool QuicConnection::on_stream(uint8_t type, ByteReader& r) {
  const bool has_offset = type & 0x04, has_length = type & 0x02, fin = type & 0x01;
  const uint64_t id = read_varint(r);
  const uint64_t offset = has_offset ? read_varint(r) : 0;
  const uint64_t length = has_length ? read_varint(r) : r.remaining();
  const ConstByteView data = r.take(length);
  if (!r.ok()) return true;
  if (stream_is_ours(id) && (id & 2)) {   // data on a stream only we send on
    fail(QuicError::Protocol, kStreamStateError);
    return false;
  }
  Stream* s = get_stream(id, true);
  if (!s) {
    if (state_ == QuicState::Failed) return false;
    if (stream_is_ours(id)) {   // a stream of ours that is not open (gone, or never opened)
      fail(QuicError::Protocol, kStreamStateError);
      return false;
    }
    fail(QuicError::Protocol, kStreamStateError);
    return false;
  }
  if (offset + length > s->recv_limit || offset + length > kMaxVarint) {
    fail(QuicError::Protocol, kFlowControlError);
    return false;
  }
  const uint64_t before = s->recv.highest();
  const RecvBuffer::Result result = s->recv.add(offset, data, fin);
  if (result == RecvBuffer::Result::FinalSize) {
    fail(QuicError::Protocol, kFinalSizeError);
    return false;
  }
  if (result != RecvBuffer::Result::Ok) {
    fail(QuicError::Internal);
    return false;
  }
  if (s->recv.highest() > before) {
    data_received_ += s->recv.highest() - before;
    if (data_received_ > recv_max_data_) {
      fail(QuicError::Protocol, kFlowControlError);
      return false;
    }
  }
  return true;
}

}  // namespace network
