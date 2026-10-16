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

namespace hardware::scsi {

constexpr uint8_t kTestUnitReady = 0x00, kInquiry = 0x12, kReadCapacity10 = 0x25, kRead10 = 0x28,
                  kWrite10 = 0x2A, kSynchronizeCache10 = 0x35, kRead16 = 0x88, kWrite16 = 0x8A,
                  kServiceActionIn16 = 0x9E;
constexpr uint8_t kReadCapacity16Action = 0x10;
constexpr uint8_t kDeviceTypeDisk = 0x00, kDeviceTypeOptical = 0x05;
constexpr uint8_t kStatusGood = 0x00, kStatusCheckCondition = 0x02;

// Fills `cdb` (zeroed first, at least 16 bytes) with a READ or WRITE of `blocks`
// blocks at `lba`; returns the number of bytes the command occupies (10 or 16).
inline uint32_t build_read_write(uint8_t* cdb, bool write, uint64_t lba, uint32_t blocks) {
  for (int i = 0; i < 16; ++i) cdb[i] = 0;
  if (lba <= 0xFFFFFFFFull && blocks <= 0xFFFF) {
    cdb[0] = write ? kWrite10 : kRead10;
    for (int i = 0; i < 4; ++i) cdb[2 + i] = static_cast<uint8_t>(lba >> (24 - 8 * i));
    cdb[7] = static_cast<uint8_t>(blocks >> 8);
    cdb[8] = static_cast<uint8_t>(blocks);
    return 10;
  }
  cdb[0] = write ? kWrite16 : kRead16;
  for (int i = 0; i < 8; ++i) cdb[2 + i] = static_cast<uint8_t>(lba >> (56 - 8 * i));
  for (int i = 0; i < 4; ++i) cdb[10 + i] = static_cast<uint8_t>(blocks >> (24 - 8 * i));
  return 16;
}

inline uint32_t be32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
inline uint64_t be64(const uint8_t* p) { return (uint64_t{be32(p)} << 32) | be32(p + 4); }

}  // namespace hardware::scsi
