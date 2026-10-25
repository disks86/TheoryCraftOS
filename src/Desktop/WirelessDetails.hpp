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

// What the Wi-Fi properties window shows and does: the networks in range of a wireless adapter, the one it is joined to, and joining or leaving
// one. The desktop knows nothing about drivers: whoever builds the system hands it an IWirelessDetails (SystemWirelessDetails, in the real system).
// The enumerations have the values of hardware::WifiSecurity, WifiState and WifiResult.
#pragma once
#include <stdint.h>
#include "Utility/StringView.hpp"

namespace desktop {

enum class WirelessSecurity : uint8_t { Open, Wep, Wpa, Wpa2, Wpa3, Enterprise };
enum class WirelessState : uint8_t { Disconnected, Scanning, Connecting, Connected };
enum class WirelessResult : uint8_t { Ok, NoSuchNetwork, BadPassphrase, Unsupported, InvalidArgument, Busy, Failed };

struct WirelessNetworkInfo {
  char ssid[33] = {};
  uint8_t bssid[6] = {};
  WirelessSecurity security = WirelessSecurity::Open;
  uint8_t channel = 0;
  uint16_t frequency_mhz = 0;
  uint8_t signal_percent = 0;
};

struct WirelessAdapterInfo {
  char name[64] = {};
  WirelessState state = WirelessState::Disconnected;
  WirelessResult last_result = WirelessResult::Ok;   // why the last attempt to join did not work
  WirelessNetworkInfo network;                        // the network joined (while Connecting or Connected)
};

class IWirelessDetails {
 public:
  static constexpr uint32_t kMaxNetworks = 64;

  // True if the adapter with this number (HardwareEntry::id of its entry in the hardware tree) is a Wi-Fi adapter.
  virtual bool is_wireless(uint32_t id) = 0;
  // The adapter's state. False if there is no such Wi-Fi adapter (any more).
  virtual bool adapter(uint32_t id, WirelessAdapterInfo* out) = 0;
  // Starts a search for networks.
  virtual bool scan(uint32_t id) = 0;
  // The networks the latest search found, strongest first; writes up to `capacity` and returns how many.
  virtual uint32_t networks(uint32_t id, WirelessNetworkInfo* out, uint32_t capacity) = 0;
  virtual WirelessResult connect(uint32_t id, StringView ssid, StringView passphrase) = 0;
  virtual void disconnect(uint32_t id) = 0;

 protected:
  constexpr IWirelessDetails() = default;
  ~IWirelessDetails() = default;
};

const char* wireless_security_name(WirelessSecurity security);
// "Connected", "Not connected", ...
const char* wireless_state_name(WirelessState state);
// Why a connection attempt failed, as a sentence ("" for Ok).
const char* wireless_result_text(WirelessResult result);

}  // namespace desktop
