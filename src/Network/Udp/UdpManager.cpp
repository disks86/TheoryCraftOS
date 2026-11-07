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

#include "Network/Udp/UdpManager.hpp"
#include "Network/Checksum.hpp"

namespace network {

bool UdpManager::port_in_use(uint16_t port, IpFamilies families) const {
  return table_.any(hash_port(port), [&](const SocketKey& k, const UdpSocket& s) {
    return k.local_port == port && !s.closed() && families_overlap(k.families, families);
  });
}

SharedPointer<UdpSocket> UdpManager::open(uint16_t port, IpFamilies families) {
  kernel::ScopedMutex guard(table_mutex_);
  if (port == 0) {
    for (uint32_t tries = 0; tries < 16384 && port == 0; ++tries) {
      const uint16_t candidate = next_ephemeral_;
      next_ephemeral_ = next_ephemeral_ == 65535 ? 49152 : next_ephemeral_ + 1;
      if (!port_in_use(candidate, IpFamilies::Both)) port = candidate;  // (an ephemeral port is free for both versions)
    }
    if (port == 0) return SharedPointer<UdpSocket>();
  } else if (port_in_use(port, families)) {
    return SharedPointer<UdpSocket>();
  }
  SharedPointer<UdpSocket> socket = SharedPointer<UdpSocket>::make(allocator_, ip_, allocator_, port, families);
  if (!socket || !sockets_.push_back(socket)) return SharedPointer<UdpSocket>();
  SocketKey key;
  key.local_port = port;
  key.families = families;
  if (!table_.insert(hash_port(port), key, socket)) {
    sockets_.remove_at(sockets_.count() - 1);
    return SharedPointer<UdpSocket>();
  }
  return socket;
}

bool UdpManager::handle(const IpAddress& source, const IpAddress& destination, ConstByteView segment, bool checksum_verified) {
  ByteReader in(segment);
  const uint16_t source_port = in.be16();
  const uint16_t port = in.be16();
  const uint16_t udp_length = in.be16();
  const uint16_t wire_checksum = in.be16();
  if (!in.ok() || udp_length < 8 || udp_length > segment.size()) return false;
  const ConstByteView datagram = segment.first(udp_length);  // anything after it is padding
  if (wire_checksum == 0 && source.v6) return false;  // (over IPv6 the checksum is not optional, RFC 8200 8.1)
  if (wire_checksum != 0 && !checksum_verified) {  // a zero checksum means the sender did not compute one
    const uint32_t sum = checksum_add(pseudo_header_sum(source, destination, kProtocolUdp, udp_length), datagram);
    if (checksum_finish(sum) != 0) return false;
  }
  // (a closed socket may linger in the table until reap(); one opened on its port since then comes after it in the bucket)
  const SharedPointer<UdpSocket> to = table_.find(hash_port(port), [&](const SocketKey& k, const UdpSocket& s) {
    return k.local_port == port && !s.closed() && families_take(k.families, source.v6);
  });
  if (!to) return false;
  to->deliver(source, source_port, datagram.dropped(8));
  return true;
}

void UdpManager::reap() {
  kernel::ScopedMutex guard(table_mutex_);
  for (uint64_t i = sockets_.count(); i > 0; --i) {
    SharedPointer<UdpSocket>& s = sockets_[i - 1];
    // closed, or held by nobody but this list and the table (2 references)
    if (!s->closed() && s.use_count() > 2) continue;
    table_.remove(hash_port(s->local_port()), s.get());
    sockets_.remove_at(i - 1);
  }
}

}  // namespace network
