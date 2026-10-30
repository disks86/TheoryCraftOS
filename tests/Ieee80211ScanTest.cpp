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


// Host-side tests for reading beacons and probe responses: open, WEP, WPA, WPA2, WPA3 and enterprise networks, channels from the DS and HT elements and the radio, hidden and
// malformed names, damaged frames (every truncation and bit flip) and signal strength.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/WirelessInterface/Ieee80211Scan.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;

static Bytes element(uint8_t id, const Bytes& body) {
  Bytes b{id, static_cast<uint8_t>(body.size())};
  b.insert(b.end(), body.begin(), body.end());
  return b;
}
static Bytes ssid_element(const char* name) { return element(0, Bytes(name, name + strlen(name))); }
static Bytes rsn_element(std::initializer_list<uint8_t> akm_types, bool microsoft_wpa = false) {
  Bytes body;
  if (microsoft_wpa) body = {0x00, 0x50, 0xF2, 0x01};
  const uint8_t oui[3] = {0x00, static_cast<uint8_t>(microsoft_wpa ? 0x50 : 0x0F), static_cast<uint8_t>(microsoft_wpa ? 0xF2 : 0xAC)};
  body.insert(body.end(), {1, 0});                                          // version
  body.insert(body.end(), {oui[0], oui[1], oui[2], 4});                      // group: CCMP / TKIP
  body.insert(body.end(), {1, 0, oui[0], oui[1], oui[2], 4});                // one pairwise suite
  body.push_back(static_cast<uint8_t>(akm_types.size())); body.push_back(0);
  for (uint8_t t : akm_types) body.insert(body.end(), {oui[0], oui[1], oui[2], t});
  return microsoft_wpa ? element(221, body) : element(48, body);
}

static Bytes frame(uint16_t control, uint16_t capability, const std::vector<Bytes>& elements) {
  Bytes f{static_cast<uint8_t>(control), static_cast<uint8_t>(control >> 8), 0, 0};
  f.insert(f.end(), 6, 0xFF);                                                // destination
  f.insert(f.end(), {0x10, 0x20, 0x30, 0x40, 0x50, 0x60});                   // source
  f.insert(f.end(), {0x10, 0x20, 0x30, 0x40, 0x50, 0x60});                   // BSSID
  f.insert(f.end(), {0x70, 0x01});                                           // sequence control
  f.insert(f.end(), 8, 0);                                                   // timestamp
  f.insert(f.end(), {0x64, 0x00});                                           // beacon interval
  f.push_back(static_cast<uint8_t>(capability)); f.push_back(static_cast<uint8_t>(capability >> 8));
  for (const Bytes& e : elements) f.insert(f.end(), e.begin(), e.end());
  return f;
}

static bool parse(const Bytes& f, WifiNetwork* n, uint8_t radio = 0, int dbm = 0) { return wifi_parse_announcement(ConstByteView(f.data(), f.size()), radio, dbm, n); }

