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
uint32_t iwl_build_dqa_enable(uint32_t command_queue, ByteView out);
// The auxiliary station: id `station_id`, bound to the lmac index in the id/colour word, serving the transmit queues in `queue_mask`.
uint32_t iwl_build_add_aux_station(uint8_t station_id, uint32_t queue_mask, ByteView out);
// SCAN_CFG_CMD, the reduced form: no automatic CAM mode, no promiscuous mode, the auxiliary station for the scan frames, the antennas.
uint32_t iwl_build_scan_config(uint8_t aux_station_id, uint32_t tx_antennas, uint32_t rx_antennas, ByteView out);

struct IwlMacContext {
  uint8_t id = 0, color = 0;
  uint8_t address[6] = {};
  uint32_t action = iwl_mvm::kCtxtActionAdd;
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

// ---- notifications -------------------------------------------------------------------------------------------------------------------------------------------------------------

struct IwlScanComplete {
  uint32_t uid = 0;
  uint8_t status = 0, ebs_status = 0;
  bool completed() const { return status == iwl_mvm::kScanCompleted; }
};
bool iwl_parse_scan_complete(ConstByteView payload, IwlScanComplete* out);

// A received frame from REPLY_RX_MPDU_CMD (the multi-queue receive descriptor, version 1 layout): the 802.11 frame, the channel and the signal.
struct IwlRxFrame {
  ConstByteView frame;                             // from the frame control field; the firmware strips the FCS
  uint8_t channel = 0;
  int32_t signal_dbm = 0;                          // 0 when the firmware did not measure
  bool crc_ok = false;
};
bool iwl_parse_rx_mpdu(const IwlRxPacket& packet, IwlRxFrame* out);

}  // namespace hardware
