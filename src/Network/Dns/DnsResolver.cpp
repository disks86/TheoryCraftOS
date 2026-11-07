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

#include "Network/Dns/DnsResolver.hpp"
#include "Network/Dns/DnsMessage.hpp"

namespace network {

const char* dns_status_name(DnsStatus status) {
  switch (status) {
    case DnsStatus::Pending:  return "pending";
    case DnsStatus::Resolved: return "resolved";
    case DnsStatus::NotFound: return "not found";
    case DnsStatus::Failed:   return "failed";
  }
  return "?";
}

// One lookup. Its fields are the resolver's working state; the application sees
// only the IDnsQuery part.
struct DnsResolver::Query final : IDnsQuery {
  enum class Phase : uint8_t { Udp, Tcp };

  // The resolver updates a query from the stack's poll(); readers take the same lock.
  DnsStatus status() const override {
    kernel::ScopedRecursiveMutex guard(*mutex);
    return status_;
  }
  Ipv4Address address() const override {
    kernel::ScopedRecursiveMutex guard(*mutex);
    return address_.v6 ? kAnyAddress : address_.ipv4;
  }
  IpAddress ip_address() const override {
    kernel::ScopedRecursiveMutex guard(*mutex);
    return address_;
  }
  uint32_t ttl_seconds() const override {
    kernel::ScopedRecursiveMutex guard(*mutex);
    return ttl_;
  }

  kernel::RecursiveMutex* mutex = nullptr;

  DnsStatus status_ = DnsStatus::Pending;
  IpAddress address_;
  uint32_t ttl_ = 0;
  uint16_t type = dns::kTypeA;

