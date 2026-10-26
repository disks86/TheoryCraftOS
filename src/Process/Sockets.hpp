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

// IPv4 sockets for user-mode processes (Windows' ws2_32.dll, behind this OS's own NT-ish services: real Windows does not reach sockets through NT services at all,
// but through a device driver (afd.sys) with its own ioctl protocol, which is not worth reproducing here). A socket is one TCP or UDP endpoint of the system's one
// set of network stacks (network::system_network_stacks()); which interface a socket ends up on is decided the way IServerNetwork already picks one: by what can
// reach the destination, falling back to the first interface for a socket that only ever listens or sends unconnected datagrams.
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
  Created create(SocketProto proto);
  void add_ref(uint32_t id);
  void release(uint32_t id);

  // All addresses and ports below are in network byte order (as a Winsock sockaddr_in carries them) except where a parameter name says `_host`; the one place
  // byte order is translated is here, since network::'s own API takes host-order ports.
  uint32_t bind(uint32_t id, uint32_t address, uint16_t port);
  // Each of these attempts the operation exactly once and says in *would_block whether it was not ready (would have had to wait); looping until it is (a blocking
  // socket) or returning WSAEWOULDBLOCK at once (a non-blocking one, see is_non_blocking()) is the NT service's job, not this one's.
  // Connects (TCP: begins the handshake, which completes later - *would_block is set the first time regardless, and the caller watches is_connected()/failed()
  // itself; UDP: just remembers the peer for send()/recv() without an address, as a real connected UDP socket does - no packet is exchanged to establish it).
  uint32_t connect(uint32_t id, uint32_t address, uint16_t port, bool* would_block);
  uint32_t listen(uint32_t id, uint32_t backlog);
  // *out_id is a new socket (already add_ref()'d once, for the caller's handle) on success.
  uint32_t accept(uint32_t id, uint32_t* out_id, uint32_t* peer_address, uint16_t* peer_port, bool* would_block);
  // `address`/`port` non-null: send to them (sendto, UDP only). Null: send to the connected peer (send; UDP must already be "connected").
  uint32_t send(uint32_t id, ConstByteView data, const uint32_t* address, const uint16_t* port, uint32_t* sent, bool* would_block);
  // `from_address`/`from_port` are filled in when given (recvfrom); for TCP they are the (already known) peer.
  uint32_t receive(uint32_t id, ByteView buffer, uint32_t* got, uint32_t* from_address, uint16_t* from_port, bool* would_block, bool* peer_closed);
  uint32_t shutdown(uint32_t id, uint32_t how);  // 0 receive, 1 send, 2 both
  bool set_non_blocking(uint32_t id, bool on);
  bool is_non_blocking(uint32_t id) const;
  bool local_name(uint32_t id, uint32_t* address, uint16_t* port);
  bool peer_name(uint32_t id, uint32_t* address, uint16_t* port);
  // How many bytes (TCP) or whole datagrams (UDP) are waiting; for select()'s read set and FIONREAD.
  uint32_t readable(uint32_t id) const;
  bool is_tcp(uint32_t id) const;
  bool exists(uint32_t id) const { return at(id) != nullptr; }
  // True if accept()/receive() would not block right now (connected or has data, for a TCP listener: has a waiting connection); for select()'s read set.
  bool ready_to_read(uint32_t id) const;
  // True if send() would not block (always, in this polled model, once connected: the 16 KiB send buffer rarely fills against a wait()); for select()'s write set.
  bool ready_to_write(uint32_t id) const;
  bool has_error(uint32_t id) const;  // for select()'s exception set

 private:
  struct Socket {
    bool used = false;
    uint32_t references = 0;
    SocketProto proto = SocketProto::Tcp;
    bool non_blocking = false;
    bool listening = false;
    bool bound = false;
    uint16_t bound_port = 0;
    network::INetworkStack* stack = nullptr;       // the interface it ended up on (chosen at connect()/first send, or bind()/listen())
    SharedPointer<network::ITcpSocket> tcp;
    SharedPointer<network::IUdpSocket> udp;
    bool udp_connected = false;  // connect() was called on a UDP socket: send()/recv() go to/from this peer without needing an address each time
    network::Ipv4Address peer_address;
    uint16_t peer_port = 0;
    // A connection ITcpSocket::accept() already gave up (it cannot be "peeked" without taking it): ready_to_read() caches one here so that checking readiness
    // (select(), or a non-blocking accept() that finds nothing yet) never drops a connection that arrived; the real accept() takes this one first if there is one.
    mutable SharedPointer<network::ITcpSocket> pending_accept;
  };
  Socket* at(uint32_t id) { return id >= 1 && id <= kCapacity && sockets_[id - 1].used ? &sockets_[id - 1] : nullptr; }
  const Socket* at(uint32_t id) const { return id >= 1 && id <= kCapacity && sockets_[id - 1].used ? &sockets_[id - 1] : nullptr; }
  // Picks (and opens, if not already) the interface a socket not yet bound to one should use for `destination` (0: the first interface that has one).
  network::INetworkStack* stack_for(Socket& s, network::Ipv4Address destination);

  network::IStackProvider& stacks_;
  Socket sockets_[kCapacity];
};

SocketManager* system_sockets();
void set_system_sockets(SocketManager* manager);

}  // namespace process
