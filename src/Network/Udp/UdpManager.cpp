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

bool UdpManager::port_in_use(uint16_t port) const {
  for (const SharedPointer<UdpSocket>& s : sockets_)
    if (s->local_port() == port && !s->closed()) return true;
  return false;
}

SharedPointer<UdpSocket> UdpManager::open(uint16_t port) {
  if (port == 0) {
    for (uint32_t tries = 0; tries < 16384 && port == 0; ++tries) {
      const uint16_t candidate = next_ephemeral_;
      next_ephemeral_ = next_ephemeral_ == 65535 ? 49152 : next_ephemeral_ + 1;
      if (!port_in_use(candidate)) port = candidate;
    }
    if (port == 0) return SharedPointer<UdpSocket>();
  } else if (port_in_use(port)) {
    return SharedPointer<UdpSocket>();
  }
  SharedPointer<UdpSocket> socket = SharedPointer<UdpSocket>::make(allocator_, ip_, allocator_, port);
  if (!socket || !sockets_.push_back(socket)) return SharedPointer<UdpSocket>();
  return socket;
}

bool UdpManager::handle(Ipv4Address source, Ipv4Address destination, ConstByteView segment) {
  ByteReader in(segment);
  const uint16_t source_port = in.be16();
  const uint16_t port = in.be16();
  const uint16_t udp_length = in.be16();
  const uint16_t wire_checksum = in.be16();
  if (!in.ok() || udp_length < 8 || udp_length > segment.size()) return false;
  const ConstByteView datagram = segment.first(udp_length);  // anything after it is padding
  if (wire_checksum != 0) {  // a zero checksum means the sender did not compute one
    const uint32_t sum = checksum_add(pseudo_header_sum(source, destination, kProtocolUdp, udp_length), datagram);
    if (checksum_finish(sum) != 0) return false;
  }
  for (SharedPointer<UdpSocket>& s : sockets_) {
    if (s->local_port() != port || s->closed()) continue;
    s->deliver(source, source_port, datagram.dropped(8));
    return true;
  }
  return false;
}

void UdpManager::reap() {
  for (uint64_t i = sockets_.count(); i > 0; --i) {
    SharedPointer<UdpSocket>& s = sockets_[i - 1];
    if (s->closed() || s.use_count() == 1) sockets_.remove_at(i - 1);
  }
}

}  // namespace network
