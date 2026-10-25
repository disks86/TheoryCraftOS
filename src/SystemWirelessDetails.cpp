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

#include "SystemWirelessDetails.hpp"
#include "Hardware/WirelessInterface/IWirelessInterface.hpp"

namespace {
void copy_text(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}

// (The desktop's enumerations have the hardware's values.)
desktop::WirelessNetworkInfo convert(const hardware::WifiNetwork& n) {
  desktop::WirelessNetworkInfo out;
  copy_text(out.ssid, sizeof out.ssid, n.ssid);
  for (int i = 0; i < 6; ++i) out.bssid[i] = n.bssid.bytes[i];
  out.security = static_cast<desktop::WirelessSecurity>(n.security);
  out.channel = n.channel;
  out.frequency_mhz = n.frequency_mhz;
  out.signal_percent = n.signal_percent;
  return out;
}
}  // namespace

hardware::IWirelessInterface* SystemWirelessDetails::radio(uint32_t id) const {
  hardware::INetworkInterface* nic = adapters_.at(id);
  return nic ? nic->wireless() : nullptr;
}

bool SystemWirelessDetails::is_wireless(uint32_t id) { return radio(id) != nullptr; }

bool SystemWirelessDetails::adapter(uint32_t id, desktop::WirelessAdapterInfo* out) {
  hardware::IWirelessInterface* r = radio(id);
  if (!r) return false;
  *out = desktop::WirelessAdapterInfo();
  copy_text(out->name, sizeof out->name, r->name());
  const hardware::WifiStatus status = r->status();
  out->state = static_cast<desktop::WirelessState>(status.state);
  out->last_result = static_cast<desktop::WirelessResult>(status.last_result);
  out->network = convert(status.network);
  return true;
}

bool SystemWirelessDetails::scan(uint32_t id) {
  hardware::IWirelessInterface* r = radio(id);
  return r && r->start_scan();
}

uint32_t SystemWirelessDetails::networks(uint32_t id, desktop::WirelessNetworkInfo* out, uint32_t capacity) {
  hardware::IWirelessInterface* r = radio(id);
  if (!r || !out) return 0;
  // (Read into the hardware's own records, then convert.)
  hardware::WifiNetwork found[desktop::IWirelessDetails::kMaxNetworks];
  const uint32_t want = capacity < desktop::IWirelessDetails::kMaxNetworks ? capacity : desktop::IWirelessDetails::kMaxNetworks;
  const uint32_t n = r->scan_results(ArrayView<hardware::WifiNetwork>(found, want));
  for (uint32_t i = 0; i < n; ++i) out[i] = convert(found[i]);
  return n;
}

desktop::WirelessResult SystemWirelessDetails::connect(uint32_t id, StringView ssid, StringView passphrase) {
  hardware::IWirelessInterface* r = radio(id);
  if (!r) return desktop::WirelessResult::Failed;
  return static_cast<desktop::WirelessResult>(r->connect(ssid, passphrase));
}

void SystemWirelessDetails::disconnect(uint32_t id) {
  if (hardware::IWirelessInterface* r = radio(id)) r->disconnect();
}
