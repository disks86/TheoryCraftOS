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

#include "Hardware/Platform/RaspberryPi/RaspberryPiBoard.hpp"

namespace hardware {

namespace {
const char* model_name(uint8_t type) {
  switch (type) {
    case 0x00: return "Pi A";
    case 0x01: return "Pi B";
    case 0x02: return "Pi A+";
    case 0x03: return "Pi B+";
    case 0x04: return "Pi 2 B";
    case 0x06: return "Compute Module 1";
    case 0x08: return "Pi 3 B";
    case 0x09: return "Pi Zero";
    case 0x0A: return "Compute Module 3";
    case 0x0C: return "Pi Zero W";
    case 0x0D: return "Pi 3 B+";
    case 0x0E: return "Pi 3 A+";
    case 0x10: return "Compute Module 3+";
    case 0x11: return "Pi 4 B";
    case 0x12: return "Pi Zero 2 W";
    case 0x13: return "Pi 400";
    case 0x14: return "Compute Module 4";
    case 0x15: return "Compute Module 4S";
    case 0x17: return "Pi 5";
    default: return "unknown";
  }
}
const char* processor_label(PiProcessor p) {
  switch (p) {
    case PiProcessor::Bcm2835: return "BCM2835";
    case PiProcessor::Bcm2836: return "BCM2836";
    case PiProcessor::Bcm2837: return "BCM2837";
    case PiProcessor::Bcm2711: return "BCM2711";
    case PiProcessor::Bcm2712: return "BCM2712";
    default: return "unknown";
  }
}
}  // namespace

PiBoard pi_decode_revision(uint32_t code) {
  PiBoard board;
  board.revision_code = code;
  if (!(code & (1u << 23))) return board;  // old-style numbered revision
  board.valid = true;
  board.board_revision = static_cast<uint8_t>(code & 0xF);
  board.type = static_cast<uint8_t>((code >> 4) & 0xFF);
  const uint32_t proc = (code >> 12) & 0xF;
  board.processor = proc <= 4 ? static_cast<PiProcessor>(proc) : PiProcessor::Unknown;
  const uint32_t memory_code = (code >> 20) & 0x7;  // 0 = 256 MiB, 1 = 512 MiB, ... 6 = 8 GiB
  board.memory_mib = 256u << memory_code;
  board.model = model_name(board.type);
  board.processor_name = processor_label(board.processor);
  return board;
}

}  // namespace hardware
