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
#include "Utility/AllocatedView.hpp"
#include "Network/Checksum.hpp"
#include "Kernel/Thread.hpp"

namespace network {

namespace {
constexpr uint32_t kArpPacketSize = 28;
constexpr uint64_t kArpLifetimeMs = 120000;
constexpr uint32_t kArpRetryMs = 1000, kArpMaxRequests = 3;
constexpr uint32_t kMaxPending = 8;
constexpr uint32_t kMaxLooped = 256;         // packets to itself not yet delivered; more are dropped (TCP sends them again)
constexpr uint32_t kLoopedPerPoll = 4096;    // a bound on the exchanges one poll() carries on over the loopback
constexpr uint32_t kFramesPerPoll = 64;  // bound the work done in one poll()
constexpr uint8_t kIcmpEchoReply = 0, kIcmpEchoRequest = 8;
constexpr uint16_t kArpRequest = 1, kArpReply = 2;
// RFC 3927: three probes about a second apart, two seconds of quiet, two announcements two seconds apart, ten conflicts before slowing down, defend an address once in ten seconds.
constexpr uint32_t kLinkLocalProbes = 3, kLinkLocalAnnouncements = 2, kLinkLocalMaxConflicts = 10;
constexpr uint64_t kLinkLocalProbeMs = 1000, kLinkLocalQuietMs = 2000, kLinkLocalAnnounceMs = 2000, kLinkLocalRateLimitMs = 60000, kLinkLocalDefendMs = 10000, kLinkLocalDhcpRetryMs = 60000;
}  // namespace

NetworkStack::NetworkStack(hardware::INetworkInterface& nic, hardware::IClock& clock, allocators::IAllocator& allocator)
    : nic_(nic),
      clock_(clock),
      allocator_(allocator),
      arp_(allocator),
      pending_(allocator),
      looped_(allocator),
      udp_(*this, allocator),
      tcp_(*this, allocator),
      pending6_(allocator) {}

NetworkStack::~NetworkStack() {
  release_view(allocator_, coalesce_storage_);
  release_view(allocator_, segment_frame_);
}

bool NetworkStack::initialize() {
  dhcp_socket_ = udp_.open(DhcpClient::kClientPort);
  if (!dhcp_socket_) return false;
  IIpLayer& ip = *this;
  IDhcpListener& listener = *this;  // private base: converted here, where it is accessible
  dhcp_ = UniquePointer<DhcpClient>::make(allocator_, ip, dhcp_socket_, nic_.mac_address(), listener);
  INetworkStack& stack = *this;
  resolver_ = UniquePointer<DnsResolver>::make(allocator_, stack, allocator_);
  // (without the memory, frames are just handled one by one and segments sent packet by packet)
  coalesce_storage_ = allocate_view<uint8_t>(allocator_, kCoalesceBytes);
  coalescer_ = TcpCoalescer(coalesce_storage_);
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
  link_local_state_ = LinkLocalState::Off;
  config_ = configuration;
}

void NetworkStack::start_dhcp() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  dhcp_->start();
}

void NetworkStack::on_lease(const IpConfiguration& configuration) {
  link_local_state_ = LinkLocalState::Off;  // a real lease replaces a made-up address
  config_ = configuration;
}

void NetworkStack::on_lease_lost() {
  if (link_local_state_ != LinkLocalState::Bound && link_local_state_ != LinkLocalState::Announcing) config_ = IpConfiguration{};
}

bool NetworkStack::wait_for_address(uint32_t timeout_ms) {
  const uint64_t deadline = now_ms() + timeout_ms;
  while (!configuration().configured()) {  // the lock is taken per step, never held while waiting
    if (now_ms() >= deadline) return false;
    poll();
    kernel::sleep_us(200);
  }
  return true;
}

SharedPointer<IUdpSocket> NetworkStack::udp_open_families(uint16_t port, IpFamilies families) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  return SharedPointer<IUdpSocket>(udp_.open(port, families));
}

SharedPointer<ITcpSocket> NetworkStack::tcp_open() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  return SharedPointer<ITcpSocket>(tcp_.create());
}

// ------------------------------------------------------------------ receiving

