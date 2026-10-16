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
  if (!socket || !sockets_.push_back(socket)) return SharedPointer<TcpSocket>();
  return socket;
}

SharedPointer<TcpSocket> TcpManager::spawn(TcpSocket* listener, Ipv4Address remote_address, uint16_t remote_port,
                                           uint16_t local_port, const TcpSegment& syn) {
  SharedPointer<TcpSocket> child = create();
  if (!child) return SharedPointer<TcpSocket>();
  child->accept_syn(listener, remote_address, remote_port, local_port, syn, next_initial_sequence());
  if (child->is_dead()) return SharedPointer<TcpSocket>();  // out of memory: reaped later
  listener->add_embryo();
  return child;
}

bool TcpManager::listening_on(uint16_t port) const {
  for (const SharedPointer<TcpSocket>& s : sockets_)
    if (s->state() == TcpState::Listen && s->local_port() == port) return true;
  return false;
}

bool TcpManager::port_in_use(uint16_t port) const {
  for (const SharedPointer<TcpSocket>& s : sockets_)
    if (s->state() != TcpState::Closed && s->local_port() == port) return true;
  return false;
}

uint16_t TcpManager::allocate_port() {
  for (uint32_t tries = 0; tries < 16384; ++tries) {
    const uint16_t candidate = next_port_;
    next_port_ = next_port_ == 65535 ? 49152 : next_port_ + 1;
    if (!port_in_use(candidate)) return candidate;
  }
  return next_port_;
}

uint32_t TcpManager::next_initial_sequence() {
  // RFC 793 suggests a counter ticking every 4 microseconds; milliseconds times
  // 250 is the same thing, plus a bump so two connections in one tick differ.
  iss_counter_ += 64000;
  return static_cast<uint32_t>(ip_.now_ms() * 250) + iss_counter_;
}

void TcpManager::detach_children(TcpSocket* listener) {
  for (SharedPointer<TcpSocket>& s : sockets_) {
    if (s->listener() != listener) continue;
    s->detach_from_listener();
    s->abort();
  }
}

void TcpManager::send_reset(Ipv4Address to, uint16_t to_port, uint16_t from_port, const TcpSegment& cause) {
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
  out.be16(0);  // checksum, filled in below
  out.be16(0);  // urgent pointer
  if (!out.ok()) return;
  uint32_t sum = pseudo_header_sum(ip_.local_address(), to, kProtocolTcp, 20);
  sum = checksum_add(sum, out.written());
  out.patch_be16(16, checksum_finish(sum));
  ip_.send_ip(to, kProtocolTcp, out.written());
}

void TcpManager::handle(Ipv4Address source, Ipv4Address destination, ConstByteView segment) {
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
  const uint32_t sum = checksum_add(pseudo_header_sum(source, destination, kProtocolTcp, static_cast<uint16_t>(segment.size())),
                                    segment);
  if (checksum_finish(sum) != 0) return;  // corrupted in transit

  // Options: only the maximum segment size matters. Each one is bounds-checked
  // against the option area; a malformed list just ends the scan.
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
    }
  }
  s.data = segment.dropped(header);

  // An existing connection first, then a listener.
  for (SharedPointer<TcpSocket>& socket : sockets_) {
    if (!socket->matches(source, source_port, destination_port)) continue;
    socket->on_segment(s);
    if (socket->take_established_event()) {
      TcpSocket* listener = socket->listener();
      if (listener) {
        listener->child_finished_handshake();
        listener->enqueue_accepted(socket);
        socket->detach_from_listener();
      }
    }
    return;
  }
  for (SharedPointer<TcpSocket>& socket : sockets_) {
    if (socket->state() != TcpState::Listen || socket->local_port() != destination_port) continue;
    if (s.flags & tcp_flags::kRst) return;
    if ((s.flags & tcp_flags::kSyn) && !(s.flags & tcp_flags::kAck)) {
      // A full backlog drops the SYN; the client's retransmission tries again later.
      if (socket->embryo_count() + socket->accepted_waiting() < TcpSocket::kBacklog)
        spawn(socket.get(), source, source_port, destination_port, s);
      return;
    }
    send_reset(source, source_port, destination_port, s);
    return;
  }
  if (!(s.flags & tcp_flags::kRst)) send_reset(source, source_port, destination_port, s);  // nobody home
}

void TcpManager::poll() {
  const uint64_t now = ip_.now_ms();
  for (uint64_t i = 0; i < sockets_.count(); ++i) sockets_[i]->on_timer(now);

  // Clean up. A connection nobody holds any more (use_count 1 = only this list)
  // is closed; one that is closed and unreferenced is forgotten.
  for (uint64_t i = sockets_.count(); i > 0; --i) {
    SharedPointer<TcpSocket>& s = sockets_[i - 1];
    if (s.use_count() != 1) continue;
    switch (s->state()) {
      case TcpState::Closed:
        sockets_.remove_at(i - 1);
        break;
      case TcpState::Listen:
      case TcpState::SynSent:
      case TcpState::Established:
      case TcpState::CloseWait:
        if (!s->listener()) s->close();  // the application let go of a live socket
        break;
      default:
        break;
    }
  }
}

}  // namespace network
