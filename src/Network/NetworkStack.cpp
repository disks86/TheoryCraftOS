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

void NetworkStack::configure(const IpConfiguration& configuration) {
  dhcp_->stop();
  config_ = configuration;
}

void NetworkStack::start_dhcp() { dhcp_->start(); }

void NetworkStack::on_lease(const IpConfiguration& configuration) { config_ = configuration; }

void NetworkStack::on_lease_lost() { config_ = IpConfiguration{}; }

bool NetworkStack::wait_for_address(uint32_t timeout_ms) {
  const uint64_t deadline = now_ms() + timeout_ms;
  while (!config_.configured()) {
    if (now_ms() >= deadline) return false;
    poll();
  }
  return true;
}

SharedPointer<IUdpSocket> NetworkStack::udp_open(uint16_t port) {
  return SharedPointer<IUdpSocket>(udp_.open(port));
}

SharedPointer<ITcpSocket> NetworkStack::tcp_open() { return SharedPointer<ITcpSocket>(tcp_.create()); }

// ------------------------------------------------------------------ receiving

void NetworkStack::poll() {
  uint8_t frame[hardware::kEthernetMaxFrame + 64];
  uint32_t length = 0;
  for (uint32_t i = 0; i < kFramesPerPoll && nic_.receive(frame, sizeof(frame), &length); ++i)
    if (length <= sizeof(frame)) handle_frame(frame, length);

  dhcp_->poll();
  resolver_->poll();
  service_pending();
  tcp_.poll();
  udp_.reap();
}

void NetworkStack::handle_frame(const uint8_t* frame, uint32_t length) {
  if (length < hardware::kEthernetHeaderSize) return;
  hardware::MacAddress destination;
  for (int i = 0; i < 6; ++i) destination.bytes[i] = frame[i];
  if (!destination.is_broadcast() && !(destination == nic_.mac_address())) return;  // not for us

  const uint16_t type = load16(frame + 12);
  const uint8_t* payload = frame + hardware::kEthernetHeaderSize;
  const uint32_t payload_length = length - hardware::kEthernetHeaderSize;
  if (type == kEtherTypeArp) handle_arp(payload, payload_length);
  else if (type == kEtherTypeIpv4) handle_ipv4(payload, payload_length);
}

void NetworkStack::learn(Ipv4Address address, const hardware::MacAddress& mac) {
  if (address.is_unspecified()) return;
  arp_.put(address.value(), ArpEntry{mac, now_ms() + kArpLifetimeMs});
}

void NetworkStack::handle_arp(const uint8_t* arp, uint32_t length) {
  // Ethernet / IPv4 ARP only: hardware type 1, protocol 0x0800, sizes 6 and 4.
  if (length < kArpPacketSize || load16(arp) != 1 || load16(arp + 2) != kEtherTypeIpv4 || arp[4] != 6 || arp[5] != 4)
    return;
  const uint16_t operation = load16(arp + 6);
  hardware::MacAddress sender_mac;
  for (int i = 0; i < 6; ++i) sender_mac.bytes[i] = arp[8 + i];
  const Ipv4Address sender_ip = Ipv4Address::read(arp + 14);
  const Ipv4Address target_ip = Ipv4Address::read(arp + 24);

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
         destination == Ipv4Address::subnet_broadcast(config_.address, config_.netmask);
}

