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
#include "Network/Tcp/TcpSocket.hpp"

namespace network {

// Owns every TCP socket (connections, listeners, connections in teardown) and
// routes arriving segments to them by port and peer.
//
// Threads. Several threads hand segments in at once (one per receive queue of the
// interface, plus the stack's control poll for the loopback), and applications use
// their sockets meanwhile. The table of sockets has its own lock (table_mutex_), taken
// only for short lookups and changes and never while calling into a socket; each
// socket has its own lock (TcpSocket::lock_). A socket's identity - its ports, its
// peer, whether it listens - is published into its table entry (the "key") whenever it
// changes, so a lookup reads only the table, never a socket's fields. Lock order:
// a socket's lock may be held when the table's is taken (a listen() checks the port),
// never the other way round; a listener's lock may be held when one of its new
// connections' is taken, never the other way round.
class TcpManager {
 public:
  TcpManager(IIpLayer& ip, allocators::IAllocator& allocator) : ip_(ip), allocator_(allocator), sockets_(allocator) {}

  // A new, unconnected socket.
  SharedPointer<TcpSocket> create();

  // A TCP segment arrived in an IP packet (`segment` = TCP header and data).
  // `checksum_verified`: the network interface checked the checksum already (and found it right).
  void handle(const IpAddress& source, const IpAddress& destination, ConstByteView segment, bool checksum_verified = false, uint8_t ecn = kEcnNotEct);
  // Runs timers, finishes accepted handshakes and forgets sockets that are done.
  void poll();

  // Some connection has data waiting only for its pacing rate: the stack should be polled again within a millisecond or so.
  bool pacing() const;
  uint32_t next_initial_sequence();
  // Disowns (and resets) connections spawned by a listener that is going away.
  void detach_children(TcpSocket* listener);

  // --- used by TcpSocket (with its own lock held): what it is, published in its entry
  // A free local port for a connection to `remote`:`remote_port`, taken for `socket` at once (0: none free).
  uint16_t claim_connection(TcpSocket* socket, const IpAddress& remote, uint16_t remote_port);
  // `socket` listens on `port` for `families`; false if another listener has the port for one of them.
  bool claim_listener(TcpSocket* socket, uint16_t port, IpFamilies families);
  // `socket` is closed: no segment finds it any more, and its port is free.
  void release(TcpSocket* socket);

  // Answers a segment for which there is no connection with a reset.
  void send_reset(const IpAddress& to, uint16_t to_port, uint16_t from_port, const TcpSegment& cause);

 private:
  enum class Kind : uint8_t { None, Listener, Connection };
  struct Entry {
    SharedPointer<TcpSocket> socket;
    Kind kind = Kind::None;
    uint16_t local_port = 0, remote_port = 0;
    IpAddress remote;
    IpFamilies families = IpFamilies::Both;
  };
  Entry* entry_of(TcpSocket* socket);
  bool port_in_use(uint16_t port) const;
  uint16_t allocate_port();
  // Copies of every socket (for work done without the table's lock).
  bool snapshot(List<SharedPointer<TcpSocket>>* out) const;
  void spawn(TcpSocket& listener, const IpAddress& remote_address, uint16_t remote_port, uint16_t local_port, const TcpSegment& syn);

  IIpLayer& ip_;
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex table_mutex_;   // guards everything below
  List<Entry> sockets_;
  uint16_t next_port_ = 49152;
  uint32_t iss_counter_ = 0;
};

}  // namespace network
