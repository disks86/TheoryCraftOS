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

namespace hardware::rpi_hevc {

// Registers of the HEVC decoder block of the BCM2711 (the "Argon" core: H.265 Main and Main 10 up to 4096x2176 at 60 frames per second; the Pi 4 has no other
// hardware decoder for HEVC and none for VP9 or AV1). Phase 1 entropy-decodes a slice into intermediate buffers; phase 2 reconstructs the picture from them.
// Offsets are in bytes from the start of the decoder's register window, as in the Raspberry Pi kernel's rpivid driver. Addresses written to the block
// are VideoCore bus addresses divided by 64, lengths in units of 64 bytes.
constexpr uint32_t bit(uint32_t n) { return 1u << n; }

// Phase 1
constexpr uint32_t kSps0 = 0, kSps1 = 4, kPps = 8, kSlice = 12, kTileStart = 16, kTileEnd = 20, kSliceStart = 24, kMode = 28;
constexpr uint32_t kLeft0 = 32, kLeft1 = 36, kLeft2 = 40, kLeft3 = 44, kQp = 48, kControl = 52, kStatus = 56, kVersion = 60;
constexpr uint32_t kBfBase = 64, kBfNum = 68, kBfControl = 72, kBfStatus = 76, kPuwBase = 80, kPuwStride = 84, kCoeffwBase = 88, kCoeffwStride = 92;
constexpr uint32_t kSliceCmds = 96, kBeginTileEnd = 100, kTransfer = 104, kCfBase = 108, kCfNum = 112, kCfStatus = 116;
// Phase 2
constexpr uint32_t kPurBase = 0x8000, kPurStride = 0x8004, kCoeffrBase = 0x8008, kCoeffrStride = 0x800C, kNumRows = 0x8010, kConfig2 = 0x8014;
constexpr uint32_t kOutYBase = 0x8018, kOutYStride = 0x801C, kOutCBase = 0x8020, kOutCStride = 0x8024, kStatus2 = 0x8028, kFrameSize = 0x802C;
constexpr uint32_t kMvBase = 0x8030, kMvStride = 0x8034, kColBase = 0x8038, kColStride = 0x803C, kCurrPoc = 0x8040;

// The block's interrupt controller (a separate window): one control register.
constexpr uint32_t kIcCtrl = 0;
constexpr uint32_t kIcActive1Int = bit(0), kIcActive1Edge = bit(1), kIcActive1Enable = bit(2), kIcActive1Status = bit(3);
constexpr uint32_t kIcActive2Int = bit(4), kIcActive2Edge = bit(5), kIcActive2Enable = bit(6), kIcActive2Status = bit(7);
constexpr uint32_t kIcTestInt = bit(8);
constexpr uint32_t kIcAioEnable = bit(20);
// Bits 19:12 and 11 are reserved: written as zero.
constexpr uint32_t kIcReservedMask = (0xFFu << 12) | bit(11);
constexpr uint32_t kIcAllInterrupts = kIcActive1Int | kIcActive2Int | bit(25) | bit(29);  // the two active lines, and the H.264 and VP9 lines

constexpr uint64_t kMaxWidth = 4096, kMaxHeight = 2176;

constexpr uint32_t to_bus_units(uint32_t bus_address) { return bus_address >> 6; }
constexpr uint32_t length_units(uint32_t bytes) { return (bytes + 63) >> 6; }

}  // namespace hardware::rpi_hevc
