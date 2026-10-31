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

// 802.11n/ac: reading an access point's HT / VHT elements from a beacon (and surviving truncated and bit-flipped ones), choosing streams, width and guard interval, the control
// channel position the firmware wants, the elements of our association request, the MCS bit rates and the rate ladder.
#include <stdint.h>
#include <stdio.h>
#include "Hardware/WirelessInterface/Ieee80211Ht.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Beacon {
  uint8_t bytes[400];
  uint32_t length = 0;
  void put(uint8_t b) { bytes[length++] = b; }
  void element(uint8_t id, const uint8_t* body, uint32_t n) {
    put(id);
    put(static_cast<uint8_t>(n));
    for (uint32_t i = 0; i < n; ++i) put(body[i]);
  }
  ConstByteView view() const { return ConstByteView(bytes, length); }
};

static Beacon beacon_start() {
  Beacon b;
  b.put(0x80); b.put(0);
  for (int i = 0; i < 22; ++i) b.put(i == 4 ? 0xFF : 0);      // header: broadcast destination; the rest does not matter here
  for (int i = 0; i < 12; ++i) b.put(0);                        // timestamp, interval, capability
  const uint8_t ssid[] = {'t', 'e', 's', 't'};
  b.element(0, ssid, 4);
  return b;
}

static void add_ht(Beacon& b, uint16_t cap_info, uint8_t streams, uint8_t primary, uint8_t secondary_and_width) {
  uint8_t cap[26] = {};
  cap[0] = static_cast<uint8_t>(cap_info);
  cap[1] = static_cast<uint8_t>(cap_info >> 8);
  cap[2] = 0x17;                                                // A-MPDU: exponent 3, spacing 5
  for (uint32_t s = 0; s < streams; ++s) cap[3 + s] = 0xFF;
  b.element(45, cap, 26);
  uint8_t op[22] = {};
  op[0] = primary;
  op[1] = secondary_and_width;
  b.element(61, op, 22);
}

static void add_vht(Beacon& b, uint32_t cap_info, uint16_t rx_map, uint8_t width, uint8_t center0) {
  uint8_t cap[12] = {};
  for (int i = 0; i < 4; ++i) cap[i] = static_cast<uint8_t>(cap_info >> (8 * i));
  cap[4] = static_cast<uint8_t>(rx_map);
  cap[5] = static_cast<uint8_t>(rx_map >> 8);
  b.element(191, cap, 12);
  const uint8_t op[5] = {width, center0, 0, 0xFC, 0xFF};
  b.element(192, op, 5);
}

static void test_parse() {
  Beacon b = beacon_start();
  add_ht(b, 0x006E | 0x0001, 2, 36, 0x05);                      // 40 MHz capable, short GI on both, 2 streams; secondary above, any width
  add_vht(b, (1u << 5) | (2u << 23), 0xFFFA, 1, 42);
  WifiPhyCapabilities c;
  CHECK(wifi_parse_phy_capabilities(b.view(), &c));
  CHECK(c.ht && c.vht);
  CHECK(c.ht_cap_info == 0x006F && c.ampdu_params == 0x17 && c.rx_mcs[0] == 0xFF && c.rx_mcs[1] == 0xFF && c.rx_mcs[2] == 0);
  CHECK(c.ht_primary_channel == 36 && c.ht_secondary == 1 && c.ht_wide);
  CHECK(c.vht_cap_info == ((1u << 5) | (2u << 23)) && c.vht_rx_mcs_map == 0xFFFA && c.vht_width == 1 && c.vht_center0 == 42);

  Beacon none = beacon_start();                                  // an access point without HT
  CHECK(wifi_parse_phy_capabilities(none.view(), &c) && !c.ht && !c.vht);

  Beacon not_beacon = beacon_start();
  not_beacon.bytes[0] = 0x08;                                    // a data frame
  CHECK(!wifi_parse_phy_capabilities(not_beacon.view(), &c));

  // an HT element that is too short says nothing; one that runs past the frame damages the frame
  Beacon shortcap = beacon_start();
  const uint8_t small[10] = {};
  shortcap.element(45, small, 10);
  CHECK(wifi_parse_phy_capabilities(shortcap.view(), &c) && !c.ht);
  Beacon cut = b;
  cut.length -= 3;
  CHECK(!wifi_parse_phy_capabilities(cut.view(), &c));
}

