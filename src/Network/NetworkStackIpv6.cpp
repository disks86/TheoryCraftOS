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

// The IPv6 half of NetworkStack: the packet format (RFC 8200, extension headers skipped where that is allowed), ICMPv6 (RFC 4443: echo),
// neighbor discovery (RFC 4861: neighbor and router solicitations and advertisements, the neighbor cache, packets waiting for an address),
// stateless address autoconfiguration (RFC 4862: a link-local address from the MAC, a global one from an advertised /64 prefix, both after
// duplicate address detection), the RDNSS option (RFC 8106) and MLDv2 reports (RFC 3810) for the solicited-node groups, so that switches
// that listen for MLD deliver neighbor solicitations.
#include "Network/NetworkStack.hpp"
#include "Network/Checksum.hpp"
#include "Kernel/Thread.hpp"

namespace network {

namespace {
constexpr uint8_t kIcmpv6EchoRequest = 128, kIcmpv6EchoReply = 129, kIcmpv6RouterSolicitation = 133, kIcmpv6RouterAdvertisement = 134,
                  kIcmpv6NeighborSolicitation = 135, kIcmpv6NeighborAdvertisement = 136, kIcmpv6Mldv2Report = 143;
constexpr uint8_t kOptionSourceLinkAddress = 1, kOptionTargetLinkAddress = 2, kOptionPrefix = 3, kOptionMtu = 5, kOptionDnsServers = 25;
constexpr uint8_t kHeaderHopByHop = 0, kHeaderRouting = 43, kHeaderFragment = 44, kHeaderDestinationOptions = 60;
constexpr uint8_t kNdHopLimit = 255;               // neighbor discovery messages must have it: they cannot have crossed a router
constexpr uint64_t kDuplicateDetectionMs = 1000;   // RetransTimer, with one solicitation (DupAddrDetectTransmits = 1)
constexpr uint64_t kRouterSolicitationMs = 4000;   // RTR_SOLICITATION_INTERVAL
constexpr uint32_t kRouterSolicitations = 3;       // MAX_RTR_SOLICITATIONS
constexpr uint64_t kNeighborReachableMs = 30000;   // REACHABLE_TIME, then the neighbor is asked again
constexpr uint64_t kNeighborRetryMs = 1000;
constexpr uint32_t kNeighborRequests = 3;          // MAX_MULTICAST_SOLICIT
constexpr uint32_t kMaxPending6 = 8;
constexpr uint8_t kMldChangeToExclude = 4;         // "listen to everything from this group" (joining it)

// The Ethernet address of an IPv6 multicast group: 33:33 and the group's last four bytes (RFC 2464 7).
hardware::MacAddress multicast_mac(const Ipv6Address& group) {
  return hardware::MacAddress{{0x33, 0x33, group.bytes[12], group.bytes[13], group.bytes[14], group.bytes[15]}};
}

// Calls fn(type, body) for each neighbor discovery option (type, length in units of 8 bytes, the rest). Stops at a malformed one: a zero
// length, or one that runs past the message (RFC 4861 4.6: the whole message is then invalid, which the caller decides).
template <class F>
bool for_each_option(ConstByteView options, F fn) {
  while (!options.empty()) {
    if (options.size() < 2) return false;
    const uint32_t length = uint32_t{options[1]} * 8;
    if (length == 0 || length > options.size()) return false;
    fn(options[0], options.subview(2, length - 2));
    options = options.dropped(length);
  }
  return true;
}

bool link_address_option(ConstByteView body, hardware::MacAddress* mac) {
  if (body.size() < 6) return false;
  ByteReader in(body);
  *mac = hardware::MacAddress::read(in);
  return in.ok();
}
}  // namespace

// ------------------------------------------------------------------ addresses and the state of autoconfiguration

Ipv6Configuration NetworkStack::configuration6() const {
  kernel::ScopedRecursiveMutex guard(const_cast<kernel::RecursiveMutex&>(mutex_));
  return config6_;
}

bool NetworkStack::is_my_ipv6(const Ipv6Address& address) const {
  if (address.is_loopback()) return true;
  return (config6_.link_local_ready && address == config6_.link_local) || (config6_.global_ready && address == config6_.global);
}

// Unicast to an address of ours that may be used, the all-nodes group, and the solicited-node groups of our addresses (tentative ones too:
// duplicate address detection listens there).
bool NetworkStack::accepts_destination6(const Ipv6Address& destination) const {
  if (is_my_ipv6(destination) || destination == kIpv6AllNodes) return true;
  if (!destination.is_multicast()) return false;
  const auto group_of = [&](AddressState state, const Ipv6Address& tentative, bool ready, const Ipv6Address& address) {
    return (state == AddressState::Tentative && destination == tentative.solicited_node()) || (ready && destination == address.solicited_node());
  };
  return group_of(link_local_state_6_, tentative_link_local_, config6_.link_local_ready, config6_.link_local) ||
         group_of(global_state_, tentative_global_, config6_.global_ready, config6_.global);
}

Ipv6Address NetworkStack::source_for6(const Ipv6Address& destination) const {
  if (destination.is_loopback()) return kIpv6Loopback;
  if (is_my_ipv6(destination)) return destination;
  const bool link_scope = destination.is_link_local() || (destination.is_multicast() && (destination.bytes[1] & 0x0F) <= 2);
  if (link_scope) return config6_.link_local_ready ? config6_.link_local : kIpv6Any;
  if (config6_.global_ready) return config6_.global;
  return kIpv6Any;
}

void NetworkStack::start_ipv6() {
  if (ipv6_started_) return;
  ipv6_started_ = true;
  // Neighbor discovery arrives on multicast groups the interface may filter out (all-nodes, solicited-node): let all of it through.
  nic_.accept_all_multicast(true);
  uint8_t mac[6];
  for (int i = 0; i < 6; ++i) mac[i] = nic_.mac_address().bytes[i];
  tentative_link_local_ = Ipv6Address::from_prefix_and_mac(kIpv6LinkLocalPrefix, mac);
  send_mld_report();  // (join the solicited-node group before asking on it, RFC 4862 5.4.2)
  begin_duplicate_detection(false);
}

void NetworkStack::stop_ipv6() {
  ipv6_started_ = false;
  config6_ = Ipv6Configuration{};
  link_local_state_6_ = global_state_ = AddressState::None;
  router_solicitations_ = 0;
  router_expires_ms_ = prefix_expires_ms_ = dns6_expires_ms_ = 0;
  prefix_on_link_ = false;
  for (NeighborEntry& n : neighbors_) n.valid = false;
  pending6_.clear();
}

void NetworkStack::begin_duplicate_detection(bool global) {
  (global ? global_state_ : link_local_state_6_) = AddressState::Tentative;
  (global ? global_dad_ms_ : link_local_dad_ms_) = now_ms() + kDuplicateDetectionMs;
  send_neighbor_solicitation(global ? tentative_global_ : tentative_link_local_, true);
}

void NetworkStack::service_ipv6() {
  if (!ipv6_started_) {
    if (!follow_link_ || nic_.link_up()) start_ipv6();
    return;
  }
  const uint64_t now = now_ms();
  if (link_local_state_6_ == AddressState::Tentative && now >= link_local_dad_ms_) {
    link_local_state_6_ = AddressState::Preferred;  // nobody else has it
    config6_.link_local = tentative_link_local_;
    config6_.link_local_ready = true;
    send_mld_report();  // (again, from the address now)
    next_router_solicitation_ms_ = now;
  }
  if (global_state_ == AddressState::Tentative && now >= global_dad_ms_) {
    global_state_ = AddressState::Preferred;
    config6_.global = tentative_global_;
    config6_.global_ready = true;
    send_mld_report();
  }
  if (config6_.link_local_ready && config6_.router.is_unspecified() && router_solicitations_ < kRouterSolicitations &&
      now >= next_router_solicitation_ms_) {
    ++router_solicitations_;
    next_router_solicitation_ms_ = now + kRouterSolicitationMs;
    send_router_solicitation();
  }
  if (router_expires_ms_ && now >= router_expires_ms_) {
    config6_.router = kIpv6Any;
    router_expires_ms_ = 0;
  }
  if (prefix_expires_ms_ && now >= prefix_expires_ms_) {  // the prefix is no longer valid: neither is the address from it
    config6_.global_ready = false;
    config6_.global = config6_.prefix = kIpv6Any;
    config6_.prefix_length = 0;
    global_state_ = AddressState::None;
    prefix_expires_ms_ = 0;
    prefix_on_link_ = false;
  }
  if (dns6_expires_ms_ && now >= dns6_expires_ms_) {
    config6_.dns = kIpv6Any;
    dns6_expires_ms_ = 0;
  }
  service_pending6();
}

// ------------------------------------------------------------------ receiving

void NetworkStack::handle_ipv6(ConstByteView packet, bool looped, const hardware::ReceivedFrame& checks) {
  if (!ipv6_started_ && !looped) return;
  ByteReader in(packet);
  const uint32_t first = in.be32();
  const uint16_t payload_length = in.be16();
  uint8_t next = in.u8();
  const uint8_t hop_limit = in.u8();
  const Ipv6Address source = Ipv6Address::read(in);
  const Ipv6Address destination = Ipv6Address::read(in);
  if (!in.ok() || (first >> 28) != 6 || payload_length > packet.size() - kIpv6HeaderSize) return;  // (Ethernet padding makes the frame longer)
  if (source.is_multicast()) return;
  if (!looped && (source.is_loopback() || destination.is_loopback() || !accepts_destination6(destination))) return;
  ConstByteView payload = packet.subview(kIpv6HeaderSize, payload_length);

  // Extension headers: hop-by-hop and destination options are skipped (no option here needs acting on), a routing header only when it has
  // nothing left to route; fragments and anything else are dropped.
  for (int headers = 0; headers < 8 && (next == kHeaderHopByHop || next == kHeaderDestinationOptions || next == kHeaderRouting); ++headers) {
    if (payload.size() < 8) return;
    const uint32_t length = (uint32_t{payload[1]} + 1) * 8;
    if (length > payload.size() || (next == kHeaderRouting && payload[3] != 0)) return;
    next = payload[0];
    payload = payload.dropped(length);
  }
  if (next == kHeaderFragment) return;

  const IpAddress from(source), to(destination);
  if (next == kProtocolIcmpv6) {
    handle_icmpv6(source, destination, hop_limit, payload, looped);
  } else if (next == kProtocolUdp) {
    udp_.handle(from, to, payload, checks.transport_checksum_good);
  } else if (next == kProtocolTcp) {
    if (looped || (!destination.is_multicast() && is_my_ipv6(destination))) tcp_.handle(from, to, payload, checks.transport_checksum_good);
  }
}

void NetworkStack::handle_icmpv6(const Ipv6Address& source, const Ipv6Address& destination, uint8_t hop_limit, ConstByteView icmp, bool looped) {
  if (icmp.size() < 4) return;
  if (checksum_finish(checksum_add(pseudo_header_sum(source, destination, kProtocolIcmpv6, static_cast<uint32_t>(icmp.size())), icmp)) != 0) return;
  const uint8_t type = icmp[0], code = icmp[1];
  if (type == kIcmpv6EchoRequest && code == 0 && icmp.size() >= 8) {
    uint8_t reply[kMaxIpv6Payload];
    if (icmp.size() > sizeof(reply)) return;
    const ByteView r = ByteView(reply).first(icmp.size());
    r.copy_from(icmp);
    r[0] = kIcmpv6EchoReply;
    r[2] = r[3] = 0;
    const Ipv6Address from = destination.is_multicast() ? source_for6(source) : destination;
    if (!from.is_unspecified()) send_icmpv6(from, source, r, 64);
  } else if (type == kIcmpv6EchoReply && code == 0 && icmp.size() >= 8) {
    ByteReader in(icmp.dropped(4));
    const uint16_t id = in.be16(), sequence = in.be16();
    if (ping_active_ && ping_v6_ && source == ping_target6_ && id == ping_id_ && sequence == ping_sequence_) {
      ping_answered_ = true;
      ping_rtt_ms_ = now_ms() - ping_sent_ms_;
    }
  } else if (looped) {
    return;  // (neighbor discovery is for the wire)
  } else if (type == kIcmpv6RouterAdvertisement && code == 0 && hop_limit == kNdHopLimit && icmp.size() >= 16) {
    handle_router_advertisement(source, icmp);
  } else if (type == kIcmpv6NeighborSolicitation && code == 0 && hop_limit == kNdHopLimit && icmp.size() >= 24) {
    handle_neighbor_solicitation(source, destination, icmp);
  } else if (type == kIcmpv6NeighborAdvertisement && code == 0 && hop_limit == kNdHopLimit && icmp.size() >= 24) {
    handle_neighbor_advertisement(icmp);
  }
}

void NetworkStack::handle_router_advertisement(const Ipv6Address& source, ConstByteView message) {
  if (!source.is_link_local()) return;
  ByteReader in(message.dropped(4));
  in.skip(2);  // current hop limit, flags (M and O: DHCPv6 is not done)
  const uint16_t router_lifetime = in.be16();
  if (!in.ok()) return;
  const uint64_t now = now_ms();
  if (router_lifetime) {
    config6_.router = source;
    router_expires_ms_ = now + uint64_t{router_lifetime} * 1000;
  } else if (config6_.router == source) {
    config6_.router = kIpv6Any;
    router_expires_ms_ = 0;
  }
  router_solicitations_ = kRouterSolicitations;  // (one answered: no more asking)
  uint8_t mac[6];
  for (int i = 0; i < 6; ++i) mac[i] = nic_.mac_address().bytes[i];
  for_each_option(message.dropped(16), [&](uint8_t type, ConstByteView body) {
    hardware::MacAddress link;
    if (type == kOptionSourceLinkAddress && link_address_option(body, &link)) {
      learn6(source, link, true);
    } else if (type == kOptionPrefix && body.size() >= 30) {
      ByteReader p(body);
      const uint8_t length = p.u8(), flags = p.u8();
      const uint32_t valid = p.be32(), preferred = p.be32();
      p.skip(4);
      const Ipv6Address prefix = Ipv6Address::read(p);
      // Autonomous (A) /64 prefixes that are not link-local make an address (RFC 4862 5.5.3); the on-link flag (L) says whether the rest of
      // the prefix is reached directly.
      if (!p.ok() || !(flags & 0x40) || length != 64 || prefix.is_link_local() || prefix.is_multicast() || preferred > valid || valid == 0) return;
      Ipv6Address network = prefix;
      for (int i = 8; i < 16; ++i) network.bytes[i] = 0;
      config6_.prefix = network;
      config6_.prefix_length = 64;
      prefix_on_link_ = flags & 0x80;
      prefix_expires_ms_ = valid == 0xFFFFFFFFu ? 0 : now + uint64_t{valid} * 1000;
      const Ipv6Address address = Ipv6Address::from_prefix_and_mac(network, mac);
      if (global_state_ == AddressState::None || (global_state_ != AddressState::Tentative && address != tentative_global_)) {
        tentative_global_ = address;
        config6_.global_ready = false;
        begin_duplicate_detection(true);
      }
    } else if (type == kOptionMtu && body.size() >= 6) {
      ByteReader m(body.dropped(2));
      const uint32_t mtu = m.be32();
      if (m.ok() && mtu >= 1280 && mtu <= 65535) config6_.mtu = mtu;
    } else if (type == kOptionDnsServers && body.size() >= 22) {
      ByteReader d(body.dropped(2));
      const uint32_t lifetime = d.be32();
      const Ipv6Address server = Ipv6Address::read(d);
      if (!d.ok()) return;
      if (lifetime == 0) {
        if (config6_.dns == server) config6_.dns = kIpv6Any;
        return;
      }
      config6_.dns = server;
      dns6_expires_ms_ = lifetime == 0xFFFFFFFFu ? 0 : now + uint64_t{lifetime} * 1000;
    }
  });
}

void NetworkStack::handle_neighbor_solicitation(const Ipv6Address& source, const Ipv6Address& destination, ConstByteView message) {
  ByteReader in(message.dropped(8));
  const Ipv6Address target = Ipv6Address::read(in);
  if (!in.ok() || target.is_multicast()) return;
  const bool tentative = (link_local_state_6_ == AddressState::Tentative && target == tentative_link_local_) ||
                         (global_state_ == AddressState::Tentative && target == tentative_global_);
  if (source.is_unspecified()) {
    // Another node's duplicate address detection: a node trying our tentative address means it is not ours (RFC 4862 5.4.3); one trying an
    // address we use is told so (unsolicited, to all nodes).
    if (!destination.is_multicast()) return;
    if (tentative) {
      if (target == tentative_global_) global_state_ = AddressState::Duplicate;
      else link_local_state_6_ = AddressState::Duplicate;
    } else if (is_my_ipv6(target)) {
      send_neighbor_advertisement(kIpv6AllNodes, target, false);
    }
    return;
  }
  hardware::MacAddress link;
  for_each_option(message.dropped(24), [&](uint8_t type, ConstByteView body) {
    if (type == kOptionSourceLinkAddress && link_address_option(body, &link)) learn6(source, link, true);
  });
  if (!tentative && is_my_ipv6(target) && !target.is_loopback()) send_neighbor_advertisement(source, target, true);
}

void NetworkStack::handle_neighbor_advertisement(ConstByteView message) {
  ByteReader in(message.dropped(8));
  const Ipv6Address target = Ipv6Address::read(in);
  if (!in.ok() || target.is_multicast()) return;
  if (link_local_state_6_ == AddressState::Tentative && target == tentative_link_local_) link_local_state_6_ = AddressState::Duplicate;
  if (global_state_ == AddressState::Tentative && target == tentative_global_) global_state_ = AddressState::Duplicate;
  hardware::MacAddress link;
  bool learned = false;
  for_each_option(message.dropped(24), [&](uint8_t type, ConstByteView body) {
    if (type == kOptionTargetLinkAddress && link_address_option(body, &link)) {
      learn6(target, link, true);
      learned = true;
    }
  });
  if (learned) flush_pending6(target);
}

// ------------------------------------------------------------------ the neighbor cache

void NetworkStack::learn6(const Ipv6Address& address, const hardware::MacAddress& mac, bool create) {
  if (address.is_unspecified() || address.is_multicast()) return;
  const uint64_t expires = now_ms() + kNeighborReachableMs;
  for (NeighborEntry& n : neighbors_) {
    if (n.valid && n.address == address) {
      n.mac = mac;
      n.expires_ms = expires;
      return;
    }
  }
  if (!create) return;
  uint32_t slot = next_neighbor_slot_;
  for (uint32_t i = 0; i < kNeighbors; ++i)
    if (!neighbors_[i].valid || neighbors_[i].expires_ms <= now_ms()) {
      slot = i;
      break;
    }
  if (slot == next_neighbor_slot_) next_neighbor_slot_ = (next_neighbor_slot_ + 1) % kNeighbors;  // (all in use: the oldest-placed goes)
  neighbors_[slot] = NeighborEntry{address, mac, expires, true};
}

const NetworkStack::NeighborEntry* NetworkStack::neighbor(const Ipv6Address& address) const {
  for (const NeighborEntry& n : neighbors_)
    if (n.valid && n.address == address && n.expires_ms > now_ms()) return &n;
  return nullptr;
}

// On the link (a link-local address, or the advertised prefix with the on-link flag): straight there; anything else: the default router.
bool NetworkStack::next_hop6_for(const Ipv6Address& destination, Ipv6Address* next_hop) const {
  if (destination.is_link_local() || (prefix_on_link_ && config6_.prefix_length == 64 && destination.in_prefix(config6_.prefix, 64))) {
    *next_hop = destination;
    return true;
  }
  if (config6_.router.is_unspecified()) return false;
  *next_hop = config6_.router;
  return true;
}

bool NetworkStack::link_address6_for(const Ipv6Address& destination, hardware::MacAddress* mac) const {
  Ipv6Address next_hop;
  if (destination.is_multicast() || destination.is_loopback() || is_my_ipv6(destination) || !next_hop6_for(destination, &next_hop)) return false;
  const NeighborEntry* n = neighbor(next_hop);
  if (!n) return false;
  *mac = n->mac;
  return true;
}

void NetworkStack::flush_pending6(const Ipv6Address& resolved) {
  const NeighborEntry* n = neighbor(resolved);
  if (!n) return;
  const hardware::MacAddress mac = n->mac;
  for (uint64_t count = pending6_.count(); count > 0; --count) {
    PendingPacket6 p;
    if (!pending6_.dequeue(&p)) break;
    if (p.next_hop == resolved) send_frame(mac, kEtherTypeIpv6, ConstByteView(p.ip_packet).first(p.length));
    else pending6_.enqueue(p);
  }
}

void NetworkStack::service_pending6() {
  if (pending6_.empty()) return;
  const uint64_t now = now_ms();
  for (uint64_t count = pending6_.count(); count > 0; --count) {
    PendingPacket6 p;
    if (!pending6_.dequeue(&p)) break;
    if (const NeighborEntry* n = neighbor(p.next_hop)) {
      send_frame(n->mac, kEtherTypeIpv6, ConstByteView(p.ip_packet).first(p.length));
      continue;
    }
    if (now >= p.next_request_ms) {
      if (p.requests >= kNeighborRequests) continue;  // nobody answered: the packet is dropped
      ++p.requests;
      p.next_request_ms = now + kNeighborRetryMs;
      send_neighbor_solicitation(p.next_hop, false);
    }
    pending6_.enqueue(p);
  }
}

// ------------------------------------------------------------------ sending

bool NetworkStack::send_ip6(const Ipv6Address& source, const Ipv6Address& destination, uint8_t next_header, ConstByteView payload, uint8_t hop_limit,
                            bool router_alert) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  const uint32_t extension = router_alert ? 8 : 0;
  if (payload.size() > kMaxIpv6Payload - extension) return false;
  uint8_t storage[kMtu];
  ByteWriter out{ByteView(storage)};
  out.be32(0x60000000u);  // version 6, traffic class 0, no flow label
  out.be16(static_cast<uint16_t>(extension + payload.size()));
  out.u8(router_alert ? kHeaderHopByHop : next_header);
  out.u8(hop_limit);
  source.write(out);
  destination.write(out);
  if (router_alert) {  // hop-by-hop: next header, length 0 (8 bytes), router alert (type 5, 2 bytes: MLD), PadN of 0
    out.u8(next_header);
    out.u8(0);
    out.u8(5);
    out.u8(2);
    out.be16(0);
    out.u8(1);
    out.u8(0);
  }
  out.bytes(payload);
  if (!out.ok()) return false;
  const ByteView writable = ByteView(storage).first(out.position());
  const ConstByteView packet = writable;

