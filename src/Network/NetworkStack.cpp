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

#include "Network/NetworkStack.hpp"
#include "Network/Checksum.hpp"
#include "Kernel/Thread.hpp"

namespace network {

namespace {
constexpr uint32_t kArpPacketSize = 28;
constexpr uint64_t kArpLifetimeMs = 120000;
constexpr uint32_t kArpRetryMs = 1000, kArpMaxRequests = 3;
constexpr uint32_t kMaxPending = 8;
constexpr uint32_t kFramesPerPoll = 64;  // bound the work done in one poll()
constexpr uint8_t kIcmpEchoReply = 0, kIcmpEchoRequest = 8;
constexpr uint16_t kArpRequest = 1, kArpReply = 2;
}  // namespace

NetworkStack::NetworkStack(hardware::INetworkInterface& nic, hardware::IClock& clock, allocators::IAllocator& allocator)
    : nic_(nic),
      clock_(clock),
      allocator_(allocator),
      arp_(allocator),
      pending_(allocator),
      udp_(*this, allocator),
      tcp_(*this, allocator) {}

NetworkStack::~NetworkStack() = default;

bool NetworkStack::initialize() {
  dhcp_socket_ = udp_.open(DhcpClient::kClientPort);
  if (!dhcp_socket_) return false;
  IIpLayer& ip = *this;
  IDhcpListener& listener = *this;  // private base: converted here, where it is accessible
  dhcp_ = UniquePointer<DhcpClient>::make(allocator_, ip, dhcp_socket_, nic_.mac_address(), listener);
  INetworkStack& stack = *this;
  resolver_ = UniquePointer<DnsResolver>::make(allocator_, stack, allocator_);
  return dhcp_ && resolver_;
}

// ------------------------------------------------------------------ configuration

IpConfiguration NetworkStack::configuration() const {
  kernel::ScopedRecursiveMutex guard(const_cast<kernel::RecursiveMutex&>(mutex_));
  return config_;
}

void NetworkStack::configure(const IpConfiguration& configuration) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  dhcp_->stop();
  config_ = configuration;
}

void NetworkStack::start_dhcp() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  dhcp_->start();
}

void NetworkStack::on_lease(const IpConfiguration& configuration) { config_ = configuration; }

void NetworkStack::on_lease_lost() { config_ = IpConfiguration{}; }

bool NetworkStack::wait_for_address(uint32_t timeout_ms) {
  const uint64_t deadline = now_ms() + timeout_ms;
  while (!configuration().configured()) {  // the lock is taken per step, never held while waiting
    if (now_ms() >= deadline) return false;
    poll();
    kernel::sleep_us(200);
  }
  return true;
}

SharedPointer<IUdpSocket> NetworkStack::udp_open(uint16_t port) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  return SharedPointer<IUdpSocket>(udp_.open(port));
}

SharedPointer<ITcpSocket> NetworkStack::tcp_open() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  return SharedPointer<ITcpSocket>(tcp_.create());
}

// ------------------------------------------------------------------ receiving

void NetworkStack::poll() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  uint8_t frame[hardware::kEthernetMaxFrame + 64];
  uint32_t length = 0;
  for (uint32_t i = 0; i < kFramesPerPoll && nic_.receive(ByteView(frame), &length); ++i)
    if (length <= sizeof(frame)) handle_frame(ConstByteView(frame).first(length));

  dhcp_->poll();
  resolver_->poll();
  service_pending();
  tcp_.poll();
  udp_.reap();
}

void NetworkStack::handle_frame(ConstByteView frame) {
  ByteReader in(frame);
  const hardware::MacAddress destination = hardware::MacAddress::read(in);
  in.skip(6);  // the sender's address is not needed here
  const uint16_t type = in.be16();
  if (!in.ok()) return;  // shorter than an Ethernet header
  const bool multicast_mac = (destination.bytes[0] & 1) && !destination.is_broadcast();
  if (!destination.is_broadcast() && !(destination == nic_.mac_address()) && !(multicast_mac && group_count_ > 0)) return;  // not for us

  if (type == kEtherTypeArp) handle_arp(in.rest());
  else if (type == kEtherTypeIpv4) handle_ipv4(in.rest());
}

void NetworkStack::learn(Ipv4Address address, const hardware::MacAddress& mac) {
  if (address.is_unspecified()) return;
  arp_.put(address.value(), ArpEntry{mac, now_ms() + kArpLifetimeMs});
}

