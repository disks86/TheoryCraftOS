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

// Realtek eFuse: the one-time-programmable memory that holds the MAC address, calibration and board data. The chip stores it "physically" as a log of patches,
// which a driver replays into the "logical" map that the chip structs (struct rtw8822c_efuse, struct rtw8852a_efuse, ...) describe.
//
// rtw88 (after rtw88/efuse.c, rtw_dump_logical_efuse_map): each patch is a header and up to four 16-bit words. A one-byte header holds the block (bits 7:4) and a
// word-enable nibble (bits 3:0); when its low five bits are 01111 it is an extended header and a second byte follows: block = (hdr2 bits 7:4) << 3 | (hdr1 bits 7:5),
// word enable = hdr2 bits 3:0. A word whose enable bit is CLEAR is present (two bytes follow, in order); a block is 8 bytes of the logical map. 0xFF ends the log (as
// does an extended header with a second byte of 0xFF). The last `protect_size` physical bytes are not part of the log.
// rtw89 (after rtw89/efuse.c, rtw89_eeprom_parser): always a two-byte header, block = (hdr1 bits 3:0) << 4 | (hdr2 bits 7:4), word enable = hdr2 bits 3:0; the log
// ends at a header byte of 0xFF.
//
// The physical map is read byte by byte through REG_EFUSE_CTRL (0x30, rtw_dump_physical_efuse_map): write the address (bits 17:8) with bit 31 clear, wait for bit 31
// to be set, the byte is bits 7:0. rtw89 reads through its own DAV/DDV engines and is not done here.
#pragma once
#include <stdint.h>
#include "Hardware/NetworkInterface/NetworkTypes.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

constexpr uint32_t kRtwRegEfuseCtrl = 0x0030, kRtwRegLdoEfuseCtrl = 0x0034, kRtwRegEfuseAccess = 0x00CF;
constexpr uint8_t kRtwEfuseAccessOn = 0x69, kRtwEfuseAccessOff = 0x00;

// Replays `physical` (all of it but the last `protect_size` bytes) into `logical`, which is first filled with 0xFF. False if a patch runs past either map.
[[nodiscard]] bool rtw88_efuse_logical_map(ConstByteView physical, uint32_t protect_size, ByteView logical);
[[nodiscard]] bool rtw89_efuse_logical_map(ConstByteView physical, uint32_t protect_size, ByteView logical);

// The six bytes at `offset` of a logical map as a MAC address. False if they do not fit or are not a usable unicast address (all 0xFF from an unwritten eFuse,
// all zero, or multicast).
[[nodiscard]] bool rtw_efuse_mac_address(ConstByteView logical, uint32_t offset, MacAddress* out);

// Reads the whole physical eFuse of an rtw88 chip into `physical` (its size is the chip's physical size). False if a byte never became ready.
[[nodiscard]] bool rtw88_read_physical_efuse(INicRegisters& registers, ByteView physical);

}  // namespace hardware