  // TCP and UDP checksums as over IPv4: the pseudo-header sum in the field, the rest by the interface or here.
  hardware::TransmitChecksum checksum;
  if (next_header == kProtocolTcp || next_header == kProtocolUdp) {
    const uint32_t start = kIpv6HeaderSize + extension;
    const uint16_t field = static_cast<uint16_t>(start + (next_header == kProtocolTcp ? 16 : 6));
    if (packet.size() < field + 2u) return false;
    const uint16_t folded = static_cast<uint16_t>(~checksum_finish(pseudo_header_sum(source, destination, next_header, static_cast<uint32_t>(payload.size()))));
    writable[field] = static_cast<uint8_t>(folded >> 8);
    writable[field + 1] = static_cast<uint8_t>(folded);
    checksum = hardware::TransmitChecksum{static_cast<uint16_t>(start), field};
  }
  const auto finish_here = [&] {
    if (!hardware::finish_transmit_checksum(writable, checksum)) return false;
    checksum = hardware::TransmitChecksum{};
    return true;
  };

  if (destination.is_loopback() || (!destination.is_multicast() && is_my_ipv6(destination))) {  // to this machine
    if (looped_.count() >= 256 || !finish_here()) return false;
    LoopedPacket p;
    p.length = static_cast<uint16_t>(packet.size());
    if (!ByteView(p.ip_packet).copy_from(packet)) return false;
    return looped_.enqueue(p);
  }
  if (destination.is_multicast()) return send_frame(multicast_mac(destination), kEtherTypeIpv6, packet, checksum);

