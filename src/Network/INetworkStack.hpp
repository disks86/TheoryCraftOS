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

#pragma once
#include "Network/Dhcp/IDhcpClient.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/StringView.hpp"
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
  // The stack's lock (also IIpLayer::mutex()): everything here is safe to call from
  // any thread; take it yourself to make several calls atomic.
  virtual kernel::RecursiveMutex& mutex() = 0;
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
  virtual bool resolve(StringView name, uint32_t timeout_ms, Ipv4Address* address) = 0;
  virtual IDnsResolver& dns() = 0;

  // Sends an ICMP echo request ("ping") and waits for the reply. On success
  // stores the round-trip time in *rtt_ms. Returns false on timeout.
  virtual bool ping(Ipv4Address address, uint32_t timeout_ms, uint32_t* rtt_ms) = 0;

  // A UDP socket bound to `port` (0 picks a free one), or null if the port is taken.
  virtual SharedPointer<IUdpSocket> udp_open(uint16_t port) = 0;
  // A new, unconnected TCP socket.
  virtual SharedPointer<ITcpSocket> tcp_open() = 0;

  // Multicast reception (IGMPv2 on the link): after joining, datagrams addressed to `group` reach UDP sockets bound to their port, and
  // the interface is asked to deliver multicast frames. Up to 8 groups; joining twice counts once. Sending to a multicast address
  // needs no membership (the packet goes to the group's Ethernet address with TTL 255 for UDP). Stacks without multicast say no.
  virtual bool join_multicast(Ipv4Address group) {
    (void)group;
    return false;
  }
  virtual void leave_multicast(Ipv4Address group) { (void)group; }

 protected:
  INetworkStack() = default;
  ~INetworkStack() = default;
};

}  // namespace network
