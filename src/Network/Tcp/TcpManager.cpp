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

#include "Network/Tcp/TcpManager.hpp"
#include "Network/Checksum.hpp"

namespace network {

namespace {
uint64_t id_of(const TcpSocket* socket) { return reinterpret_cast<uint64_t>(socket); }   // (the registry's key: the socket's address, never dereferenced)
}  // namespace

uint32_t TcpManager::hash_of(TcpSocket::Table table, const SocketKey& key) {
  return table == TcpSocket::Table::Listeners ? hash_port(key.local_port) : hash_connection(key.local_port, key.remote_port, key.remote);
}

SharedPointer<TcpSocket> TcpManager::create() {
  SharedPointer<TcpSocket> socket = SharedPointer<TcpSocket>::make(allocator_, ip_, *this, allocator_);
  if (!socket) return SharedPointer<TcpSocket>();
  kernel::ScopedMutex guard(table_mutex_);
  if (!sockets_.put(id_of(socket.get()), socket)) return SharedPointer<TcpSocket>();
  return socket;
}

SharedPointer<TcpSocket> TcpManager::shared(TcpSocket* socket) const {
  const SharedPointer<TcpSocket>* found = sockets_.find(id_of(socket));
  return found ? *found : SharedPointer<TcpSocket>();
}

SharedPointer<TcpSocket> TcpManager::reference(TcpSocket* socket) const {
  kernel::ScopedMutex guard(table_mutex_);
  return shared(socket);
}

bool TcpManager::port_in_use(uint16_t port) const { return port_uses_.contains(port); }

void TcpManager::use_port(uint16_t port) {
  uint32_t* uses = port_uses_.find(port);
  if (uses) ++*uses;
  else port_uses_.put(port, 1);
}

void TcpManager::unuse_port(uint16_t port) {
  uint32_t* uses = port_uses_.find(port);
  if (uses && --*uses == 0) port_uses_.remove(port);
}

uint16_t TcpManager::allocate_port() {
  for (uint32_t tries = 0; tries < 16384; ++tries) {
    const uint16_t candidate = next_port_;
    next_port_ = next_port_ == 65535 ? 49152 : next_port_ + 1;
    if (!port_in_use(candidate)) return candidate;
  }
  return 0;
}

// (table_mutex_ held) Puts `socket` in `table` under `key`, taking it out of wherever it was.
bool TcpManager::publish(const SharedPointer<TcpSocket>& socket, TcpSocket::Table table, const SocketKey& key) {
  unpublish(socket.get());
  const bool added = table == TcpSocket::Table::Listeners ? listeners_.insert(hash_of(table, key), key, socket)
                                                          : connections_.insert(hash_of(table, key), key, socket);
  if (!added) return false;
  socket->table_ = table;
  socket->table_key_ = key;
  use_port(key.local_port);
  return true;
}

void TcpManager::unpublish(TcpSocket* socket) {
  if (socket->table_ == TcpSocket::Table::None) return;
  const uint32_t h = hash_of(socket->table_, socket->table_key_);
  if (socket->table_ == TcpSocket::Table::Listeners) listeners_.remove(h, socket);
  else connections_.remove(h, socket);
  unuse_port(socket->table_key_.local_port);
  socket->table_ = TcpSocket::Table::None;
}

uint16_t TcpManager::claim_connection(TcpSocket* socket, const IpAddress& remote, uint16_t remote_port) {
  kernel::ScopedMutex guard(table_mutex_);
  const SharedPointer<TcpSocket> self = shared(socket);
  const uint16_t port = self ? allocate_port() : 0;
  if (port == 0) return 0;
  SocketKey key;
  key.local_port = port;
  key.remote = remote;
  key.remote_port = remote_port;
  return publish(self, TcpSocket::Table::Connections, key) ? port : 0;
}

bool TcpManager::claim_listener(TcpSocket* socket, uint16_t port, IpFamilies families) {
  kernel::ScopedMutex guard(table_mutex_);
  const SharedPointer<TcpSocket> self = shared(socket);
  if (!self) return false;
  const bool taken = listeners_.any(hash_port(port), [&](const SocketKey& k, const TcpSocket&) { return k.local_port == port && families_overlap(k.families, families); });
  if (taken) return false;
  SocketKey key;
  key.local_port = port;
  key.families = families;
  return publish(self, TcpSocket::Table::Listeners, key);
}

void TcpManager::release(TcpSocket* socket) {
  kernel::ScopedMutex guard(table_mutex_);
  unpublish(socket);
}

bool TcpManager::snapshot(List<Snapshot>* out) const {
  kernel::ScopedMutex guard(table_mutex_);
  if (!out->reserve(sockets_.count())) return false;
  for (const KeyValue<uint64_t, SharedPointer<TcpSocket>>& e : sockets_) {
    Snapshot s;
    s.socket = e.value;
    s.held = 1 + (e.value->table_ != TcpSocket::Table::None ? 1 : 0);   // (the registry, and a table's node)
    out->push_back(s);
  }
  return true;
}

bool TcpManager::pacing() const {
  kernel::ScopedMutex guard(table_mutex_);
  for (const KeyValue<uint64_t, SharedPointer<TcpSocket>>& e : sockets_)
    if (e.value->pacing_paused()) return true;   // (an atomic flag: read without the socket's lock)
  return false;
}

uint32_t TcpManager::next_initial_sequence() {
  // RFC 793 suggests a counter ticking every 4 microseconds; milliseconds times
  // 250 is the same thing, plus a bump so two connections in one tick differ.
  const uint32_t bump = __atomic_add_fetch(&iss_counter_, 64000u, __ATOMIC_RELAXED);
  return static_cast<uint32_t>(ip_.now_ms() * 250) + bump;
}

void TcpManager::detach_children(TcpSocket* listener) {
  List<Snapshot> all(allocator_);
  snapshot(&all);
  for (Snapshot& s : all) s.socket->leave_listener(listener);   // (each takes its own lock: this listener's is held, the order allows it)
}

// A connection for a SYN that arrived at `listener` (whose lock the caller holds): put in the connections table, where its next segment finds
// it, then set up under its own lock.
void TcpManager::spawn(TcpSocket& listener, const IpAddress& remote_address, uint16_t remote_port, uint16_t local_port, const TcpSegment& syn) {
  SharedPointer<TcpSocket> child = SharedPointer<TcpSocket>::make(allocator_, ip_, *this, allocator_);
  if (!child) return;
  SocketKey key;
  key.local_port = local_port;
  key.remote = remote_address;
  key.remote_port = remote_port;
  {
    kernel::ScopedMutex guard(table_mutex_);
    if (!sockets_.put(id_of(child.get()), child)) return;
    if (!publish(child, TcpSocket::Table::Connections, key)) return;
  }
  if (!child->accept_syn(&listener, remote_address, remote_port, local_port, syn, next_initial_sequence())) release(child.get());  // (out of memory)
}

void TcpManager::send_reset(const IpAddress& to, uint16_t to_port, uint16_t from_port, const TcpSegment& cause) {
  uint8_t storage[20];
  ByteWriter out{ByteView(storage)};
  uint32_t sequence = 0, acknowledgement = 0;
  uint8_t flags = tcp_flags::kRst;
  if (cause.flags & tcp_flags::kAck) {
    sequence = cause.acknowledgement;  // RFC 793: the reset takes its number from the ACK it answers
  } else {
    acknowledgement = cause.sequence + static_cast<uint32_t>(cause.data.size()) +
                      ((cause.flags & tcp_flags::kSyn) ? 1 : 0) + ((cause.flags & tcp_flags::kFin) ? 1 : 0);
    flags |= tcp_flags::kAck;
  }
  out.be16(from_port);
  out.be16(to_port);
  out.be32(sequence);
  out.be32(acknowledgement);
  out.u8(5 << 4);
  out.u8(flags);
  out.be16(0);  // window
  out.be16(0);  // checksum: the IP layer fills it in
  out.be16(0);  // urgent pointer
  if (!out.ok()) return;
  ip_.send_ip_to(to, kProtocolTcp, out.written());
}

void TcpManager::handle(const IpAddress& source, const IpAddress& destination, ConstByteView segment, bool checksum_verified, uint8_t ecn) {
  ByteReader in(segment);
  const uint16_t source_port = in.be16();
  const uint16_t destination_port = in.be16();
  TcpSegment s{};
  s.sequence = in.be32();
  s.acknowledgement = in.be32();
  const uint8_t offset_and_reserved = in.u8();
  s.flags = in.u8();
  s.window = in.be16();
  in.skip(4);  // checksum (verified below) and urgent pointer
  if (!in.ok()) return;  // shorter than a TCP header
  const uint32_t header = (offset_and_reserved >> 4) * 4u;
  if (header < 20 || header > segment.size()) return;
  const uint32_t sum = checksum_verified ? 0 : checksum_add(pseudo_header_sum(source, destination, kProtocolTcp, static_cast<uint32_t>(segment.size())), segment);
  if (!checksum_verified && checksum_finish(sum) != 0) return;  // corrupted in transit

  // Options: the maximum segment size, SACK-permitted and SACK blocks. Each one is
  // bounds-checked against the option area; a malformed list just ends the scan.
  ByteReader options(segment.subview(20, header - 20));
  while (options.ok() && !options.at_end()) {
    const uint8_t kind = options.u8();
    if (kind == 0) break;
    if (kind == 1) continue;
    const uint8_t length = options.u8();
    if (!options.ok() || length < 2) break;
    ConstByteView body = options.take(length - 2u);
    if (!options.ok()) break;
    if (kind == 2 && length == 4) {
      ByteReader value(body);
      s.mss = value.be16();
    } else if (kind == 4 && length == 2) {
      s.sack_permitted = true;
    } else if (kind == 5 && length >= 10 && (length - 2) % 8 == 0 && (length - 2) / 8 <= 4) {
      ByteReader value(body);
      s.sack_blocks = static_cast<uint8_t>((length - 2) / 8);
      for (uint32_t b = 0; b < s.sack_blocks; ++b) {
        s.sack[b][0] = value.be32();
        s.sack[b][1] = value.be32();
      }
      if (!value.ok()) s.sack_blocks = 0;
    }
  }
  s.data = segment.dropped(header);
  s.congestion_experienced = ecn == kEcnCe;

  // An existing connection first, then a listener: found in the RCU tables (no lock), handled with nothing of the manager held.
  SharedPointer<TcpSocket> connection = connections_.find(hash_connection(destination_port, source_port, source), [&](const SocketKey& k, const TcpSocket&) {
    return k.local_port == destination_port && k.remote_port == source_port && k.remote == source;
  });
  SharedPointer<TcpSocket> listener;
  if (!connection)
    listener = listeners_.find(hash_port(destination_port), [&](const SocketKey& k, const TcpSocket&) { return k.local_port == destination_port && families_take(k.families, source.v6); });
  if (connection) {
    connection->on_segment(s, connection);
    return;
  }
  if (listener) {
    if (s.flags & tcp_flags::kRst) return;
    if ((s.flags & tcp_flags::kSyn) && !(s.flags & tcp_flags::kAck)) {
      // Under the listener's lock: it cannot close meanwhile and miss the new connection when it lets its connections go. A full backlog drops
      // the SYN; the client's retransmission tries again later.
      TcpSocket::Owner hold(*listener);
      if (listener->state() == TcpState::Listen && listener->embryo_count() + listener->accepted_waiting() < TcpSocket::kBacklog)
        spawn(*listener, source, source_port, destination_port, s);
      return;
    }
    send_reset(source, source_port, destination_port, s);
    return;
  }
  if (!(s.flags & tcp_flags::kRst)) send_reset(source, source_port, destination_port, s);  // nobody home
}

void TcpManager::poll() {
  const uint64_t now = ip_.now_ms();
  List<Snapshot> all(allocator_);
  snapshot(&all);
  for (Snapshot& s : all) s.socket->on_timer(now);
  // A connection nobody but the manager holds any more (this pass's copy aside) is closed - the application let go of it. (A node a table retired
  // but has not freed yet still counts: such a socket waits for the next pass.)
  for (Snapshot& s : all)
    if (s.socket.use_count() == s.held + 1) s.socket->close_if_abandoned();
  all.clear();

  // Forget sockets that are in no table and that nobody else holds (use_count 1: only the registry).
  kernel::ScopedMutex guard(table_mutex_);
  List<uint64_t> done(allocator_);
  for (const KeyValue<uint64_t, SharedPointer<TcpSocket>>& e : sockets_)
    if (e.value->table_ == TcpSocket::Table::None && e.value.use_count() == 1) done.push_back(e.key);
  for (uint64_t id : done) sockets_.remove(id);
}

}  // namespace network
