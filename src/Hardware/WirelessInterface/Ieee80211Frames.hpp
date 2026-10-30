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


// The 802.11 frames a client needs to join a network: authentication (open system), association request and response, deauthentication / disassociation, the data frames that
// carry EAPOL, and the RSN element negotiation for WPA2-Personal with CCMP. Independent of any chipset. Frames from the air are untrusted: parsers check every length.
#pragma once
#include <stdint.h>
#include "Hardware/NetworkInterface/NetworkTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace ieee80211 {

constexpr uint16_t kSubtypeAssocRequest = 0x00, kSubtypeAssocResponse = 0x10, kSubtypeProbeResponse = 0x50, kSubtypeBeacon = 0x80, kSubtypeDisassociation = 0xA0, kSubtypeAuthentication = 0xB0,
                   kSubtypeDeauthentication = 0xC0;   // frame control with type management
constexpr uint32_t kMgmtHeaderBytes = 24, kDataHeaderBytes = 24;
constexpr uint16_t kStatusSuccess = 0;
constexpr uint16_t kCapabilityEss = 0x0001, kCapabilityPrivacy = 0x0010;
constexpr uint16_t kEtherTypeEapol = 0x888E;

// Management frame header (24 bytes) for a frame to / from the access point `bssid` with our address `sa`, into `w`-less raw bytes.
uint32_t build_authentication(const uint8_t bssid[6], const uint8_t sa[6], ByteView out);
// `ssid` (1-32 bytes), `five_ghz` picks the rate set, `rsn_ie` is our RSN element (id 48 included) or empty for an open network.
uint32_t build_association_request(const uint8_t bssid[6], const uint8_t sa[6], ConstByteView ssid, bool five_ghz, ConstByteView rsn_ie, ByteView out);
uint32_t build_deauthentication(const uint8_t bssid[6], const uint8_t sa[6], uint16_t reason, ByteView out);
// An 802.11 data frame to the access point carrying an EAPOL frame (LLC/SNAP 0x888E, to-DS, not encrypted).
uint32_t build_eapol_data(const uint8_t bssid[6], const uint8_t sa[6], ConstByteView eapol, ByteView out);

// What a management frame from `bssid` to us says. The type is the frame control's management subtype; false when the frame is not a well-formed management frame of an
// access point we talk to (wrong BSSID / not addressed to us / too short).
struct MgmtFrame {
  uint16_t subtype = 0;
  uint16_t algorithm = 0, sequence = 0;      // authentication
  uint16_t status = 0;                       // authentication, association response
  uint16_t reason = 0;                       // deauthentication, disassociation
  uint16_t association_id = 0;               // association response, without the two top bits
};
bool parse_management(ConstByteView frame, const uint8_t bssid[6], const uint8_t our_address[6], MgmtFrame* out);
// The EAPOL frame inside a data frame from the access point (plain or QoS data, from the distribution system, LLC/SNAP 0x888E) or an empty view.
ConstByteView parse_eapol_data(ConstByteView frame, const uint8_t bssid[6], const uint8_t our_address[6]);

// ---- RSN negotiation ---------------------------------------------------------------------------------------------------------------------------------------------------

// The RSN element (id 48 included) found in a beacon / probe response frame, or empty.
ConstByteView find_rsn_element(ConstByteView frame);
// Checks that the access point's RSN element offers what we speak (group CCMP, pairwise CCMP, a pre-shared key, management frame protection not required) and builds ours for the
// association request (22 bytes) into `out`. Returns its length, or 0 with *why naming the problem.
uint32_t negotiate_rsn(ConstByteView ap_rsn_element, ByteView out, const char** why);

}  // namespace ieee80211
}  // namespace hardware
