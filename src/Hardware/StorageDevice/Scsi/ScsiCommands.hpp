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
#include "Utility/ByteStream.hpp"

namespace hardware::scsi {

constexpr uint8_t kTestUnitReady = 0x00, kInquiry = 0x12, kReadCapacity10 = 0x25, kRead10 = 0x28,
                  kWrite10 = 0x2A, kSynchronizeCache10 = 0x35, kRead16 = 0x88, kWrite16 = 0x8A,
                  kServiceActionIn16 = 0x9E;
constexpr uint8_t kReadCapacity16Action = 0x10;
constexpr uint8_t kDeviceTypeDisk = 0x00, kDeviceTypeOptical = 0x05;
constexpr uint8_t kStatusGood = 0x00, kStatusCheckCondition = 0x02;

// Fills `cdb` (at least 16 bytes, zeroed first) with a READ or WRITE of `blocks`
// blocks at `lba`; returns the number of bytes the command occupies (10 or 16).
inline uint32_t build_read_write(ByteView cdb, bool write, uint64_t lba, uint32_t blocks) {
  cdb.fill(0);
  ByteWriter out(cdb);
  if (lba <= 0xFFFFFFFFull && blocks <= 0xFFFF) {
    out.u8(write ? kWrite10 : kRead10);
    out.u8(0);
    out.be32(static_cast<uint32_t>(lba));
    out.u8(0);
    out.be16(static_cast<uint16_t>(blocks));
    return 10;
  }
  out.u8(write ? kWrite16 : kRead16);
  out.u8(0);
  out.be32(static_cast<uint32_t>(lba >> 32));
  out.be32(static_cast<uint32_t>(lba));
  out.be32(blocks);
  return 16;
}

// READ CAPACITY(10) data: last block number and block size. False if short.
inline bool parse_capacity10(ConstByteView data, uint64_t* last_block, uint32_t* block_size) {
  ByteReader in(data);
  *last_block = in.be32();
  *block_size = in.be32();
  return in.ok();
}

// READ CAPACITY(16) data (the first 12 bytes matter).
inline bool parse_capacity16(ConstByteView data, uint64_t* last_block, uint32_t* block_size) {
  ByteReader in(data);
  const uint64_t high = in.be32();
  *last_block = (high << 32) | in.be32();
  *block_size = in.be32();
  return in.ok();
}

// The standard INQUIRY data: what kind of device, removable or not, and the
// vendor + product text (bytes 8..31).
struct InquiryInfo {
  bool present = false;        // a logical unit is connected
  uint8_t device_type = 0;
  bool removable = false;
  ConstByteView vendor_and_product;
};
inline bool parse_inquiry(ConstByteView data, InquiryInfo* out) {
  ByteReader in(data);
  const uint8_t qualifier_and_type = in.u8();
  const uint8_t flags = in.u8();
  in.skip(6);
  const ConstByteView text = in.take(24);
  if (!in.ok()) return false;
  out->present = (qualifier_and_type >> 5) == 0;
  out->device_type = qualifier_and_type & 0x1F;
  out->removable = flags & 0x80;
  out->vendor_and_product = text;
  return true;
}

}  // namespace hardware::scsi
