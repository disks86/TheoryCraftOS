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

// What goes over the USB endpoints of the Xbox Wireless Adapter for Windows (045E:02E6, 02FE, 02F9, 091E): an MT7612U-style MediaTek radio
// (its Windows driver is mt7612us.sys) that the host gives firmware, and that then carries the controllers' GIP messages inside 802.11 frames.
// Pure encoders and decoders, so the layouts can be tested on their own; the driver is XboxWirelessAdapter.
//
//   bulk OUT, command endpoint   [TXINFO: length | CPU_TX_PORT | TYPE_CMD | command | sequence] [payload] [pad to 4] [4 zero bytes]
//                                (firmware chunks and MCU commands)
//   bulk OUT, data endpoint      [TXINFO: length | WLAN_PORT | 802.11 | WIV | QSEL_EDCA] [TXWI, 20 bytes] [802.11 frame] [pad to 4] [4 zero bytes]
//   bulk IN                      [DMA header: length L] [L bytes: RXWI (32) + 802.11 frame, or an MCU event's data] [FCE info: port, event type]
//
// SOURCES AND CONFIDENCE. Nothing here was checked against an adapter. The register offsets, vendor requests and descriptor bit fields are
// those of the Linux mt76 driver (mt76x02_regs.h, mt76x02_mcu.h, mt76x02_usb_mcu.c, mt76x2/usb_mcu.c) as remembered; xow (the user-space
// Linux driver for this adapter) and xone (its kernel successor) use the same chip interface. The parts that are specific to the Xbox
// protocol - the event numbers, the vendor element of the beacon, the pairing frames, the association response body - are remembered from
// xow and are the least certain. Every such value is in the kTable namespaces below, each with a note, so that a capture from a real adapter
// can correct them in one place. Messages come from a radio: every decoder checks every length first.
#pragma once
#include <stdint.h>
#include "Hardware/GameController/GamepadTypes.hpp"
#include "Hardware/NetworkInterface/NetworkTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace xbox_wireless {

