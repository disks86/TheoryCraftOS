#pragma once
#include "Network/Dhcp/IDhcpClient.hpp"
#include "Network/Dns/IDnsResolver.hpp"
#include "Network/NetworkTypes.hpp"
#include "Network/Tcp/ITcpSocket.hpp"
#include "Network/Udp/IUdpSocket.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

// IPv4 networking over one network interface: Ethernet framing, ARP, IPv4, ICMP
// echo, UDP, TCP, a DHCP client and a DNS resolver. Single-threaded and polled: call poll()
// regularly (from one core) - frames are received, timers run and sockets make
// progress only then. All the blocking helpers below simply call poll() in a loop.
//
// Not supported: IPv6, IP fragmentation (larger packets are dropped or refused),
// IP options, TCP congestion control, selective acknowledgements, window scaling,
// DNS over anything but A records.
class INetworkStack {
 public:
  // Processes received frames and timers. Cheap when nothing is happening.
  virtual void poll() = 0;
  // Monotonic milliseconds from the clock the stack runs on.
  virtual uint64_t now_ms() const = 0;

  virtual IpConfiguration configuration() const = 0;
  // Sets the configuration by hand (and stops DHCP if it was running).
  virtual void configure(const IpConfiguration& configuration) = 0;
  // Starts obtaining a configuration from a DHCP server in the background.
  virtual void start_dhcp() = 0;
  virtual const IDhcpClient& dhcp() const = 0;
  // Polls until an address is configured or `timeout_ms` passes.
  virtual bool wait_for_address(uint32_t timeout_ms) = 0;

  // Looks up a host name (or accepts a dotted-quad address) and waits up to
  // `timeout_ms` for the answer. For lookups that do not block, use dns().
  virtual bool resolve(const char* name, uint32_t timeout_ms, Ipv4Address* address) = 0;
  virtual IDnsResolver& dns() = 0;

  // Sends an ICMP echo request ("ping") and waits for the reply. On success
  // stores the round-trip time in *rtt_ms. Returns false on timeout.
  virtual bool ping(Ipv4Address address, uint32_t timeout_ms, uint32_t* rtt_ms) = 0;

  // A UDP socket bound to `port` (0 picks a free one), or null if the port is taken.
  virtual SharedPointer<IUdpSocket> udp_open(uint16_t port) = 0;
  // A new, unconnected TCP socket.
  virtual SharedPointer<ITcpSocket> tcp_open() = 0;

 protected:
  INetworkStack() = default;
  ~INetworkStack() = default;
};

}  // namespace network
