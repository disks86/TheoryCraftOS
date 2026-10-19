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

// The network adapter properties window's view of the machine: each adapter of the system (its link, speed and counters) together with the network
// stack that runs on it (its address, mask, gateway and DNS server, and whether DHCP gave them). Both must outlive it.
#pragma once
#include "Desktop/NetworkDetails.hpp"
#include "Hardware/NetworkInterface/SystemNetworkInterfaces.hpp"
#include "Network/NetworkStackSet.hpp"

class SystemNetworkDetails final : public desktop::INetworkDetails {
 public:
  SystemNetworkDetails(hardware::SystemNetworkInterfaces& adapters, network::NetworkStackSet& stacks) : adapters_(adapters), stacks_(stacks) {}

  bool adapter(uint32_t id, desktop::NetworkAdapterInfo* out) override;

 private:
  hardware::SystemNetworkInterfaces& adapters_;
  network::NetworkStackSet& stacks_;
};