void NetworkStack::poll() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  uint8_t frame[hardware::kEthernetMaxFrame + 64];
  hardware::ReceivedFrame received;
  saturated_ = false;
  uint32_t i = 0;
  for (; i < kFramesPerPoll && nic_.receive_frame(ByteView(frame), &received); ++i) {
    if (received.length > sizeof(frame)) continue;
    const ConstByteView f = ConstByteView(frame).first(received.length);
    // Receive offload: a TCP segment for this machine may join the run being put together; anything else hands the run on first (order is kept).
    const bool ours = received.length >= hardware::kEthernetHeaderSize && hardware::MacAddress{{f[0], f[1], f[2], f[3], f[4], f[5]}} == nic_.mac_address();
    if (ours && coalescer_.offer(f, received)) continue;
    flush_coalesced();
    if (ours && coalescer_.offer(f, received)) continue;
    handle_frame(f, received);
  }
  saturated_ = i == kFramesPerPoll;
  // A poll that stopped at its limit is followed by another at once (NetworkStackSet): the run may go on in it. Otherwise it is handed on now,
  // so coalescing never holds a segment back for longer than the frames already waiting take.
  if (!saturated_) flush_coalesced();

  if (follow_link_) {
    const bool up = nic_.link_up();
    if (up != link_was_up_) {
      link_was_up_ = up;
      if (up) {
        dhcp_->start();
      } else {
        dhcp_->stop();
        link_local_state_ = LinkLocalState::Off;
        config_ = IpConfiguration{};
        stop_ipv6();  // (autoconfiguration starts over when the link is back)
      }
    }
  }
  deliver_looped();
  dhcp_->poll();
  service_ipv6();
  service_link_local();
  resolver_->poll();
  service_pending();
  tcp_.poll();
  deliver_looped();  // (what the timers and the received frames sent to this stack itself)
  udp_.reap();
}

// Packets to 127.0.0.0/8 or to our own address go no further than this queue. Delivering one may send
// others (an ACK, an answer), which are delivered in the same call, up to a bound.
void NetworkStack::deliver_looped() {
  for (uint32_t i = 0; i < kLoopedPerPoll && !looped_.empty(); ++i) {
    LoopedPacket p;
    if (!looped_.dequeue(&p)) break;
    const ConstByteView packet = ConstByteView(p.ip_packet).first(p.length);
    if (p.length && (p.ip_packet[0] >> 4) == 6) handle_ipv6(packet, true);
    else handle_ipv4(packet, true);
  }
}

// (also called before anything that must see the frames received so far in order: a send, a timer, another poll that is not straight after)
void NetworkStack::flush_coalesced() {
  if (!coalescer_.holding()) return;
  hardware::ReceivedFrame checked;  // (every piece was checked before it joined)
  checked.ipv4_header_checksum_good = true;
  checked.transport_checksum_good = true;
  const ConstByteView run = coalescer_.held();
  if ((run[0] >> 4) == 6) handle_ipv6(run, false, checked);
  else handle_ipv4(run, false, checked);
  coalescer_.clear();
}