  Ipv6Address next_hop;
  if (!next_hop6_for(destination, &next_hop)) return false;  // off the link with no router
  if (const NeighborEntry* n = neighbor(next_hop)) return send_frame(n->mac, kEtherTypeIpv6, packet, checksum);

  // Unknown next hop: park the packet (complete) and ask for it, unless that is being done already.
  if (pending6_.count() >= kMaxPending6 || !finish_here()) return false;
  bool asking = false;
  ObjectCursor cursor;
  for (bool more = pending6_.first(&cursor); more; more = pending6_.next(&cursor)) asking = asking || pending6_.element(cursor)->next_hop == next_hop;
  PendingPacket6 p;
  p.next_hop = next_hop;
  p.requests = 1;
  p.next_request_ms = now_ms() + kNeighborRetryMs;
  p.length = static_cast<uint16_t>(packet.size());
  if (!ByteView(p.ip_packet).copy_from(packet) || !pending6_.enqueue(p)) return false;
  if (!asking) send_neighbor_solicitation(next_hop, false);
  return true;
}

// ICMPv6 checksums cover the pseudo-header too; they are always computed here.
bool NetworkStack::send_icmpv6(const Ipv6Address& source, const Ipv6Address& destination, ConstByteView message, uint8_t hop_limit, bool router_alert) {
  uint8_t storage[kMaxIpv6Payload];
  if (message.size() < 4 || message.size() > sizeof(storage)) return false;
  const ByteView m = ByteView(storage).first(message.size());
  m.copy_from(message);
  m[2] = m[3] = 0;
  const uint16_t sum = checksum_finish(checksum_add(pseudo_header_sum(source, destination, kProtocolIcmpv6, static_cast<uint32_t>(m.size())), m));
  m[2] = static_cast<uint8_t>(sum >> 8);
  m[3] = static_cast<uint8_t>(sum);
  return send_ip6(source, destination, kProtocolIcmpv6, m, hop_limit, router_alert);
}

