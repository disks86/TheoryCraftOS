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

namespace hardware {

enum class PiProcessor : uint8_t { Bcm2835, Bcm2836, Bcm2837, Bcm2711, Bcm2712, Unknown };

// A Raspberry Pi board as its revision code (the VideoCore firmware's "get board revision") describes it. Only the new-style code (bit 23 set) is decoded;
// the numbered revisions of the first boards say nothing a driver needs.
struct PiBoard {
  bool valid = false;
  uint32_t revision_code = 0;
  uint8_t type = 0;           // 0x13 = Pi 400
  uint8_t board_revision = 0;
  PiProcessor processor = PiProcessor::Unknown;
  uint32_t memory_mib = 0;
  const char* model = "unknown";
  const char* processor_name = "unknown";
  // The SoC is a BCM2711: the GPU is a V3D 4.2 and the video engine has the HEVC decoder block.
  bool is_bcm2711() const { return processor == PiProcessor::Bcm2711; }
  bool is_pi400() const { return type == 0x13; }
};

PiBoard pi_decode_revision(uint32_t revision_code);

}  // namespace hardware