void NetworkStack::handle_arp(ConstByteView arp) {
  // Ethernet / IPv4 ARP only: hardware type 1, protocol 0x0800, sizes 6 and 4.
  ByteReader in(arp);
  if (in.be16() != 1 || in.be16() != kEtherTypeIpv4 || in.u8() != 6 || in.u8() != 4) return;
  const uint16_t operation = in.be16();
  const hardware::MacAddress sender_mac = hardware::MacAddress::read(in);
  const Ipv4Address sender_ip = Ipv4Address::read(in);
  in.skip(6);  // the target's hardware address
  const Ipv4Address target_ip = Ipv4Address::read(in);
  if (!in.ok()) return;  // truncated

  const bool for_us = config_.configured() && target_ip == config_.address;
  // Learn the sender if we were addressed or already know it (RFC 826): this
  // refreshes entries without letting every broadcast fill the cache.
  if (for_us || arp_.contains(sender_ip.value())) learn(sender_ip, sender_mac);
  if (operation == kArpRequest && for_us) send_arp(kArpReply, sender_mac, sender_ip);
  if (for_us || operation == kArpReply) flush_pending(sender_ip);
}

bool NetworkStack::accepts_destination(Ipv4Address destination) const {
  if (!config_.configured()) return true;  // before DHCP finishes we take what the server sends us
  return destination == config_.address || destination.is_limited_broadcast() ||
         destination == Ipv4Address::subnet_broadcast(config_.address, config_.netmask) || is_member(destination);
}

bool NetworkStack::is_member(Ipv4Address group) const {
  if (!group.is_multicast()) return false;
  if (group_count_ > 0 && group == Ipv4Address::make(224, 0, 0, 1)) return true;  // every member listens to "all hosts" (IGMP queries)
  for (uint32_t i = 0; i < group_count_; ++i)
    if (groups_[i] == group) return true;
  return false;
}

// ------------------------------------------------------------------ multicast

bool NetworkStack::join_multicast(Ipv4Address group) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (!group.is_multicast()) return false;
  if (is_member(group)) return true;
  if (group_count_ >= kMaxGroups) return false;
  groups_[group_count_++] = group;
  nic_.accept_all_multicast(true);
  send_igmp_report(group);
  return true;
}

void NetworkStack::leave_multicast(Ipv4Address group) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  for (uint32_t i = 0; i < group_count_; ++i) {
    if (!(groups_[i] == group)) continue;
    groups_[i] = groups_[--group_count_];
    if (group_count_ == 0) nic_.accept_all_multicast(false);
    return;
  }
}

// IGMPv2 (RFC 2236): a membership report is the 8 bytes {0x16, 0, checksum, group}, sent to the group itself.
void NetworkStack::send_igmp_report(Ipv4Address group) {
  if (!config_.configured()) return;
  uint8_t msg[8] = {0x16, 0, 0, 0};
  for (int i = 0; i < 4; ++i) msg[4 + i] = group.octets[i];
  const uint16_t sum = checksum_finish(checksum_add(0, ConstByteView(msg, 8)));
  msg[2] = static_cast<uint8_t>(sum >> 8);
  msg[3] = static_cast<uint8_t>(sum);
  send_ip(group, kProtocolIgmp, ConstByteView(msg, 8));
}

void NetworkStack::handle_igmp(ConstByteView igmp) {
  if (igmp.size() < 8 || checksum_finish(checksum_add(0, igmp.first(8))) != 0) return;
  if (igmp[0] != 0x11) return;  // only queries matter: a general query (group 0) or one for a group we are in
  const Ipv4Address asked = Ipv4Address::make(igmp[4], igmp[5], igmp[6], igmp[7]);
  for (uint32_t i = 0; i < group_count_; ++i)
    if (asked.is_unspecified() || asked == groups_[i]) send_igmp_report(groups_[i]);
}

void NetworkStack::handle_ipv4(ConstByteView packet) {
  ByteReader in(packet);
  const uint8_t version_and_length = in.u8();
  in.skip(1);  // type of service
  const uint16_t total = in.be16();
  in.skip(2);  // identification
  const uint16_t flags_and_offset = in.be16();
  in.skip(1);  // time to live
  const uint8_t protocol = in.u8();
  in.skip(2);  // header checksum (verified below over the whole header)
  const Ipv4Address source = Ipv4Address::read(in);
  const Ipv4Address destination = Ipv4Address::read(in);
  if (!in.ok() || (version_and_length >> 4) != 4) return;

  const uint32_t header = (version_and_length & 0x0F) * 4u;
  if (header < kIpv4HeaderSize || header > total || total > packet.size()) return;  // Ethernet padding makes the frame longer than total
  if (checksum_finish(checksum_add(0, packet.first(header))) != 0) return;
  if (flags_and_offset & 0x3FFF) return;  // a fragment (more-fragments flag or an offset): unsupported
  if (!accepts_destination(destination)) return;

  ConstByteView payload = packet.subview(header, total - header);
  if (protocol == kProtocolUdp) {
    udp_.handle(source, destination, payload);
  } else if (protocol == kProtocolTcp) {
    if (config_.configured() && destination == config_.address) tcp_.handle(source, destination, payload);
  } else if (protocol == kProtocolIcmp) {
    if (config_.configured() && destination == config_.address) handle_icmp(source, payload);
  } else if (protocol == kProtocolIgmp) {
    handle_igmp(payload);
  }
}

