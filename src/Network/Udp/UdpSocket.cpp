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

#include "Network/Udp/UdpSocket.hpp"
#include "Network/Checksum.hpp"

namespace network {

uint32_t UdpSocket::pending() const {
  StackLock guard(ip_);
  return static_cast<uint32_t>(queue_.count());
}

void UdpSocket::close() {
  StackLock guard(ip_);
  closed_ = true;
}

bool UdpSocket::send_to(Ipv4Address address, uint16_t port, ConstByteView data) {
  StackLock guard(ip_);
  return send_from(ip_.source_for(address), address, port, data, false);
}

bool UdpSocket::send_from(Ipv4Address source, Ipv4Address address, uint16_t port, ConstByteView data,
                          bool link_broadcast) {
  StackLock guard(ip_);
  if (closed_ || data.size() > kMaxDatagram) return false;
  uint8_t storage[8 + kMaxDatagram];
  ByteWriter out{ByteView(storage)};
  out.be16(port_);
  out.be16(port);
  out.be16(static_cast<uint16_t>(8 + data.size()));
  out.be16(0);  // checksum: the IP layer fills it in
  out.bytes(data);
  if (!out.ok()) return false;
  return ip_.send_ip_from(source, address, kProtocolUdp, out.written(), link_broadcast);
}

bool UdpSocket::receive_from(ByteView buffer, Ipv4Address* from_address, uint16_t* from_port, uint32_t* length) {
  IpAddress from;
  if (!receive_from_ip(buffer, &from, from_port, length)) return false;
  if (from_address) *from_address = from.v6 ? kAnyAddress : from.ipv4;
  return true;
}

bool UdpSocket::receive_from_ip(ByteView buffer, IpAddress* from_address, uint16_t* from_port, uint32_t* length) {
  StackLock guard(ip_);
  Datagram d;
  if (!queue_.dequeue(&d)) return false;
  buffer.copy_from(ConstByteView(d.data).first(d.length).at_most(buffer.size()));  // as much as fits
  if (from_address) *from_address = d.from_address;
  if (from_port) *from_port = d.from_port;
  if (length) *length = d.length;
  return true;
}

// Over IPv6 the same datagram, sent with send_ip_to() (the IP layer fills in the checksum, which IPv6 requires) and 20 bytes shorter at most.
bool UdpSocket::send_to_ip(const IpAddress& address, uint16_t port, ConstByteView data) {
  if (!address.v6) return send_to(address.ipv4, port, data);
  StackLock guard(ip_);
  if (closed_ || data.size() > kMaxIpv6Payload - 8) return false;
  uint8_t storage[8 + kMaxDatagram];
  ByteWriter out{ByteView(storage)};
  out.be16(port_);
  out.be16(port);
  out.be16(static_cast<uint16_t>(8 + data.size()));
  out.be16(0);
  out.bytes(data);
  if (!out.ok()) return false;
  return ip_.send_ip_to(address, kProtocolUdp, out.written());
}

void UdpSocket::deliver(const IpAddress& from_address, uint16_t from_port, ConstByteView data) {
  StackLock guard(ip_);
  if (closed_ || data.size() > kMaxDatagram || queue_.count() >= kQueueLimit) return;
  Datagram d;
  d.from_address = from_address;
  d.from_port = from_port;
  d.length = static_cast<uint16_t>(data.size());
  ByteView(d.data).copy_from(data);
  queue_.enqueue(d);
}

}  // namespace network