static void test_robust() {
  Beacon b = beacon_start();
  add_ht(b, 0x006F, 2, 36, 0x05);
  add_vht(b, 0x20, 0xFFFA, 1, 42);
  WifiPhyCapabilities c;
  for (uint32_t n = 0; n <= b.length; ++n) {                    // every truncation: never reads past the end
    Beacon t = b;
    t.length = n;
    wifi_parse_phy_capabilities(t.view(), &c);
  }
  uint32_t accepted = 0;
  for (uint32_t byte = 0; byte < b.length; ++byte)
    for (int bit = 0; bit < 8; ++bit) {                         // every single bit flip
      Beacon f = b;
      f.bytes[byte] ^= static_cast<uint8_t>(1u << bit);
      if (wifi_parse_phy_capabilities(f.view(), &c)) {
        ++accepted;
        const WifiPhyNegotiation n = wifi_negotiate_phy(c, 36, 2);       // whatever it said, negotiating must stay sane
        CHECK(n.streams >= 1 && n.streams <= 2 && (n.width_mhz == 20 || n.width_mhz == 40 || n.width_mhz == 80) && n.max_mcs <= 9);
        WifiRateLadder ladder;
        wifi_build_rate_ladder(n, &ladder);
        CHECK(ladder.count >= 1 && ladder.count <= kWifiRateLadderMax);
        uint8_t out[kWifiPhyElementsMax];
        CHECK(wifi_build_phy_elements(n, ByteView(out, sizeof out)) <= kWifiPhyElementsMax);
      }
    }
  CHECK(accepted > 0);
}

static void test_negotiate() {
  WifiPhyCapabilities c;
  {   // 5 GHz, VHT 80 MHz, two streams, short GI, the primary channel 36 in the segment centred on 42
    Beacon b = beacon_start();
    add_ht(b, 0x006F, 2, 36, 0x05);
    add_vht(b, (1u << 5) | (3u << 23), 0xFFFA, 1, 42);
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    const WifiPhyNegotiation n = wifi_negotiate_phy(c, 36, 2);
    CHECK(n.ht && n.vht && n.streams == 2 && n.width_mhz == 80 && n.short_gi && n.max_mcs == 9);
    CHECK(n.primary_mhz == 5180 && n.center_mhz == 5210 && n.control_position() == 1);
    CHECK(n.ampdu_exponent == 3 && n.ampdu_density == 5);
    const WifiPhyNegotiation one = wifi_negotiate_phy(c, 36, 1);          // a radio with one chain
    CHECK(one.streams == 1 && one.our_streams == 1);
  }
  {   // the primary channel above the centre: 44 in the segment of 42 -> offset +10 -> above, position 0
    Beacon b = beacon_start();
    add_ht(b, 0x006F, 2, 44, 0x05);
    add_vht(b, 0x20, 0xFFFA, 1, 42);
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    WifiPhyNegotiation n = wifi_negotiate_phy(c, 44, 2);
    CHECK(n.width_mhz == 80 && n.control_position() == 4);
    n = wifi_negotiate_phy(c, 48, 2);                                      // 48 is +30 from 42
    CHECK(n.width_mhz == 80 && n.control_position() == 5);
    n = wifi_negotiate_phy(c, 100, 2);                                     // a channel far from the segment: not 80 MHz
    CHECK(n.width_mhz != 80);
  }
  {   // VHT with one stream and MCS 0-8 only
    Beacon b = beacon_start();
    add_ht(b, 0x006F, 1, 149, 0x05);
    add_vht(b, 0x20, 0xFFFD, 1, 155);                                       // stream 1: code 1 = MCS 0-8
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    const WifiPhyNegotiation n = wifi_negotiate_phy(c, 149, 2);
    CHECK(n.vht && n.streams == 1 && n.max_mcs == 8 && n.width_mhz == 80 && n.center_mhz == 5775);
  }
  {   // 2.4 GHz: HT only (VHT is not used there), 40 MHz with the secondary channel below
    Beacon b = beacon_start();
    add_ht(b, 0x006F, 2, 11, 0x07);                                         // below, wide
    add_vht(b, 0x20, 0xFFFA, 1, 0);
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    const WifiPhyNegotiation n = wifi_negotiate_phy(c, 11, 2);
    CHECK(n.ht && !n.vht && n.streams == 2 && n.width_mhz == 40 && n.primary_mhz == 2462 && n.center_mhz == 2452 && n.max_mcs == 7);
    CHECK(n.control_position() == 4);                                       // primary is +10 from the centre
  }
  {   // 20 MHz in use although the card could do 40: the operation element says narrow
    Beacon b = beacon_start();
    add_ht(b, 0x006F, 2, 6, 0x01);
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    const WifiPhyNegotiation n = wifi_negotiate_phy(c, 6, 2);
    CHECK(n.ht && n.width_mhz == 20 && n.control_position() == 0 && n.center_mhz == n.primary_mhz);
  }
  {   // no HT in the access point: nothing is used
    Beacon b = beacon_start();
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    const WifiPhyNegotiation n = wifi_negotiate_phy(c, 6, 2);
    CHECK(!n.ht && !n.vht);
    uint8_t out[kWifiPhyElementsMax];
    CHECK(wifi_build_phy_elements(n, ByteView(out, sizeof out)) == 0);
    WifiRateLadder ladder;
    wifi_build_rate_ladder(n, &ladder);
    CHECK(ladder.count == 1 && ladder.rate[0].kbps == 6000);
  }
}