void NetworkStack::handle_ipv4(const uint8_t* packet, uint32_t length) {
  if (length < kIpv4HeaderSize || (packet[0] >> 4) != 4) return;
  const uint32_t header = (packet[0] & 0x0F) * 4u;
  const uint32_t total = load16(packet + 2);
  if (header < kIpv4HeaderSize || header > total || total > length) return;  // Ethernet padding makes length > total
  if (checksum_finish(checksum_add(0, packet, header)) != 0) return;
  if (load16(packet + 6) & 0x3FFF) return;  // a fragment (more-fragments flag or an offset): unsupported
  const Ipv4Address source = Ipv4Address::read(packet + 12);
  const Ipv4Address destination = Ipv4Address::read(packet + 16);
  if (!accepts_destination(destination)) return;

  const uint8_t protocol = packet[9];
  const uint8_t* payload = packet + header;
  const uint32_t payload_length = total - header;
  if (protocol == kProtocolUdp) {
    udp_.handle(source, destination, payload, payload_length);
  } else if (protocol == kProtocolTcp) {
    if (config_.configured() && destination == config_.address) tcp_.handle(source, destination, payload, payload_length);
  } else if (protocol == kProtocolIcmp) {
    if (config_.configured() && destination == config_.address) handle_icmp(source, payload, payload_length);
  }
}

void NetworkStack::handle_icmp(Ipv4Address source, const uint8_t* icmp, uint32_t length) {
  if (length < 8 || checksum_finish(checksum_add(0, icmp, length)) != 0) return;
  if (icmp[0] == kIcmpEchoRequest && icmp[1] == 0 && length <= kMaxIpPayload) {
    uint8_t reply[kMaxIpPayload];
    for (uint32_t i = 0; i < length; ++i) reply[i] = icmp[i];
    reply[0] = kIcmpEchoReply;
    store16(reply + 2, 0);
    store16(reply + 2, checksum_finish(checksum_add(0, reply, length)));
    send_ip(source, kProtocolIcmp, reply, length);
  } else if (icmp[0] == kIcmpEchoReply && ping_active_ && source == ping_target_ && load16(icmp + 4) == ping_id_ &&
             load16(icmp + 6) == ping_sequence_) {
    ping_answered_ = true;
    ping_rtt_ms_ = now_ms() - ping_sent_ms_;
  }
}

// ------------------------------------------------------------------ sending

bool NetworkStack::send_frame(const hardware::MacAddress& destination, uint16_t ether_type, const uint8_t* payload,
                              uint32_t length) {
  if (length > kMtu) return false;
  uint8_t frame[hardware::kEthernetMaxFrame];
  for (int i = 0; i < 6; ++i) {
    frame[i] = destination.bytes[i];
    frame[6 + i] = nic_.mac_address().bytes[i];
  }
  store16(frame + 12, ether_type);
  for (uint32_t i = 0; i < length; ++i) frame[hardware::kEthernetHeaderSize + i] = payload[i];
  return nic_.transmit(frame, hardware::kEthernetHeaderSize + length);
}

void NetworkStack::send_arp(uint16_t operation, const hardware::MacAddress& target_mac, Ipv4Address target_ip) {
  uint8_t arp[kArpPacketSize];
  store16(arp, 1);
  store16(arp + 2, kEtherTypeIpv4);
  arp[4] = 6;
  arp[5] = 4;
  store16(arp + 6, operation);
  for (int i = 0; i < 6; ++i) {
    arp[8 + i] = nic_.mac_address().bytes[i];
    arp[18 + i] = operation == kArpRequest ? 0 : target_mac.bytes[i];
  }
  config_.address.write(arp + 14);
  target_ip.write(arp + 24);
  const hardware::MacAddress to = operation == kArpRequest ? hardware::MacAddress{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}
                                                           : target_mac;
  send_frame(to, kEtherTypeArp, arp, kArpPacketSize);
}

bool NetworkStack::send_ip(Ipv4Address destination, uint8_t protocol, const uint8_t* payload, uint32_t length) {
  if (!config_.configured()) return false;
  return send_ip_from(config_.address, destination, protocol, payload, length, false);
}