  char name[dns::kMaxTextName + 2] = {};
  uint16_t id = 0;
  Phase phase = Phase::Udp;
  uint32_t attempts = 0;
  uint64_t next_ms = 0;  // when to retry (UDP) or give up (TCP)
  SharedPointer<IUdpSocket> udp;
  SharedPointer<ITcpSocket> tcp;
  bool tcp_sent = false;
  uint8_t tcp_buffer[2 + 4096];
  uint32_t tcp_received = 0;
};

void DnsResolver::clear_cache() {
  kernel::ScopedRecursiveMutex guard(stack_.mutex());
  cache_.clear();
}

uint32_t DnsResolver::cache_size() const {
  kernel::ScopedRecursiveMutex guard(stack_.mutex());
  return static_cast<uint32_t>(cache_.count());
}

uint32_t DnsResolver::random() {
  if (random_state_ == 0x2545F491) random_state_ ^= static_cast<uint32_t>(stack_.now_ms() * 2654435761u);
  random_state_ ^= random_state_ << 13;  // xorshift32
  random_state_ ^= random_state_ >> 17;
  random_state_ ^= random_state_ << 5;
  return random_state_ + (++counter_ * 0x9E3779B9u);
}

SharedPointer<IUdpSocket> DnsResolver::open_random_socket() {
  // A random source port makes forged answers harder to land.
  for (int i = 0; i < 8; ++i) {
    SharedPointer<IUdpSocket> socket = stack_.udp_open(static_cast<uint16_t>(49152 + random() % 16384));
    if (socket) return socket;
  }
  return stack_.udp_open(0);
}

const DnsResolver::CacheEntry* DnsResolver::lookup(StringView name, uint16_t type) {
  const uint64_t now = stack_.now_ms();
  for (uint64_t i = cache_.count(); i > 0; --i) {
    CacheEntry& e = cache_[i - 1];
    if (e.expires_ms <= now) {
      cache_.remove_at(i - 1);
    } else if (e.type == type && dns::names_equal(StringView(e.name), name)) {
      return &cache_[i - 1];
    }
  }
  return nullptr;
}

void DnsResolver::remember(StringView name, uint16_t type, bool found, const IpAddress& address, uint32_t ttl_seconds) {
  if (ttl_seconds == 0) return;  // "do not cache"
  if (ttl_seconds > kMaxTtlSeconds) ttl_seconds = kMaxTtlSeconds;
  CacheEntry entry{};
  if (name.ends_with('.')) name = name.without_suffix(1);
  if (name.empty() || !name.copy_to(ArrayView<char>(entry.name))) return;  // names too long for the cache are simply not cached
  entry.type = type;
  entry.found = found;
  entry.address = address;
  entry.expires_ms = stack_.now_ms() + uint64_t{ttl_seconds} * 1000;

  for (CacheEntry& e : cache_) {
    if (e.type == type && dns::names_equal(StringView(e.name), StringView(entry.name))) {
      e = entry;
      return;
    }
  }
  if (cache_.count() >= kMaxCacheEntries) {  // make room: drop the entry that expires first
    uint64_t oldest = 0;
    for (uint64_t i = 1; i < cache_.count(); ++i)
      if (cache_[i].expires_ms < cache_[oldest].expires_ms) oldest = i;
    cache_.remove_at(oldest);
  }
  cache_.push_back(entry);
}

SharedPointer<IDnsQuery> DnsResolver::query(StringView name) { return start(name, dns::kTypeA); }
SharedPointer<IDnsQuery> DnsResolver::query_aaaa(StringView name) { return start(name, dns::kTypeAaaa); }

// The server: the IPv4 one the configuration names, else the IPv6 one from the router advertisements.
bool DnsResolver::server(IpAddress* address) const {
  const IpConfiguration config = stack_.configuration();
  if (config.configured() && !config.dns.is_unspecified()) {
    *address = IpAddress(config.dns);
    return true;
  }
  const Ipv6Configuration config6 = stack_.configuration6();
  if (config6.configured() && !config6.dns.is_unspecified()) {
    *address = IpAddress(config6.dns);
    return true;
  }
  return false;
}

SharedPointer<IDnsQuery> DnsResolver::start(StringView name, uint16_t type) {
  kernel::ScopedRecursiveMutex guard(stack_.mutex());
  uint8_t scratch[dns::kMaxName + 1];
  Ipv4Address literal4;
  Ipv6Address literal6;
  const bool is_v4 = parse_ipv4(name, &literal4), is_v6 = !is_v4 && parse_ipv6(name, &literal6);
  if (!is_v4 && !is_v6 && !dns::encode_name(name, ByteView(scratch))) return SharedPointer<IDnsQuery>();  // not a valid host name

  SharedPointer<Query> q = SharedPointer<Query>::make(allocator_);
  if (!q) return SharedPointer<IDnsQuery>();
  q->mutex = &stack_.mutex();
  q->type = type;
  if (is_v4 || is_v6) {  // an address needs no lookup (one of the other version has no address of this one)
    const bool matches = (type == dns::kTypeA) == is_v4;
    q->status_ = matches ? DnsStatus::Resolved : DnsStatus::NotFound;
    if (matches) q->address_ = is_v4 ? IpAddress(literal4) : IpAddress(literal6);
    return SharedPointer<IDnsQuery>(q);
  }
  if (const CacheEntry* cached = lookup(name, type)) {
    q->status_ = cached->found ? DnsStatus::Resolved : DnsStatus::NotFound;
    q->address_ = cached->address;
    q->ttl_ = static_cast<uint32_t>((cached->expires_ms - stack_.now_ms() + 999) / 1000);  // what is left of it, rounded up
    return SharedPointer<IDnsQuery>(q);
  }

  if (!name.copy_to(ArrayView<char>(q->name))) return SharedPointer<IDnsQuery>();  // longer than any valid name
  q->id = static_cast<uint16_t>(random());
  if (!queries_.push_back(q)) return SharedPointer<IDnsQuery>();
  step(*q);  // send the first packet now rather than at the next poll
  return SharedPointer<IDnsQuery>(q);
}

void DnsResolver::finish(Query& q, DnsStatus status) {
  q.status_ = status;
  q.udp.reset();
  if (q.tcp) q.tcp->close();
  q.tcp.reset();
}

void DnsResolver::step(Query& q) {
  if (q.status_ != DnsStatus::Pending) return;
  IpAddress dns_server;
  if (!server(&dns_server)) {
    finish(q, DnsStatus::Failed);
    return;
  }
  const uint64_t now = stack_.now_ms();
  uint8_t request[dns::kHeaderSize + dns::kMaxName + 4];
  const uint32_t request_length = dns::build_query(q.id, StringView(q.name), ByteView(request), q.type);
  if (!request_length) {
    finish(q, DnsStatus::Failed);
    return;
  }

  if (q.phase == Query::Phase::Udp) {
    if (!q.udp) q.udp = open_random_socket();
    if (!q.udp) {
      finish(q, DnsStatus::Failed);
      return;
    }
    // Answers that arrived since the last look.
    uint8_t reply[1500];
    IpAddress from;
    uint16_t from_port = 0;
    uint32_t length = 0;
    while (q.udp->receive_from_ip(ByteView(reply), &from, &from_port, &length)) {
      if (from != dns_server || from_port != 53 || length > sizeof(reply)) continue;  // not from our server
      dns::Answer answer;
      switch (dns::parse_response(ConstByteView(reply).first(length), q.id, StringView(q.name), &answer, q.type)) {
        case dns::ParseResult::Answer:
          q.address_ = answer.address;
          q.ttl_ = answer.ttl;
          remember(StringView(q.name), q.type, true, answer.address, answer.ttl);
          finish(q, DnsStatus::Resolved);
          return;
        case dns::ParseResult::NoSuchName:
        case dns::ParseResult::NoAddress:
          remember(StringView(q.name), q.type, false, IpAddress(), kNegativeTtlSeconds);
          finish(q, DnsStatus::NotFound);
          return;
        case dns::ParseResult::ServerError:
          finish(q, DnsStatus::Failed);
          return;
        case dns::ParseResult::Truncated:  // the answer does not fit a UDP datagram: ask again over TCP
          q.udp.reset();
          q.phase = Query::Phase::Tcp;
          q.next_ms = now + kTcpTimeoutMs;
          q.tcp = stack_.tcp_open();
          if (!q.tcp || !q.tcp->connect_ip(dns_server, 53)) finish(q, DnsStatus::Failed);
          return;
        case dns::ParseResult::Invalid:
          break;  // a stray or forged packet: keep waiting for the real one
      }
    }
    if (q.attempts == 0 || now >= q.next_ms) {
      if (q.attempts >= kUdpAttempts) {
        finish(q, DnsStatus::Failed);
        return;
      }
      q.udp->send_to_ip(dns_server, 53, ConstByteView(request).first(request_length));
      q.next_ms = now + (1000u << q.attempts);  // 1 s, 2 s, 4 s
      ++q.attempts;
    }
    return;
  }

  // TCP: the same message with a two-byte length in front, in both directions.
  if (now >= q.next_ms || q.tcp->failed()) {
    finish(q, DnsStatus::Failed);
    return;
  }
  if (q.tcp->is_connected() && !q.tcp_sent) {
    uint8_t framed_storage[2 + sizeof(request)];
    ByteWriter framed{ByteView(framed_storage)};
    framed.be16(static_cast<uint16_t>(request_length));
    framed.bytes(ConstByteView(request).first(request_length));
    if (!framed.ok() || q.tcp->send(framed.written()) != framed.position()) {
      finish(q, DnsStatus::Failed);
      return;
    }
    q.tcp_sent = true;
  }
  if (q.tcp_sent) {
    q.tcp_received += q.tcp->receive(ByteView(q.tcp_buffer).dropped(q.tcp_received));
    if (q.tcp_received >= 2) {
      const uint32_t expected = (uint32_t{q.tcp_buffer[0]} << 8) | q.tcp_buffer[1];  // the length prefix
      if (expected + 2 > sizeof(q.tcp_buffer)) {
        finish(q, DnsStatus::Failed);
      } else if (q.tcp_received >= expected + 2) {
        dns::Answer answer;
        switch (dns::parse_response(ConstByteView(q.tcp_buffer).subview(2, expected), q.id, StringView(q.name), &answer, q.type)) {
          case dns::ParseResult::Answer:
            q.address_ = answer.address;
            q.ttl_ = answer.ttl;
            remember(StringView(q.name), q.type, true, answer.address, answer.ttl);
            finish(q, DnsStatus::Resolved);
            break;
          case dns::ParseResult::NoSuchName:
          case dns::ParseResult::NoAddress:
            remember(StringView(q.name), q.type, false, IpAddress(), kNegativeTtlSeconds);
            finish(q, DnsStatus::NotFound);
            break;
          default:
            finish(q, DnsStatus::Failed);
            break;
        }
      }
    } else if (q.tcp->peer_closed()) {
      finish(q, DnsStatus::Failed);  // the server hung up without answering
    }
  }
}

void DnsResolver::poll() {
  for (uint64_t i = 0; i < queries_.count(); ++i) step(*queries_[i]);
  for (uint64_t i = queries_.count(); i > 0; --i) {
    SharedPointer<Query>& q = queries_[i - 1];
    // Finished and read, or abandoned by the application: nothing more to do.
    if (q.use_count() == 1) {
      if (q->status() == DnsStatus::Pending) finish(*q, DnsStatus::Failed);
      queries_.remove_at(i - 1);
    }
  }
}

}  // namespace network