static void test_elements() {
  WifiPhyCapabilities c;
  Beacon b = beacon_start();
  add_ht(b, 0x006F, 2, 36, 0x05);
  add_vht(b, 0x20, 0xFFFA, 1, 42);
  CHECK(wifi_parse_phy_capabilities(b.view(), &c));
  WifiPhyNegotiation n = wifi_negotiate_phy(c, 36, 2);
  uint8_t out[kWifiPhyElementsMax];
  uint32_t length = wifi_build_phy_elements(n, ByteView(out, sizeof out));
  CHECK(length == 9 + 28 + 14);
  CHECK(out[0] == 221 && out[1] == 7 && out[2] == 0x00 && out[3] == 0x50 && out[4] == 0xF2 && out[5] == 2);        // WMM
  CHECK(out[9] == 45 && out[10] == 26 && out[37] == 191 && out[38] == 12);
  // what we send, read back as an access point's elements: two streams of MCS 0-7, 40 MHz, short GI, VHT MCS 0-9 for two streams
  WifiPhyCapabilities back;
  CHECK(wifi_parse_phy_elements(ConstByteView(out, length), &back));
  CHECK(back.ht && back.vht && back.rx_mcs[0] == 0xFF && back.rx_mcs[1] == 0xFF && back.rx_mcs[2] == 0 && (back.ht_cap_info & 0x62) == 0x62);
  CHECK(back.vht_rx_mcs_map == 0xFFFA && (back.vht_cap_info & 0x20));
  // one chain: one stream in both
  n = wifi_negotiate_phy(c, 36, 1);
  length = wifi_build_phy_elements(n, ByteView(out, sizeof out));
  CHECK(wifi_parse_phy_elements(ConstByteView(out, length), &back) && back.rx_mcs[0] == 0xFF && back.rx_mcs[1] == 0 && back.vht_rx_mcs_map == 0xFFFE);
  // without VHT: HT only
  Beacon h = beacon_start();
  add_ht(h, 0x006F, 2, 6, 0x01);
  CHECK(wifi_parse_phy_capabilities(h.view(), &c));
  n = wifi_negotiate_phy(c, 6, 2);
  CHECK(wifi_build_phy_elements(n, ByteView(out, sizeof out)) == 9 + 28);
  // too small an output is refused
  uint8_t small[20];
  CHECK(wifi_build_phy_elements(n, ByteView(small, sizeof small)) == 0);
}

