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

// What the network adapter properties window shows about an adapter - its link, speed, hardware address and the IP configuration it was given.
// The desktop knows nothing about drivers: whoever builds the system hands it an INetworkDetails (SystemNetworkDetails, in the real system).
#pragma once
#include <stdint.h>

namespace desktop {

struct NetworkAdapterInfo {
  char name[64] = {};
  uint8_t mac[6] = {};
  bool link_up = false;
  uint32_t speed_mbps = 0;      // 0: down, or the adapter does not say
  uint8_t duplex = 0;           // 0 not said, 1 half, 2 full
  bool has_stack = false;       // a network stack runs on the adapter (without one it has no IP configuration)
  bool configured = false;      // an address was set or obtained
  bool dhcp = false;            // the configuration comes from DHCP
  char dhcp_state[16] = {};     // "bound", "selecting", "failed", ...
  uint32_t lease_seconds = 0;
  uint8_t address[4] = {}, netmask[4] = {}, gateway[4] = {}, dns[4] = {}, dhcp_server[4] = {};
  uint64_t frames_sent = 0, frames_received = 0, send_failures = 0, frames_dropped = 0;
};

class INetworkDetails {
 public:
  // The adapter with this number (HardwareEntry::id of its entry in the hardware tree). False if there is none (any more).
  virtual bool adapter(uint32_t id, NetworkAdapterInfo* out) = 0;

 protected:
  constexpr INetworkDetails() = default;
  ~INetworkDetails() = default;
};

}  // namespace desktop
