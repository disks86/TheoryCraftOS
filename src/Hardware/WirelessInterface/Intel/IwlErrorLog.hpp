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

// What the firmware leaves behind when it stops: the "error event table" (iwl_error_event_table, 38 dwords) at the address its ALIVE notification gave. `valid` counts the entries (0: the log
// is empty), `error_id` says what it asserted on, and data1 / data2 / data3, the link registers and the interrupt status say where. Nothing here interprets the ids (the names are Intel's);
// the words are kept and printed so that a failure on a machine can be looked up.
#pragma once
#include <stdint.h>
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"

namespace hardware {

struct IwlErrorLog {
  static constexpr uint32_t kWords = 38;
  uint32_t words[kWords] = {};
  bool valid() const { return words[0] != 0; }
  uint32_t error_id() const { return words[1]; }
  uint32_t data1() const { return words[7]; }
  uint32_t data2() const { return words[8]; }
  uint32_t data3() const { return words[9]; }
  uint32_t ilink2() const { return words[6]; }
  uint32_t blink2() const { return words[4]; }
  uint32_t hcmd() const { return words[23]; }
  uint32_t last_command_id() const { return words[29]; }
};

// Reads the table at device address `table` (0 and addresses that are not dword aligned are refused). The MAC is woken for the read. False if the pointer is unusable.
bool iwl_read_error_log(IwlPcieDevice& device, uint32_t table, IwlErrorLog* out);
// "firmware error 0x00000070: data 0x... 0x... 0x..., ilink2 0x..., blink2 0x..., hcmd 0x..., last command 0x..": into `out` (NUL terminated, cut to fit); returns the length.
uint32_t iwl_format_error_log(const IwlErrorLog& log, char* out, uint32_t capacity);

}  // namespace hardware
