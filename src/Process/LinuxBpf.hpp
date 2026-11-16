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
#pragma once

#include <stdint.h>

#include "Utility/ArrayView.hpp"

// Classic BPF: the little programs a socket filter is (SO_ATTACH_FILTER, struct sock_filter), which say for each packet how much of it the socket
// wants (0: none). One accumulator, one index register, sixteen words of memory, loads from the packet, arithmetic, forward jumps. A program is checked
// once when it is attached; a checked program always ends, and a load outside the packet ends it with 0.
namespace process::linuxabi::bpf {

constexpr uint32_t kInstructionSize = 8;      // struct sock_filter: code (2), jt (1), jf (1), k (4)
constexpr uint32_t kMostInstructions = 4096;

struct Packet {
  ConstByteView data;              // what the filter's offsets count from: the frame, or (a socket without link headers) what follows the link header
  ConstByteView link;              // the link header when `data` does not start with it (for loads relative to it)
  uint32_t network_offset = 0;     // where the network header is in `data`
  uint32_t wire_length = 0;        // the packet's whole length
  uint16_t protocol = 0;           // the ether type
  uint8_t packet_type = 0;         // PACKET_HOST, ...
  uint32_t interface_index = 0;
};

// Is `program` one that may be run? (Whole instructions, at most kMostInstructions, known operations, jumps and memory within bounds, no constant
// division by zero, a return at the end.)
[[nodiscard]] bool valid(ConstByteView program);
// What a checked program says of a packet: how many bytes of it to keep (0: drop it).
uint32_t run(ConstByteView program, const Packet& packet);

}  // namespace process::linuxabi::bpf