// --- Vendor requests and registers of the chip (mt76; confident unless noted) ----------------------------------------------------------------
namespace mt {
constexpr uint8_t kTypeVendorOut = 0x40, kTypeVendorIn = 0xC0;
// enum mt_vendor_req
constexpr uint8_t kVendDevMode = 0x01, kVendMultiWrite = 0x06, kVendMultiRead = 0x07, kVendReadEeprom = 0x09, kVendWriteFce = 0x42,
                  kVendWriteCfg = 0x46, kVendReadCfg = 0x47;
// MT_VEND_DEV_MODE values: 0x01 resets the MCU (mt76x02u_mcu_fw_reset), 0x12 loads the interrupt vector block and starts the firmware.
constexpr uint16_t kDevModeReset = 0x01, kDevModeLoadIvb = 0x12;

// Registers (MT_*). kUsbU3DmaCfg is a "CFG" register (MT_VEND_WRITE_CFG / READ_CFG), the others plain (MULTI_WRITE / MULTI_READ); the FCE
// pair is written with MT_VEND_WRITE_FCE as two 16-bit halves (mt76u_single_wr).
constexpr uint32_t kFceDmaAddr = 0x0230, kFceDmaLen = 0x0234, kUsbDmaCfg = 0x0238, kMcuComReg0 = 0x0730, kFcePseCtrl = 0x0800,
                   kTxCpuFromFceBasePtr = 0x09A0, kTxCpuFromFceMaxCount = 0x09A4, kTxCpuFromFceCpuDescIdx = 0x09A8, kFcePdmaGlobalConf = 0x09C4,
                   kFceSkipFs = 0x0A6C, kMacSysCtrl = 0x1004, kMacAddrDw0 = 0x1008, kMacAddrDw1 = 0x100C, kMacBssidDw0 = 0x1010,
                   kMacBssidDw1 = 0x1014, kBeaconTimeCfg = 0x1114, kChIdle = 0x1130, kChBusy = 0x1134, kWcidAddrBase = 0x1800,
                   kBeaconBase = 0xC000, kUsbU3DmaCfg = 0x9018;
constexpr uint32_t kWcidEntryBytes = 8;               // MT_WCID_ADDR(n) = 0x1800 + 8 n: six address bytes, two spare
constexpr uint32_t kMcuMemmapWlan = 0x410000;         // CMD_BURST_WRITE addresses a WLAN register as this plus its offset

// The values of the firmware download (mt76x2u_mcu_load_firmware).
constexpr uint32_t kUsbDmaCfgValue = (1u << 23) | (1u << 22) | 0x20;   // TX_BULK_EN | RX_BULK_EN | RX_BULK_AGG_TOUT 0x20 (no RX aggregation)
constexpr uint32_t kFceBasePtrValue = 0x400230, kFcePdmaValue = 0x44, kFceSkipFsValue = 0x03;
constexpr uint32_t kIlmOffset = 0x80000;              // MT76U_MCU_ILM_OFFSET
constexpr uint32_t kDlmOffset = 0x110800;             // MT76U_MCU_DLM_OFFSET + 0x800 (chips from revision E3 on). UNCERTAIN: xow's value as remembered
constexpr uint32_t kFirmwareChunk = 0x3900;           // MCU_FW_URB_MAX_PAYLOAD: firmware bytes per bulk transfer
constexpr uint32_t kMacSysCtrlEnable = (1u << 2) | (1u << 3);   // MT_MAC_SYS_CTRL_ENABLE_TX | ENABLE_RX
// MT_BEACON_TIME_CFG: interval in 1/16 TU (bits 15:0), TIMER_EN 16, SYNC_MODE 18:17 (3: AP), TBTT_EN 19, BEACON_TX 20.
constexpr uint32_t kBeaconTimeEnable = (1u << 16) | (3u << 17) | (1u << 19) | (1u << 20);
constexpr uint16_t kBeaconIntervalTu = 100;
constexpr uint32_t kEepromMacOffset = 0x04;           // MT_EE_MAC_ADDR

// MCU commands (enum mcu_cmd) and their arguments.
constexpr uint8_t kCmdFunSetOp = 1, kCmdBurstWrite = 8, kCmdLedModeOp = 16, kCmdPowerSavingOp = 20, kCmdSwitchChannelOp = 30;
constexpr uint32_t kFunQSelect = 1;                   // CMD_FUN_SET_OP Q_SELECT
constexpr uint32_t kRadioOn = 0x31, kRadioOff = 0x30; // CMD_POWER_SAVING_OP
// CMD_LED_MODE_OP argument. UNCERTAIN: xow's numbers as remembered (the adapter's white light: steady, blinking while pairing, off).
constexpr uint32_t kLedOn = 1, kLedBlink = 0, kLedOff = 2;

// TXINFO / MCU message word (MT_TXD_INFO_*, MT_MCU_MSG_*)
constexpr uint32_t kInfoLengthMask = 0xFFFF;
constexpr uint32_t kInfo80211 = 1u << 19, kInfoWiv = 1u << 24;
constexpr uint32_t kInfoQselShift = 25, kQselEdca = 2;
constexpr uint32_t kInfoPortShift = 27, kWlanPort = 0, kCpuRxPort = 1, kCpuTxPort = 2;
constexpr uint32_t kMsgSequenceShift = 16, kMsgCommandShift = 20, kMsgTypeCmd = 1u << 30;
// RX: the FCE info word after the packet (MT_RX_FCE_INFO_*): event type bits 23:20, destination port bits 29:27.
constexpr uint32_t kFceEventShift = 20, kFceEventMask = 0xF, kFcePortShift = 27, kFcePortMask = 0x7;
// RXWI (struct mt76x02_rxwi, 32 bytes): rxinfo word (L2PAD bit 14), ctl word (WCID bits 7:0, MPDU length bits 29:16).
constexpr uint32_t kRxwiBytes = 32, kRxInfoL2Pad = 1u << 14, kRxwiMpduShift = 16, kRxwiMpduMask = 0x3FFF;
// TXWI (struct mt76x02_txwi, 20 bytes): rate word PHY bits 15:13 (1: OFDM) and MCS 5:0; ack_ctl bit 0 asks for an acknowledgement.
constexpr uint32_t kTxwiBytes = 20;
constexpr uint16_t kTxwiRateOfdm6 = 1u << 13;
constexpr uint8_t kTxwiAckRequest = 0x01;
}  // namespace mt

