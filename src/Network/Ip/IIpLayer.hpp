#pragma once
#include "Network/NetworkTypes.hpp"
#include "hardware/NetworkInterface/NetworkTypes.hpp"

namespace network {

// What the transport protocols (UDP, TCP, DHCP) need from the IP layer below
// them: a way to send an IP packet, the local address, and a clock.
class IIpLayer {
 public:
  // The address packets are sent from (0.0.0.0 until the stack is configured).
  virtual Ipv4Address local_address() const = 0;
  virtual uint32_t max_payload() const = 0;  // largest IP payload that fits one frame
  virtual uint64_t now_ms() const = 0;       // monotonic milliseconds

  // Wraps `payload` (UDP/TCP/ICMP bytes) in an IP header and sends it to
  // `destination`, via the gateway when it is off-link. False if the packet
  // cannot be sent now (not configured, too large, link-layer trouble); a packet
  // that is waiting for an ARP answer counts as sent.
  virtual bool send_ip(Ipv4Address destination, uint8_t protocol, const uint8_t* payload, uint32_t length) = 0;
  // The same with an explicit source address (0.0.0.0 is allowed), optionally as
  // a link-layer broadcast. DHCP needs this before it has an address.
  virtual bool send_ip_from(Ipv4Address source, Ipv4Address destination, uint8_t protocol, const uint8_t* payload,
                            uint32_t length, bool link_broadcast) = 0;

 protected:
  IIpLayer() = default;
  ~IIpLayer() = default;
};

}  // namespace network
