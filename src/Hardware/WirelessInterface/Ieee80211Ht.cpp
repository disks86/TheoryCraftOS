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

#include "Hardware/WirelessInterface/Ieee80211Ht.hpp"
#include "Hardware/WirelessInterface/WirelessTypes.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint8_t kElementHtCapabilities = 45, kElementHtOperation = 61, kElementVhtCapabilities = 191, kElementVhtOperation = 192, kElementVendor = 221;
constexpr uint16_t kHtCap40Mhz = 1u << 1, kHtCapSgi20 = 1u << 5, kHtCapSgi40 = 1u << 6;
constexpr uint32_t kVhtCapSgi80 = 1u << 5;

// bits per OFDM symbol per data subcarrier, times 6 (modulation bits times coding rate: 1/2 BPSK is 3, 5/6 256-QAM is 40), for MCS 0-9
constexpr uint32_t kCodedBitsTimes6[10] = {3, 6, 9, 12, 18, 24, 27, 30, 36, 40};

bool is_announcement(uint16_t control) {
  const uint16_t kind = control & 0x00FC;
  return kind == 0x0080 || kind == 0x0050;
}

// A VHT MCS map holds two bits per stream; the highest MCS a stream carries, or -1 for none.
int32_t vht_stream_max_mcs(uint16_t map, uint32_t stream) {
  const uint32_t code = (map >> (2 * stream)) & 3;
  return code == 3 ? -1 : static_cast<int32_t>(7 + code);
}

void add_rate(WifiRateLadder* ladder, const WifiRate& rate) {
  if (ladder->count < kWifiRateLadderMax && rate.kbps != 0) ladder->rate[ladder->count++] = rate;
}
}  // namespace

bool wifi_parse_phy_elements(ConstByteView elements, WifiPhyCapabilities* out) {
  ByteReader r(elements);
  WifiPhyCapabilities c;
  while (r.remaining() >= 2) {
    const uint8_t id = r.u8();
    const uint8_t length = r.u8();
    ConstByteView body = r.take(length);
    if (!r.ok()) return false;
    if (id == kElementHtCapabilities && length >= 26) {
      ByteReader b(body);
      c.ht_cap_info = b.le16();
      c.ampdu_params = b.u8();
      for (uint32_t i = 0; i < 4; ++i) c.rx_mcs[i] = b.u8();
      c.ht = b.ok();
    } else if (id == kElementHtOperation && length >= 2) {
      c.ht_primary_channel = body[0];
      c.ht_secondary = body[1] & 3;
      c.ht_wide = (body[1] & 4) != 0;
    } else if (id == kElementVhtCapabilities && length >= 12) {
      ByteReader b(body);
      c.vht_cap_info = b.le32();
      c.vht_rx_mcs_map = b.le16();
      c.vht = b.ok();
    } else if (id == kElementVhtOperation && length >= 3) {
      c.vht_width = body[0];
      c.vht_center0 = body[1];
    } else if (id == kElementVendor) {
      // (vendor specific elements say nothing about the PHY here)
    }
  }
  *out = c;
  return true;
}

bool wifi_parse_phy_capabilities(ConstByteView frame, WifiPhyCapabilities* out) {
  ByteReader r(frame);
  const uint16_t control = r.le16();
  if (!r.ok() || !is_announcement(control)) return false;
  r.skip(24 - 2 + 8 + 2 + 2);                      // the rest of the header, timestamp, beacon interval, capability
  if (!r.ok()) return false;
  return wifi_parse_phy_elements(r.rest(), out);
}

uint8_t WifiPhyNegotiation::control_position() const {
  const int32_t offset = static_cast<int32_t>(primary_mhz) - static_cast<int32_t>(center_mhz);
  if (offset == 0 || width_mhz == 20) return 0;
  const uint32_t magnitude = static_cast<uint32_t>(offset < 0 ? -offset : offset);
  uint32_t position = magnitude >= 10 ? (magnitude - 10) / 20 : 0;       // 0-7
  position = (position & 3) | ((position & 4) << 1);
  if (offset > 0) position |= 4;
  return static_cast<uint8_t>(position);
}