static void test_rates() {
  CHECK(wifi_mcs_kbps(0, 1, 20, false) == 6500 && wifi_mcs_kbps(7, 1, 20, false) == 65000 && wifi_mcs_kbps(7, 1, 20, true) == 72222);
  CHECK(wifi_mcs_kbps(7, 1, 40, false) == 135000 && wifi_mcs_kbps(7, 2, 40, true) == 300000);
  CHECK(wifi_mcs_kbps(9, 1, 80, false) == 390000 && wifi_mcs_kbps(9, 2, 80, false) == 780000 && wifi_mcs_kbps(9, 2, 80, true) == 866666);
  CHECK(wifi_mcs_kbps(8, 1, 20, false) == 78000 && wifi_mcs_kbps(9, 1, 20, false) == 0);
  CHECK(wifi_mcs_kbps(10, 1, 20, false) == 0 && wifi_mcs_kbps(0, 3, 20, false) == 0 && wifi_mcs_kbps(0, 1, 160, false) == 0);

  WifiPhyCapabilities c;
  {   // VHT 80 MHz, two streams, short GI: legacy 6, MCS 0-9 of one stream, then the two stream ones beyond 433 Mbit/s
    Beacon b = beacon_start();
    add_ht(b, 0x006F, 2, 36, 0x05);
    add_vht(b, 0x20, 0xFFFA, 1, 42);
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    const WifiPhyNegotiation n = wifi_negotiate_phy(c, 36, 2);
    WifiRateLadder l;
    wifi_build_rate_ladder(n, &l);
    CHECK(l.rate[0].mode == WifiRate::Mode::Legacy && l.rate[0].kbps == 6000);
    CHECK(l.rate[1].mode == WifiRate::Mode::Vht && l.rate[1].mcs == 0 && l.rate[1].streams == 1 && l.rate[1].width_mhz == 80 && l.rate[1].short_gi);
    bool increasing = true;
    for (uint32_t i = 1; i < l.count; ++i) increasing = increasing && l.rate[i].kbps > l.rate[i - 1].kbps;
    CHECK(increasing);
    CHECK(l.mimo_first == 11 && l.rate[l.mimo_first].streams == 2 && l.rate[l.count - 1].kbps == 866666 && l.rate[l.count - 1].mcs == 9);
    CHECK(l.rate[l.mimo_first - 1].streams == 1 && l.rate[l.mimo_first - 1].kbps == 433333);
  }
  {   // HT 20 MHz, two streams, long GI: single stream MCS 0-7, then the MCS 8-15 above 65 Mbit/s
    Beacon b = beacon_start();
    add_ht(b, 0x0001, 2, 6, 0x00);
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    const WifiPhyNegotiation n = wifi_negotiate_phy(c, 6, 2);
    WifiRateLadder l;
    wifi_build_rate_ladder(n, &l);
    CHECK(l.rate[1].mode == WifiRate::Mode::Ht && l.rate[1].mcs == 0 && l.rate[1].kbps == 6500 && !l.rate[1].short_gi);
    CHECK(l.mimo_first == 9 && l.rate[9].mcs == 12 && l.rate[9].kbps == 78000 && l.rate[l.count - 1].mcs == 15 && l.rate[l.count - 1].kbps == 130000);
    bool increasing = true;
    for (uint32_t i = 1; i < l.count; ++i) increasing = increasing && l.rate[i].kbps > l.rate[i - 1].kbps;
    CHECK(increasing && l.count == 13);
  }
  {   // one stream only: no two stream rates
    Beacon b = beacon_start();
    add_ht(b, 0x0001, 1, 6, 0x00);
    CHECK(wifi_parse_phy_capabilities(b.view(), &c));
    WifiRateLadder l;
    wifi_build_rate_ladder(wifi_negotiate_phy(c, 6, 2), &l);
    CHECK(l.count == 9 && l.mimo_first == 9);
  }
}

int main() {
  test_parse();
  test_robust();
  test_negotiate();
  test_elements();
  test_rates();
  printf(g_failed ? "Ieee80211HtTest: %d FAILED\n" : "Ieee80211HtTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