void NetworkStack::send_neighbor_solicitation(const Ipv6Address& target, bool duplicate_detection) {
  const Ipv6Address source = duplicate_detection ? kIpv6Any : source_for6(target);
  if (!duplicate_detection && source.is_unspecified()) return;
  uint8_t message[32];
  ByteWriter out{ByteView(message)};
  out.u8(kIcmpv6NeighborSolicitation);
  out.u8(0);
  out.be16(0);
  out.be32(0);
  target.write(out);
  if (!duplicate_detection) {  // our link-layer address (not from :: - RFC 4861 4.3)
    out.u8(kOptionSourceLinkAddress);
    out.u8(1);
    nic_.mac_address().write(out);
  }
  if (out.ok()) send_icmpv6(source, target.solicited_node(), out.written(), kNdHopLimit);
}

void NetworkStack::send_neighbor_advertisement(const Ipv6Address& to, const Ipv6Address& target, bool solicited) {
  uint8_t message[32];
  ByteWriter out{ByteView(message)};
  out.u8(kIcmpv6NeighborAdvertisement);
  out.u8(0);
  out.be16(0);
  out.u8(static_cast<uint8_t>((solicited ? 0x40 : 0) | 0x20));  // solicited, override (not a router)
  out.zeros(3);
  target.write(out);
  out.u8(kOptionTargetLinkAddress);
  out.u8(1);
  nic_.mac_address().write(out);
  if (out.ok()) send_icmpv6(target, to.is_unspecified() ? kIpv6AllNodes : to, out.written(), kNdHopLimit);
}

