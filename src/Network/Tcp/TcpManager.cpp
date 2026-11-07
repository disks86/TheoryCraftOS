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

SharedPointer<TcpSocket> TcpManager::create() {
  SharedPointer<TcpSocket> socket = SharedPointer<TcpSocket>::make(allocator_, ip_, *this, allocator_);
  if (!socket) return SharedPointer<TcpSocket>();
  Entry entry;
  entry.socket = socket;
  kernel::ScopedMutex guard(table_mutex_);
  if (!sockets_.push_back(entry)) return SharedPointer<TcpSocket>();
  return socket;
}

TcpManager::Entry* TcpManager::entry_of(TcpSocket* socket) {
  for (Entry& e : sockets_)
    if (e.socket.get() == socket) return &e;
  return nullptr;
}

bool TcpManager::port_in_use(uint16_t port) const {
  for (const Entry& e : sockets_)
    if (e.kind != Kind::None && e.local_port == port) return true;
  return false;
}

uint16_t TcpManager::allocate_port() {
  for (uint32_t tries = 0; tries < 16384; ++tries) {
    const uint16_t candidate = next_port_;
    next_port_ = next_port_ == 65535 ? 49152 : next_port_ + 1;
    if (!port_in_use(candidate)) return candidate;
  }
  return 0;
}

uint16_t TcpManager::claim_connection(TcpSocket* socket, const IpAddress& remote, uint16_t remote_port) {
  kernel::ScopedMutex guard(table_mutex_);
  Entry* e = entry_of(socket);
  const uint16_t port = e ? allocate_port() : 0;
  if (port == 0) return 0;
  e->kind = Kind::Connection;
  e->local_port = port;
  e->remote = remote;
  e->remote_port = remote_port;
  return port;
}

bool TcpManager::claim_listener(TcpSocket* socket, uint16_t port, IpFamilies families) {
  kernel::ScopedMutex guard(table_mutex_);
  Entry* mine = entry_of(socket);
  if (!mine) return false;
  for (const Entry& e : sockets_)
    if (e.kind == Kind::Listener && e.local_port == port && families_overlap(e.families, families)) return false;
  mine->kind = Kind::Listener;
  mine->local_port = port;
  mine->families = families;
  return true;
}

void TcpManager::release(TcpSocket* socket) {
  kernel::ScopedMutex guard(table_mutex_);
  if (Entry* e = entry_of(socket)) e->kind = Kind::None;
}

bool TcpManager::snapshot(List<SharedPointer<TcpSocket>>* out) const {
  kernel::ScopedMutex guard(table_mutex_);
  if (!out->reserve(sockets_.count())) return false;
  for (const Entry& e : sockets_) out->push_back(e.socket);
  return true;
}

bool TcpManager::pacing() const {
  kernel::ScopedMutex guard(table_mutex_);
  for (const Entry& e : sockets_)
    if (e.socket->pacing_paused()) return true;   // (an atomic flag: read without the socket's lock)
  return false;
}

uint32_t TcpManager::next_initial_sequence() {
  // RFC 793 suggests a counter ticking every 4 microseconds; milliseconds times
  // 250 is the same thing, plus a bump so two connections in one tick differ.
  const uint32_t bump = __atomic_add_fetch(&iss_counter_, 64000u, __ATOMIC_RELAXED);
  return static_cast<uint32_t>(ip_.now_ms() * 250) + bump;
}

void TcpManager::detach_children(TcpSocket* listener) {
  List<SharedPointer<TcpSocket>> all(allocator_);
  snapshot(&all);
  for (SharedPointer<TcpSocket>& s : all) s->leave_listener(listener);   // (each takes its own lock: this listener's is held, the order allows it)
}

// A connection for a SYN that arrived at `listener` (whose lock the caller holds): set up under its own lock, then put in the table where its
// next segment finds it.
void TcpManager::spawn(TcpSocket& listener, const IpAddress& remote_address, uint16_t remote_port, uint16_t local_port, const TcpSegment& syn) {
  SharedPointer<TcpSocket> child = SharedPointer<TcpSocket>::make(allocator_, ip_, *this, allocator_);
  if (!child) return;
  Entry entry;
  entry.socket = child;
  entry.kind = Kind::Connection;
  entry.local_port = local_port;
  entry.remote = remote_address;
  entry.remote_port = remote_port;
  {
    kernel::ScopedMutex guard(table_mutex_);
    if (!sockets_.push_back(entry)) return;
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

  // An existing connection first, then a listener: found in the table (by the published keys), handled with the table let go.
  SharedPointer<TcpSocket> connection, listener;
  {
    kernel::ScopedMutex guard(table_mutex_);
    for (const Entry& e : sockets_) {
      if (e.kind == Kind::Connection && e.local_port == destination_port && e.remote_port == source_port && e.remote == source) {
        connection = e.socket;
        break;
      }
      if (!listener && e.kind == Kind::Listener && e.local_port == destination_port && families_take(e.families, source.v6)) listener = e.socket;
    }
  }
  if (connection) {
    connection->on_segment(s, connection);
    return;
  }
  if (listener) {
    if (s.flags & tcp_flags::kRst) return;
    if ((s.flags & tcp_flags::kSyn) && !(s.flags & tcp_flags::kAck)) {
      // Under the listener's lock: it cannot close meanwhile and miss the new connection when it lets its connections go. A full backlog drops
      // the SYN; the client's retransmission tries again later.
      kernel::ScopedRecursiveMutex hold(listener->lock());
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
  List<SharedPointer<TcpSocket>> all(allocator_);
  snapshot(&all);
  for (SharedPointer<TcpSocket>& s : all) s->on_timer(now);
  // A connection nobody but the table holds any more (this pass's copy aside: use_count 2) is closed - the application let go of it.
  for (SharedPointer<TcpSocket>& s : all)
    if (s.use_count() == 2) s->close_if_abandoned();
  all.clear();

  // Forget sockets that are closed and that nobody holds (use_count 1 = only the table).
  kernel::ScopedMutex guard(table_mutex_);
  for (uint64_t i = sockets_.count(); i > 0; --i) {
    const Entry& e = sockets_[i - 1];
    if (e.kind == Kind::None && e.socket.use_count() == 1) sockets_.remove_at(i - 1);
  }
}

}  // namespace network
