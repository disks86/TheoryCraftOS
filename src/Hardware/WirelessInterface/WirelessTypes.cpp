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

#include "Hardware/WirelessInterface/WirelessTypes.hpp"

namespace hardware {

const char* wifi_security_name(WifiSecurity security) {
  switch (security) {
    case WifiSecurity::Open: return "Open";
    case WifiSecurity::Wep: return "WEP";
    case WifiSecurity::Wpa: return "WPA";
    case WifiSecurity::Wpa2: return "WPA2";
    case WifiSecurity::Wpa3: return "WPA3";
    case WifiSecurity::Enterprise: return "Enterprise";
  }
  return "";
}

const char* wifi_state_name(WifiState state) {
  switch (state) {
    case WifiState::Disconnected: return "Not connected";
    case WifiState::Scanning: return "Scanning";
    case WifiState::Connecting: return "Connecting";
    case WifiState::Connected: return "Connected";
  }
  return "";
}

const char* wifi_result_name(WifiResult result) {
  switch (result) {
    case WifiResult::Ok: return "";
    case WifiResult::NoSuchNetwork: return "The network is not in range.";
    case WifiResult::BadPassphrase: return "The password is not right.";
    case WifiResult::Unsupported: return "This kind of network is not supported.";
    case WifiResult::InvalidArgument: return "Not a valid network name.";
    case WifiResult::Busy: return "The adapter is busy; try again in a moment.";
    case WifiResult::Failed: return "The connection failed.";
  }
  return "";
}

uint16_t wifi_channel_frequency_mhz(uint8_t channel) {
  if (channel >= 1 && channel <= 13) return static_cast<uint16_t>(2407 + 5 * channel);
  if (channel == 14) return 2484;
  if (channel >= 32 && channel <= 177) return static_cast<uint16_t>(5000 + 5 * channel);
  return 0;
}

}  // namespace hardware