WifiPhyNegotiation wifi_negotiate_phy(const WifiPhyCapabilities& ap, uint8_t primary_channel, uint32_t our_streams) {
  WifiPhyNegotiation n;
  n.our_streams = static_cast<uint8_t>(our_streams < 1 ? 1 : our_streams > 2 ? 2 : our_streams);
  n.primary_channel = primary_channel;
  n.primary_mhz = wifi_channel_frequency_mhz(primary_channel);
  n.center_mhz = n.primary_mhz;
  if (!ap.ht || n.primary_mhz == 0) return n;
  const bool five_ghz = primary_channel > 14;
  n.ht = true;
  n.vht = ap.vht && five_ghz && vht_stream_max_mcs(ap.vht_rx_mcs_map, 0) >= 0;
  // streams and the highest MCS of one stream
  uint32_t ap_streams = 0;
  if (n.vht) {
    n.max_mcs = 9;
    for (uint32_t s = 0; s < 2; ++s) {
      const int32_t m = vht_stream_max_mcs(ap.vht_rx_mcs_map, s);
      if (m < 0) break;
      ++ap_streams;
      if (m < n.max_mcs) n.max_mcs = static_cast<uint8_t>(m);
    }
  } else {
    n.max_mcs = 7;
    for (uint32_t s = 0; s < 2 && ap.rx_mcs[s] != 0; ++s) ++ap_streams;
  }
  if (ap_streams == 0) ap_streams = 1;
  n.streams = static_cast<uint8_t>(ap_streams < n.our_streams ? ap_streams : n.our_streams);
  // width
  n.width_mhz = 20;
  if (n.vht && ap.vht_width >= 1 && ap.vht_center0 != 0) {
    const uint16_t center = static_cast<uint16_t>(5000 + 5u * ap.vht_center0);
    const int32_t away = static_cast<int32_t>(center) - static_cast<int32_t>(n.primary_mhz);
    if (away == -30 || away == -10 || away == 10 || away == 30) {          // the 80 MHz segment has the primary channel in it
      n.width_mhz = 80;
      n.center_mhz = center;
    }
  }
  if (n.width_mhz == 20 && ap.ht_wide && (ap.ht_cap_info & kHtCap40Mhz) && (ap.ht_secondary == 1 || ap.ht_secondary == 3)) {
    n.width_mhz = 40;
    n.center_mhz = static_cast<uint16_t>(ap.ht_secondary == 1 ? n.primary_mhz + 10 : n.primary_mhz - 10);
  }
  n.short_gi = n.width_mhz == 80 ? (ap.vht_cap_info & kVhtCapSgi80) != 0 : n.width_mhz == 40 ? (ap.ht_cap_info & kHtCapSgi40) != 0 : (ap.ht_cap_info & kHtCapSgi20) != 0;
  n.ampdu_exponent = static_cast<uint8_t>(n.vht ? (ap.vht_cap_info >> 23) & 7 : ap.ampdu_params & 3);
  n.ampdu_density = static_cast<uint8_t>((ap.ampdu_params >> 2) & 7);
  return n;
}