void NetworkStack::send_router_solicitation() {
  const Ipv6Address source = config6_.link_local_ready ? config6_.link_local : kIpv6Any;
  uint8_t message[16];
  ByteWriter out{ByteView(message)};
  out.u8(kIcmpv6RouterSolicitation);
  out.u8(0);
  out.be16(0);
  out.be32(0);
  if (!source.is_unspecified()) {
    out.u8(kOptionSourceLinkAddress);
    out.u8(1);
    nic_.mac_address().write(out);
  }
  if (out.ok()) send_icmpv6(source, kIpv6AllRouters, out.written(), kNdHopLimit);
}

// MLDv2 (RFC 3810 5.2): "change to exclude nothing" for the solicited-node groups of our addresses, which is joining them.
void NetworkStack::send_mld_report() {
  Ipv6Address groups[2];
  uint32_t count = 0;
  const auto add = [&](const Ipv6Address& address) {
    const Ipv6Address group = address.solicited_node();
    for (uint32_t i = 0; i < count; ++i)
      if (groups[i] == group) return;
    if (count < 2) groups[count++] = group;
  };
  add(config6_.link_local_ready ? config6_.link_local : tentative_link_local_);
  if (global_state_ == AddressState::Tentative || config6_.global_ready) add(config6_.global_ready ? config6_.global : tentative_global_);
  uint8_t message[8 + 2 * 20];
  ByteWriter out{ByteView(message)};
  out.u8(kIcmpv6Mldv2Report);
  out.u8(0);
  out.be16(0);
  out.be16(0);
  out.be16(static_cast<uint16_t>(count));
  for (uint32_t i = 0; i < count; ++i) {
    out.u8(kMldChangeToExclude);
    out.u8(0);   // no auxiliary data
    out.be16(0); // no sources
    groups[i].write(out);
  }
  if (!out.ok()) return;
  send_icmpv6(config6_.link_local_ready ? config6_.link_local : kIpv6Any, kIpv6AllMldv2Routers, out.written(), 1, true);
}

