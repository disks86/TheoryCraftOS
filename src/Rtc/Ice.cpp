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

#include "Rtc/Ice.hpp"
#include "Utility/Move.hpp"

namespace rtc {

namespace {

bool crypto_equal(const uint8_t* a, const uint8_t* b) {
  uint8_t d = 0;
  for (int i = 0; i < 12; ++i) d |= a[i] ^ b[i];
  return d == 0;
}

constexpr uint32_t kTa = 20, kMaxPairs = 64, kRtoStart = 100, kMaxAttempts = 7, kConsentMs = 2500, kDisconnectedMs = 6000, kFailedMs = 30000;

uint32_t type_preference(CandidateType t) {
  switch (t) {
    case CandidateType::Host: return 126;
    case CandidateType::PeerReflexive: return 110;
    case CandidateType::ServerReflexive: return 100;
    default: return 0;
  }
}

}  // namespace

uint32_t ice_priority(CandidateType type, uint32_t local_preference, uint32_t component) { return (type_preference(type) << 24) | ((local_preference & 0xFFFF) << 8) | (256 - component); }

const char* candidate_type_name(CandidateType t) {
  switch (t) {
    case CandidateType::Host: return "host";
    case CandidateType::ServerReflexive: return "srflx";
    case CandidateType::PeerReflexive: return "prflx";
    default: return "relay";
  }
}

IceAgent::IceAgent(allocators::IAllocator& allocator, IRtcNetwork& network)
    : allocator_(allocator), net_(network), local_ufrag_(allocator), local_pwd_(allocator), remote_ufrag_(allocator), remote_pwd_(allocator), local_(allocator), remote_(allocator), outgoing_(allocator),
      pairs_(allocator), gather_(allocator), inbox_(allocator) {
  uint8_t t[8] = {};
  net_.random(ByteView(t, 8));
  for (const uint8_t b : t) tie_breaker_ = (tie_breaker_ << 8) | b;
}

IceAgent::~IceAgent() { close(); }

void IceAgent::set_local_credentials(StringView ufrag, StringView pwd) {
  local_ufrag_.assign(ufrag);
  local_pwd_.assign(pwd);
}

void IceAgent::set_remote_credentials(StringView ufrag, StringView pwd) {
  remote_ufrag_.assign(ufrag);
  remote_pwd_.assign(pwd);
}

void IceAgent::make_transaction(uint8_t out[12]) { net_.random(ByteView(out, 12)); }

bool IceAgent::start_gathering(const StringView* servers, uint32_t count) {
  if (closed_ || socket_) return false;
  socket_ = net_.open_udp(0);
  if (!socket_ || !net_.local_address(&host_address_) || host_address_.is_unspecified()) return false;
  started_ms_ = gather_started_ms_ = net_.now_ms();
  IceCandidate host;
  host.type = CandidateType::Host;
  host.address = host_address_;
  host.port = socket_->local_port();
  host.priority = ice_priority(CandidateType::Host, 65535, 1);
  local_.push_back(host);
  outgoing_.push_back(host);
  gathering_ = IceGathering::Gathering;
  for (uint32_t i = 0; i < count && i < kMaxStunServers; ++i) {
    StringView name = servers[i], port_text;
    uint64_t port = 3478;
    const uint64_t colon = name.rfind(':');
    if (colon != StringView::npos) {
      port_text = name.substr(colon + 1);
      name = name.first(colon);
      if (!port_text.parse_uint(&port) || port == 0 || port > 65535) continue;
    }
    Gather g;
    if (!net_.resolve(name, &g.server)) continue;
    g.port = static_cast<uint16_t>(port);
    make_transaction(g.transaction);
    gather_.push_back(g);
  }
  if (gather_.count() == 0) gathering_ = IceGathering::Complete;
  return true;
}

bool IceAgent::add_remote_candidate(const IceCandidate& c) {
  if (closed_ || remote_.count() >= kMaxRemote || c.port == 0 || c.address.is_unspecified() || c.address.is_multicast()) return false;
  if (find_remote(c.address, c.port) >= 0) return true;
  remote_.push_back(c);
  if (checks_started_) add_pair(static_cast<uint32_t>(remote_.count() - 1));
  return true;
}

void IceAgent::start_checks() {
  if (checks_started_ || remote_pwd_.empty()) return;
  checks_started_ = true;
  for (uint32_t i = 0; i < remote_.count(); ++i)
    if (find_pair(i) < 0) add_pair(i);
  set_state(IceState::Checking);
}

void IceAgent::close() {
  if (closed_) return;
  closed_ = true;
  if (socket_) socket_->close();
  socket_ = SharedPointer<network::IUdpSocket>();
  set_state(IceState::Closed);
}

int32_t IceAgent::find_remote(network::Ipv4Address a, uint16_t port) const {
  for (uint64_t i = 0; i < remote_.count(); ++i)
    if (remote_[i].address == a && remote_[i].port == port) return static_cast<int32_t>(i);
  return -1;
}

int32_t IceAgent::find_pair(uint32_t remote) const {
  for (uint64_t i = 0; i < pairs_.count(); ++i)
    if (pairs_[i].remote == remote) return static_cast<int32_t>(i);
  return -1;
}

uint64_t IceAgent::pair_priority(uint32_t local, uint32_t remote) const {
  const uint64_t g = controlling_ ? local : remote, d = controlling_ ? remote : local;
  return ((g < d ? g : d) << 32) + 2 * (g > d ? g : d) + (g > d ? 1 : 0);
}

int32_t IceAgent::add_pair(uint32_t remote) {
  if (pairs_.count() >= kMaxPairs || remote >= remote_.count()) return -1;
  Pair p;
  p.remote = remote;
  p.priority = pair_priority(ice_priority(CandidateType::Host, 65535, 1), remote_[remote].priority);
  return pairs_.push_back(p) ? static_cast<int32_t>(pairs_.count() - 1) : -1;
}

bool IceAgent::take_local_candidate(IceCandidate* out) {
  if (outgoing_.count() == 0) return false;
  *out = outgoing_[0];
  outgoing_.remove_at(0);
  return true;
}

bool IceAgent::selected(IceCandidate* local, IceCandidate* remote) const {
  if (selected_ < 0) return false;
  *local = local_[0];
  *remote = remote_[pairs_[selected_].remote];
  return true;
}

bool IceAgent::state_changed(IceState* out) {
  if (reported_ == state_) return false;
  reported_ = state_;
  *out = state_;
  return true;
}

void IceAgent::set_state(IceState s) {
  if (closed_ && s != IceState::Closed) return;
  state_ = s;
}

bool IceAgent::send(ConstByteView data) {
  if (!socket_ || selected_ < 0 || data.size() > 1472) return false;
  const IceCandidate& r = remote_[pairs_[selected_].remote];
  return socket_->send_to(r.address, r.port, data);
}

uint32_t IceAgent::receive(ByteView buffer) {
  if (inbox_.count() == 0) return 0;
  const Datagram& d = *inbox_[0];
  const uint32_t n = d.length < buffer.size() ? d.length : static_cast<uint32_t>(buffer.size());
  buffer.first(n).copy_from(ConstByteView(d.data, n));
  inbox_.remove_at(0);
  return n;
}

void IceAgent::poll() {
  if (closed_ || !socket_) return;
  net_.poll();
  uint8_t buffer[1500];
  network::Ipv4Address from;
  uint16_t port;
  uint32_t length;
  for (uint32_t guard = 0; guard < 256 && socket_->receive_from(ByteView(buffer, sizeof(buffer)), &from, &port, &length); ++guard)
    if (length <= sizeof(buffer)) handle_datagram(ConstByteView(buffer, length), from, port);
  run_gathering();
  run_checks();
  update_state();
}

void IceAgent::handle_datagram(ConstByteView data, network::Ipv4Address from, uint16_t port) {
  const int32_t known = find_remote(from, port);
  if (known >= 0 && selected_ >= 0 && static_cast<uint32_t>(known) == pairs_[selected_].remote) last_rx_ms_ = net_.now_ms();
  if (stun_looks_like(data)) {
    StunMessage m(allocator_);
    if (!stun_parse(data, &m)) return;
    if (m.type == kStunBindingRequest) handle_request(data, m, from, port);
    else if (m.type == kStunBindingSuccess || m.type == kStunBindingError) handle_response(data, m, from, port);
    return;
  }
  if (known < 0) return;   // (only peers we have a pair with may talk to the layers above)
  if (inbox_.count() >= kMaxDatagrams) inbox_.remove_at(0);
  UniquePointer<Datagram> d = UniquePointer<Datagram>::make(allocator_);
  if (!d || data.size() > sizeof(d->data)) return;
  ByteView(d->data, sizeof(d->data)).first(data.size()).copy_from(data);
  d->length = static_cast<uint32_t>(data.size());
  d->from = from;
  d->port = port;
  inbox_.push_back(util::move(d));
}

void IceAgent::reply(const StunMessage& m, network::Ipv4Address to, uint16_t port, bool ok, uint16_t error) {
  ByteBuffer out(allocator_);
  if (!stun_begin(&out, ok ? kStunBindingSuccess : kStunBindingError, m.transaction)) return;
  if (ok) {
    if (!stun_xor_address(&out, kStunXorMapped, to, port, m.transaction)) return;
  } else if (!stun_error_attribute(&out, error, error == 487 ? "Role Conflict" : "Unauthorized")) {
    return;
  }
  const ConstByteView key(reinterpret_cast<const uint8_t*>(local_pwd_.view().data()), local_pwd_.size());
  if (!stun_finish(&out, ok ? key : ConstByteView(), true)) return;
  socket_->send_to(to, port, out.view());
}

void IceAgent::handle_request(ConstByteView data, const StunMessage& m, network::Ipv4Address from, uint16_t port) {
  ++requests_received_;
  const StringView user = m.username.view();
  const uint64_t colon = user.find(':');
  const ConstByteView key(reinterpret_cast<const uint8_t*>(local_pwd_.view().data()), local_pwd_.size());
  if (colon == StringView::npos || !user.first(colon).equals(local_ufrag_.view()) || (!remote_ufrag_.empty() && !user.substr(colon + 1).equals(remote_ufrag_.view())) ||
      !m.has_integrity || !stun_check_integrity(data, m, key)) {
    reply(m, from, port, false, 401);
    return;
  }
  if (controlling_ && m.has_controlling) {   // role conflict (RFC 8445 7.3.1.1)
    if (tie_breaker_ >= m.tie_breaker) {
      reply(m, from, port, false, 487);
      return;
    }
    controlling_ = false;
  } else if (!controlling_ && m.has_controlled) {
    if (tie_breaker_ >= m.tie_breaker) {
      controlling_ = true;
    } else {
      reply(m, from, port, false, 487);
      return;
    }
  }
  reply(m, from, port, true, 0);
  int32_t r = find_remote(from, port);
  if (r < 0) {   // a peer reflexive candidate
    if (remote_.count() >= kMaxRemote) return;
    IceCandidate c;
    c.type = CandidateType::PeerReflexive;
    c.address = from;
    c.port = port;
    c.priority = m.has_priority ? m.priority : ice_priority(CandidateType::PeerReflexive, 65535, 1);
    remote_.push_back(c);
    r = static_cast<int32_t>(remote_.count() - 1);
  }
  if (!checks_started_) return;   // (the pair is made when the checks start)
  int32_t pi = find_pair(static_cast<uint32_t>(r));
  if (pi < 0) pi = add_pair(static_cast<uint32_t>(r));
  if (pi < 0) return;
  Pair& p = pairs_[pi];
  if (p.state == PairState::Waiting || p.state == PairState::Failed) {
    p.state = PairState::Waiting;
    p.triggered = true;   // a triggered check: answer the request with a check of our own soon
  }
  if (m.use_candidate && !controlling_) p.nominated = true;
}

void IceAgent::send_check(Pair& p) {
  const IceCandidate& r = remote_[p.remote];
  if (p.attempts == 0 || p.state == PairState::Waiting) make_transaction(p.transaction);
  ByteBuffer out(allocator_);
  String user(allocator_);
  user.assign(remote_ufrag_.view());
  user.append(":");
  user.append(local_ufrag_.view());
  if (!stun_begin(&out, kStunBindingRequest, p.transaction) || !stun_text_attribute(&out, kStunUsername, user.view())) return;
  uint8_t prio[4], tie[8];
  store_be32(ByteView(prio, 4), 0, ice_priority(CandidateType::PeerReflexive, 65535, 1));
  store_be32(ByteView(tie, 8), 0, static_cast<uint32_t>(tie_breaker_ >> 32));
  store_be32(ByteView(tie, 8), 4, static_cast<uint32_t>(tie_breaker_));
  if (!stun_attribute(&out, kStunPriority, ConstByteView(prio, 4)) || !stun_attribute(&out, controlling_ ? kStunIceControlling : kStunIceControlled, ConstByteView(tie, 8))) return;
  if (controlling_ && p.nominate && !stun_attribute(&out, kStunUseCandidate, ConstByteView())) return;
  if (!stun_finish(&out, ConstByteView(reinterpret_cast<const uint8_t*>(remote_pwd_.view().data()), remote_pwd_.size()), true)) return;
  socket_->send_to(r.address, r.port, out.view());
  p.sent_ms = net_.now_ms();
  ++p.attempts;
  p.state = PairState::InProgress;
  p.triggered = false;
  ++requests_sent_;
}

void IceAgent::run_checks() {
  if (!checks_started_) return;
  const uint64_t now = net_.now_ms();
  for (uint64_t i = 0; i < pairs_.count(); ++i) {   // retransmissions, and giving up
    Pair& p = pairs_[i];
    if (p.state != PairState::InProgress) continue;
    uint64_t rto = kRtoStart << (p.attempts - 1 < 4 ? p.attempts - 1 : 4);
    if (now - p.sent_ms < rto) continue;
    if (p.attempts >= kMaxAttempts) {
      p.state = PairState::Failed;   // (a selected pair stays selected: it is the silence that takes the connection down, see update_state)
    } else {
      send_check(p);
    }
  }
  // Nomination (controlling): the best pair that works is checked again with USE-CANDIDATE.
  if (controlling_) {
    bool any_nominated = false;
    int32_t best = -1;
    for (uint64_t i = 0; i < pairs_.count(); ++i) {
      if (pairs_[i].nominated) any_nominated = true;
      if (pairs_[i].state == PairState::Succeeded && (best < 0 || pairs_[i].priority > pairs_[best].priority)) best = static_cast<int32_t>(i);
    }
    if (!any_nominated && best >= 0 && !pairs_[best].nominate) {
      pairs_[best].nominate = true;
      pairs_[best].state = PairState::Waiting;
      pairs_[best].triggered = true;
      pairs_[best].attempts = 0;
    }
  }
  // Consent: the selected pair is checked from time to time.
  if (selected_ >= 0 && now - last_consent_ms_ >= kConsentMs) {
    last_consent_ms_ = now;
    Pair& p = pairs_[selected_];
    if (p.state == PairState::Succeeded || p.state == PairState::Failed) {
      p.state = PairState::Waiting;
      p.triggered = true;
      p.attempts = 0;
    }
  }
  if (now - last_check_ms_ < kTa) return;
  int32_t pick = -1;
  for (uint64_t i = 0; i < pairs_.count(); ++i) {   // triggered checks first, then by priority
    const Pair& p = pairs_[i];
    if (p.state != PairState::Waiting) continue;
    if (pick < 0 || (p.triggered && !pairs_[pick].triggered) || (p.triggered == pairs_[pick].triggered && p.priority > pairs_[pick].priority)) pick = static_cast<int32_t>(i);
  }
  if (pick >= 0) {
    last_check_ms_ = now;
    pairs_[pick].attempts = 0;
    send_check(pairs_[pick]);
  }
}

void IceAgent::run_gathering() {
  if (gathering_ != IceGathering::Gathering) return;
  const uint64_t now = net_.now_ms();
  bool all_done = true;
  for (uint64_t i = 0; i < gather_.count(); ++i) {
    Gather& g = gather_[i];
    if (g.done) continue;
    if (g.attempts >= 4 && now - g.sent_ms >= 1000) {
      g.done = true;
      continue;
    }
    all_done = false;
    if (g.attempts == 0 || now - g.sent_ms >= (250u << (g.attempts - 1))) {
      ByteBuffer out(allocator_);
      if (stun_begin(&out, kStunBindingRequest, g.transaction) && stun_finish(&out, ConstByteView(), true)) socket_->send_to(g.server, g.port, out.view());
      g.sent_ms = now;
      ++g.attempts;
    }
  }
  if (all_done) gathering_ = IceGathering::Complete;
}

void IceAgent::handle_response(ConstByteView data, const StunMessage& m, network::Ipv4Address from, uint16_t port) {
  for (uint64_t i = 0; i < gather_.count(); ++i) {   // from a STUN server
    Gather& g = gather_[i];
    if (g.done || !(g.server == from && g.port == port) || !crypto_equal(g.transaction, m.transaction)) continue;
    g.done = true;
    if (m.type == kStunBindingSuccess && m.has_mapped && !(m.mapped == host_address_ && m.mapped_port == local_port())) {
      IceCandidate c;
      c.type = CandidateType::ServerReflexive;
      c.address = m.mapped;
      c.port = m.mapped_port;
      c.priority = ice_priority(CandidateType::ServerReflexive, 65535, 1);
      c.foundation = 2;
      c.related_address = host_address_;
      c.related_port = local_port();
      local_.push_back(c);
      outgoing_.push_back(c);
    }
    return;
  }
  for (uint64_t i = 0; i < pairs_.count(); ++i) {
    Pair& p = pairs_[i];
    if (p.state != PairState::InProgress || !crypto_equal(p.transaction, m.transaction)) continue;
    const IceCandidate& r = remote_[p.remote];
    if (!(r.address == from && r.port == port)) return;   // (it must come from where the request went)
    ++responses_received_;
    if (m.type == kStunBindingError) {
      if (m.has_error && m.error_code == 487) {   // we and the peer both thought they were controlling (or controlled): switch and check again
        controlling_ = !controlling_;
        p.state = PairState::Waiting;
        p.triggered = true;
        p.attempts = 0;
      } else {
        p.state = PairState::Failed;
      }
      return;
    }
    if (!m.has_integrity || !stun_check_integrity(data, m, ConstByteView(reinterpret_cast<const uint8_t*>(remote_pwd_.view().data()), remote_pwd_.size()))) return;
    p.state = PairState::Succeeded;
    if (p.nominate && controlling_) p.nominated = true;
    last_rx_ms_ = net_.now_ms();
    return;
  }
}

void IceAgent::update_state() {
  if (closed_) return;
  if (selected_ < 0) {
    int32_t best = -1;
    for (uint64_t i = 0; i < pairs_.count(); ++i)
      if (pairs_[i].state == PairState::Succeeded && pairs_[i].nominated && (best < 0 || pairs_[i].priority > pairs_[best].priority)) best = static_cast<int32_t>(i);
    if (best >= 0) {
      selected_ = best;
      last_rx_ms_ = last_consent_ms_ = net_.now_ms();
    }
  }
  const uint64_t now = net_.now_ms();
  if (selected_ >= 0) {
    const uint64_t quiet = now - last_rx_ms_;
    set_state(quiet > kFailedMs ? IceState::Failed : quiet > kDisconnectedMs ? IceState::Disconnected : IceState::Completed);
    return;
  }
  bool succeeded = false, waiting = false;
  for (uint64_t i = 0; i < pairs_.count(); ++i) {
    succeeded = succeeded || pairs_[i].state == PairState::Succeeded;
    waiting = waiting || pairs_[i].state == PairState::Waiting || pairs_[i].state == PairState::InProgress;
  }
  if (succeeded) set_state(IceState::Connected);
  else if (checks_started_ && !waiting && pairs_.count() > 0 && remote_done_) set_state(IceState::Failed);
  else if (checks_started_) set_state(IceState::Checking);
}

}  // namespace rtc
