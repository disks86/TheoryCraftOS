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


#include "Hardware/WirelessInterface/Ieee80211Scan.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint8_t kElementSsid = 0, kElementDsParameter = 3, kElementHtOperation = 61, kElementRsn = 48, kElementVendor = 221;
constexpr uint16_t kCapabilityPrivacy = 0x0010;

// Cipher suite selectors are an OUI and a type: 00-0F-AC is the IEEE 802.11 OUI, 00-50-F2 Microsoft's (WPA).
struct Akm {
  bool psk = false, sae = false, enterprise = false, any = false;
};

void classify_akm(const uint8_t suite[4], bool wpa, Akm* akm) {
  const bool ieee = suite[0] == 0x00 && suite[1] == 0x0F && suite[2] == 0xAC;
  const bool microsoft = suite[0] == 0x00 && suite[1] == 0x50 && suite[2] == 0xF2;
  if (!(wpa ? microsoft : ieee)) return;
  akm->any = true;
  const uint8_t type = suite[3];
  if (type == 2 || type == 4 || type == 6 || type == 19) akm->psk = true;            // PSK, FT-PSK, PSK-SHA256, FT-PSK-SHA384
  else if (type == 8 || type == 9 || type == 24 || type == 25) akm->sae = true;      // SAE, FT-SAE (and their SHA384 forms)
  else akm->enterprise = true;                                                       // 802.1X and its variants, anything else
}

// The RSN element body (after the element header) or, for WPA, the vendor element body after its OUI and type: version, group suite, pairwise suites, AKM suites.
bool read_akms(ConstByteView body, bool wpa, Akm* akm) {
  ByteReader r(body);
  const uint16_t version = r.le16();
  if (!r.ok() || version != 1) return false;
  r.skip(4);                                                      // group cipher suite
  const uint16_t pairwise = r.le16();
  r.skip(static_cast<uint64_t>(pairwise) * 4);
  const uint16_t akm_count = r.le16();
  if (!r.ok()) return false;
  for (uint16_t i = 0; i < akm_count; ++i) {
    ConstByteView suite = r.take(4);
    if (!r.ok()) return false;
    const uint8_t s[4] = {suite[0], suite[1], suite[2], suite[3]};
    classify_akm(s, wpa, akm);
  }
  return true;
}
}  // namespace

uint8_t wifi_frequency_channel(uint16_t mhz) {
  if (mhz == 2484) return 14;
  if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0) return static_cast<uint8_t>((mhz - 2407) / 5);
  if (mhz >= 5160 && mhz <= 5885 && mhz % 5 == 0) return static_cast<uint8_t>((mhz - 5000) / 5);
  return 0;
}

uint8_t wifi_signal_percent(int32_t dbm) {
  if (dbm >= -50) return 100;
  if (dbm <= -100) return 0;
  return static_cast<uint8_t>(2 * (dbm + 100));
}

bool wifi_parse_announcement(ConstByteView frame, uint8_t radio_channel, int32_t signal_dbm, WifiNetwork* out) {
  ByteReader r(frame);
  const uint16_t control = r.le16();
  // (version bits 0-1 and the type management = bits 2-3 zero, subtype in bits 4-7; the flags in the high byte do not matter)
  const uint16_t kind = control & 0x00FC;
  if (!r.ok() || (kind != kWifiFrameBeacon && kind != kWifiFrameProbeResponse)) return false;
  r.skip(2);                                                       // duration
  r.skip(6);                                                       // destination
  MacAddress source = MacAddress::read(r);
  MacAddress bssid = MacAddress::read(r);
  r.skip(2);                                                       // sequence control
  r.skip(8);                                                       // timestamp
  r.skip(2);                                                       // beacon interval
  const uint16_t capability = r.le16();
  if (!r.ok()) return false;
  (void)source;

  WifiNetwork n;
  bool have_ssid = false;
  uint8_t channel = 0;
  uint8_t ht_channel = 0;
  Akm rsn, wpa;
  bool have_rsn = false, have_wpa = false;
  while (r.remaining() >= 2) {
    const uint8_t id = r.u8();
    const uint8_t length = r.u8();
    ConstByteView body = r.take(length);
    if (!r.ok()) return false;                                     // an element that runs past the frame: the frame is damaged
    if (id == kElementSsid) {
      if (length > kMaxSsidLength) return false;
      bool all_zero = true;
      for (uint64_t i = 0; i < body.size(); ++i)
        if (body[i] != 0) all_zero = false;
      if (length == 0 || all_zero) return false;                   // hidden
      for (uint64_t i = 0; i < body.size(); ++i) n.ssid[i] = static_cast<char>(body[i]);
      n.ssid[body.size()] = 0;
      have_ssid = true;
    } else if (id == kElementDsParameter && length == 1) {
      channel = body[0];
    } else if (id == kElementHtOperation && length >= 1) {
      ht_channel = body[0];
    } else if (id == kElementRsn) {
      have_rsn = read_akms(body, false, &rsn) || have_rsn;
    } else if (id == kElementVendor && length >= 4 && body[0] == 0x00 && body[1] == 0x50 && body[2] == 0xF2 && body[3] == 0x01) {
      have_wpa = read_akms(body.subview(4), true, &wpa) || have_wpa;
    }
  }
  if (!have_ssid) return false;
  for (uint32_t i = 0; i < kMaxSsidLength && n.ssid[i]; ++i)
    if (static_cast<uint8_t>(n.ssid[i]) < 0x20) return false;      // a name with control characters is not one to list
  n.bssid = bssid;
  if (!channel) channel = ht_channel;
  if (!channel) channel = radio_channel;
  n.channel = channel;
  n.frequency_mhz = channel ? wifi_channel_frequency_mhz(channel) : 0;
  // security: the strongest thing the network offers a client that knows a passphrase
  if (have_rsn && rsn.any) {
    if (rsn.psk) n.security = WifiSecurity::Wpa2;
    else if (rsn.sae) n.security = WifiSecurity::Wpa3;
    else n.security = WifiSecurity::Enterprise;
  } else if (have_wpa && wpa.any) {
    n.security = wpa.psk ? WifiSecurity::Wpa : WifiSecurity::Enterprise;
  } else {
    n.security = (capability & kCapabilityPrivacy) ? WifiSecurity::Wep : WifiSecurity::Open;
  }
  n.signal_percent = signal_dbm ? wifi_signal_percent(signal_dbm) : 0;
  *out = n;
  return true;
}

}  // namespace hardware