bool NetworkStack::send_ip_from(Ipv4Address source, Ipv4Address destination, uint8_t protocol, const uint8_t* payload,
                                uint32_t length, bool link_broadcast) {
  if (length > kMaxIpPayload || (length && !payload)) return false;

  uint8_t packet[kMtu];
  packet[0] = 0x45;  // IPv4, 5 words of header
  packet[1] = 0;
  store16(packet + 2, static_cast<uint16_t>(kIpv4HeaderSize + length));
  store16(packet + 4, ip_id_++);
  store16(packet + 6, 0x4000);  // don't fragment
  packet[8] = 64;               // TTL
  packet[9] = protocol;
  store16(packet + 10, 0);
  source.write(packet + 12);
  destination.write(packet + 16);
  store16(packet + 10, checksum_finish(checksum_add(0, packet, kIpv4HeaderSize)));
  for (uint32_t i = 0; i < length; ++i) packet[kIpv4HeaderSize + i] = payload[i];
  const uint32_t total = kIpv4HeaderSize + length;

  const bool broadcast = link_broadcast || destination.is_limited_broadcast() ||
                         (config_.configured() && destination == Ipv4Address::subnet_broadcast(config_.address, config_.netmask));
  if (broadcast) return send_frame(hardware::MacAddress{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}, kEtherTypeIpv4, packet, total);

  // On our subnet we talk to the destination directly, otherwise to the gateway.
  Ipv4Address next_hop = destination;
  if (!config_.configured() || !destination.in_subnet(config_.address, config_.netmask)) {
    if (config_.gateway.is_unspecified()) return false;
    next_hop = config_.gateway;
  }

  ArpEntry* entry = arp_.find(next_hop.value());
  if (entry && entry->expires_ms > now_ms()) return send_frame(entry->mac, kEtherTypeIpv4, packet, total);

  // Unknown (or stale) next hop: park the packet and ask who has it.
  if (pending_.count() >= kMaxPending) return false;
  PendingPacket p;
  p.next_hop = next_hop;
  p.requests = 1;
  p.next_request_ms = now_ms() + kArpRetryMs;
  p.length = static_cast<uint16_t>(total);
  for (uint32_t i = 0; i < total; ++i) p.ip_packet[i] = packet[i];
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
    if (p.next_hop == resolved) send_frame(mac, kEtherTypeIpv4, p.ip_packet, p.length);
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
      send_frame(known->mac, kEtherTypeIpv4, p.ip_packet, p.length);
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

bool NetworkStack::resolve(const char* name, uint32_t timeout_ms, Ipv4Address* address) {
  SharedPointer<IDnsQuery> query = resolver_->query(name);
  if (!query) return false;
  const uint64_t deadline = now_ms() + timeout_ms;
  while (query->status() == DnsStatus::Pending && now_ms() < deadline) poll();
  if (query->status() != DnsStatus::Resolved) return false;
  if (address) *address = query->address();
  return true;
}

// ------------------------------------------------------------------ ping

bool NetworkStack::ping(Ipv4Address address, uint32_t timeout_ms, uint32_t* rtt_ms) {
  if (!config_.configured()) return false;
  uint8_t request[8 + 32];
  request[0] = kIcmpEchoRequest;
  request[1] = 0;
  store16(request + 2, 0);
  store16(request + 4, ping_id_);
  store16(request + 6, ++ping_sequence_);
  for (uint32_t i = 0; i < 32; ++i) request[8 + i] = static_cast<uint8_t>('a' + i % 26);
  store16(request + 2, checksum_finish(checksum_add(0, request, sizeof(request))));

  ping_active_ = true;
  ping_answered_ = false;
  ping_target_ = address;
  ping_sent_ms_ = now_ms();
  const uint64_t deadline = ping_sent_ms_ + timeout_ms;
  bool ok = send_ip(address, kProtocolIcmp, request, sizeof(request));
  while (ok && !ping_answered_ && now_ms() < deadline) poll();
  ok = ok && ping_answered_;
  ping_active_ = false;
  if (ok && rtt_ms) *rtt_ms = static_cast<uint32_t>(ping_rtt_ms_);
  return ok;
}

}  // namespace network
