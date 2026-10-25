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

#include "Browser/StackRtcNetwork.hpp"
#include "Browser/WebRandom.hpp"

namespace rtc {

network::INetworkStack* StackRtcNetwork::stack() {
  for (uint32_t i = 0; i < stacks_.count(); ++i)
    if (!stacks_.at(i)->configuration().address.is_unspecified()) return stacks_.at(i);
  return nullptr;
}

SharedPointer<network::IUdpSocket> StackRtcNetwork::open_udp(uint16_t port) {
  network::INetworkStack* s = stack();
  return s ? s->udp_open(port) : SharedPointer<network::IUdpSocket>();
}

bool StackRtcNetwork::local_address(network::Ipv4Address* address) {
  network::INetworkStack* s = stack();
  if (!s) return false;
  *address = s->configuration().address;
  return true;
}

bool StackRtcNetwork::resolve(StringView host, network::Ipv4Address* address) {
  network::INetworkStack* s = stack();
  return s && s->resolve(host, 1500, address);
}

uint64_t StackRtcNetwork::now_ms() {
  network::INetworkStack* s = stack();
  return s ? s->now_ms() : 0;
}

bool StackRtcNetwork::random(ByteView out) { return browser::system_random(out); }

}  // namespace rtc
