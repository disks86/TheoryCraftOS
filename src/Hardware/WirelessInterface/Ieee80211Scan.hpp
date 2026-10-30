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


// Reading what an access point announces: the management frames a radio hands up while scanning - beacons and probe responses (IEEE 802.11-2020 9.3.3.2 and 9.3.3.11) - turned
// into the WifiNetwork the rest of the system lists. Independent of any chipset. The frame is untrusted air: every length is checked and anything malformed is refused.
//
// The network's name is the SSID element (a hidden network sends it empty or as zeros; those are refused because there is nothing to list). The channel is the DS Parameter Set
// element, else the HT Operation element's primary channel, else the channel the radio received it on. The security is read from the RSN element (WPA2 and WPA3) and the
// Microsoft WPA vendor element (WPA): a pre-shared key or SAE suite makes the network joinable, an 802.1X suite makes it Enterprise; with neither, the Privacy bit of the
// capability field says WEP or open.
#pragma once
#include <stdint.h>
#include "Hardware/WirelessInterface/WirelessTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

constexpr uint16_t kWifiFrameBeacon = 0x80, kWifiFrameProbeResponse = 0x50;       // frame control with the type / subtype bits (version 0), flags masked off

// The channel a frequency belongs to (2.4 GHz 1-14, 5 GHz 32-177 in steps of 1 over 5000 MHz); 0 for none.
uint8_t wifi_frequency_channel(uint16_t mhz);

// Parses a beacon or probe response (the whole MAC frame from the frame control field, without the FCS). `radio_channel` is the channel the radio says it heard it on (0 if it
// does not say) and `signal_dbm` its signal strength (0 if unknown, else negative). On true `out` has the ssid, bssid, security, channel, frequency and signal.
bool wifi_parse_announcement(ConstByteView frame, uint8_t radio_channel, int32_t signal_dbm, WifiNetwork* out);

// A signal strength in dBm as 0-100: -100 dBm and below is 0, -50 and above is 100 (the usual linear quality mapping).
uint8_t wifi_signal_percent(int32_t dbm);

}  // namespace hardware
