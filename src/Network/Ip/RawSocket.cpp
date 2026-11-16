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
#include "Network/Ip/RawSocket.hpp"

#include "Network/Checksum.hpp"

namespace network {

namespace {
constexpr uint8_t kEchoRequest4 = 8, kEchoReply4 = 0, kEchoRequest6 = 128, kEchoReply6 = 129;
}

void RawSocket::set_blocked_types(ArrayView<const uint32_t> blocked) {
  for (uint32_t i = 0; i < kTypeWords; ++i) __atomic_store_n(&blocked_[i], i < blocked.size() ? blocked[i] : 0, __ATOMIC_RELEASE);
}

bool RawSocket::send_to_ip(const IpAddress& address, uint16_t, ConstByteView data) {
  if (closed() || address.v6 != v6_) return false;
  const uint8_t hops = __atomic_load_n(&hop_limit_, __ATOMIC_ACQUIRE);
  if (kind_ == RawKind::Echo) {      // an echo request, sent under this socket's identifier whatever the program wrote there
    uint8_t storage[kMaxIpPayload];
    if (data.size() < 8 || data.size() > ip_.max_payload_for(address) || data.size() > sizeof(storage)) return false;
    if (data[0] != (v6_ ? kEchoRequest6 : kEchoRequest4) || data[1] != 0) return false;
    const ByteView message = ByteView(storage).first(data.size());
    message.copy_from(data);
    message[2] = message[3] = 0;
    message[4] = static_cast<uint8_t>(identifier_ >> 8);
    message[5] = static_cast<uint8_t>(identifier_);
    if (!v6_) {      // (ICMPv6's checksum covers the addresses: the IP layer computes it)
      const uint16_t sum = checksum_finish(checksum_add(0, message));
      message[2] = static_cast<uint8_t>(sum >> 8);
      message[3] = static_cast<uint8_t>(sum);
    }
    return ip_.send_raw(address, protocol_, message, hops);
  }
  if (!v6_ && (protocol_ == kProtocolRaw || __atomic_load_n(&header_included_, __ATOMIC_ACQUIRE))) {
    // A whole IPv4 packet: where it goes, what it carries and how far it may go are read from its header; the header itself is written anew (the
    // source address, the identification, the lengths and the checksum are this stack's to say, and options are not carried).
    ByteReader in(data);
    const uint8_t version_and_length = in.u8();
    in.skip(7);
    const uint8_t time_to_live = in.u8(), protocol = in.u8();
    in.skip(6);
    const Ipv4Address destination = Ipv4Address::read(in);
    const uint32_t header = (version_and_length & 0x0Fu) * 4u;
    if (!in.ok() || (version_and_length >> 4) != 4 || header < kIpv4HeaderSize || header > data.size()) return false;
    return ip_.send_raw(destination.is_unspecified() ? address : IpAddress(destination), protocol, data.dropped(header), time_to_live != 0 ? time_to_live : hops);
  }
  if (protocol_ == kProtocolRaw) return false;
  return ip_.send_raw(address, protocol_, data, hops);
}

void RawSocket::deliver(bool v6, uint8_t protocol, const IpAddress& from, ConstByteView packet, uint32_t header) {
  if (closed() || v6 != v6_ || protocol != protocol_ || header > packet.size() || packet.size() > kMtu) return;
  const ConstByteView message = packet.dropped(header);
  ConstByteView taken = packet;
  if (kind_ == RawKind::Echo) {      // only the replies to what this socket sent
    if (message.size() < 8 || message[0] != (v6_ ? kEchoReply6 : kEchoReply4) || message[1] != 0) return;
    if ((static_cast<uint16_t>(message[4]) << 8 | message[5]) != identifier_) return;
    taken = message;
  } else if (protocol == kProtocolIcmp || protocol == kProtocolIcmpv6) {
    if (message.empty()) return;
    const uint8_t type = message[0];
    if (__atomic_load_n(&blocked_[type >> 5], __ATOMIC_ACQUIRE) & (uint32_t{1} << (type & 31))) return;
  }
  Packet p;      // (filled before the lock: only the enqueue's copy happens under it)
  p.from = from;
  p.length = static_cast<uint16_t>(taken.size());
  ByteView(p.data).first(taken.size()).copy_from(taken);
  kernel::ScopedMutex guard(queue_lock_);
  if (queue_.count() >= kQueueLimit) return;
  queue_.enqueue(p);
}

bool RawSocket::receive_from_ip(ByteView buffer, IpAddress* from_address, uint16_t* from_port, uint32_t* length) {
  Packet p;
  {
    kernel::ScopedMutex guard(queue_lock_);
    if (!queue_.dequeue(&p)) return false;
  }
  buffer.copy_from(ConstByteView(p.data).first(p.length).at_most(buffer.size()));      // as much as fits
  if (from_address) *from_address = p.from;
  if (from_port) *from_port = 0;
  if (length) *length = p.length;
  return true;
}

bool RawSocket::receive_from(ByteView buffer, Ipv4Address* from_address, uint16_t* from_port, uint32_t* length) {
  IpAddress from;
  if (!receive_from_ip(buffer, &from, from_port, length)) return false;
  if (from_address) *from_address = from.v6 ? kAnyAddress : from.ipv4;
  return true;
}

uint32_t RawSocket::pending() const {
  kernel::ScopedMutex guard(queue_lock_);
  return static_cast<uint32_t>(queue_.count());
}

void RawSocket::close() {
  __atomic_store_n(&closed_, true, __ATOMIC_RELEASE);
  kernel::ScopedMutex guard(queue_lock_);
  queue_.clear();
}

bool RawManager::identifier_in_use(bool v6, uint16_t identifier) const {
  for (const SharedPointer<RawSocket>& s : sockets_)
    if (!s->closed() && s->kind() == RawKind::Echo && s->v6() == v6 && s->local_port() == identifier) return true;
  return false;
}

SharedPointer<RawSocket> RawManager::open(bool v6, uint8_t protocol, RawKind kind, uint16_t identifier) {
  kernel::ScopedMutex guard(mutex_);
  if (kind == RawKind::Echo) {
    if (identifier == 0) {
      for (uint32_t tries = 0; tries < 65536 && identifier == 0; ++tries) {
        const uint16_t candidate = next_identifier_;
        next_identifier_ = next_identifier_ == 65535 ? 1 : static_cast<uint16_t>(next_identifier_ + 1);
        if (!identifier_in_use(v6, candidate)) identifier = candidate;
      }
      if (identifier == 0) return SharedPointer<RawSocket>();
    } else if (identifier_in_use(v6, identifier)) {
      return SharedPointer<RawSocket>();
    }
  }
  SharedPointer<RawSocket> socket = SharedPointer<RawSocket>::make(allocator_, ip_, allocator_, v6, protocol, kind, identifier);
  if (!socket || !sockets_.push_back(socket)) return SharedPointer<RawSocket>();
  __atomic_store_n(&open_, static_cast<uint32_t>(sockets_.count()), __ATOMIC_RELEASE);
  return socket;
}

void RawManager::deliver(bool v6, uint8_t protocol, const IpAddress& from, ConstByteView packet, uint32_t header) {
  if (__atomic_load_n(&open_, __ATOMIC_ACQUIRE) == 0) return;
  kernel::ScopedMutex guard(mutex_);
  for (const SharedPointer<RawSocket>& s : sockets_) s->deliver(v6, protocol, from, packet, header);
}

void RawManager::reap() {
  if (__atomic_load_n(&open_, __ATOMIC_ACQUIRE) == 0) return;
  kernel::ScopedMutex guard(mutex_);
  for (uint64_t i = sockets_.count(); i > 0; --i) {
    SharedPointer<RawSocket>& s = sockets_[i - 1];
    if (!s->closed() && s.use_count() > 1) continue;      // (held by somebody besides this list)
    sockets_.remove_at(i - 1);
  }
  __atomic_store_n(&open_, static_cast<uint32_t>(sockets_.count()), __ATOMIC_RELEASE);
}

}  // namespace network
