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
#include "Kernel/Sync.hpp"
#include "Network/NetworkTypes.hpp"
#include "Hardware/NetworkInterface/NetworkTypes.hpp"

namespace network {

// What the transport protocols (UDP, TCP, DHCP) need from the IP layer below
// them: a way to send an IP packet, the local address, and a clock.
class IIpLayer {
 public:
  // The address packets are sent from (0.0.0.0 until the stack is configured).
  virtual Ipv4Address local_address() const = 0;
  // The address a packet to `destination` is sent from: the loopback address it
  // is sent to (so that the answer comes back from where it was sent), else the
  // local address. Transport checksums are computed over this address.
  virtual Ipv4Address source_for(Ipv4Address destination) const { return destination.is_loopback() ? destination : local_address(); }
  virtual uint32_t max_payload() const = 0;  // largest IP payload that fits one frame
  virtual uint64_t now_ms() const = 0;       // monotonic milliseconds
  // The stack's lock. The stack, its sockets and its resolver are used by many
  // threads; every public entry point takes this (it is recursive).
  virtual kernel::RecursiveMutex& mutex() = 0;

  // Wraps `payload` (UDP/TCP/ICMP bytes) in an IP header and sends it to
  // `destination`, via the gateway when it is off-link; a packet to 127.0.0.0/8 or
  // to the local address is delivered to this stack itself. False if the packet
  // cannot be sent now (not configured, too large, link-layer trouble); a packet
  // that is waiting for an ARP answer counts as sent. A TCP or UDP payload is passed with its checksum field zero: the
  // IP layer fills it in, in software or through the network interface's checksum offload.
  virtual bool send_ip(Ipv4Address destination, uint8_t protocol, ConstByteView payload) = 0;
  // The same with an explicit source address (0.0.0.0 is allowed), optionally as
  // a link-layer broadcast. DHCP needs this before it has an address.
  virtual bool send_ip_from(Ipv4Address source, Ipv4Address destination, uint8_t protocol, ConstByteView payload,
                            bool link_broadcast) = 0;

  // TCP segmentation offload. The largest TCP segment (header and data) that may go to `destination` in one send_tcp_segmented(): more
  // than one packet's worth when the interface cuts segments into frames itself and the way there is known; 0 otherwise (send packet by
  // packet, with send_ip()).
  virtual uint32_t max_segmented_payload(Ipv4Address destination) const {
    (void)destination;
    return 0;
  }
  // Sends a TCP segment (checksum field zero, as for send_ip()) that the interface cuts into frames of at most `mss` bytes of data. False if
  // it cannot now (the segment is then as good as lost: TCP retransmits).
  virtual bool send_tcp_segmented(Ipv4Address destination, ConstByteView segment, uint16_t mss) {
    (void)destination;
    (void)segment;
    (void)mss;
    return false;
  }

 protected:
  IIpLayer() = default;
  ~IIpLayer() = default;
};

// Holds a stack's lock for a scope.
class StackLock {
 public:
  explicit StackLock(IIpLayer& ip) : guard_(ip.mutex()) {}

 private:
  kernel::ScopedRecursiveMutex guard_;
};

}  // namespace network
