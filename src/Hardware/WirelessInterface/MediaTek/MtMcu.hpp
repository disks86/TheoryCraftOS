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

// Messages to and from the MCU of a connac2 MediaTek chip (MT7921/MT7922/MT7925), after mt76_connac2_mcu_fill_message of mt76_connac_mcu.c and the structs of
// mt76_connac2_mac.h / mt76_connac_mcu.h:
//
// - A command is the 32-byte hardware TXD (txd[0]: total length, packet format CMD, queue MCU_PORT_RX_Q0; txd[1]: long format, header format CMD) followed by the MCU
//   header: struct mt76_connac2_mcu_txd (64 bytes in all: length after the TXD, port/queue id, command id, packet type 0xa0, set/query, sequence number, extended
//   command id, S2D index) for the legacy commands, or struct mt76_connac2_mcu_uni_txd (48 bytes: length, 16 bit command id, packet type, sequence, S2D, option) for
//   the "unified" commands of newer firmware. Then the payload.
// - FW_SCATTER (the firmware's bytes themselves) is sent raw, with no header, on the firmware download ring.
// - An event (the answer) is struct mt76_connac2_mcu_rxd (36 bytes: six hardware RXD words, length, packet type, event id, sequence number, option, extended event id,
//   S2D index) followed by its data. The answer to a command carries the command's sequence number. PATCH_SEM_CONTROL and PATCH_FINISH_REQ answer with a status byte
//   (mt7921_mcu_parse_response reads it at offset sizeof(rxd) - 4 = 32).
//
// Only the commands of the firmware download are here (PATCH_SEM_CONTROL, PATCH_START_REQ / TARGET_ADDRESS_LEN_REQ, FW_SCATTER, PATCH_FINISH_REQ, FW_START_REQ).
// Unverified against a chip: the bit positions were transcribed from the Linux headers; no real MT7921 has been driven with them.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace mt_mcu {
// MCU_CMD_* (legacy command ids)
constexpr uint8_t kTargetAddressLenReq = 0x01, kFwStartReq = 0x02, kPatchStartReq = 0x05, kPatchFinishReq = 0x07, kPatchSemControl = 0x10, kFwScatter = 0xEE,
                  kRestartDlReq = 0xEF;
constexpr uint32_t kTxdBytes = 32;              // txd[8]
constexpr uint32_t kHeaderBytes = 64;           // struct mt76_connac2_mcu_txd, txd[8] included
constexpr uint32_t kUniHeaderBytes = 48;        // struct mt76_connac2_mcu_uni_txd
constexpr uint32_t kEventHeaderBytes = 36;      // struct mt76_connac2_mcu_rxd
constexpr uint8_t kPacketId = 0xA0;             // MCU_PKT_ID
constexpr uint8_t kQueryQuery = 0, kQuerySet = 1, kQueryNa = 2;      // MCU_Q_*
constexpr uint8_t kS2dH2n = 0, kS2dC2n = 1, kS2dH2c = 2, kS2dH2cn = 3;  // MCU_S2D_*
constexpr uint8_t kUniExtAck = 0x07;            // MCU_CMD_UNI_EXT_ACK = ACK | UNI | QUERY
// txd[0] / txd[1] fields
constexpr uint32_t kTxd0TxBytesMask = 0xFFFF, kTxd0PacketFormatShift = 23, kTxd0QueueShift = 25;
constexpr uint32_t kTxTypeCmd = 2, kTxMcuPortRxQ0 = 0x20, kTxPortIndexMcu = 1;
constexpr uint32_t kTxd1LongFormat = 1u << 31, kTxd1HeaderFormatShift = 16, kHeaderFormatCmd = 1;
// rxd[0]
constexpr uint32_t kRxd0LengthMask = 0xFFFF, kRxd0PacketTypeShift = 27, kPacketTypeRxEvent = 7;
// PATCH_SEM_CONTROL
constexpr uint32_t kSemRelease = 0, kSemGet = 1;
constexpr uint8_t kPatchNotDlSemFail = 0, kPatchIsDl = 1, kPatchNotDlSemSuccess = 2, kPatchRelSemSuccess = 3;
// Addresses that are downloaded with PATCH_START_REQ instead of TARGET_ADDRESS_LEN_REQ (mt76_connac_mcu_init_download).
constexpr uint32_t kPatchAddress = 0x200000, kConnac2PatchAddress = 0x900000, kMt7925PatchAddress2 = 0xE0002800;
}  // namespace mt_mcu

// The sequence numbers of a connac2 MCU: 1..15 (++msg_seq & 0xf, skipping 0).
class MtMcuSequence {
 public:
  uint8_t next() {
    uint8_t seq = static_cast<uint8_t>(++counter_ & 0xF);
    if (!seq) seq = static_cast<uint8_t>(++counter_ & 0xF);
    return seq;
  }

 private:
  uint32_t counter_ = 0;
};

// Writes a legacy command (TXD + mt76_connac2_mcu_txd + payload) to the start of `out`. `ext_cid` 0 for a plain MCU_CMD. Returns the bytes written, 0 if `out` is too
// small or the message is too long for the TXD's 16 bit length.
[[nodiscard]] uint32_t mt_mcu_build_command(uint8_t cid, uint8_t ext_cid, bool query, uint8_t seq, ConstByteView payload, ByteView out);
// The same for a unified command (TXD + mt76_connac2_mcu_uni_txd + payload).
[[nodiscard]] uint32_t mt_mcu_build_uni_command(uint16_t cid, uint8_t seq, ConstByteView payload, ByteView out);
// The command used to start the download of `length` bytes at `address`: PATCH_START_REQ for the patch addresses, TARGET_ADDRESS_LEN_REQ otherwise.
uint8_t mt_mcu_init_download_command(uint32_t address, bool mt7925);

// Payloads (little endian), each into `out`; the bytes written or 0.
[[nodiscard]] uint32_t mt_mcu_patch_sem_payload(bool get, ByteView out);                                   // 4 bytes
[[nodiscard]] uint32_t mt_mcu_init_download_payload(uint32_t address, uint32_t length, uint32_t mode, ByteView out);   // 12 bytes
[[nodiscard]] uint32_t mt_mcu_patch_finish_payload(ByteView out);                                         // 4 bytes (check_crc 0)
[[nodiscard]] uint32_t mt_mcu_fw_start_payload(uint32_t option, uint32_t address, ByteView out);          // 8 bytes

struct MtMcuEvent {
  uint32_t rx_bytes = 0;       // rxd[0] length
  uint8_t packet_type = 0;     // rxd[0] bits 27-31 (7: event)
  uint16_t length = 0;
  uint16_t packet_type_id = 0;
  uint8_t eid = 0, seq = 0, option = 0, ext_eid = 0, s2d_index = 0;
  ConstByteView message;       // the whole event, as far as rxd[0] says
  ConstByteView data;          // after the 36-byte header
};

// Parses an event received from the MCU ring. False if it is shorter than its header, not of the event type, or rxd[0]'s length disagrees with what was received.
[[nodiscard]] bool mt_mcu_parse_event(ConstByteView received, MtMcuEvent* out);
// The status byte of the answer to PATCH_SEM_CONTROL / PATCH_FINISH_REQ (offset 32). False if the event is too short.
[[nodiscard]] bool mt_mcu_event_status(const MtMcuEvent& event, uint8_t* status);

}  // namespace hardware
