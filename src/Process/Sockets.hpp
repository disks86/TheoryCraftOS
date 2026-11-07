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
#include <stdint.h>
#include "Network/NetworkStackSet.hpp"
#include "Network/Tcp/ITcpSocket.hpp"
#include "Network/Udp/IUdpSocket.hpp"

// IPv4 and IPv6 sockets for user-mode processes (Windows' ws2_32.dll, behind this OS's own NT-ish services: real Windows does not reach sockets through NT services at
// all, but through a device driver (afd.sys) with its own ioctl protocol, which is not worth reproducing here). A socket is one TCP or UDP endpoint of the system's one
// set of network stacks (network::system_network_stacks()); which interface a socket ends up on is decided the way IServerNetwork already picks one: by what can
// reach the destination, falling back to the first interface for a socket that only ever listens or sends unconnected datagrams.
//
// A socket is created for one address family. An IPv6 one is IPv6-only (IPV6_V6ONLY, on by default as on Windows) until set_v6_only(false) makes it dual-stack: it
// then also reaches and hears IPv4 hosts, which its program sees as IPv4-mapped addresses (::ffff:a.b.c.d). Addresses in and out of the manager are as the program
// gives and sees them (mapped ones included); the manager turns them into what the network stack uses.
namespace process {

enum class SocketProto : uint8_t { Tcp, Udp };

class SocketManager {
 public:
  static constexpr uint32_t kCapacity = 256;

  // False (system_sockets() is then null) until some interface exists; see network::system_network_stacks().
  explicit SocketManager(network::IStackProvider& stacks) : stacks_(stacks) {}

  struct Created {
    uint32_t id = 0;     // 0 on failure
    uint32_t status = 0;  // an NTSTATUS
  };
  Created create(SocketProto proto, bool ipv6 = false);
  void add_ref(uint32_t id);
  void release(uint32_t id);

  // Ports are in host byte order; the NT services turn a sockaddr's bytes into an address and a port and back (see WindowsSockets.cpp).
  uint32_t bind(uint32_t id, const network::IpAddress& address, uint16_t port);
  // Each of these attempts the operation exactly once and says in *would_block whether it was not ready (would have had to wait); looping until it is (a blocking
  // socket) or returning WSAEWOULDBLOCK at once (a non-blocking one, see is_non_blocking()) is the NT service's job, not this one's.
  // Connects (TCP: begins the handshake, which completes later - *would_block is set the first time regardless, and the caller watches is_connected()/failed()
  // itself; UDP: just remembers the peer for send()/recv() without an address, as a real connected UDP socket does - no packet is exchanged to establish it).
  uint32_t connect(uint32_t id, const network::IpAddress& address, uint16_t port, bool* would_block);
  uint32_t listen(uint32_t id, uint32_t backlog);
  // *out_id is a new socket (already add_ref()'d once, for the caller's handle) on success.
  uint32_t accept(uint32_t id, uint32_t* out_id, network::IpAddress* peer_address, uint16_t* peer_port, bool* would_block);
  // `address`/`port` non-null: send to them (sendto, UDP only). Null: send to the connected peer (send; UDP must already be "connected").
  uint32_t send(uint32_t id, ConstByteView data, const network::IpAddress* address, const uint16_t* port, uint32_t* sent, bool* would_block);
  // `from_address`/`from_port` are filled in when given (recvfrom); for TCP they are the (already known) peer.
  uint32_t receive(uint32_t id, ByteView buffer, uint32_t* got, network::IpAddress* from_address, uint16_t* from_port, bool* would_block, bool* peer_closed);
  uint32_t shutdown(uint32_t id, uint32_t how);  // 0 receive, 1 send, 2 both
  bool set_non_blocking(uint32_t id, bool on);
  bool is_non_blocking(uint32_t id) const;
  // IPV6_V6ONLY. Only an IPv6 socket has it, and it can change only before bind/connect/listen/send fixes which versions the socket uses.
  uint32_t set_v6_only(uint32_t id, bool on);
  bool v6_only(uint32_t id) const;
  bool is_ipv6(uint32_t id) const;
  bool local_name(uint32_t id, network::IpAddress* address, uint16_t* port);
  bool peer_name(uint32_t id, network::IpAddress* address, uint16_t* port);
  // The IPv4 configuration of the `index`th network interface (for iphlpapi's adapter list); false past the last one.
  bool interface_configuration(uint32_t index, network::IpConfiguration* out) const;
  // Its IPv6 configuration (SLAAC); false past the last one.
  bool interface_configuration6(uint32_t index, network::Ipv6Configuration* out) const;
  // A DNS lookup (A, or AAAA when `ipv6`) on every interface that has a DNS server, all at once; start_lookup() begins it and lookup_result() is polled, like
  // the other would-block operations, until it says Found or NotFound.
  struct NameLookup {
    static constexpr uint32_t kMaxQueries = 4;
    SharedPointer<network::IDnsQuery> queries[kMaxQueries];
    uint32_t count = 0;
  };
  enum class LookupResult : uint8_t { Pending, Found, NotFound };
  bool start_lookup(StringView name, bool ipv6, NameLookup* lookup) const;   // false: not a name DNS can look up, or no interface has a DNS server
  LookupResult lookup_result(const NameLookup& lookup, network::IpAddress* address) const;
  // True if some interface has a global IPv6 address and a router (getaddrinfo asks for AAAA records only then, like Windows' AI_ADDRCONFIG default).
  bool has_ipv6_route() const;
  // How many bytes (TCP) or whole datagrams (UDP) are waiting; for select()'s read set and FIONREAD.
  uint32_t readable(uint32_t id) const;
  bool is_tcp(uint32_t id) const;
  bool exists(uint32_t id) const { return at(id) != nullptr; }
  // True if accept()/receive() would not block right now (connected or has data, for a TCP listener: has a waiting connection); for select()'s read set.
  bool ready_to_read(uint32_t id) const;
  // True if send() would not block (always, in this polled model, once connected: the 16 KiB send buffer rarely fills against a wait()); for select()'s write set.
  bool ready_to_write(uint32_t id) const;
  bool has_error(uint32_t id) const;  // for select()'s exception set
  // See IStackProvider: read progress() before an attempt that may block; wait_for_progress() returns when some stack did something (or the
  // timeout passed), false if the stacks cannot be waited on that way (then the caller sleeps).
  uint64_t progress() const { return stacks_.progress(); }
  bool wait_for_progress(uint64_t seen, uint64_t timeout_us) { return stacks_.wait_for_progress(seen, timeout_us); }

