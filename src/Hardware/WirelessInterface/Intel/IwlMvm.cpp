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


#include "Hardware/WirelessInterface/Intel/IwlMvm.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace iwl_mvm;

namespace {
constexpr uint32_t kRxDescriptorBytes = 48;        // IWL_RX_DESC_SIZE_V1
constexpr uint32_t kStatusCrcOk = 1u << 0, kStatusOverrunOk = 1u << 1;
constexpr uint8_t kMacFlags2Pad = 0x20;            // IWL_RX_MPDU_MFLG2_PAD: two bytes of padding after the 802.11 header
}  // namespace

uint32_t iwl_build_tx_ant_config(uint32_t valid_antennas, ByteView out) {
  ByteWriter w(out);
  w.le32(valid_antennas);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t iwl_build_dqa_enable(uint32_t command_queue, ByteView out) {
  ByteWriter w(out);
  w.le32(command_queue);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t iwl_build_add_aux_station(uint8_t station_id, uint32_t queue_mask, ByteView out) {
  ByteWriter w(out);
  w.u8(0);                                         // add_modify: add
  w.u8(0);                                         // awake_acs
  w.le16(0xFFFF);                                  // tid_disable_tx
  w.le32(kMacIndexAux);                            // mac_id_n_color: the lmac index for the old station API (id | color << 8)
  w.zeros(6);                                      // addr
  w.le16(0);
  w.u8(station_id);
  w.u8(0);                                         // modify_mask
  w.le16(0);
  w.le32(0);                                       // station_flags
  w.le32(0);                                       // station_flags_msk
  w.u8(0);                                         // add_immediate_ba_tid
  w.u8(0);
  w.le16(0);
  w.le16(0);                                       // sleep_tx_count
  w.u8(0);                                         // sleep_state_flags
  w.u8(kStationTypeAuxActivity);
  w.le16(0);                                       // assoc_id
  w.le16(0);                                       // beamform_flags
  w.le32(queue_mask);
  w.le16(0);                                       // rx_ba_window
  w.u8(0);
  w.u8(0);
  return w.ok() && w.position() == kAddStaBytes ? kAddStaBytes : 0;
}

uint32_t iwl_build_scan_config(uint8_t aux_station_id, uint32_t tx_antennas, uint32_t rx_antennas, ByteView out) {
  ByteWriter w(out);
  w.u8(0);                                         // enable_cam_mode
  w.u8(0);                                         // enable_promiscouos_mode
  w.u8(aux_station_id);
  w.u8(0);
  w.le32(tx_antennas);
  w.le32(rx_antennas);
  return w.ok() && w.position() == kScanConfigBytes ? kScanConfigBytes : 0;
}

uint32_t iwl_build_station_mac_context(const IwlMacContext& c, ByteView out) {
  ByteWriter w(out);
  w.le32(static_cast<uint32_t>(c.id) | (static_cast<uint32_t>(c.color) << 8));
  w.le32(c.action);
  w.le32(kMacTypeBssStation);
  w.le32(0);                                       // tsf_id: TSF A
  for (uint32_t i = 0; i < 6; ++i) w.u8(c.address[i]);
  w.le16(0);
  for (uint32_t i = 0; i < 6; ++i) w.u8(0xFF);     // no BSSID yet: broadcast
  w.le16(0);
  w.le32(0x0F);                                    // cck_rates: the four mandatory ones
  w.le32(0x15);                                    // ofdm_rates: 6, 12 and 24 Mbit/s
  w.le32(0);                                       // protection_flags
  w.le32(0);                                       // cck_short_preamble
  w.le32(0);                                       // short_slot
  w.le32((1u << 2) | (1u << 6));                   // filter_flags: not associated, so group frames (MAC_FILTER_ACCEPT_GRP) and other networks' beacons (MAC_FILTER_IN_BEACON)
  w.le32(0);                                       // qos_flags
  // the four access categories in the firmware's order (background, best effort, video, voice) and the spare entry
  struct Ac { uint16_t cw_min, cw_max; uint8_t aifsn, fifo; uint16_t txop; };
  const Ac acs[5] = {{15, 1023, 7, 1u << 0, 0}, {15, 1023, 3, 1u << 1, 0}, {7, 15, 2, 1u << 2, 3008 / 32}, {3, 7, 2, 1u << 3, 1504 / 32}, {0, 0, 0, 0, 0}};
  for (const Ac& a : acs) {
    w.le16(a.cw_min);
    w.le16(a.cw_max);
    w.u8(a.aifsn);
    w.u8(a.fifo);
    w.le16(a.txop);
  }
  // the station data: not associated (is_assoc 0), the rest zero
  w.zeros(48);
  if (!w.ok() || w.position() != kMacContextBytes) return 0;
  return kMacContextBytes;
}

uint32_t iwl_scan_request_bytes(uint32_t slots) { return kScanRequestFixedBytes + kScanChannelConfigBytes * slots + kScanRequestTailBytes; }

uint32_t iwl_build_scan_request(const IwlScanRequest& r, ByteView out) {
  if (r.channel_count == 0 || r.channel_count > r.firmware_channel_slots || !r.channels) return 0;
  const uint32_t total = iwl_scan_request_bytes(r.firmware_channel_slots);
  if (out.size() < total) return 0;
  out.subview(0, total).fill(0);
  ByteWriter w(out.subview(0, total));
  // header
  w.le32(0);                                       // flags
  w.le32(r.uid);
  w.le32(kScanPriorityExt6);                       // ooc_priority
  uint16_t general = kScanGenPassAll | kScanGenAdaptiveDwell;
  if (!r.active) general |= kScanGenPassive;
  w.le16(general);
  w.u8(0);
  w.u8(r.mac_id);                                  // scan_start_mac_id
  // the version 8 part
  w.u8(kScanDwellActive);                          // active_dwell[0]
  w.u8(0);                                         // active_dwell[1] (second lmac)
  w.u8(0);                                         // reserved2
  w.u8(kScanAdwellDefaultNAps);
  w.u8(kScanAdwellDefaultNApsSocial);
  w.u8(kScanGenFlags2AllowChannelReorder);
  w.le16(kScanAdwellMaxBudgetFull);
  w.le32(0); w.le32(0);                            // max_out_time[2]
  w.le32(0); w.le32(0);                            // suspend_time[2]
  w.le32(kScanPriorityExt6);                       // scan_priority
  w.u8(kScanDwellPassive); w.u8(0);                // passive_dwell[2]
  w.u8(0); w.u8(0);                                // num_of_fragments[2]
  w.u8(0);                                         // channel.flags (no energy based skipping)
  w.u8(static_cast<uint8_t>(r.channel_count));
  w.le16(0);
  if (!w.ok() || w.position() != kScanRequestFixedBytes) return 0;
  // the channel list: the firmware's slots, only the first channel_count used
  for (uint32_t i = 0; i < r.firmware_channel_slots; ++i) {
    if (i < r.channel_count) {
      w.le32(r.active && r.channels[i].probe ? 1u : 0u);     // probe with entry 0 (the wildcard name) or only listen
      w.u8(r.channels[i].number);
      w.u8(r.channels[i].is_5ghz ? kPhyBand5 : kPhyBand24);
      w.u8(1);                                     // iter_count
      w.u8(0);                                     // iter_interval
    } else {
      w.zeros(kScanChannelConfigBytes);
    }
  }
  // the tail: one schedule plan of one iteration, the probe request template, the direct scan names
  w.le16(0); w.u8(1); w.u8(0);                     // schedule[0]: interval 0, 1 iteration
  w.le16(0); w.u8(0); w.u8(0);                     // schedule[1]
  w.le16(0);                                       // delay
  w.le16(0);
  // preq: mac_header, band_data[3], common_data segments, then the buffer
  uint8_t buf[kProbeRequestBytes] = {};
  uint32_t at = 0;
  auto put = [&](const uint8_t* bytes, uint32_t n) {
    for (uint32_t i = 0; i < n && at < sizeof buf; ++i) buf[at++] = bytes[i];
  };
  // the probe request frame header: type management / probe request, broadcast destination, our address, broadcast BSSID
  const uint8_t header[24] = {0x40, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, r.source_address[0], r.source_address[1], r.source_address[2], r.source_address[3],
                              r.source_address[4], r.source_address[5], 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
  put(header, 24);
  const uint8_t wildcard_ssid[2] = {0, 0};
  put(wildcard_ssid, 2);
  const uint32_t mac_len = at;
  // 2.4 GHz: supported rates 1 2 5.5 11 (basic) 6 9 12 18, a DS parameter set the firmware fills in, extended rates 24 36 48 54
  const uint32_t b24 = at;
  const uint8_t rates24[] = {1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 3, 1, 0, 50, 4, 0x30, 0x48, 0x60, 0x6C};
  put(rates24, sizeof rates24);
  const uint32_t b24len = at - b24;
  const uint32_t b5 = at;
  const uint8_t rates5[] = {1, 8, 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
  put(rates5, sizeof rates5);
  const uint32_t b5len = at - b5;
  const uint32_t b6 = at;
  const uint32_t common = at;
  if (at >= sizeof buf) return 0;
  (void)b6;
  w.le16(0); w.le16(static_cast<uint16_t>(mac_len));                       // mac_header
  w.le16(static_cast<uint16_t>(b24)); w.le16(static_cast<uint16_t>(b24len));    // band_data[0]: 2.4 GHz
  w.le16(static_cast<uint16_t>(b5)); w.le16(static_cast<uint16_t>(b5len));      // band_data[1]: 5 GHz
  w.le16(static_cast<uint16_t>(b6)); w.le16(0);                                  // band_data[2]: 6 GHz (none)
  w.le16(static_cast<uint16_t>(common)); w.le16(0);                              // common_data
  w.bytes(ConstByteView(buf, sizeof buf));
  // direct_scan[20]: entry 0 is the wildcard name (id 0, length 0), the rest unused
  w.zeros(static_cast<uint64_t>(kMaxScanSsids) * kScanSsidEntryBytes);
  return w.ok() && w.position() == total ? total : 0;
}

bool iwl_parse_scan_complete(ConstByteView payload, IwlScanComplete* out) {
  ByteReader r(payload);
  out->uid = r.le32();
  r.skip(2);                                       // last_schedule, last_iter
  out->status = r.u8();
  out->ebs_status = r.u8();
  return r.ok();
}

bool iwl_parse_rx_mpdu(const IwlRxPacket& packet, IwlRxFrame* out) {
  if (packet.command != kRxMpduCommand || packet.group != 0) return false;
  ByteReader r(packet.payload);
  const uint16_t mpdu_len = r.le16();
  r.u8();                                          // mac_flags1
  const uint8_t flags2 = r.u8();
  r.skip(1 + 2 + 1);                               // amsdu_info, phy_info, mac_phy_band
  r.skip(4);                                       // raw_csum, l3l4 flags
  const uint32_t status = r.le32();
  r.skip(4);                                       // reorder data
  r.skip(4 + 4 + 4);                               // rss hash / phy data 2, filter match / phy data 3, rate_n_flags
  const uint8_t energy_a = r.u8(), energy_b = r.u8(), channel = r.u8();
  r.u8();                                          // mac context
  r.skip(4 + 8);                                   // gp2, tsf / phy data 0 and 1
  if (!r.ok() || r.position() != kRxDescriptorBytes) return false;
  ConstByteView rest = r.rest();
  if (mpdu_len > rest.size()) return false;
  // a frame the hardware padded: two bytes after the 802.11 header, not part of the frame as we see it; management frames come without
  (void)flags2;
  out->frame = rest.subview(0, mpdu_len);
  out->channel = channel;
  const int32_t a = energy_a ? -static_cast<int32_t>(energy_a) : -256, b = energy_b ? -static_cast<int32_t>(energy_b) : -256;
  const int32_t best = a > b ? a : b;
  out->signal_dbm = best == -256 ? 0 : best;
  out->crc_ok = (status & kStatusCrcOk) && (status & kStatusOverrunOk);
  return true;
}

}  // namespace hardware
