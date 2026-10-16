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
