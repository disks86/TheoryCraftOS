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

// 802.11n (HT) and 802.11ac (VHT) for a client, independent of any chipset: what an access point announces in its HT / VHT Capabilities and Operation elements (IEEE 802.11-2020
// 9.4.2.55 - 9.4.2.158), what we use with it (spatial streams, channel width, guard interval), the elements we put into the association request, and the ladder of data rates
// (legacy 6 Mbit/s, then single stream MCS, then two streams) a rate controller walks. Frames from the air are untrusted: every length is checked.
//
// Not done here: aggregation and block acknowledgement (frames go out one MPDU at a time), LDPC, space-time block coding, beamforming, 160 MHz, three or four streams.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// What one access point offers, read from its beacon / probe response (or association response) elements.
struct WifiPhyCapabilities {
  bool ht = false, vht = false;
  uint16_t ht_cap_info = 0;                  // HT Capabilities Info: bit 1 40 MHz, bit 5 short GI 20, bit 6 short GI 40
  uint8_t ampdu_params = 0;                  // maximum A-MPDU length exponent (bits 0-1), minimum MPDU start spacing (bits 2-4)
  uint8_t rx_mcs[4] = {};                    // the first four bytes of the Rx MCS bitmask: one byte per spatial stream (MCS 0-7 of that stream)
  uint8_t ht_primary_channel = 0;
  uint8_t ht_secondary = 0;                  // secondary channel offset: 0 none, 1 above, 3 below
  bool ht_wide = false;                      // HT Operation: any channel width (40 MHz) in use
  uint32_t vht_cap_info = 0;                 // VHT Capabilities Info: bit 5 short GI 80, bits 23-25 maximum A-MPDU length exponent
  uint16_t vht_rx_mcs_map = 0;               // two bits per stream: 0 MCS 0-7, 1 MCS 0-8, 2 MCS 0-9, 3 none
  uint8_t vht_width = 0, vht_center0 = 0;    // VHT Operation: channel width (0 20/40, 1 80 or more, ...) and the centre channel of the 80 MHz segment
};

// Reads the elements of a beacon or probe response frame (the whole MAC frame from the frame control field). False when the frame is not one or is damaged.
bool wifi_parse_phy_capabilities(ConstByteView frame, WifiPhyCapabilities* out);
// The same for a run of elements alone (an association response's after its fixed fields).
bool wifi_parse_phy_elements(ConstByteView elements, WifiPhyCapabilities* out);

// What we do with that access point.
struct WifiPhyNegotiation {
  bool ht = false, vht = false;
  uint8_t our_streams = 1;                   // what our radio can do (1 or 2); the elements we send say so
  uint8_t streams = 1;                       // used with this access point: the lesser of ours and its
  uint8_t width_mhz = 20;                    // 20, 40 or 80
  bool short_gi = false;                     // for the width in use
  uint8_t max_mcs = 7;                       // highest MCS of one stream: 7 (HT) or 7-9 (VHT)
  uint8_t ampdu_exponent = 0, ampdu_density = 0;
  uint8_t primary_channel = 0;
  uint16_t primary_mhz = 0, center_mhz = 0;  // the centre of the channel in use (the primary's own for 20 MHz)
  // The PHY context's control channel position for the firmware (iwl_mvm_get_ctrl_pos): 0 for a 20 MHz channel.
  uint8_t control_position() const;
};
// Picks what to use with an access point on `primary_channel` for a radio with `our_streams` (1-2) chains. Without HT in the access point nothing is used (ht and vht false).
WifiPhyNegotiation wifi_negotiate_phy(const WifiPhyCapabilities& ap, uint8_t primary_channel, uint32_t our_streams);

constexpr uint32_t kWifiPhyElementsMax = 64;
// The elements of the association request for a negotiated PHY: the WMM information element (HT stations are QoS stations), the HT Capabilities and, on 5 GHz with VHT, the VHT
// Capabilities. Returns the length, 0 when there is nothing to add (no HT) or `out` is too small.
uint32_t wifi_build_phy_elements(const WifiPhyNegotiation& negotiation, ByteView out);

// ---- rates ---------------------------------------------------------------------------------------------------------------------------------------------------------------------

// One step of the rate ladder.
struct WifiRate {
  enum class Mode : uint8_t { Legacy, Ht, Vht };
  Mode mode = Mode::Legacy;
  uint8_t mcs = 0;                           // legacy: index into the OFDM rates (0 is 6 Mbit/s); HT: 0-15 (the stream count is in it); VHT: 0-9
  uint8_t streams = 1;
  uint8_t width_mhz = 20;
  bool short_gi = false;
  uint32_t kbps = 0;
};

constexpr uint32_t kWifiRateLadderMax = 40;
// The rates to use, slowest first: 6 Mbit/s legacy (the fallback every access point speaks), then the single stream MCS, then the two stream ones that are faster than all of those.
struct WifiRateLadder {
  WifiRate rate[kWifiRateLadderMax];
  uint32_t count = 0;
  uint32_t mimo_first = 0;                   // index of the first two stream rate (count when there is none)
};
void wifi_build_rate_ladder(const WifiPhyNegotiation& negotiation, WifiRateLadder* out);
// Bit rate of an HT / VHT MCS (MCS 0-9 of one stream) in kbit/s: `streams` streams on a `width_mhz` channel, with the long or the short guard interval. 0 for a combination that does
// not exist (MCS 9 on 20 MHz with one or two streams).
uint32_t wifi_mcs_kbps(uint32_t mcs, uint32_t streams, uint32_t width_mhz, bool short_gi);

}  // namespace hardware
