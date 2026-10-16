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
  return send_from(ip_.local_address(), address, port, data, false);
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
  out.be16(0);  // checksum, filled in below
  out.bytes(data);
  if (!out.ok()) return false;

  uint32_t sum = pseudo_header_sum(source, address, kProtocolUdp, static_cast<uint16_t>(out.position()));
  sum = checksum_add(sum, out.written());
  uint16_t checksum = checksum_finish(sum);
  if (checksum == 0) checksum = 0xFFFF;  // 0 means "no checksum" on the wire
  out.patch_be16(6, checksum);
  return ip_.send_ip_from(source, address, kProtocolUdp, out.written(), link_broadcast);
}

bool UdpSocket::receive_from(ByteView buffer, Ipv4Address* from_address, uint16_t* from_port, uint32_t* length) {
  StackLock guard(ip_);
  Datagram d;
  if (!queue_.dequeue(&d)) return false;
  buffer.copy_from(ConstByteView(d.data).first(d.length).at_most(buffer.size()));  // as much as fits
  if (from_address) *from_address = d.from_address;
  if (from_port) *from_port = d.from_port;
  if (length) *length = d.length;
  return true;
}

void UdpSocket::deliver(Ipv4Address from_address, uint16_t from_port, ConstByteView data) {
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
