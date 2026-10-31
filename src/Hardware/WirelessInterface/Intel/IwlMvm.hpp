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


// The firmware commands the runtime needs to get to scanning, built the way iwlwifi's MVM op mode builds them for firmware that speaks: TX_ANT_CONFIGURATION (version 1),
// DQA_ENABLE, ADD_STA (version 10: the auxiliary station), SCAN_CFG_CMD (the reduced configuration), MAC_CONTEXT_CMD (a station interface), SCAN_REQ_UMAC (version 8, adaptive
// dwell version 2, the layout the firmware's scan channel count decides). Only the versions named are written; the caller checks the firmware's command versions first.
// The structures are written to a byte view in the firmware's little-endian packed layout; each builder returns the length, or 0 when the output is too small.
#pragma once
#include <stdint.h>
#include "Hardware/WirelessInterface/Intel/IwlRx.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace iwl_mvm {
// command identifiers: legacy group 0 unless noted; the "long" group commands are sent with the wide header
constexpr uint8_t kTxAntConfigCommand = 0x98, kAddStaCommand = 0x18, kMacContextCommand = 0x28;
constexpr uint8_t kLongGroup = 0x01, kScanConfigCommand = 0x0C, kScanRequestUmacCommand = 0x0D, kScanAbortUmacCommand = 0x0E, kScanCompleteUmacCommand = 0x0F;
constexpr uint8_t kDataPathGroup = 0x05, kDqaEnableCommand = 0x00;
constexpr uint8_t kRxMpduCommand = 0xC1;
constexpr uint8_t kBtConfigCommand = 0x9B, kTimeEventCommand = 0x29, kTimeEventNotification = 0x2A, kReplyError = 0x02;
constexpr uint32_t kBtCoexNetwork = 1, kBtCoexHighBandRet = 0x10;
constexpr uint32_t kTimeEventBytes = 36, kTeBssStaAggressiveAssoc = 0;
constexpr uint32_t kTePolicyNotifyStart = 1u << 0, kTePolicyNotifyEnd = 1u << 1, kTePolicyStartImmediately = 1u << 11, kTeFragNone = 0;
constexpr uint8_t kPhyContextCommand = 0x08, kAddStaKeyCommand = 0x17, kTxCommand = 0x1C, kBindingContextCommand = 0x2B;
// firmware command versions this file writes
constexpr uint32_t kTxAntConfigVersion = 1, kAddStaVersion = 10, kScanConfigVersionMin = 1, kScanRequestVersion = 8;
// capabilities / API bits (iwl fw_file.h)
constexpr uint32_t kCapabilityDqaSupport = 12, kApiStaType = 30, kApiReducedScanConfig = 34, kApiAdaptiveDwell = 32, kApiAdaptiveDwellV2 = 42;
// identifiers
constexpr uint32_t kMacIndexAux = 4;
constexpr uint32_t kStationTypeAuxActivity = 4;
constexpr uint8_t kMacTypeBssStation = 5;
constexpr uint32_t kCtxtActionAdd = 1, kCtxtActionModify = 2, kCtxtActionRemove = 3;
constexpr uint8_t kPhyBand5 = 0, kPhyBand24 = 1;
// sizes of the structures
constexpr uint32_t kAddStaBytes = 48, kMacContextBytes = 148, kScanConfigBytes = 12, kScanRequestFixedBytes = 52, kScanChannelConfigBytes = 8, kScanRequestTailBytes = 1224;
constexpr uint32_t kProbeRequestBytes = 512, kScanSsidEntryBytes = 34, kMaxScanSsids = 20;
// ADD_STA status
constexpr uint32_t kAddStaStatusMask = 0xFF, kAddStaSuccess = 0x1;
constexpr uint32_t kPhyContextBytes = 36, kBindingBytes = 24, kStaKeyBytes = 64, kTxCommandHeaderBytes = 4, kTxParamBytes = 56, kCtxtInvalid = 0xFFFFFFFFu;
constexpr uint32_t kStaFlagFatMask = 3u << 26, kStaFlagMimoMask = 3u << 28, kStaFlagRtsMimoProt = 1u << 17;
constexpr uint16_t kStaKeyCcm = 2, kStaKeyMapFlag = 1u << 3, kStaKeyIdPos = 8, kStaKeyMulticast = 1u << 14;
constexpr uint32_t kTxFlagAck = 1u << 3, kTxFlagSeqCtl = 1u << 13, kTxSecCcm = 0x02;
constexpr uint32_t kRateCckV1 = 1u << 9, kRateAntA = 1u << 14, kRatePlcp1M = 10, kRatePlcp6M = 13, kRatePlcp24M = 9;
constexpr uint32_t kTxLifeTimeInfinite = 0xFFFFFFFFu, kPmFrameNone = 0, kPmFrameMgmt = 2, kPmFrameAssoc = 3, kTidNonQos = 8;
// scan general flags (UMAC_SCAN_GEN_FLAGS_*) and channel flags
constexpr uint16_t kScanGenPassAll = 1u << 2, kScanGenPassive = 1u << 3, kScanGenPreConnect = 1u << 4, kScanGenIterComplete = 1u << 5, kScanGenAdaptiveDwell = 1u << 13;
constexpr uint8_t kScanGenFlags2AllowChannelReorder = 1u << 1;
constexpr uint32_t kScanPriorityExt2 = 2, kScanPriorityExt6 = 6;
constexpr uint8_t kScanDwellActive = 10, kScanDwellPassive = 110, kScanAdwellDefaultNAps = 2, kScanAdwellDefaultNApsSocial = 10;
constexpr uint16_t kScanAdwellMaxBudgetFull = 300, kScanAdwellMaxBudgetDirected = 100;
// scan complete status (IWL_SCAN_OFFLOAD_*)
constexpr uint8_t kScanCompleted = 1, kScanAborted = 2;
}  // namespace iwl_mvm

