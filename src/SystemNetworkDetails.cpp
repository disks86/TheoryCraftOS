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

#include "SystemNetworkDetails.hpp"
#include "Network/Dhcp/IDhcpClient.hpp"

namespace {
void copy_text(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}
void copy_ip(uint8_t* out, const network::Ipv4Address& a) {
  for (int i = 0; i < 4; ++i) out[i] = a.octets[i];
}
}  // namespace

bool SystemNetworkDetails::adapter(uint32_t id, desktop::NetworkAdapterInfo* out) {
  hardware::INetworkInterface* nic = adapters_.at(id);
  if (!nic) return false;
  *out = desktop::NetworkAdapterInfo();
  copy_text(out->name, sizeof out->name, nic->name());
  const hardware::MacAddress mac = nic->mac_address();
  for (int i = 0; i < 6; ++i) out->mac[i] = mac.bytes[i];
  out->link_up = nic->link_up();
  out->speed_mbps = out->link_up ? nic->link_speed_mbps() : 0;
  const hardware::LinkDuplex duplex = nic->link_duplex();
  out->duplex = duplex == hardware::LinkDuplex::Full ? 2 : duplex == hardware::LinkDuplex::Half ? 1 : 0;
  const hardware::NetworkStatistics stats = nic->statistics();
  out->frames_sent = stats.frames_sent;
  out->frames_received = stats.frames_received;
  out->send_failures = stats.send_failures;
  out->frames_dropped = stats.frames_dropped;
  for (uint32_t i = 0; i < stacks_.count(); ++i) {
    network::NetworkStack* stack = stacks_.at(i);
    if (!stack || &stack->interface() != nic) continue;
    out->has_stack = true;
    const network::IpConfiguration config = stack->configuration();
    out->configured = config.configured();
    copy_ip(out->address, config.address);
    copy_ip(out->netmask, config.netmask);
    copy_ip(out->gateway, config.gateway);
    copy_ip(out->dns, config.dns);
    const network::IDhcpClient& dhcp = stack->dhcp();
    const network::DhcpState state = dhcp.state();
    out->dhcp = state != network::DhcpState::Stopped;
    if (out->dhcp) {
      copy_text(out->dhcp_state, sizeof out->dhcp_state, network::dhcp_state_name(state));
      if (state == network::DhcpState::Bound || state == network::DhcpState::Renewing) {
        out->lease_seconds = dhcp.lease_seconds();
        copy_ip(out->dhcp_server, dhcp.server());
      }
    }
    break;
  }
  return true;
}
