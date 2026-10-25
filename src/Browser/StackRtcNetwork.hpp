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

// IRtcNetwork over the machine's network stacks: UDP sockets, the address and the clock of the first stack that has an address. (The stacks are polled by their own thread.)
#pragma once
#include "Network/NetworkStackSet.hpp"
#include "Rtc/Ice.hpp"

namespace rtc {

class StackRtcNetwork final : public IRtcNetwork {
 public:
  explicit StackRtcNetwork(network::NetworkStackSet& stacks) : stacks_(stacks) {}
  SharedPointer<network::IUdpSocket> open_udp(uint16_t port) override;
  bool local_address(network::Ipv4Address* address) override;
  bool resolve(StringView host, network::Ipv4Address* address) override;
  uint64_t now_ms() override;
  void poll() override {}
  bool random(ByteView out) override;

 private:
  network::INetworkStack* stack();
  network::NetworkStackSet& stacks_;
};

}  // namespace rtc
