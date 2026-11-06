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

// USB Attached SCSI (UAS, T10 UAS-2 / USB-IF UASP 1.0): the information units a host and a UAS device exchange, and the interface a
// device offers for it. A UAS interface is an alternate setting (protocol 0x62) with four bulk pipes named by Pipe Usage descriptors:
// command (out), status (in), data in and data out. Every SCSI command is a Command IU with a tag; the device answers on the status pipe
// with a Sense IU (the SCSI status and sense data) or a Response IU (a transport error). On SuperSpeed the status and data pipes have
// streams and a command's transfers go on the stream of its tag, so many commands are in flight and finish in any order; without streams
// the device first sends Read Ready / Write Ready on the status pipe for the tag whose data is next. Pure: no hardware, host-tested.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware::uas {

constexpr uint8_t kProtocolUas = 0x62;
constexpr uint8_t kPipeCommand = 1, kPipeStatus = 2, kPipeDataIn = 3, kPipeDataOut = 4;
constexpr uint8_t kDescriptorPipeUsage = 0x24, kDescriptorSuperSpeedCompanion = 0x30;
constexpr uint8_t kIuCommand = 0x01, kIuSense = 0x03, kIuResponse = 0x04, kIuReadReady = 0x06, kIuWriteReady = 0x07;
constexpr uint32_t kCommandIuBytes = 32;
constexpr uint32_t kStatusIuBytes = 16 + 252;   // a Sense IU with the most sense data the status byte count allows here

// A Command IU for `tag` (1..65535) with a CDB of at most 16 bytes, simple task attribute, LUN 0. False if `out` is short or the CDB long.
bool build_command_iu(ByteView out, uint16_t tag, ConstByteView cdb);

// What came on the status pipe.
struct StatusIu {
  uint8_t id = 0;              // kIuSense, kIuResponse, kIuReadReady or kIuWriteReady
  uint16_t tag = 0;
  uint8_t status = 0;          // Sense IU: the SCSI status (0 GOOD, 2 CHECK CONDITION)
  ConstByteView sense;         // Sense IU: the sense data (a view into the input)
  uint8_t response_code = 0;   // Response IU
};
// False for anything that is not a whole IU of a kind above (short, an unknown id, a sense length past the end).
bool parse_status_iu(ConstByteView data, StatusIu* out);

// The UAS alternate setting of a configuration: its four pipes and how many streams the stream pipes have (0: none, so not SuperSpeed).
struct UasInterface {
  uint8_t number = 0, alternate = 0;
  uint8_t endpoint[4] = {};        // by pipe id - 1: command, status, data in, data out
  uint16_t max_packet[4] = {};
  uint32_t streams = 0;            // the least the status and data pipes' companion descriptors offer (2^MaxStreams)
};
bool find_uas_interface(ConstByteView configuration, UasInterface* out);

}  // namespace hardware::uas
