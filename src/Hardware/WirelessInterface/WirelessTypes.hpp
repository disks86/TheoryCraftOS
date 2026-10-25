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

// What a Wi-Fi (IEEE 802.11) adapter reports about the networks around it and the one it is joined to.
#pragma once
#include <stdint.h>
#include "Hardware/NetworkInterface/NetworkTypes.hpp"

namespace hardware {

constexpr uint32_t kMaxSsidLength = 32;
constexpr uint32_t kMinPassphraseLength = 8;    // WPA-PSK passphrases are 8 to 63 characters
constexpr uint32_t kMaxPassphraseLength = 63;

// How a network protects its traffic. Enterprise (802.1X) networks are listed but need a login this layer does not carry.
enum class WifiSecurity : uint8_t { Open, Wep, Wpa, Wpa2, Wpa3, Enterprise };

enum class WifiState : uint8_t {
  Disconnected,   // the radio is on and joined to nothing
  Scanning,       // looking for networks (the old results stay until the scan ends)
  Connecting,     // joining a network
  Connected,
};

// What happened to the last connect() - and, until the next one, why the adapter is not joined.
enum class WifiResult : uint8_t {
  Ok,
  NoSuchNetwork,      // the name is not among the networks in range
  BadPassphrase,      // wrong, or not a valid WPA passphrase
  Unsupported,        // a security mode the adapter cannot join (enterprise)
  InvalidArgument,    // an empty or too long name
  Busy,               // a scan or a connection is under way
  Failed,             // the radio gave up
};

// One network in range: what an access point announces.
struct WifiNetwork {
  char ssid[kMaxSsidLength + 1] = {};   // the name, text, zero-terminated
  MacAddress bssid{};                   // the access point's own address
  WifiSecurity security = WifiSecurity::Open;
  uint8_t channel = 0;
  uint16_t frequency_mhz = 0;
  uint8_t signal_percent = 0;           // 0-100
};

struct WifiStatus {
  WifiState state = WifiState::Disconnected;
  WifiResult last_result = WifiResult::Ok;
  WifiNetwork network;                  // the network joined (valid while Connecting or Connected)
};

const char* wifi_security_name(WifiSecurity security);
const char* wifi_state_name(WifiState state);
const char* wifi_result_name(WifiResult result);
// The centre frequency of a channel: 1-14 in the 2.4 GHz band, 32 and up in the 5 GHz band; 0 for a number that is none.
uint16_t wifi_channel_frequency_mhz(uint8_t channel);

}  // namespace hardware
