#pragma once
#include "Network/NetworkTypes.hpp"

namespace network {

enum class DhcpState : uint8_t {
  Stopped,     // not running
  Selecting,   // DISCOVER sent, waiting for offers
  Requesting,  // REQUEST sent for an offer, waiting for the ACK
  Bound,       // has a lease
  Renewing,    // lease half over: asking the server to extend it
  Failed,      // gave up (no server answered)
};

const char* dhcp_state_name(DhcpState state);

// A DHCP client (RFC 2131): obtains an IP address, netmask, gateway and DNS
// server from a DHCP server on the local network, and keeps the lease alive.
// Created and driven by the network stack; applications use it through
// INetworkStack::start_dhcp().
class IDhcpClient {
 public:
  virtual DhcpState state() const = 0;
  // The configuration of the current lease (meaningful while Bound or Renewing).
  virtual IpConfiguration lease() const = 0;
  virtual uint32_t lease_seconds() const = 0;
  virtual Ipv4Address server() const = 0;

 protected:
  IDhcpClient() = default;
  ~IDhcpClient() = default;
};

}  // namespace network
