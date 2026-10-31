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
#include "Hardware/WirelessInterface/RateScaler.hpp"
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

uint32_t iwl_rate_code(const WifiRate& rate) {
  uint32_t width = rate.width_mhz == 80 ? 2 : rate.width_mhz == 40 ? 1 : 0;
  uint32_t code = 0;
  switch (rate.mode) {
    case WifiRate::Mode::Legacy:
      return WifiRateScaler::kPlcp[rate.mcs < WifiRateScaler::kRateCount ? rate.mcs : 0];
    case WifiRate::Mode::Ht:
      code = kRateHtV1 | (rate.mcs & 0x1Fu);
      break;
    case WifiRate::Mode::Vht:
      code = kRateVhtV1 | (rate.mcs & 0xFu) | (static_cast<uint32_t>(rate.streams > 1 ? rate.streams - 1 : 0) << kRateVhtStreamsShift);
      break;
  }
  code |= width << kRateWidthShift;
  if (rate.short_gi) code |= kRateSgiV1;
  return code;
}

uint32_t iwl_build_link_quality(uint8_t station_id, uint32_t tx_antennas, const uint32_t rates[kLinkQualityTableSize], ByteView out, uint32_t mimo_delim) {
  ByteWriter w(out);
  const uint32_t antenna = tx_antennas & 7 ? tx_antennas & 7 : 1;
  w.u8(station_id);
  w.u8(0);                                         // reduced_tpc
  w.le16(0);                                       // control (unused)
  w.u8(0);                                         // flags: no RTS, colour 0
  w.u8(static_cast<uint8_t>(mimo_delim > kLinkQualityTableSize ? kLinkQualityTableSize : mimo_delim));      // mimo_delim
  w.u8(static_cast<uint8_t>(antenna & 1 ? 1 : antenna & 2 ? 2 : 4));      // single_stream_ant_msk
  w.u8(static_cast<uint8_t>(antenna));             // dual_stream_ant_msk
  for (int ac = 0; ac < 4; ++ac) w.u8(0);          // initial_rate_index
  w.le16(4000);                                    // agg_time_limit
  w.u8(3);                                         // agg_disable_start_th
  w.u8(1);                                         // agg_frame_cnt_limit: no aggregation
  w.le32(0);                                       // reserved2
  for (uint32_t i = 0; i < kLinkQualityTableSize; ++i) w.le32(rates[i]);
  w.le32(0);                                       // ss_params: not valid
  return w.ok() && w.position() == kLinkQualityBytes ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t iwl_build_bt_config(ByteView out) {
  ByteWriter w(out);
  w.le32(kBtCoexNetwork);                          // mode
  w.le32(kBtCoexHighBandRet);                      // enabled_modules
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t iwl_build_time_event(const IwlTimeEvent& event, ByteView out) {
  ByteWriter w(out);
  w.le32(event.mac_id | (uint32_t{event.mac_color} << 8));   // id_and_color
  w.le32(event.action);
  w.le32(event.event);                             // id
  w.le32(0);                                       // apply_time: now
  w.le32(event.max_delay_tu);
  w.le32(0);                                       // depends_on
  w.le32(1);                                       // interval
  w.le32(event.duration_tu);
  w.u8(1);                                         // repeat
  w.u8(kTeFragNone);                               // max_frags
  w.le16(kTePolicyNotifyStart | kTePolicyNotifyEnd | kTePolicyStartImmediately);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

bool iwl_parse_time_event_response(ConstByteView payload, uint32_t* status, uint32_t* unique_id) {
  ByteReader r(payload);
  *status = r.le32();
  r.le32();                                        // id
  *unique_id = r.le32();
  return r.ok();
}

bool iwl_parse_error_response(ConstByteView payload, IwlErrorResponse* out) {
  ByteReader r(payload);
  out->error_type = r.le32();
  out->command = r.u8();
  r.u8();
  out->sequence = r.le16();
  out->service = r.le32();
  return r.ok();
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
  for (uint32_t i = 0; i < 6; ++i) w.u8(c.bssid[i]);
  w.le16(0);
  w.le32(0x0F);                                    // cck_rates: the four mandatory ones
  w.le32(0x15);                                    // ofdm_rates: 6, 12 and 24 Mbit/s
  w.le32(0);                                       // protection_flags
  w.le32(0);                                       // cck_short_preamble
  w.le32(0);                                       // short_slot
  w.le32((1u << 2) | (c.associated ? 0u : (1u << 6)));        // filter_flags: group frames (MAC_FILTER_ACCEPT_GRP), and before the association other networks' beacons (MAC_FILTER_IN_BEACON)
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
  // the station data (STA_MAC_DATA): is_assoc, dtim time and tsf, beacon interval, dtim interval, data policy, listen interval, association id, beacon arrival time
  w.le32(c.associated ? 1 : 0);
  w.le32(0);
  w.le64(0);
  w.le32(c.beacon_interval_tu);
  w.le32(0);
  w.le32(c.beacon_interval_tu * c.dtim_period);
  w.le32(0);
  w.le32(0);                                       // listen interval
  w.le32(c.association_id);
  w.le32(0);
  w.zeros(4);                                      // (the union is 48 bytes: the P2P station form is the largest)
  if (!w.ok() || w.position() != kMacContextBytes) return 0;
  return kMacContextBytes;
}

uint32_t iwl_build_phy_context(uint32_t action, uint8_t channel, bool five_ghz, uint32_t tx_antennas, uint32_t rx_antennas, ByteView out, uint8_t width, uint8_t control_position) {
  ByteWriter w(out);
  w.le32(0);                                       // id 0, colour 0
  w.le32(action);
  w.le32(0);                                       // apply_time
  w.le32(0);                                       // tx_param_color
  w.u8(five_ghz ? kPhyBand5 : kPhyBand24);
  w.u8(channel);
  w.u8(width);                                     // 0 20 MHz, 1 40, 2 80
  w.u8(width ? control_position : 0);              // control channel position
  w.le32(tx_antennas);
  // rx chains: the valid ones, and two of them in use whether idle or active (receive diversity for single stream legacy rates)
  w.le32((rx_antennas << 1) | (2u << 10) | (2u << 12));
  w.le32(0);                                       // acquisition_data
  w.le32(0);                                       // dsp_cfg_flags
  return w.ok() && w.position() == kPhyContextBytes ? kPhyContextBytes : 0;
}

uint32_t iwl_build_binding(uint32_t action, uint8_t phy_id, uint8_t mac_id, uint8_t mac_color, ByteView out) {
  ByteWriter w(out);
  w.le32(phy_id);                                  // the binding has the PHY context's number
  w.le32(action);
  w.le32(static_cast<uint32_t>(mac_id) | (static_cast<uint32_t>(mac_color) << 8));
  w.le32(kCtxtInvalid);
  w.le32(kCtxtInvalid);
  w.le32(phy_id);
  return w.ok() && w.position() == kBindingBytes ? kBindingBytes : 0;
}

uint32_t iwl_build_add_station(const IwlStationSpec& st, ByteView out) {
  ByteWriter w(out);
  w.u8(st.modify ? 1 : 0);
  w.u8(0);                                         // awake_acs
  w.le16(0xFFFF);                                  // tid_disable_tx: no aggregation on any TID
  w.le32(static_cast<uint32_t>(st.mac_id) | (static_cast<uint32_t>(st.mac_color) << 8));
  for (uint32_t i = 0; i < 6; ++i) w.u8(st.address[i]);
  w.le16(0);
  w.u8(st.station_id);
  w.u8(0);                                         // modify_mask
  w.le16(0);
  // station_flags: the width, the streams and (HT) the A-MPDU limits; without HT: 20 MHz, one spatial stream
  uint32_t flags = 0, mask = kStaFlagFatMask | kStaFlagMimoMask | kStaFlagRtsMimoProt;
  if (st.ht) {
    flags = (static_cast<uint32_t>(st.width > 3 ? 0 : st.width) << 26) | (st.streams >= 2 ? 1u << 28 : 0u) | (static_cast<uint32_t>(st.ampdu_exponent > 9 ? 9 : st.ampdu_exponent) << kStaFlagAggSizeShift) |
            (static_cast<uint32_t>(st.ampdu_density & 7) << kStaFlagAggDensityShift);
    mask |= kStaFlagAggSizeMask | kStaFlagAggDensityMask;
  }
  w.le32(flags);
  w.le32(mask);
  w.u8(0);
  w.u8(0);
  w.le16(0);
  w.le16(0);
  w.u8(0);                                         // sleep_state_flags
  w.u8(0);                                         // station_type: a link station
  w.le16(st.association_id);
  w.le16(0);
  w.le32(st.queue_mask);
  w.le16(0);
  w.u8(0);
  w.u8(0);
  return w.ok() && w.position() == kAddStaBytes ? kAddStaBytes : 0;
}

uint32_t iwl_build_station_key(uint8_t station_id, uint8_t key_offset, uint8_t key_id, bool multicast, const uint8_t key[16], const uint8_t rsc[8], ByteView out) {
  ByteWriter w(out);
  w.u8(station_id);
  w.u8(key_offset);
  w.le16(kStaKeyCcm | kStaKeyMapFlag | (static_cast<uint16_t>(key_id & 3) << kStaKeyIdPos) | (multicast ? kStaKeyMulticast : 0));
  w.bytes(ConstByteView(key, 16));
  w.zeros(16);                                     // the rest of the 32 byte key field
  w.bytes(ConstByteView(rsc, 8));
  w.zeros(8);                                      // rx_secur_seq_cnt is 16 bytes
  w.u8(0);                                         // tkip_rx_tsc_byte2
  w.u8(0);
  w.zeros(10);                                     // tkip_rx_ttak
  return w.ok() && w.position() == kStaKeyBytes ? kStaKeyBytes : 0;
}

uint32_t iwl_build_tx_prefix(const IwlTxParams& p, ByteView out) {
  if (p.header.size() < 24 || p.header.size() > 32) return 0;
  const uint32_t hdr_len = static_cast<uint32_t>(p.header.size());
  const bool multicast = (p.header[4] & 1) != 0;
  ByteWriter w(out);
  w.u8(kTxCommand);                                // device command header: command, group, sequence (filled in by the queue)
  w.u8(0);
  w.le16(0);
  w.le16(p.frame_bytes);                           // len
  const uint32_t mh_len = hdr_len + (p.protect ? 8 : 0);
  w.le16(static_cast<uint16_t>((mh_len / 2) << 8) | (hdr_len % 4 ? (1u << 13) : 0));      // offload_assist: the header size in words (and the padding bit)
  w.le32((multicast ? 0u : kTxFlagAck) | kTxFlagSeqCtl | (p.data && p.station_rate ? kTxFlagStaRate : 0u));
  w.le32(0);                                       // scratch (the firmware's status word; its address is set by the queue)
  uint32_t rate;
  if (p.data) rate = p.data_plcp;
  else rate = p.five_ghz ? kRatePlcp6M : (kRatePlcp1M | kRateCckV1);
  uint32_t antenna = p.tx_antennas & 7;
  antenna = antenna & 1 ? 1 : antenna & 2 ? 2 : 4;
  w.le32(rate | (antenna << 14));
  w.u8(p.station_id);
  w.u8(p.protect ? kTxSecCcm : 0);                 // sec_ctl
  w.u8(0);                                         // initial_rate_index
  w.u8(0);
  if (p.protect) w.bytes(ConstByteView(p.tk, 16)); else w.zeros(16);        // key
  w.le32(0);
  w.le32(kTxLifeTimeInfinite);
  w.le32(0);                                       // dram_lsb_ptr (set by the queue)
  w.u8(0);                                         // dram_msb_ptr
  w.u8(60);                                        // rts_retry_limit
  w.u8(p.data ? 15 : 3);                           // data_retry_limit
  w.u8(kTidNonQos);                                // tid_tspec: not QoS
  w.le16(p.association_request ? kPmFrameAssoc : (p.data ? kPmFrameNone : kPmFrameMgmt));
  w.le16(0);
  if (!w.ok() || w.position() != kTxCommandHeaderBytes + kTxParamBytes) return 0;
  w.bytes(p.header);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

bool iwl_parse_tx_response(ConstByteView payload, IwlTxResponse* out) {
  ByteReader r(payload);
  IwlTxResponse t;
  const uint8_t frames = r.u8();
  r.skip(2);                                       // bt_kill_count, failure_rts
  t.failure_frame = r.u8();
  t.initial_rate = r.le32();
  r.skip(26);                                      // up to the frame control (offset 34)
  t.frame_control = r.le16();
  r.skip(4);                                       // tx_queue, reserved: up to the first frame's status (offset 40)
  if (!r.ok() || frames == 0 || frames > 16) return false;
  t.frame_count = frames;
  t.status = r.le16();
  r.skip(2);
  r.skip(static_cast<uint64_t>(frames - 1) * 4);
  t.next_index = r.le32() & 0xFFF;
  if (!r.ok()) return false;
  *out = t;
  return true;
}

uint32_t iwl_data_frame_to_ethernet(ConstByteView frame, bool decrypted, ByteView out) {
  ByteReader r(frame);
  const uint16_t control = r.le16();
  r.skip(2);
  uint8_t a1[6], a2[6], a3[6];
  r.read_into(ByteView(a1));
  r.read_into(ByteView(a2));
  r.read_into(ByteView(a3));
  r.skip(2);
  if (!r.ok() || (control & 0x0003) != 0 || (control & 0x000C) != 0x0008 || (control & 0x0300) != 0x0200) return 0;     // data, from the distribution system
  const uint16_t subtype = control & 0x00F0;
  if (subtype == 0x80) {
    const uint16_t qos = r.le16();
    if (qos & 0x0080) return 0;                                        // an A-MSDU: several frames in one, not unpacked here (the access point sends them only inside a block ack session)
    if (control & 0x8000) r.skip(4);
  } else if (subtype != 0x00) {
    return 0;
  }
  const bool protected_frame = (control & 0x4000) != 0;
  if (protected_frame && !decrypted) return 0;                         // a frame we cannot read
  if (protected_frame) r.skip(8);                                      // the CCMP header
  ConstByteView snap = r.take(8);
  if (!r.ok() || snap[0] != 0xAA || snap[1] != 0xAA || snap[2] != 0x03 || snap[3] != 0 || snap[4] != 0 || snap[5] != 0) return 0;
  ConstByteView payload = r.rest();
  ByteWriter w(out);
  for (uint32_t i = 0; i < 6; ++i) w.u8(a1[i]);                        // destination
  for (uint32_t i = 0; i < 6; ++i) w.u8(a3[i]);                        // source (address 3 in a frame from the access point)
  w.u8(snap[6]);
  w.u8(snap[7]);
  w.bytes(payload);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t iwl_ethernet_to_data_frame(ConstByteView eth, const uint8_t bssid[6], const uint8_t source[6], bool protect, uint64_t pn, uint8_t key_id, ByteView header, ByteView body) {
  if (eth.size() < 14) return 0;
  ByteWriter h(header);
  h.le16(protect ? 0x4108 : 0x0108);               // data, to the distribution system, protected when the hardware encrypts
  h.le16(0);
  for (uint32_t i = 0; i < 6; ++i) h.u8(bssid[i]);                      // receiver
  for (uint32_t i = 0; i < 6; ++i) h.u8(source[i]);                     // transmitter
  for (uint32_t i = 0; i < 6; ++i) h.u8(eth[i]);                        // destination
  h.le16(0);
  if (!h.ok()) return 0;
  ByteWriter w(body);
  if (protect) {
    w.u8(static_cast<uint8_t>(pn));
    w.u8(static_cast<uint8_t>(pn >> 8));
    w.u8(0);
    w.u8(static_cast<uint8_t>(0x20 | ((key_id & 3) << 6)));
    w.u8(static_cast<uint8_t>(pn >> 16));
    w.u8(static_cast<uint8_t>(pn >> 24));
    w.u8(static_cast<uint8_t>(pn >> 32));
    w.u8(static_cast<uint8_t>(pn >> 40));
  }
  const uint8_t snap[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
  w.bytes(ConstByteView(snap, 6));
  w.u8(eth[12]);
  w.u8(eth[13]);
  w.bytes(eth.subview(14));
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
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
  out->decrypted = (status & (7u << 8)) == (2u << 8) && (status & (1u << 11));           // CCMP and decrypted by the hardware
  return true;
}

}  // namespace hardware