// ---- builders ----------------------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t iwl_build_tx_ant_config(uint32_t valid_antennas, ByteView out);
// BT_CONFIG (iwl_bt_coex_cmd): the network (Wi-Fi owns the antenna arbitration with the firmware's tables) mode and the high band retention module; sent by iwlwifi at every start.
uint32_t iwl_build_bt_config(ByteView out);
// TIME_EVENT_CMD (iwl_time_event_cmd, 36 bytes): the firmware keeps the channel of the interface for `duration_tu` time units from now, so that the frames of the join are not lost to a
// scan or to leaving the channel (iwl_mvm_protect_session). `action` is kCtxtActionAdd, with the event type in `event`; to remove, kCtxtActionRemove with the unique id the add returned.
struct IwlTimeEvent {
  uint32_t action = iwl_mvm::kCtxtActionAdd;
  uint8_t mac_id = 0, mac_color = 0;
  uint32_t event = iwl_mvm::kTeBssStaAggressiveAssoc;      // the type when adding, the firmware's unique id when removing
  uint32_t duration_tu = 500, max_delay_tu = 0;
};
uint32_t iwl_build_time_event(const IwlTimeEvent& event, ByteView out);
// The response (iwl_time_event_resp): status (nonzero: scheduled), the id and unique id.
bool iwl_parse_time_event_response(ConstByteView payload, uint32_t* status, uint32_t* unique_id);
// REPLY_ERROR (iwl_error_resp): the firmware refused or could not run a command.
struct IwlErrorResponse {
  uint32_t error_type = 0;
  uint8_t command = 0;
  uint16_t sequence = 0;
  uint32_t service = 0;
};
bool iwl_parse_error_response(ConstByteView payload, IwlErrorResponse* out);
uint32_t iwl_build_dqa_enable(uint32_t command_queue, ByteView out);
// The auxiliary station: id `station_id`, bound to the lmac index in the id/colour word, serving the transmit queues in `queue_mask`.
uint32_t iwl_build_add_aux_station(uint8_t station_id, uint32_t queue_mask, ByteView out);
// SCAN_CFG_CMD, the reduced form: no automatic CAM mode, no promiscuous mode, the auxiliary station for the scan frames, the antennas.
uint32_t iwl_build_scan_config(uint8_t aux_station_id, uint32_t tx_antennas, uint32_t rx_antennas, ByteView out);

struct IwlMacContext {
  uint8_t id = 0, color = 0;
  uint8_t address[6] = {};
  uint8_t bssid[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};      // the access point once there is one
  uint32_t action = iwl_mvm::kCtxtActionAdd;
  bool associated = false;
  uint16_t association_id = 0;
  uint32_t beacon_interval_tu = 100, dtim_period = 1;
};
// A station (client) interface that is not associated: it passes beacons up to the host and accepts group frames.
uint32_t iwl_build_station_mac_context(const IwlMacContext& context, ByteView out);

struct IwlScanChannelEntry {
  uint8_t number = 0;
  bool is_5ghz = false;
  bool probe = true;                               // send probe requests on this channel (false: listen only, as on channels where transmitting first is not allowed)
};
struct IwlScanRequest {
  uint32_t uid = 1;
  uint8_t mac_id = 0;
  bool active = true;                              // probe with the wildcard name; false: listen only
  uint8_t source_address[6] = {};
  uint32_t firmware_channel_slots = 40;            // the firmware's n_scan_channels: the room the request has for channels
  const IwlScanChannelEntry* channels = nullptr;
  uint32_t channel_count = 0;
};
uint32_t iwl_scan_request_bytes(uint32_t firmware_channel_slots);
// 0 when the request has more channels than the firmware has slots, no channel, or the output is too small.
uint32_t iwl_build_scan_request(const IwlScanRequest& request, ByteView out);