uint32_t wifi_build_phy_elements(const WifiPhyNegotiation& n, ByteView out) {
  if (!n.ht) return 0;
  ByteWriter w(out);
  // WMM information element: version 1, no U-APSD
  const uint8_t wmm[] = {kElementVendor, 7, 0x00, 0x50, 0xF2, 2, 0, 1, 0};
  w.bytes(ConstByteView(wmm, sizeof wmm));
  // HT Capabilities (26 bytes): 40 MHz, spatial multiplexing power save disabled, short GI on both widths; our streams' MCS 0-7 each
  w.u8(kElementHtCapabilities);
  w.u8(26);
  w.le16(kHtCap40Mhz | (3u << 2) | kHtCapSgi20 | kHtCapSgi40);
  w.u8(0);                                                  // A-MPDU parameters: nothing we need to receive aggregated
  for (uint32_t s = 0; s < 4; ++s) w.u8(s < n.our_streams ? 0xFF : 0x00);        // Rx MCS bitmask, bytes 0-3
  w.zeros(6);                                               // bitmask bytes 4-9
  w.le16(0);                                                // highest supported rate: not stated
  w.u8(1);                                                  // Tx MCS set defined, equal to Rx
  w.zeros(3);
  w.le16(0);                                                // HT extended capabilities
  w.le32(0);                                                // transmit beamforming
  w.u8(0);                                                  // antenna selection
  if (n.vht) {
    w.u8(kElementVhtCapabilities);
    w.u8(12);
    w.le32(kVhtCapSgi80);                                   // 3895 byte MPDUs, 80 MHz, short GI 80, nothing else
    uint16_t map = 0;
    for (uint32_t s = 0; s < 8; ++s) map |= static_cast<uint16_t>((s < n.our_streams ? 2u : 3u) << (2 * s));     // MCS 0-9 for our streams, none for the others
    w.le16(map);
    w.le16(0);                                              // highest Rx rate: not stated
    w.le16(map);
    w.le16(0);
  }
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t wifi_mcs_kbps(uint32_t mcs, uint32_t streams, uint32_t width_mhz, bool short_gi) {
  if (mcs > 9 || streams < 1 || streams > 2) return 0;
  const uint32_t subcarriers = width_mhz == 20 ? 52 : width_mhz == 40 ? 108 : width_mhz == 80 ? 234 : 0;
  if (subcarriers == 0) return 0;
  if (mcs == 9 && width_mhz == 20) return 0;                // not defined for one or two streams
  // bits per symbol * (1000 / 4 us); a short guard interval makes the symbol 3.6 us
  const uint64_t base = uint64_t{subcarriers} * kCodedBitsTimes6[mcs] * streams * 250 / 6;
  return static_cast<uint32_t>(short_gi ? base * 10 / 9 : base);
}

void wifi_build_rate_ladder(const WifiPhyNegotiation& n, WifiRateLadder* out) {
  WifiRateLadder ladder;
  WifiRate base;
  base.mode = WifiRate::Mode::Legacy;
  base.kbps = 6000;
  add_rate(&ladder, base);
  uint32_t single_top = 0;
  if (n.ht) {
    for (uint32_t mcs = 0; mcs <= n.max_mcs; ++mcs) {
      WifiRate rate;
      rate.mode = n.vht ? WifiRate::Mode::Vht : WifiRate::Mode::Ht;
      rate.mcs = static_cast<uint8_t>(mcs);
      rate.streams = 1;
      rate.width_mhz = n.width_mhz;
      rate.short_gi = n.short_gi;
      rate.kbps = wifi_mcs_kbps(mcs, 1, n.width_mhz, n.short_gi);
      if (rate.kbps > single_top) single_top = rate.kbps;
      add_rate(&ladder, rate);
    }
    ladder.mimo_first = ladder.count;
    if (n.streams >= 2) {
      for (uint32_t mcs = 0; mcs <= n.max_mcs; ++mcs) {
        WifiRate rate;
        rate.mode = n.vht ? WifiRate::Mode::Vht : WifiRate::Mode::Ht;
        rate.mcs = static_cast<uint8_t>(n.vht ? mcs : 8 + mcs);
        rate.streams = 2;
        rate.width_mhz = n.width_mhz;
        rate.short_gi = n.short_gi;
        rate.kbps = wifi_mcs_kbps(mcs, 2, n.width_mhz, n.short_gi);
        if (rate.kbps > single_top) add_rate(&ladder, rate);            // the two stream rates that go beyond the single stream ones
      }
    }
  } else {
    ladder.mimo_first = ladder.count;
  }
  *out = ladder;
}

}  // namespace hardware
