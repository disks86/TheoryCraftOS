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

// The HCI packets, built and taken apart (Core spec vol 4 part E sections 5 and 7). Everything that comes from the controller is untrusted: lengths are checked against the
// bytes that are there and a packet that does not fit is refused.
#pragma once
#include <stdint.h>
#include "Hardware/Bluetooth/HciTypes.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace hci {

// --- commands: opcode (2), parameter length (1), parameters.
bool build_command(uint16_t opcode, ConstByteView params, ByteWriter& out);

// --- events: code (1), parameter length (1), parameters.
struct Event {
  uint8_t code = 0;
  ConstByteView params;
};
bool parse_event(ConstByteView packet, Event* out);

struct CommandComplete {
  uint8_t packets = 0;          // how many commands the controller takes now
  uint16_t opcode = 0;
  ConstByteView returned;       // the return parameters (the first byte is the status for nearly every command)
};
bool parse_command_complete(const Event& event, CommandComplete* out);

struct CommandStatus {
  uint8_t status = 0;
  uint8_t packets = 0;
  uint16_t opcode = 0;
};
bool parse_command_status(const Event& event, CommandStatus* out);

// --- ACL data: handle (12 bits) and flags (2 bits of packet boundary, 2 of broadcast), data length (2), data.
constexpr uint8_t kPbFirstNonFlushable = 0, kPbContinuing = 1, kPbFirstFlushable = 2;
struct AclPacket {
  uint16_t handle = 0;
  uint8_t boundary = 0, broadcast = 0;
  ConstByteView data;
};
bool build_acl(uint16_t handle, uint8_t boundary, ConstByteView data, ByteWriter& out);
bool parse_acl(ConstByteView packet, AclPacket* out);

// --- Decoded events the host acts on.
struct ConnectionComplete {
  uint8_t status = 0;
  uint16_t handle = 0;
  BdAddr address;
  uint8_t link_type = 0, encryption = 0;
};
bool parse_connection_complete(const Event& event, ConnectionComplete* out);

struct ConnectionRequest {
  BdAddr address;
  uint32_t class_of_device = 0;
  uint8_t link_type = 0;
};
bool parse_connection_request(const Event& event, ConnectionRequest* out);

struct DisconnectionComplete {
  uint8_t status = 0;
  uint16_t handle = 0;
  uint8_t reason = 0;
};
bool parse_disconnection_complete(const Event& event, DisconnectionComplete* out);

// One device of an inquiry (the three result formats are brought to this): the address, class, signal strength (127 when the result has none) and the extended inquiry data.
struct InquiryResult {
  BdAddr address;
  uint32_t class_of_device = 0;
  uint16_t clock_offset = 0;
  int8_t rssi = 127;
  ConstByteView eir;            // extended inquiry response data (AD structures), empty for the older formats
};
// Reads the results of an inquiry result event (any of the three formats) into `out`; returns how many were stored, or -1 if the event is damaged. Results beyond the room are dropped.
int parse_inquiry_results(const Event& event, ArrayView<InquiryResult> out);

// --- LE advertising reports (LE meta event, subevent 2): the address and its type, the kind of report, the data and the signal strength.
struct AdvertisingReport {
  uint8_t event_type = 0;       // 0 connectable undirected, 1 directed, 2 scannable, 3 non-connectable, 4 scan response
  uint8_t address_type = 0;
  BdAddr address;
  ConstByteView data;
  int8_t rssi = 127;
};
// Up to `out.size()` reports; the same return convention as above.
int parse_advertising_reports(const Event& event, ArrayView<AdvertisingReport> out);

struct LeConnectionComplete {
  uint8_t status = 0;
  uint16_t handle = 0;
  uint8_t role = 0;             // 0 central, 1 peripheral
  uint8_t peer_address_type = 0;
  BdAddr peer;
  uint16_t interval = 0, latency = 0, timeout = 0;
};
bool parse_le_connection_complete(const Event& event, LeConnectionComplete* out);

// The AD structures of advertising or extended inquiry data (length, type, value...): the first of `type`, or an empty view. A structure that runs past the data ends the walk.
ConstByteView find_ad_structure(ConstByteView data, uint8_t type);
constexpr uint8_t kAdFlags = 0x01, kAdUuids16Incomplete = 0x02, kAdUuids16 = 0x03, kAdUuids128 = 0x07, kAdShortName = 0x08, kAdName = 0x09, kAdTxPower = 0x0A,
                  kAdAppearance = 0x19, kAdManufacturer = 0xFF;

}  // namespace hci
}  // namespace hardware