// The PHY context (PHY_CONTEXT_CMD version below 3: the 20 MHz channel and the chains) and the binding of a MAC context to it (BINDING_CONTEXT_CMD, version 1 layout).
uint32_t iwl_build_phy_context(uint32_t action, uint8_t channel, bool five_ghz, uint32_t tx_antennas, uint32_t rx_antennas, ByteView out);
uint32_t iwl_build_binding(uint32_t action, uint8_t phy_id, uint8_t mac_id, uint8_t mac_color, ByteView out);

// A station entry for the access point (ADD_STA version 10, a plain legacy station without aggregation).
struct IwlStationSpec {
  uint8_t station_id = 1;
  uint8_t mac_id = 0, mac_color = 0;
  uint8_t address[6] = {};
  uint32_t queue_mask = 0;
  bool modify = false;
  uint16_t association_id = 0;
};
uint32_t iwl_build_add_station(const IwlStationSpec& station, ByteView out);

// ADD_STA_KEY (version 1 layout): a CCMP key for the station `station_id`. `multicast` marks the group key; `key_offset` is its slot in the firmware's key table.
uint32_t iwl_build_station_key(uint8_t station_id, uint8_t key_offset, uint8_t key_id, bool multicast, const uint8_t key[16], const uint8_t rsc[8], ByteView out);

// The frame to send as TX_CMD (version 6): the device command header, the TX command and the 802.11 header, ready for IwlCommandQueue::enqueue_frame (the payload follows it
// there). `header` is the 802.11 header (24 bytes, the Protected bit already set when the hardware is to encrypt), `frame_bytes` the length the hardware transmits (the header, the
// CCMP header when protected and the payload), `tk` the pairwise key of a protected frame.
struct IwlTxParams {
  ConstByteView header;
  uint16_t frame_bytes = 0;
  uint8_t station_id = 1;
  bool protect = false;
  bool five_ghz = false;
  bool data = false;                               // a data frame (sent at the data rate) rather than management / EAPOL (at the lowest basic rate)
  bool association_request = false;
  uint32_t tx_antennas = 1;
  uint8_t tk[16] = {};
};
uint32_t iwl_build_tx_prefix(const IwlTxParams& params, ByteView out);
// The TX response (a notification of the TX command): the sequence it answers, the first frame's status and the ring position after the last frame it covers.
struct IwlTxResponse {
  uint8_t frame_count = 0;
  uint32_t status = 0;
  uint32_t next_index = 0;
  bool success() const { return (status & 0xFF) == 1 || (status & 0xFF) == 2; }
};
bool iwl_parse_tx_response(ConstByteView payload, IwlTxResponse* out);
// A received data frame, converted to an Ethernet frame (destination, source, ethertype, payload): `frame` is the 802.11 frame from the access point (plain or QoS data, the
// CCMP header still in place when `decrypted`). False for anything else (not data, not from the AP, not protected when it should be, or too short).
uint32_t iwl_data_frame_to_ethernet(ConstByteView frame, bool decrypted, ByteView out);
// The reverse: an Ethernet frame to the body of a data frame (an optional CCMP header with packet number `pn` and key id, the LLC/SNAP header and the payload) after `header`.
// Fills `header` (24 bytes) for a frame to the access point `bssid` from `source`, to destination `destination`.
uint32_t iwl_ethernet_to_data_frame(ConstByteView ethernet, const uint8_t bssid[6], const uint8_t source[6], bool protect, uint64_t pn, uint8_t key_id, ByteView header, ByteView body);

// ---- notifications -------------------------------------------------------------------------------------------------------------------------------------------------------------

struct IwlScanComplete {
  uint32_t uid = 0;
  uint8_t status = 0, ebs_status = 0;
  bool completed() const { return status == iwl_mvm::kScanCompleted; }
};
bool iwl_parse_scan_complete(ConstByteView payload, IwlScanComplete* out);

// A received frame from REPLY_RX_MPDU_CMD (the multi-queue receive descriptor, version 1 layout): the 802.11 frame, the channel and the signal.
struct IwlRxFrame {
  bool decrypted = false;                          // the hardware decrypted it (CCMP): the CCMP header is left in the frame, the MIC is gone
  ConstByteView frame;                             // from the frame control field; the firmware strips the FCS
  uint8_t channel = 0;
  int32_t signal_dbm = 0;                          // 0 when the firmware did not measure
  bool crc_ok = false;
};
bool iwl_parse_rx_mpdu(const IwlRxPacket& packet, IwlRxFrame* out);

}  // namespace hardware
