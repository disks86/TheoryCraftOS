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
#include "Utility/StringView.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

enum class DnsStatus : uint8_t {
  Pending,   // waiting for the server
  Resolved,  // address() holds the answer
  NotFound,  // the name does not exist, or has no IPv4 address
  Failed,    // no answer (no server configured, timeout) or the server reported an error
};

const char* dns_status_name(DnsStatus status);

// One lookup in progress or finished.
class IDnsQuery {
 public:
  virtual DnsStatus status() const = 0;
  // The first IPv4 address of the name; valid when status() is Resolved.
  virtual Ipv4Address address() const = 0;
  // How many seconds the answer may be cached (as the server stated it).
  virtual uint32_t ttl_seconds() const = 0;

 protected:
  IDnsQuery() = default;
  ~IDnsQuery() = default;
};

// A DNS client (RFC 1035): turns host names into IPv4 addresses by asking the
// DNS server the network configuration names (usually learned through DHCP).
//
//   - Queries go out over UDP and are retried; a server that answers "truncated"
//     is asked again over TCP.
//   - CNAME chains in the answer are followed.
//   - Answers are checked against the question and the random query ID before
//     they are believed, and arrive on a random source port. (The randomness
//     comes from the clock and a counter: enough to stop accidents and blind
//     guesses by a casual attacker, not a cryptographic defence.)
//   - Results, including "no such name", are cached for as long as their TTL.
//   - Dotted-quad text ("10.0.2.2") is accepted and resolves to itself.
//
// Driven by the network stack's poll(). IPv6 (AAAA) records are not requested.
class IDnsResolver {
 public:
  // Starts looking up `name`, or returns a finished query at once if the answer
  // is cached or `name` is an address. Null if memory ran out or the name is
  // not a valid host name (longer than 253 characters, a label over 63, empty
  // labels, ...).
  virtual SharedPointer<IDnsQuery> query(StringView name) = 0;

  // Throws away every cached result.
  virtual void clear_cache() = 0;
  // Cached results currently held (including negative ones).
  virtual uint32_t cache_size() const = 0;

 protected:
  IDnsResolver() = default;
  ~IDnsResolver() = default;
};

// Parses "a.b.c.d" into an address. False for anything else.
bool parse_ipv4(StringView text, Ipv4Address* address);

}  // namespace network
