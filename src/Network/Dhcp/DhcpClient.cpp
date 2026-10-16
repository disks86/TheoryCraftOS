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
uint32_t DhcpClient::build(uint8_t* m, uint8_t type, bool broadcast_reply) const {
  for (uint32_t i = 0; i < 576; ++i) m[i] = 0;
  m[0] = kBootRequest;
  m[1] = 1;  // hardware type: Ethernet
  m[2] = 6;  // hardware address length
  store32(m + 4, xid_);
  // Ask the server to answer by broadcast while we cannot receive unicast to an address we do not have yet.
  store16(m + 10, broadcast_reply ? 0x8000 : 0);
  for (int i = 0; i < 6; ++i) m[28 + i] = mac_.bytes[i];
  store32(m + 236, kMagicCookie);
  uint32_t o = kFixedSize;
  m[o++] = kOptionMessageType; m[o++] = 1; m[o++] = type;
  m[o++] = kOptionClientId; m[o++] = 7; m[o++] = 1;
  for (int i = 0; i < 6; ++i) m[o++] = mac_.bytes[i];
  m[o++] = kOptionParameterRequest; m[o++] = 5;
  m[o++] = kOptionSubnetMask; m[o++] = kOptionRouter; m[o++] = kOptionDns; m[o++] = kOptionLeaseTime;
  m[o++] = kOptionRenewalTime;
  return o;
}

void DhcpClient::send_discover() {
  uint8_t message[576];
  uint32_t o = build(message, kMessageDiscover, true);
  message[o++] = kOptionEnd;
  socket_->send_from(kAnyAddress, kBroadcastAddress, kServerPort, message, o < 300 ? 300 : o, true);
  schedule_retry();
}

void DhcpClient::send_request(bool renewing) {
  uint8_t message[576];
  uint32_t o = build(message, kMessageRequest, !renewing);
  if (renewing) {
    lease_.address.write(message + 12);  // ciaddr: the address we are renewing
  } else {
    message[o++] = kOptionRequestedAddress; message[o++] = 4;
    offered_.address.write(message + o); o += 4;
    message[o++] = kOptionServerId; message[o++] = 4;
    server_.write(message + o); o += 4;
  }
  message[o++] = kOptionEnd;
  const uint32_t length = o < 300 ? 300 : o;
  if (renewing) socket_->send_from(lease_.address, server_, kServerPort, message, length, false);
  else socket_->send_from(kAnyAddress, kBroadcastAddress, kServerPort, message, length, true);
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

void DhcpClient::handle(const uint8_t* m, uint32_t length) {
  if (length < kFixedSize + 4 || m[0] != kBootReply || load32(m + 4) != xid_ || load32(m + 236) != kMagicCookie) return;
  for (int i = 0; i < 6; ++i)
    if (m[28 + i] != mac_.bytes[i]) return;  // meant for another client

  uint8_t type = 0;
  Ipv4Address mask = kAnyAddress, router = kAnyAddress, dns = kAnyAddress, server = kAnyAddress;
  uint32_t lease = 0, renewal = 0;
  for (uint32_t i = kFixedSize; i < length;) {
    const uint8_t code = m[i];
    if (code == kOptionEnd) break;
    if (code == kOptionPad) {
      ++i;
      continue;
    }
    if (i + 1 >= length || i + 2 + m[i + 1] > length) break;
    const uint8_t size = m[i + 1];
    const uint8_t* value = m + i + 2;
    switch (code) {
      case kOptionMessageType:  if (size >= 1) type = value[0]; break;
      case kOptionSubnetMask:   if (size >= 4) mask = Ipv4Address::read(value); break;
      case kOptionRouter:       if (size >= 4) router = Ipv4Address::read(value); break;  // the first one
      case kOptionDns:          if (size >= 4) dns = Ipv4Address::read(value); break;
      case kOptionServerId:     if (size >= 4) server = Ipv4Address::read(value); break;
      case kOptionLeaseTime:    if (size >= 4) lease = load32(value); break;
      case kOptionRenewalTime:  if (size >= 4) renewal = load32(value); break;
      default: break;
    }
    i += 2 + size;
  }

  const Ipv4Address yiaddr = Ipv4Address::read(m + 16);
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
  while (socket_->receive_from(message, sizeof(message), nullptr, nullptr, &length))
    if (length <= sizeof(message)) handle(message, length);

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