 private:
  struct Socket {
    bool used = false;
    uint32_t references = 0;
    SocketProto proto = SocketProto::Tcp;
    bool ipv6 = false;        // created AF_INET6
    bool v6_only = true;      // (an IPv6 socket's IPV6_V6ONLY)
    bool non_blocking = false;
    bool listening = false;
    bool bound = false;
    uint16_t bound_port = 0;
    network::INetworkStack* stack = nullptr;       // the interface it ended up on (chosen at connect()/first send, or bind()/listen())
    SharedPointer<network::ITcpSocket> tcp;
    SharedPointer<network::IUdpSocket> udp;
    bool udp_connected = false;  // connect() was called on a UDP socket: send()/recv() go to/from this peer without needing an address each time
    network::IpAddress peer_address;   // as the network stack has it (an IPv4 peer of a dual-stack socket is IPv4 here, mapped only when shown)
    uint16_t peer_port = 0;
    // A connection ITcpSocket::accept() already gave up (it cannot be "peeked" without taking it): ready_to_read() caches one here so that checking readiness
    // (select(), or a non-blocking accept() that finds nothing yet) never drops a connection that arrived; the real accept() takes this one first if there is one.
    mutable SharedPointer<network::ITcpSocket> pending_accept;
  };
  Socket* at(uint32_t id) { return id >= 1 && id <= kCapacity && sockets_[id - 1].used ? &sockets_[id - 1] : nullptr; }
  const Socket* at(uint32_t id) const { return id >= 1 && id <= kCapacity && sockets_[id - 1].used ? &sockets_[id - 1] : nullptr; }
  // Picks (and opens, if not already) the interface a socket not yet bound to one should use for `destination` (unspecified: the first interface that has an
  // address of the socket's family).
  network::INetworkStack* stack_for(Socket& s, const network::IpAddress& destination);
  // The program's address as the network stack has it: an IPv4-mapped address of a dual-stack socket becomes IPv4. False if the socket cannot use it (an IPv6
  // address on an IPv4 socket, or anything IPv4 on an IPv6-only one).
  static bool to_network(const Socket& s, const network::IpAddress& program, network::IpAddress* out);
  // The other way: an IPv4 address seen by an IPv6 socket is shown mapped.
  static network::IpAddress to_program(const Socket& s, const network::IpAddress& address);
  static network::IpFamilies families_of(const Socket& s);
  uint32_t open_udp(Socket& s, network::INetworkStack& stack, uint16_t port);

  network::IStackProvider& stacks_;
  Socket sockets_[kCapacity];
};

SocketManager* system_sockets();
void set_system_sockets(SocketManager* manager);

}  // namespace process