// ------------------------------------------------------------------ names

bool NetworkStack::resolve_ip(StringView name, uint32_t timeout_ms, IpAddress* address, bool prefer_ipv6) {
  const uint64_t deadline = now_ms() + timeout_ms;
  const auto wait = [&](SharedPointer<IDnsQuery> query) {
    if (!query) return false;
    while (query->status() == DnsStatus::Pending && now_ms() < deadline) {
      poll();
      kernel::sleep_us(200);
    }
    if (query->status() != DnsStatus::Resolved) return false;
    if (address) *address = query->ip_address();
    return true;
  };
  bool reachable6;
  {
    kernel::ScopedRecursiveMutex guard(mutex_);
    reachable6 = config6_.global_ready && !config6_.router.is_unspecified();
  }
  Ipv6Address literal;
  if (parse_ipv6(name, &literal)) {
    if (address) *address = IpAddress(literal);
    return true;
  }
  if (prefer_ipv6 && reachable6 && wait(resolver_->query_aaaa(name))) return true;
  return wait(resolver_->query(name));
}

// ------------------------------------------------------------------ ping

bool NetworkStack::ping6(const Ipv6Address& address, uint32_t timeout_ms, uint32_t* rtt_ms) {
  kernel::ScopedMutex one_at_a_time(ping_mutex_);
  uint8_t request_storage[8 + 32];
  bool ok;
  uint64_t deadline;
  {
    kernel::ScopedRecursiveMutex guard(mutex_);
    const Ipv6Address source = source_for6(address);
    if (source.is_unspecified()) return false;
    ByteWriter request{ByteView(request_storage)};
    request.u8(kIcmpv6EchoRequest);
    request.u8(0);
    request.be16(0);
    request.be16(ping_id_);
    request.be16(++ping_sequence_);
    for (uint32_t i = 0; i < 32; ++i) request.u8(static_cast<uint8_t>('a' + i % 26));
    ping_active_ = true;
    ping_v6_ = true;
    ping_answered_ = false;
    ping_target6_ = address;
    ping_sent_ms_ = now_ms();
    deadline = ping_sent_ms_ + timeout_ms;
    ok = request.ok() && send_icmpv6(source, address, request.written(), 64);
  }
  for (;;) {
    {
      kernel::ScopedRecursiveMutex guard(mutex_);
      if (!ok || ping_answered_ || now_ms() >= deadline) break;
    }
    poll();
    kernel::sleep_us(200);
  }
  kernel::ScopedRecursiveMutex guard(mutex_);
  ok = ok && ping_answered_;
  ping_active_ = false;
  ping_v6_ = false;
  if (ok && rtt_ms) *rtt_ms = static_cast<uint32_t>(ping_rtt_ms_);
  return ok;
}

}  // namespace network