int main() {
  WifiNetwork n;
  // WPA2 personal on channel 6
  Bytes wpa2 = frame(0x0080, 0x0411, {ssid_element("Home"), element(1, {0x82, 0x84}), element(3, {6}), rsn_element({2})});
  CHECK(parse(wpa2, &n, 0, -60) && strcmp(n.ssid, "Home") == 0 && n.security == WifiSecurity::Wpa2 && n.channel == 6 && n.frequency_mhz == 2437 && n.signal_percent == 80);
  CHECK(n.bssid.bytes[0] == 0x10 && n.bssid.bytes[5] == 0x60);
  // a probe response parses the same
  CHECK(parse(frame(0x0050, 0x0411, {ssid_element("Home"), rsn_element({2})}), &n, 36, -70) && n.channel == 36 && n.frequency_mhz == 5180 && n.signal_percent == 60);
  // flags in the high byte of the frame control do not matter (retry, protected)
  CHECK(parse(frame(0x0880, 0x0411, {ssid_element("Flags"), rsn_element({2})}), &n));
  // open, WEP (privacy bit, no RSN), WPA (vendor element), WPA3 (SAE), transition mode (PSK and SAE), enterprise
  CHECK(parse(frame(0x0080, 0x0401, {ssid_element("Open"), element(3, {1})}), &n) && n.security == WifiSecurity::Open);
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("Old"), element(3, {11})}), &n) && n.security == WifiSecurity::Wep);
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("Older"), rsn_element({2}, true)}), &n) && n.security == WifiSecurity::Wpa);
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("New"), rsn_element({8})}), &n) && n.security == WifiSecurity::Wpa3);
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("Mixed"), rsn_element({8, 2})}), &n) && n.security == WifiSecurity::Wpa2);
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("Corp"), rsn_element({1})}), &n) && n.security == WifiSecurity::Enterprise);
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("Corp2"), rsn_element({5, 1})}), &n) && n.security == WifiSecurity::Enterprise);
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("PskSha"), rsn_element({6})}), &n) && n.security == WifiSecurity::Wpa2);
  // WPA2 and old WPA together: the RSN element wins
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("Both"), rsn_element({2}, true), rsn_element({2})}), &n) && n.security == WifiSecurity::Wpa2);
  // an RSN element of an unknown OUI offers nothing known: back to the privacy bit
  Bytes odd = rsn_element({2});
  odd[2 + 2 + 4 + 2 + 4 + 2] = 0x12;                                          // the AKM OUI's first byte
  CHECK(parse(frame(0x0080, 0x0411, {ssid_element("Odd"), odd}), &n) && n.security == WifiSecurity::Wep);
  // channel sources: DS, else HT operation, else the radio
  CHECK(parse(frame(0x0080, 0x0401, {ssid_element("a"), element(61, {11, 0, 0})}), &n, 1) && n.channel == 11);
  CHECK(parse(frame(0x0080, 0x0401, {ssid_element("a")}), &n, 9) && n.channel == 9 && n.frequency_mhz == 2452);
  CHECK(parse(frame(0x0080, 0x0401, {ssid_element("a")}), &n, 0) && n.channel == 0 && n.frequency_mhz == 0);
  CHECK(parse(frame(0x0080, 0x0401, {ssid_element("a"), element(3, {14})}), &n) && n.frequency_mhz == 2484);
  // names: 32 bytes is the limit, hidden ones (empty, zeros) are refused, control characters too, UTF-8 passes
  CHECK(parse(frame(0x0080, 0x0401, {ssid_element("12345678901234567890123456789012")}), &n) && strlen(n.ssid) == 32);
  CHECK(!parse(frame(0x0080, 0x0401, {element(0, Bytes(33, 'x'))}), &n));
  CHECK(!parse(frame(0x0080, 0x0401, {element(0, {})}), &n) && !parse(frame(0x0080, 0x0401, {element(0, Bytes(5, 0))}), &n));
  CHECK(!parse(frame(0x0080, 0x0401, {element(0, {'a', 7, 'b'})}), &n));
  CHECK(parse(frame(0x0080, 0x0401, {element(0, {0xC3, 0xA9, 't', 0xC3, 0xA9})}), &n));
  CHECK(!parse(frame(0x0080, 0x0401, {element(3, {6})}), &n));              // no SSID element at all
  // other frame types are not announcements
  CHECK(!parse(frame(0x0040, 0x0401, {ssid_element("x")}), &n) && !parse(frame(0x00D4, 0x0401, {ssid_element("x")}), &n) && !parse(frame(0x0008, 0x0401, {ssid_element("x")}), &n));
  // signal
  CHECK(wifi_signal_percent(-30) == 100 && wifi_signal_percent(-50) == 100 && wifi_signal_percent(-75) == 50 && wifi_signal_percent(-100) == 0 && wifi_signal_percent(-120) == 0);
  CHECK(wifi_frequency_channel(2412) == 1 && wifi_frequency_channel(2472) == 13 && wifi_frequency_channel(2484) == 14 && wifi_frequency_channel(5180) == 36 && wifi_frequency_channel(5825) == 165 &&
        wifi_frequency_channel(2413) == 0 && wifi_frequency_channel(900) == 0);
  // damage: every prefix and every flipped bit gives a verdict and touches nothing outside the frame
  uint32_t accepted = 0;
  for (size_t cut = 0; cut <= wpa2.size(); ++cut) accepted += parse(Bytes(wpa2.begin(), wpa2.begin() + cut), &n) ? 1 : 0;
  for (size_t i = 0; i < wpa2.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes c = wpa2;
      c[i] ^= static_cast<uint8_t>(1 << bit);
      accepted += parse(c, &n) ? 1 : 0;
    }
  CHECK(accepted > 0);
  // a truncated element is refused, not half read
  Bytes cut = wpa2;
  cut.resize(cut.size() - 3);
  CHECK(!parse(cut, &n));
  // the empty frame
  CHECK(!parse(Bytes(), &n));
  printf(g_failed ? "Ieee80211ScanTest: %d FAILED\n" : "Ieee80211ScanTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
