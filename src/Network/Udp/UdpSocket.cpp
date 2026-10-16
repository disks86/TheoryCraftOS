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

bool UdpSocket::send_to(Ipv4Address address, uint16_t port, const uint8_t* data, uint32_t length) {
  return send_from(ip_.local_address(), address, port, data, length, false);
}

bool UdpSocket::send_from(Ipv4Address source, Ipv4Address address, uint16_t port, const uint8_t* data,
                          uint32_t length, bool link_broadcast) {
  if (closed_ || (length && !data) || length > kMaxDatagram) return false;
  uint8_t packet[8 + kMaxDatagram];
  store16(packet, port_);
  store16(packet + 2, port);
  store16(packet + 4, static_cast<uint16_t>(8 + length));
  store16(packet + 6, 0);
  for (uint32_t i = 0; i < length; ++i) packet[8 + i] = data[i];

  uint32_t sum = pseudo_header_sum(source, address, kProtocolUdp, static_cast<uint16_t>(8 + length));
  sum = checksum_add(sum, packet, 8 + length);
  uint16_t checksum = checksum_finish(sum);
  if (checksum == 0) checksum = 0xFFFF;  // 0 means "no checksum" on the wire
  store16(packet + 6, checksum);
  return ip_.send_ip_from(source, address, kProtocolUdp, packet, 8 + length, link_broadcast);
}

bool UdpSocket::receive_from(uint8_t* buffer, uint32_t capacity, Ipv4Address* from_address, uint16_t* from_port,
                             uint32_t* length) {
  Datagram d;
  if (!queue_.dequeue(&d)) return false;
  const uint32_t n = d.length < capacity ? d.length : capacity;
  if (buffer)
    for (uint32_t i = 0; i < n; ++i) buffer[i] = d.data[i];
  if (from_address) *from_address = d.from_address;
  if (from_port) *from_port = d.from_port;
  if (length) *length = d.length;
  return true;
}

void UdpSocket::deliver(Ipv4Address from_address, uint16_t from_port, const uint8_t* data, uint32_t length) {
  if (closed_ || length > kMaxDatagram || queue_.count() >= kQueueLimit) return;
  Datagram d;
  d.from_address = from_address;
  d.from_port = from_port;
  d.length = static_cast<uint16_t>(length);
  for (uint32_t i = 0; i < length; ++i) d.data[i] = data[i];
  queue_.enqueue(d);
}

}  // namespace network
