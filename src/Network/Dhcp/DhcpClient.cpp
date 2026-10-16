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

#include "Network/Dhcp/DhcpClient.hpp"

namespace network {

const char* dhcp_state_name(DhcpState state) {
  switch (state) {
    case DhcpState::Stopped:    return "stopped";
    case DhcpState::Selecting:  return "selecting";
    case DhcpState::Requesting: return "requesting";
    case DhcpState::Bound:      return "bound";
    case DhcpState::Renewing:   return "renewing";
    case DhcpState::Failed:     return "failed";
  }
  return "?";
}

namespace {
constexpr uint8_t kBootRequest = 1, kBootReply = 2;
constexpr uint32_t kMagicCookie = 0x63825363;
constexpr uint32_t kFixedSize = 240;  // BOOTP header plus the magic cookie

constexpr uint8_t kMessageDiscover = 1, kMessageOffer = 2, kMessageRequest = 3, kMessageAck = 5, kMessageNak = 6;
enum Option : uint8_t {
  kOptionSubnetMask = 1, kOptionRouter = 3, kOptionDns = 6, kOptionRequestedAddress = 50, kOptionLeaseTime = 51,
  kOptionMessageType = 53, kOptionServerId = 54, kOptionParameterRequest = 55, kOptionRenewalTime = 58,
  kOptionClientId = 61, kOptionEnd = 255, kOptionPad = 0,
};

constexpr uint32_t kMaxAttempts = 6;
constexpr uint32_t kMinimumLeaseSeconds = 60;
}  // namespace

void DhcpClient::start() {
  if (state_ != DhcpState::Stopped && state_ != DhcpState::Failed) return;
  begin_discovery();
}

void DhcpClient::stop() {
  if (state_ == DhcpState::Bound || state_ == DhcpState::Renewing) listener_.on_lease_lost();
  state_ = DhcpState::Stopped;
  lease_ = IpConfiguration{};
}

void DhcpClient::begin_discovery() {
  // A transaction ID that differs between boots and attempts.
  xid_ = static_cast<uint32_t>(ip_.now_ms() * 2654435761u) ^ (uint32_t{mac_.bytes[4]} << 8) ^ mac_.bytes[5] ^ (xid_ + 1);
  state_ = DhcpState::Selecting;
  attempts_ = 0;
  send_discover();
}

// Fills in the fixed part and returns the offset where options start.
void DhcpClient::build(ByteWriter& out, uint8_t type, bool broadcast_reply) const {
  out.u8(kBootRequest);
  out.u8(1);  // hardware type: Ethernet
  out.u8(6);  // hardware address length
  out.u8(0);  // hops
  out.be32(xid_);
  out.be16(0);  // seconds elapsed
  // Ask the server to answer by broadcast while we cannot receive unicast to an address we do not have yet.
  out.be16(broadcast_reply ? 0x8000 : 0);
  out.zeros(16);  // ciaddr, yiaddr, siaddr, giaddr
  mac_.write(out);
  out.zeros(10 + 64 + 128);  // the rest of chaddr, then sname and file
  out.be32(kMagicCookie);
  out.u8(kOptionMessageType); out.u8(1); out.u8(type);
  out.u8(kOptionClientId); out.u8(7); out.u8(1);
  mac_.write(out);
  out.u8(kOptionParameterRequest); out.u8(5);
  out.u8(kOptionSubnetMask); out.u8(kOptionRouter); out.u8(kOptionDns); out.u8(kOptionLeaseTime);
  out.u8(kOptionRenewalTime);
}

void DhcpClient::send_discover() {
  uint8_t message[576];
  ByteWriter out{ByteView(message)};
  build(out, kMessageDiscover, true);
  out.u8(kOptionEnd);
  if (out.position() < 300) out.zeros(300 - out.position());  // BOOTP servers expect at least 300 bytes
  if (out.ok()) socket_->send_from(kAnyAddress, kBroadcastAddress, kServerPort, out.written(), true);
  schedule_retry();
}

void DhcpClient::send_request(bool renewing) {
  uint8_t message[576];
  ByteWriter out{ByteView(message)};
  build(out, kMessageRequest, !renewing);
  if (renewing) {
    out.patch_bytes(12, ConstByteView(lease_.address.octets));  // ciaddr: the address we are renewing
  } else {
    out.u8(kOptionRequestedAddress); out.u8(4);
    offered_.address.write(out);
    out.u8(kOptionServerId); out.u8(4);
    server_.write(out);
  }
  out.u8(kOptionEnd);
  if (out.position() < 300) out.zeros(300 - out.position());
  if (out.ok()) {
    if (renewing) socket_->send_from(lease_.address, server_, kServerPort, out.written(), false);
    else socket_->send_from(kAnyAddress, kBroadcastAddress, kServerPort, out.written(), true);
  }
  schedule_retry();
}

void DhcpClient::schedule_retry() {
  // 2 s, 4 s, 8 s ... while discovering/requesting; every 10 s while renewing.
  const uint32_t delay = (state_ == DhcpState::Renewing) ? 10000 : (2000u << (attempts_ < 4 ? attempts_ : 4));
  next_action_ms_ = ip_.now_ms() + delay;
}

void DhcpClient::bind(const IpConfiguration& configuration, uint32_t lease_seconds, uint32_t t1_seconds) {
  const bool changed = !(lease_.address == configuration.address && lease_.gateway == configuration.gateway &&
                         lease_.netmask == configuration.netmask && lease_.dns == configuration.dns);
  lease_ = configuration;
  lease_seconds_ = lease_seconds < kMinimumLeaseSeconds ? kMinimumLeaseSeconds : lease_seconds;
  bound_at_ms_ = ip_.now_ms();
  const uint32_t t1 = (t1_seconds && t1_seconds < lease_seconds_) ? t1_seconds : lease_seconds_ / 2;
  t1_ms_ = uint64_t{t1} * 1000;
  state_ = DhcpState::Bound;
  attempts_ = renew_attempts_ = 0;
  if (changed) listener_.on_lease(lease_);
}

void DhcpClient::handle(ConstByteView message) {
  if (message.size() < kFixedSize + 4) return;
  ByteReader in(message);
  const uint8_t op = in.u8();
  in.skip(3);  // hardware type and length, hops
  const uint32_t transaction = in.be32();
  in.skip(8);  // seconds, flags, ciaddr
  const Ipv4Address yiaddr = Ipv4Address::read(in);
  in.skip(8);  // siaddr, giaddr
  const hardware::MacAddress client = hardware::MacAddress::read(in);
  in.skip(10 + 64 + 128);  // rest of chaddr, sname, file
  const uint32_t cookie = in.be32();
  if (!in.ok() || op != kBootReply || transaction != xid_ || cookie != kMagicCookie) return;
  if (!(client == mac_)) return;  // meant for another client

  uint8_t type = 0;
  Ipv4Address mask = kAnyAddress, router = kAnyAddress, dns = kAnyAddress, server = kAnyAddress;
  uint32_t lease = 0, renewal = 0;
  ByteReader options(in.rest());
  while (options.ok() && !options.at_end()) {
    const uint8_t code = options.u8();
    if (code == kOptionEnd) break;
    if (code == kOptionPad) continue;
    const uint8_t size = options.u8();
    const ConstByteView value = options.take(size);
    if (!options.ok()) break;  // an option that runs past the end of the packet ends the scan
    ByteReader field(value);
    switch (code) {
      case kOptionMessageType:  if (size >= 1) type = field.u8(); break;
      case kOptionSubnetMask:   if (size >= 4) mask = Ipv4Address::read(field); break;
      case kOptionRouter:       if (size >= 4) router = Ipv4Address::read(field); break;  // the first one
      case kOptionDns:          if (size >= 4) dns = Ipv4Address::read(field); break;
      case kOptionServerId:     if (size >= 4) server = Ipv4Address::read(field); break;
      case kOptionLeaseTime:    if (size >= 4) lease = field.be32(); break;
      case kOptionRenewalTime:  if (size >= 4) renewal = field.be32(); break;
      default: break;
    }
  }

  if (state_ == DhcpState::Selecting && type == kMessageOffer) {
    offered_.address = yiaddr;
    offered_.netmask = mask;
    offered_.gateway = router;
    offered_.dns = dns;
    server_ = server;
    offered_lease_seconds_ = lease;
    offered_t1_seconds_ = renewal;
    state_ = DhcpState::Requesting;
    attempts_ = 0;
    send_request(false);
  } else if ((state_ == DhcpState::Requesting || state_ == DhcpState::Renewing) && type == kMessageAck) {
    IpConfiguration granted = (state_ == DhcpState::Renewing) ? lease_ : offered_;
    if (state_ == DhcpState::Requesting || !yiaddr.is_unspecified()) granted.address = yiaddr;
    if (!mask.is_unspecified()) granted.netmask = mask;
    if (!router.is_unspecified()) granted.gateway = router;
    if (!dns.is_unspecified()) granted.dns = dns;
    bind(granted, lease ? lease : offered_lease_seconds_, renewal ? renewal : offered_t1_seconds_);
  } else if (type == kMessageNak && (state_ == DhcpState::Requesting || state_ == DhcpState::Renewing)) {
    if (state_ == DhcpState::Renewing) listener_.on_lease_lost();
    lease_ = IpConfiguration{};
    begin_discovery();  // the server no longer honours this address: start over
  }
}

void DhcpClient::poll() {
  if (state_ == DhcpState::Stopped) return;
  uint8_t message[576];
  uint32_t length;
  while (socket_->receive_from(ByteView(message), nullptr, nullptr, &length))
    if (length <= sizeof(message)) handle(ConstByteView(message).first(length));

  const uint64_t now = ip_.now_ms();
  switch (state_) {
    case DhcpState::Selecting:
    case DhcpState::Requesting:
      if (now < next_action_ms_) break;
      if (++attempts_ >= kMaxAttempts) {
        state_ = DhcpState::Failed;
      } else if (state_ == DhcpState::Selecting) {
        send_discover();
      } else {
        send_request(false);
      }
      break;
    case DhcpState::Bound:
      if (now - bound_at_ms_ >= t1_ms_) {
        state_ = DhcpState::Renewing;
        send_request(true);
      }
      break;
    case DhcpState::Renewing:
      if (now - bound_at_ms_ >= uint64_t{lease_seconds_} * 1000) {  // lease expired: give the address up
        listener_.on_lease_lost();
        lease_ = IpConfiguration{};
        begin_discovery();
      } else if (now >= next_action_ms_) {
        ++renew_attempts_;
        send_request(true);
      }
      break;
    default:
      break;
  }
}

}  // namespace network