void NetworkStack::handle_icmp(Ipv4Address source, ConstByteView icmp) {
  if (icmp.size() < 8 || checksum_finish(checksum_add(0, icmp)) != 0) return;
  ByteReader in(icmp);
  const uint8_t type = in.u8();
  const uint8_t code = in.u8();
  in.skip(2);  // checksum, verified above
  const uint16_t identifier = in.be16();
  const uint16_t sequence = in.be16();
  if (type == kIcmpEchoRequest && code == 0 && icmp.size() <= kMaxIpPayload) {
    uint8_t reply[kMaxIpPayload];
    ByteWriter out{ByteView(reply)};
    out.u8(kIcmpEchoReply);
    out.u8(0);
    out.be16(0);  // checksum, filled in below
    out.bytes(icmp.dropped(4));  // identifier, sequence and the echoed data
    if (!out.ok()) return;
    out.patch_be16(2, checksum_finish(checksum_add(0, out.written())));
    send_ip(source, kProtocolIcmp, out.written());
  } else if (type == kIcmpEchoReply && ping_active_ && source == ping_target_ && identifier == ping_id_ &&
             sequence == ping_sequence_) {
    ping_answered_ = true;
    ping_rtt_ms_ = now_ms() - ping_sent_ms_;
  }
}

// ------------------------------------------------------------------ sending

bool NetworkStack::send_frame(const hardware::MacAddress& destination, uint16_t ether_type, ConstByteView payload) {
  if (payload.size() > kMtu) return false;
  uint8_t frame[hardware::kEthernetMaxFrame];
  ByteWriter out{ByteView(frame)};
  destination.write(out);
  nic_.mac_address().write(out);
  out.be16(ether_type);
  out.bytes(payload);
  if (!out.ok()) return false;
  return nic_.transmit(out.written());
}

void NetworkStack::send_arp(uint16_t operation, const hardware::MacAddress& target_mac, Ipv4Address target_ip) {
  uint8_t arp[kArpPacketSize];
  ByteWriter out{ByteView(arp)};
  out.be16(1);  // Ethernet
  out.be16(kEtherTypeIpv4);
  out.u8(6);
  out.u8(4);
  out.be16(operation);
  nic_.mac_address().write(out);
  config_.address.write(out);
  if (operation == kArpRequest) out.zeros(6);  // asking: the target's address is what we want to learn
  else target_mac.write(out);
  target_ip.write(out);
  if (!out.ok()) return;
  const hardware::MacAddress to = operation == kArpRequest ? hardware::MacAddress{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}
                                                           : target_mac;
  send_frame(to, kEtherTypeArp, out.written());
}

bool NetworkStack::send_ip(Ipv4Address destination, uint8_t protocol, ConstByteView payload) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (!config_.configured()) return false;
  return send_ip_from(config_.address, destination, protocol, payload, false);
}

bool NetworkStack::send_ip_from(Ipv4Address source, Ipv4Address destination, uint8_t protocol, ConstByteView payload,
                                bool link_broadcast) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (payload.size() > kMaxIpPayload) return false;

  uint8_t packet_storage[kMtu];
  ByteWriter out{ByteView(packet_storage)};
  out.u8(0x45);  // IPv4, 5 words of header
  out.u8(0);
  out.be16(static_cast<uint16_t>(kIpv4HeaderSize + payload.size()));
  out.be16(ip_id_++);
  out.be16(0x4000);  // don't fragment
  out.u8(destination.is_multicast() ? (protocol == kProtocolIgmp ? 1 : 255) : 64);  // TTL (mDNS wants 255 for multicast UDP)
  out.u8(protocol);
  out.be16(0);  // header checksum, filled in below
  source.write(out);
  destination.write(out);
  out.patch_be16(10, checksum_finish(checksum_add(0, out.written())));  // so far: just the 20 header bytes
  out.bytes(payload);
  if (!out.ok()) return false;
  const ConstByteView packet = out.written();

  const bool broadcast = link_broadcast || destination.is_limited_broadcast() ||
                         (config_.configured() && destination == Ipv4Address::subnet_broadcast(config_.address, config_.netmask));
  if (broadcast) return send_frame(hardware::MacAddress{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}, kEtherTypeIpv4, packet);
  if (destination.is_multicast()) {  // the group's Ethernet address: 01:00:5e and the low 23 bits of the group
    return send_frame(hardware::MacAddress{{0x01, 0x00, 0x5E, static_cast<uint8_t>(destination.octets[1] & 0x7F), destination.octets[2], destination.octets[3]}}, kEtherTypeIpv4, packet);
  }

  // On our subnet we talk to the destination directly, otherwise to the gateway.
  Ipv4Address next_hop = destination;
  if (!config_.configured() || !destination.in_subnet(config_.address, config_.netmask)) {
    if (config_.gateway.is_unspecified()) return false;
    next_hop = config_.gateway;
  }

  ArpEntry* entry = arp_.find(next_hop.value());
  if (entry && entry->expires_ms > now_ms()) return send_frame(entry->mac, kEtherTypeIpv4, packet);

  // Unknown (or stale) next hop: park the packet and ask who has it.
  if (pending_.count() >= kMaxPending) return false;
  PendingPacket p;
  p.next_hop = next_hop;
  p.requests = 1;
  p.next_request_ms = now_ms() + kArpRetryMs;
  p.length = static_cast<uint16_t>(packet.size());
  if (!ByteView(p.ip_packet).copy_from(packet)) return false;
  if (!pending_.enqueue(p)) return false;
  send_arp(kArpRequest, hardware::MacAddress{}, next_hop);
  return true;
}

