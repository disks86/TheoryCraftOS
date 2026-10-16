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
  virtual uint32_t max_payload() const = 0;  // largest IP payload that fits one frame
  virtual uint64_t now_ms() const = 0;       // monotonic milliseconds
  // The stack's lock. The stack, its sockets and its resolver are used by many
  // threads; every public entry point takes this (it is recursive).
  virtual kernel::RecursiveMutex& mutex() = 0;

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

// Holds a stack's lock for a scope.
class StackLock {
 public:
  explicit StackLock(IIpLayer& ip) : guard_(ip.mutex()) {}

 private:
  kernel::ScopedRecursiveMutex guard_;
};

}  // namespace network