// --- What the adapter adds for Xbox controllers (from xow; the least certain part) ------------------------------------------------------------
namespace xbox_table {
// MCU events that arrive on the CPU port: the adapter's pair button, a management frame from a controller, a client that went out of range.
// UNCERTAIN: xow's EVT_BUTTON_PRESS / EVT_PACKET_RX / EVT_CLIENT_LOST as remembered. The client-lost event carries the client's WCID in byte 0.
constexpr uint8_t kEventButtonPress = 0x04, kEventPacketRx = 0x0C, kEventClientLost = 0x0E;
// The management subtype a controller uses to ask to pair, and the host's answer (802.11 "reserved" subtype 7). UNCERTAIN.
constexpr uint8_t kSubtypePair = 0x07;
// The pairing answer's body. UNCERTAIN: remembered from xow's Mt76::pairClient, not understood field by field.
constexpr uint8_t kPairResponseBody[] = {0x70, 0x02, 0x00, 0x45, 0x55, 0x01, 0x0F, 0x8F, 0xFF, 0x87, 0x1F};
// The beacon's vendor element: Microsoft's OUI 00:50:F2, type 0x11, then a few bytes of which one says whether pairing is open. UNCERTAIN:
// the bytes after the type and which of them is the pairing flag (kBeaconPairingIndex, counted from the element's first byte, 0xDD).
constexpr uint8_t kBeaconVendorElement[] = {0xDD, 0x10, 0x00, 0x50, 0xF2, 0x11, 0x01, 0x10, 0x00, 0xA5, 0x30, 0x99, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint32_t kBeaconPairingIndex = 8;
// The capability field of the beacon and association response. UNCERTAIN (0: neither ESS nor IBSS, as remembered).
constexpr uint16_t kCapability = 0x0000;
// The channels the adapter may pick, in order of preference when equally quiet (5 GHz first: the controllers prefer it). UNCERTAIN as a set.
constexpr uint8_t kChannels[] = {36, 40, 44, 48, 149, 153, 157, 161, 165, 1, 6, 11};
}  // namespace xbox_table

// GIP commands this layer handles beyond XboxProtocol.hpp's: the controller's announcement (the first thing it sends), and the power payloads.
constexpr uint8_t kGipAnnounce = 0x02;
constexpr uint8_t kGipPowerOff = 0x04;

// --- 802.11 ------------------------------------------------------------------------------------------------------------------------------------
constexpr uint8_t kTypeManagement = 0, kTypeData = 2;
constexpr uint8_t kSubtypeAssociationRequest = 0x00, kSubtypeAssociationResponse = 0x01, kSubtypeBeacon = 0x08, kSubtypeDisassociation = 0x0A,
                  kSubtypeDeauthentication = 0x0C, kSubtypeQosData = 0x08;
constexpr uint32_t kHeaderBytes = 24, kQosHeaderBytes = 26;

// An 802.11 frame: header fields and the body (for QoS data, what follows the QoS control field and any padding: the GIP message).
struct Frame {
  uint8_t type = 0, subtype = 0;
  MacAddress destination{}, source{}, bssid{};
  ConstByteView body;
};
// Builds a management frame (24-byte header, from the adapter) or a QoS data frame to a controller (from-DS, 26-byte header); 0 if `out` is too small.
uint32_t encode_management(uint8_t subtype, const MacAddress& destination, const MacAddress& bssid, uint16_t sequence, ConstByteView body, ByteView out);
uint32_t encode_qos_data(const MacAddress& destination, const MacAddress& bssid, uint16_t sequence, ConstByteView payload, ByteView out);
// Splits an 802.11 frame; `l2_pad`: the radio put two bytes between a 26-byte header and the body (RXWI L2PAD). False if it is cut short.
[[nodiscard]] bool decode_frame(ConstByteView frame, bool l2_pad, Frame* out);

// The adapter's beacon body (timestamp, interval, capability, hidden SSID, channel, vendor element with the pairing flag).
uint32_t encode_beacon_body(uint8_t channel, bool pairing, ByteView out);
// The association response body: capability, status 0, association id 0xC000 | wcid.
uint32_t encode_association_response_body(uint8_t wcid, ByteView out);

// --- USB framing -------------------------------------------------------------------------------------------------------------------------------
// A firmware chunk or MCU command for the command endpoint; 0 if `out` is too small or the payload too long (more than 0xFFFF bytes).
uint32_t encode_mcu_message(uint8_t command, uint8_t sequence, ConstByteView payload, ByteView out);
// An 802.11 frame for the data endpoint, sent to `wcid` (0xFF: no station, a broadcast).
uint32_t encode_wlan_packet(uint8_t wcid, ConstByteView frame, ByteView out);
// CMD_BURST_WRITE's payload: the WLAN register address, then the values (`data`, a multiple of 4 bytes).
uint32_t encode_burst_write(uint32_t register_offset, ConstByteView data, ByteView out);
// The TXWI that goes in front of a frame the radio keeps (the beacon in MT_BEACON_BASE), as encode_wlan_packet makes it.
uint32_t encode_txwi(uint8_t wcid, uint32_t frame_length, bool ack, ByteView out);
// CMD_SWITCH_CHANNEL_OP's payload (mt76x2_mcu_set_channel: channel, no scan, 20 MHz, chain mask, no extension channel).
uint32_t encode_switch_channel(uint8_t channel, ByteView out);

// What arrived on a bulk IN endpoint.
enum class RxKind : uint8_t { None, Frame, ButtonPress, ClientLost, OtherEvent };
struct RxPacket {
  RxKind kind = RxKind::None;
  uint8_t event = 0;         // the MCU event type (events)
  uint8_t wcid = 0xFF;       // the station table entry the radio matched (frames), or the client that was lost
  Frame frame;               // (RxKind::Frame) its body is a view into the packet
};
[[nodiscard]] bool decode_rx(ConstByteView packet, RxPacket* out);

// The GIP status message's battery (byte 0 of the payload: level bits 1:0, kind bits 3:2). False for another or a short message.
[[nodiscard]] bool decode_gip_battery(ConstByteView message, gamepad::State* state);

// The quietest channel: the sample with the smallest busy share (busy / (busy + idle)); the first on a tie or with no time measured. 0 if none.
struct ChannelSample {
  uint8_t channel = 0;
  uint32_t busy = 0, idle = 0;
};
uint8_t choose_channel(ArrayView<const ChannelSample> samples);

}  // namespace xbox_wireless
}  // namespace hardware
