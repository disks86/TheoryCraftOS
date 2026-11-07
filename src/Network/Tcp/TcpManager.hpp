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
#include "ObjectContainer/Dictionary.hpp"

namespace network {

// Owns every TCP socket (connections, listeners, connections in teardown) and
// routes arriving segments to them by port and peer.
//
// Threads. Several threads hand segments in at once (one per receive queue of the
// interface, plus the stack's control poll for the loopback), and applications use
// their sockets meanwhile. A segment finds its socket in one of two hash tables read
// with RCU (RcuSocketTable: no lock, no write) - connections by their four-tuple,
// listeners by their port - and is handed to it with nothing of the manager held.
// A socket's identity is published there when it changes (claim_connection,
// claim_listener, release), so a lookup never reads a socket's own fields. Changes,
// and the list of every socket the timers walk, take table_mutex_. Lock order: a
// socket's lock may be held when the table's is taken (a listen() claims its port),
// never the other way round; a listener's lock may be held when one of its new
// connections' is taken, never the other way round.
class TcpManager {
 public:
  TcpManager(IIpLayer& ip, allocators::IAllocator& allocator)
      : ip_(ip), allocator_(allocator), connections_(allocator), listeners_(allocator), sockets_(allocator), port_uses_(allocator) {}

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

  // --- used by TcpSocket (with its own lock held): what it is, published in the tables
  // A free local port for a connection to `remote`:`remote_port`, taken for `socket` at once (0: none free).
  uint16_t claim_connection(TcpSocket* socket, const IpAddress& remote, uint16_t remote_port);
  // `socket` listens on `port` for `families`; false if another listener has the port for one of them.
  bool claim_listener(TcpSocket* socket, uint16_t port, IpFamilies families);
  // `socket` is closed: no segment finds it any more, and its port is free.
  void release(TcpSocket* socket);
  // The manager's reference to `socket` (null once it was forgotten).
  SharedPointer<TcpSocket> reference(TcpSocket* socket) const;

  // Answers a segment for which there is no connection with a reset.
  void send_reset(const IpAddress& to, uint16_t to_port, uint16_t from_port, const TcpSegment& cause);

  uint32_t connection_count() const { return connections_.count(); }   // (for tests)

 private:
  static constexpr uint32_t kConnectionBuckets = 1024, kListenerBuckets = 64;
  struct Snapshot {   // a socket the timers look at, and how many references the manager itself holds to it
    SharedPointer<TcpSocket> socket;
    uint32_t held = 0;
  };
  static uint32_t hash_of(TcpSocket::Table table, const SocketKey& key);   // (the bucket hash of an entry of `table`)
  bool port_in_use(uint16_t port) const;
  uint16_t allocate_port();
  void use_port(uint16_t port);
  void unuse_port(uint16_t port);
  bool publish(const SharedPointer<TcpSocket>& socket, TcpSocket::Table table, const SocketKey& key);
  void unpublish(TcpSocket* socket);
  SharedPointer<TcpSocket> shared(TcpSocket* socket) const;
  bool snapshot(List<Snapshot>* out) const;
  void spawn(TcpSocket& listener, const IpAddress& remote_address, uint16_t remote_port, uint16_t local_port, const TcpSegment& syn);

  IIpLayer& ip_;
  allocators::IAllocator& allocator_;
  RcuSocketTable<TcpSocket, kConnectionBuckets> connections_;
  RcuSocketTable<TcpSocket, kListenerBuckets> listeners_;
  mutable kernel::Mutex table_mutex_;   // guards the tables' changes and everything below
  Dictionary<uint64_t, SharedPointer<TcpSocket>> sockets_;   // every socket, by its address (for the timers and to forget the finished ones)
  Dictionary<uint32_t, uint32_t> port_uses_;   // local port -> listeners and connections on it
  uint16_t next_port_ = 49152;
  uint32_t iss_counter_ = 0;
};

}  // namespace network