void NetworkStack::flush_pending(Ipv4Address resolved) {
  ArpEntry* entry = arp_.find(resolved.value());
  if (!entry) return;
  const hardware::MacAddress mac = entry->mac;
  // Walk the queue once, sending what is now deliverable and re-queueing the rest.
  for (uint64_t n = pending_.count(); n > 0; --n) {
    PendingPacket p;
    if (!pending_.dequeue(&p)) break;
    if (p.next_hop == resolved) send_frame(mac, kEtherTypeIpv4, ConstByteView(p.ip_packet).first(p.length));
    else pending_.enqueue(p);
  }
}

void NetworkStack::service_pending() {
  if (pending_.empty()) return;
  const uint64_t now = now_ms();
  for (uint64_t n = pending_.count(); n > 0; --n) {
    PendingPacket p;
    if (!pending_.dequeue(&p)) break;
    ArpEntry* known = arp_.find(p.next_hop.value());
    if (known && known->expires_ms > now) {
      send_frame(known->mac, kEtherTypeIpv4, ConstByteView(p.ip_packet).first(p.length));
      continue;
    }
    if (now >= p.next_request_ms) {
      if (p.requests >= kArpMaxRequests) continue;  // nobody answered: drop the packet
      ++p.requests;
      p.next_request_ms = now + kArpRetryMs;
      send_arp(kArpRequest, hardware::MacAddress{}, p.next_hop);
    }
    pending_.enqueue(p);
  }
}

// ------------------------------------------------------------------ DNS

bool NetworkStack::resolve(StringView name, uint32_t timeout_ms, Ipv4Address* address) {
  SharedPointer<IDnsQuery> query = resolver_->query(name);
  if (!query) return false;
  const uint64_t deadline = now_ms() + timeout_ms;
  while (query->status() == DnsStatus::Pending && now_ms() < deadline) {
    poll();
    kernel::sleep_us(200);
  }
  if (query->status() != DnsStatus::Resolved) return false;
  if (address) *address = query->address();
  return true;
}

// ------------------------------------------------------------------ ping

bool NetworkStack::ping(Ipv4Address address, uint32_t timeout_ms, uint32_t* rtt_ms) {
  kernel::ScopedMutex one_at_a_time(ping_mutex_);
  if (!configuration().configured()) return false;
  uint8_t request_storage[8 + 32];
  ByteWriter request{ByteView(request_storage)};
  request.u8(kIcmpEchoRequest);
  request.u8(0);
  request.be16(0);  // checksum, filled in below
  request.be16(ping_id_);
  request.be16(++ping_sequence_);
  for (uint32_t i = 0; i < 32; ++i) request.u8(static_cast<uint8_t>('a' + i % 26));
  request.patch_be16(2, checksum_finish(checksum_add(0, request.written())));

  bool ok;
  uint64_t deadline;
  {
    kernel::ScopedRecursiveMutex guard(mutex_);
    ping_active_ = true;
    ping_answered_ = false;
    ping_target_ = address;
    ping_sent_ms_ = now_ms();
    deadline = ping_sent_ms_ + timeout_ms;
    ok = request.ok() && send_ip(address, kProtocolIcmp, request.written());
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
  if (ok && rtt_ms) *rtt_ms = static_cast<uint32_t>(ping_rtt_ms_);
  return ok;
}

}  // namespace network