void NetworkStack::handle_frame(ConstByteView frame, const hardware::ReceivedFrame& checks) {
  ByteReader in(frame);
  const hardware::MacAddress destination = hardware::MacAddress::read(in);
  in.skip(6);  // the sender's address is not needed here
  const uint16_t type = in.be16();
  if (!in.ok()) return;  // shorter than an Ethernet header
  const bool multicast_mac = (destination.bytes[0] & 1) && !destination.is_broadcast();
  const bool ipv6_multicast = destination.bytes[0] == 0x33 && destination.bytes[1] == 0x33;
  if (!destination.is_broadcast() && !(destination == nic_.mac_address()) && !(multicast_mac && (group_count_ > 0 || (ipv6_multicast && ipv6_started_))))
    return;  // not for us

  if (type == kEtherTypeArp) handle_arp(in.rest());
  else if (type == kEtherTypeIpv4) handle_ipv4(in.rest(), false, checks);
  else if (type == kEtherTypeIpv6) handle_ipv6(in.rest(), false, checks);
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

  check_link_local_claim(operation, sender_mac, sender_ip, target_ip);
  if (using_link_local() && target_ip == link_local_address_ && (sender_ip.is_unspecified() || sender_ip == link_local_address_) && !(sender_mac == nic_.mac_address()))
    return;  // a probe or a claim on our address: check_link_local_claim has defended it, and answering would only confirm the clash
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

void NetworkStack::handle_ipv4(ConstByteView packet, bool looped, const hardware::ReceivedFrame& checks) {
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
  if (!checks.ipv4_header_checksum_good && checksum_finish(checksum_add(0, packet.first(header))) != 0) return;
  if (flags_and_offset & 0x3FFF) return;  // a fragment (more-fragments flag or an offset): unsupported
  if (!looped && (!accepts_destination(destination) || destination.is_loopback())) return;  // (127/8 never arrives from a wire)

  ConstByteView payload = packet.subview(header, total - header);
  if (protocol == kProtocolUdp) {
    udp_.handle(source, destination, payload, checks.transport_checksum_good);
  } else if (protocol == kProtocolTcp) {
    if (looped || (config_.configured() && destination == config_.address)) tcp_.handle(source, destination, payload, checks.transport_checksum_good);
  } else if (protocol == kProtocolIcmp) {
    if (looped || (config_.configured() && destination == config_.address)) handle_icmp(source, payload);
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

bool NetworkStack::send_frame(const hardware::MacAddress& destination, uint16_t ether_type, ConstByteView payload, hardware::TransmitChecksum checksum) {
  if (payload.size() > kMtu) return false;
  uint8_t frame[hardware::kEthernetMaxFrame];
  ByteWriter out{ByteView(frame)};
  destination.write(out);
  nic_.mac_address().write(out);
  out.be16(ether_type);
  out.bytes(payload);
  if (!out.ok()) return false;
  if (!checksum.requested()) return nic_.transmit(out.written());
  checksum.start = static_cast<uint16_t>(checksum.start + hardware::kEthernetHeaderSize);
  checksum.field = static_cast<uint16_t>(checksum.field + hardware::kEthernetHeaderSize);
  if (nic_.offloads().transmit_checksums) return nic_.transmit_frame(out.written(), checksum);
  const ByteView written = ByteView(frame).first(out.position());
  return hardware::finish_transmit_checksum(written, checksum) && nic_.transmit(written);
}

// The MAC address a packet to `destination` goes to, when it is known now (an ARP entry that has not expired): for unicast to the wire only.
bool NetworkStack::next_hop_for(Ipv4Address destination, hardware::MacAddress* mac) const {
  if (!config_.configured() || destination.is_loopback() || destination == config_.address || destination.is_multicast() ||
      destination.is_limited_broadcast() || destination == Ipv4Address::subnet_broadcast(config_.address, config_.netmask))
    return false;
  Ipv4Address next_hop = destination;
  if (!destination.in_subnet(config_.address, config_.netmask)) {
    if (config_.gateway.is_unspecified()) return false;
    next_hop = config_.gateway;
  }
  const ArpEntry* entry = const_cast<Dictionary<uint32_t, ArpEntry>&>(arp_).find(next_hop.value());
  if (!entry || entry->expires_ms <= now_ms()) return false;
  *mac = entry->mac;
  return true;
}

// How large a frame the interface takes for segmentation (0: none), capped at what one IP packet can say.
static uint32_t segmentation_bytes(const hardware::INetworkInterface& nic) {
  const uint32_t bytes = nic.offloads().max_segmentation_bytes;
  if (bytes <= hardware::kEthernetMaxFrame) return 0;
  return bytes < 65535 + hardware::kEthernetHeaderSize ? bytes : 65535 + hardware::kEthernetHeaderSize;
}

uint32_t NetworkStack::max_segmented_payload(const IpAddress& destination) const {
  kernel::ScopedRecursiveMutex guard(const_cast<kernel::RecursiveMutex&>(mutex_));
  hardware::MacAddress mac;
  const uint32_t bytes = segmentation_bytes(nic_);
  if (bytes == 0) return 0;
  if (destination.v6) return link_address6_for(destination.ipv6, &mac) ? bytes - hardware::kEthernetHeaderSize - kIpv6HeaderSize : 0;
  if (!next_hop_for(destination.ipv4, &mac)) return 0;
  return bytes - hardware::kEthernetHeaderSize - kIpv4HeaderSize;
}

// The frame of a large TCP segment, built like any other (IP header, the pseudo-header seed in the checksum field) and given to the interface
// to cut up: TCP to a next hop whose address is known.
bool NetworkStack::send_tcp_segmented(const IpAddress& destination, ConstByteView segment, uint16_t mss) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  hardware::MacAddress mac;
  const uint32_t bytes = segmentation_bytes(nic_);
  const uint32_t ip_header = destination.v6 ? kIpv6HeaderSize : kIpv4HeaderSize;
  if (bytes == 0 || mss == 0 || segment.size() < 20) return false;
  if (destination.v6 ? !link_address6_for(destination.ipv6, &mac) : !next_hop_for(destination.ipv4, &mac)) return false;
  if (segment.size() > bytes - hardware::kEthernetHeaderSize - ip_header) return false;
  if (segment_frame_.size() != bytes) {  // (made when the first large segment goes out)
    release_view(allocator_, segment_frame_);
    segment_frame_ = allocate_view<uint8_t>(allocator_, bytes);
    if (segment_frame_.empty()) return false;
  }
  const IpAddress source = source_for_ip(destination);
  if (source.is_unspecified() || source.v6 != destination.v6) return false;
  ByteWriter out{segment_frame_};
  mac.write(out);
  nic_.mac_address().write(out);
  if (destination.v6) {
    out.be16(kEtherTypeIpv6);
    out.be32(0x60000000u);
    out.be16(static_cast<uint16_t>(segment.size()));
    out.u8(kProtocolTcp);
    out.u8(64);
    source.ipv6.write(out);
    destination.ipv6.write(out);
  } else {
    out.be16(kEtherTypeIpv4);
    out.u8(0x45);
    out.u8(0);
    out.be16(static_cast<uint16_t>(kIpv4HeaderSize + segment.size()));
    out.be16(ip_id_);
    ip_id_ = static_cast<uint16_t>(ip_id_ + (segment.size() - 20 + mss - 1) / mss);  // (each frame takes the next identification)
    out.be16(0x4000);
    out.u8(64);
    out.u8(kProtocolTcp);
    out.be16(0);
    source.ipv4.write(out);
    destination.ipv4.write(out);
    out.patch_be16(hardware::kEthernetHeaderSize + 10, checksum_finish(checksum_add(0, out.written().dropped(hardware::kEthernetHeaderSize))));
  }
  out.bytes(segment);
  if (!out.ok()) return false;
  const ByteView frame = segment_frame_.first(out.position());
  const uint32_t tcp = hardware::kEthernetHeaderSize + ip_header;
  const uint16_t seed = static_cast<uint16_t>(~checksum_finish(pseudo_header_sum(source, destination, kProtocolTcp, static_cast<uint32_t>(segment.size()))));
  frame[tcp + 16] = static_cast<uint8_t>(seed >> 8);
  frame[tcp + 17] = static_cast<uint8_t>(seed);
  return nic_.transmit_segmented(frame, hardware::TransmitChecksum{static_cast<uint16_t>(tcp), static_cast<uint16_t>(tcp + 16)}, mss);
}

IpAddress NetworkStack::source_for_ip(const IpAddress& destination) const {
  kernel::ScopedRecursiveMutex guard(const_cast<kernel::RecursiveMutex&>(mutex_));
  if (destination.v6) return IpAddress(source_for6(destination.ipv6));
  return IpAddress(source_for(destination.ipv4));
}

bool NetworkStack::send_ip_to(const IpAddress& destination, uint8_t protocol, ConstByteView payload) {
  if (!destination.v6) return send_ip(destination.ipv4, protocol, payload);
  kernel::ScopedRecursiveMutex guard(mutex_);
  const Ipv6Address source = source_for6(destination.ipv6);
  if (source.is_unspecified()) return false;
  return send_ip6(source, destination.ipv6, protocol, payload, 64);
}

void NetworkStack::send_arp(uint16_t operation, const hardware::MacAddress& target_mac, Ipv4Address target_ip) {
  send_arp_from(operation, config_.address, target_mac, target_ip);
}

void NetworkStack::send_arp_from(uint16_t operation, Ipv4Address sender_ip, const hardware::MacAddress& target_mac, Ipv4Address target_ip) {
  uint8_t arp[kArpPacketSize];
  ByteWriter out{ByteView(arp)};
  out.be16(1);  // Ethernet
  out.be16(kEtherTypeIpv4);
  out.u8(6);
  out.u8(4);
  out.be16(operation);
  nic_.mac_address().write(out);
  sender_ip.write(out);
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
  if (!config_.configured() && !destination.is_loopback()) return false;  // (the loopback works before an address is configured)
  return send_ip_from(source_for(destination), destination, protocol, payload, false);
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
  const ByteView writable = ByteView(packet_storage).first(out.position());
  const ConstByteView packet = writable;

  // TCP and UDP come without their checksum: the pseudo-header sum goes into the field, and the rest of the sum is added by the interface
  // (on the way to the wire, when it offloads checksums) or right here (for everything else).
  hardware::TransmitChecksum checksum;
  if (protocol == kProtocolTcp || protocol == kProtocolUdp) {
    const uint16_t field = static_cast<uint16_t>(kIpv4HeaderSize + (protocol == kProtocolTcp ? 16 : 6));
    if (packet.size() < field + 2u) return false;
    const uint32_t seed = pseudo_header_sum(source, destination, protocol, static_cast<uint16_t>(payload.size()));
    const uint16_t folded = static_cast<uint16_t>(~checksum_finish(seed));  // (finish complements: undo that, keep the fold)
    writable[field] = static_cast<uint8_t>(folded >> 8);
    writable[field + 1] = static_cast<uint8_t>(folded);
    checksum = hardware::TransmitChecksum{static_cast<uint16_t>(kIpv4HeaderSize), field};
  }
  const auto finish_here = [&] {
    if (!hardware::finish_transmit_checksum(writable, checksum)) return false;
    checksum = hardware::TransmitChecksum{};
    return true;
  };

  if (destination.is_loopback() || (config_.configured() && destination == config_.address)) {  // to this machine: not to the wire
    if (looped_.count() >= kMaxLooped || !finish_here()) return false;
    LoopedPacket p;
    p.length = static_cast<uint16_t>(packet.size());
    if (!ByteView(p.ip_packet).copy_from(packet)) return false;
    return looped_.enqueue(p);
  }

  const bool broadcast = link_broadcast || destination.is_limited_broadcast() ||
                         (config_.configured() && destination == Ipv4Address::subnet_broadcast(config_.address, config_.netmask));
  if (broadcast) return send_frame(hardware::MacAddress{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}, kEtherTypeIpv4, packet, checksum);
  if (destination.is_multicast()) {  // the group's Ethernet address: 01:00:5e and the low 23 bits of the group
    return send_frame(hardware::MacAddress{{0x01, 0x00, 0x5E, static_cast<uint8_t>(destination.octets[1] & 0x7F), destination.octets[2], destination.octets[3]}}, kEtherTypeIpv4, packet, checksum);
  }

  // On our subnet we talk to the destination directly, otherwise to the gateway.
  Ipv4Address next_hop = destination;
  if (!config_.configured() || !destination.in_subnet(config_.address, config_.netmask)) {
    if (config_.gateway.is_unspecified()) return false;
    next_hop = config_.gateway;
  }

  ArpEntry* entry = arp_.find(next_hop.value());
  if (entry && entry->expires_ms > now_ms()) return send_frame(entry->mac, kEtherTypeIpv4, packet, checksum);

  // Unknown (or stale) next hop: park the packet (complete) and ask who has it.
  if (pending_.count() >= kMaxPending || !finish_here()) return false;
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

namespace network {

// ------------------------------------------------------------------ link-local addresses (RFC 3927)

Ipv4Address NetworkStack::link_local_candidate(const hardware::MacAddress& mac, uint32_t attempt) {
  uint32_t hash = 2166136261u;  // FNV-1a over the MAC address and the attempt
  for (const uint8_t byte : mac.bytes) hash = (hash ^ byte) * 16777619u;
  for (uint32_t i = 0; i < 4; ++i) hash = (hash ^ ((attempt >> (8 * i)) & 0xFF)) * 16777619u;
  return Ipv4Address::make(169, 254, static_cast<uint8_t>(1 + (hash >> 8) % 254), static_cast<uint8_t>(hash >> 20));
}

void NetworkStack::service_link_local() {
  if (!link_local_enabled_) return;
  const uint64_t now = now_ms();
  if (link_local_state_ == LinkLocalState::Off) {
    if (config_.configured() || dhcp_->state() != DhcpState::Failed || (follow_link_ && !link_was_up_)) return;
    link_local_address_ = link_local_candidate(nic_.mac_address(), link_local_attempt_);
    link_local_state_ = LinkLocalState::Probing;
    link_local_step_ = 0;
    link_local_next_ms_ = now;
    return;
  }
  if (link_local_state_ == LinkLocalState::Bound && dhcp_->state() == DhcpState::Failed && now >= link_local_dhcp_retry_ms_) {
    link_local_dhcp_retry_ms_ = now + kLinkLocalDhcpRetryMs;  // a server may have appeared
    dhcp_->start();
  }
  if (now < link_local_next_ms_) return;
  const uint32_t jitter = (nic_.mac_address().bytes[5] + link_local_step_ * 37u) % 1000;  // spreads two hosts that start together
  switch (link_local_state_) {
    case LinkLocalState::Probing:
      if (link_local_step_ < kLinkLocalProbes) {
        send_arp_from(kArpRequest, kAnyAddress, hardware::MacAddress{}, link_local_address_);  // "does anyone have this address?"
        ++link_local_step_;
        link_local_next_ms_ = now + (link_local_step_ == kLinkLocalProbes ? kLinkLocalQuietMs : kLinkLocalProbeMs + jitter);
      } else {
        config_ = IpConfiguration{link_local_address_, Ipv4Address::make(255, 255, 0, 0), kAnyAddress, kAnyAddress};
        link_local_state_ = LinkLocalState::Announcing;
        link_local_step_ = 0;
        link_local_next_ms_ = now;
      }
      break;
    case LinkLocalState::Announcing:
      if (link_local_step_ < kLinkLocalAnnouncements) {
        send_arp_from(kArpRequest, link_local_address_, hardware::MacAddress{}, link_local_address_);  // "this address is mine"
        ++link_local_step_;
        link_local_next_ms_ = now + kLinkLocalAnnounceMs;
      } else {
        link_local_state_ = LinkLocalState::Bound;
        link_local_dhcp_retry_ms_ = now + kLinkLocalDhcpRetryMs;
      }
      break;
    default:
      break;
  }
}

// Picks another address after a conflict; after ten of them waits a minute between tries.
void NetworkStack::link_local_conflict() {
  if (link_local_state_ == LinkLocalState::Announcing || link_local_state_ == LinkLocalState::Bound) config_ = IpConfiguration{};
  ++link_local_attempt_;
  link_local_address_ = link_local_candidate(nic_.mac_address(), link_local_attempt_);
  link_local_state_ = LinkLocalState::Probing;
  link_local_step_ = 0;
  link_local_next_ms_ = now_ms() + (link_local_attempt_ % kLinkLocalMaxConflicts == 0 ? kLinkLocalRateLimitMs : 0);
}

// An ARP packet from another host that claims, or asks for, the address we are probing or hold.
void NetworkStack::check_link_local_claim(uint16_t operation, const hardware::MacAddress& sender_mac, Ipv4Address sender_ip, Ipv4Address target_ip) {
  if (link_local_state_ == LinkLocalState::Off || sender_mac == nic_.mac_address()) return;
  const bool claims = sender_ip == link_local_address_;                                                  // it uses the address
  const bool probes = sender_ip.is_unspecified() && target_ip == link_local_address_ && operation == kArpRequest;  // it is about to
  if (!claims && !probes) return;
  if (link_local_state_ == LinkLocalState::Probing || link_local_state_ == LinkLocalState::Announcing) {
    link_local_conflict();
    return;
  }
  // Bound. A probe is answered by defending; a claim is defended once, and the second one in ten seconds gives the address up.
  const uint64_t now = now_ms();
  if (claims && link_local_defended_ms_ != 0 && now - link_local_defended_ms_ < kLinkLocalDefendMs) {
    link_local_conflict();
    return;
  }
  if (claims) link_local_defended_ms_ = now;
  send_arp_from(kArpRequest, link_local_address_, hardware::MacAddress{}, link_local_address_);
}

}  // namespace network
