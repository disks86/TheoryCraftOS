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

// The RTP modules' network in the kernel: the system's network stacks (network::system_network_stacks), the first interface that has an address
// (looked up at each use: interfaces come and get addresses after the sound server starts). Header only: the PipeWire code itself does not link the
// network stack.
#pragma once
#include "Network/NetworkStackSet.hpp"
#include "PipeWire/Rtp.hpp"

namespace pipewire {

class StackRtpNetwork final : public IRtpNetwork {
 public:
  SharedPointer<network::IUdpSocket> open(uint16_t port) override {
    network::INetworkStack* s = stack();
    return s ? s->udp_open(port) : SharedPointer<network::IUdpSocket>();
  }
  bool join(network::Ipv4Address group) override {
    network::INetworkStack* s = stack();
    return s && s->join_multicast(group);
  }
  void leave(network::Ipv4Address group) override {
    if (network::INetworkStack* s = stack()) s->leave_multicast(group);
  }
  network::Ipv4Address local_address() override {
    network::INetworkStack* s = stack();
    return s ? s->dhcp().lease().address : network::kAnyAddress;
  }

 private:
  static network::INetworkStack* stack() {
    network::NetworkStackSet* set = network::system_network_stacks();
    if (!set) return nullptr;
    for (uint32_t i = 0; i < set->count(); ++i)
      if (network::INetworkStack* s = set->at(i); s && !s->dhcp().lease().address.is_unspecified()) return s;
    return set->count() ? set->at(0) : nullptr;
  }
};

}  // namespace pipewire
