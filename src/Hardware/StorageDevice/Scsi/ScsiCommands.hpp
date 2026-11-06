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
                  kServiceActionIn16 = 0x9E, kUnmap = 0x42, kWriteSame16 = 0x93;
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

// READ CAPACITY(16) byte 14 bit 7, LBPME: the unit is thin provisioned (logical block provisioning management), the start of UNMAP support;
// bit 6, LBPRZ: unmapped blocks read back as zeros.
inline bool capacity16_thin_provisioned(ConstByteView data) { return data.size() > 14 && (data[14] & 0x80); }
inline bool capacity16_unmapped_reads_zeros(ConstByteView data) { return data.size() > 14 && (data[14] & 0x40); }

// INQUIRY with EVPD for a vital product data page (in a 6-byte CDB): `length` bytes of page `page`.
constexpr uint8_t kVpdBlockLimits = 0xB0, kVpdLogicalBlockProvisioning = 0xB2;
inline void build_vpd_inquiry(ByteView cdb, uint8_t page, uint16_t length) {
  cdb.fill(0);
  cdb[0] = kInquiry;
  cdb[1] = 1;   // EVPD
  cdb[2] = page;
  cdb[3] = static_cast<uint8_t>(length >> 8);
  cdb[4] = static_cast<uint8_t>(length);
}
// The Logical Block Provisioning page (B2h): byte 5 bit 7, LBPU - UNMAP works. False if the page is short or another one.
inline bool parse_lbp_vpd_unmap(ConstByteView data) { return data.size() > 5 && data[1] == kVpdLogicalBlockProvisioning && (data[5] & 0x80); }
// The Block Limits page (B0h): the most blocks one UNMAP may name (bytes 20-23) and descriptors it may carry (24-27); 0 when not given.
inline bool parse_block_limits(ConstByteView data, uint32_t* max_unmap_blocks, uint32_t* max_unmap_descriptors) {
  ByteReader in(data);
  in.skip(1);
  const uint8_t page = in.u8();
  in.skip(18);
  *max_unmap_blocks = in.be32();
  *max_unmap_descriptors = in.be32();
  return in.ok() && page == kVpdBlockLimits;
}

// The Block Limits page's MAXIMUM WRITE SAME LENGTH (bytes 36-43; 0: none given). False if the page is short or another one.
inline bool parse_write_same_limit(ConstByteView data, uint64_t* max_blocks) {
  ByteReader in(data);
  in.skip(1);
  const uint8_t page = in.u8();
  in.skip(34);
  const uint64_t high = in.be32();
  *max_blocks = (high << 32) | in.be32();
  return in.ok() && page == kVpdBlockLimits;
}

// WRITE SAME(16) without UNMAP: the one block of data the command carries is written to `blocks` blocks from `lba`.
inline void build_write_same16(ByteView cdb, uint64_t lba, uint32_t blocks) {
  cdb.first(16).fill(0);
  ByteWriter out(cdb.first(16));
  out.u8(kWriteSame16);
  out.u8(0);
  out.be32(static_cast<uint32_t>(lba >> 32));
  out.be32(static_cast<uint32_t>(lba));
  out.be32(blocks);
}

// UNMAP of one range: the 10-byte CDB in `cdb` and the 24-byte parameter list (header and one block descriptor) in `parameters`.
constexpr uint32_t kUnmapParameterBytes = 24;
inline bool build_unmap(ByteView cdb, ByteView parameters, uint64_t lba, uint32_t blocks) {
  if (cdb.size() < 10 || parameters.size() < kUnmapParameterBytes) return false;
  cdb.first(10).fill(0);
  cdb[0] = kUnmap;
  cdb[7] = 0;
  cdb[8] = kUnmapParameterBytes;   // parameter list length
  ByteWriter out(parameters.first(kUnmapParameterBytes));
  out.be16(kUnmapParameterBytes - 2);   // data length after this field
  out.be16(16);                         // block descriptor data length
  out.be32(0);
  out.be32(static_cast<uint32_t>(lba >> 32));
  out.be32(static_cast<uint32_t>(lba));
  out.be32(blocks);
  out.be32(0);
  return true;
}

// What a unit says about UNMAP, from READ CAPACITY(16) and the provisioning pages (probe_unmap).
struct UnmapSupport {
  bool unmap = false;          // UNMAP works
  bool reads_zeros = false;    // unmapped blocks read as zeros (LBPRZ)
  uint32_t max_blocks = 0;     // the most one UNMAP may name
};

// The drivers that speak SCSI share these. `run(cdb, data, data_in)` runs one command on the unit with `data` as its data stage (to the host
// when data_in) and says whether it succeeded; `scratch` is at least 64 bytes the transport can move data through.
// Thin provisioning (LBPME), then UNMAP support (the LBP page's LBPU) and its limit (the Block Limits page: 0 there means no UNMAP after all;
// no page means no limit was given, so a conservative 2 GiB of 512-byte blocks).
template <class Run>
UnmapSupport probe_unmap(Run&& run, ByteView scratch) {
  UnmapSupport support;
  if (scratch.size() < 64) return support;
  const ByteView data = scratch.first(64);
  const uint8_t capacity16[16] = {kServiceActionIn16, kReadCapacity16Action, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
  data.fill(0);
  if (!run(ConstByteView(capacity16), data.first(32), true) || !capacity16_thin_provisioned(data.first(32))) return support;
  const bool zeros = capacity16_unmapped_reads_zeros(data.first(32));
  uint8_t cdb[6];
  build_vpd_inquiry(ByteView(cdb), kVpdLogicalBlockProvisioning, 64);
  data.fill(0);
  if (!run(ConstByteView(cdb), data, true) || !parse_lbp_vpd_unmap(data)) return support;
  uint32_t max_blocks = 0x400000, descriptors = 1;
  build_vpd_inquiry(ByteView(cdb), kVpdBlockLimits, 64);
  data.fill(0);
  if (run(ConstByteView(cdb), data, true) && parse_block_limits(data, &max_blocks, &descriptors) && (max_blocks == 0 || descriptors == 0)) return support;
  support.unmap = true;
  support.reads_zeros = zeros;
  support.max_blocks = max_blocks;
  return support;
}

// UNMAP of [lba, lba + blocks), one block descriptor a command, `support.max_blocks` at most each; `scratch` (24 bytes or more) carries the
// parameter list.
template <class Run>
bool unmap_range(Run&& run, const UnmapSupport& support, ByteView scratch, uint64_t lba, uint64_t blocks) {
  if (!support.unmap || support.max_blocks == 0 || scratch.size() < kUnmapParameterBytes) return false;
  while (blocks > 0) {
    const uint32_t n = blocks < support.max_blocks ? static_cast<uint32_t>(blocks) : support.max_blocks;
    uint8_t cdb[10];
    if (!build_unmap(ByteView(cdb), scratch, lba, n) || !run(ConstByteView(cdb), scratch.first(kUnmapParameterBytes), false)) return false;
    lba += n;
    blocks -= n;
  }
  return true;
}

// WRITE SAME as write zeroes: a unit with a Block Limits page is taken to have it (the limit there, or 65535 blocks a command when it
// gives none); whether it really does shows on the first use (write_same_zeroes fails, and the caller stops trying).
template <class Run>
uint64_t probe_write_same(Run&& run, ByteView scratch) {
  if (scratch.size() < 64) return 0;
  const ByteView data = scratch.first(64);
  uint8_t cdb[6];
  build_vpd_inquiry(ByteView(cdb), kVpdBlockLimits, 64);
  data.fill(0);
  uint64_t max_blocks = 0;
  if (!run(ConstByteView(cdb), data, true) || !parse_write_same_limit(data, &max_blocks)) return 0;
  return max_blocks ? max_blocks : 0xFFFF;
}

// Zeroes [lba, lba + blocks) with WRITE SAME(16) commands of at most `max_blocks` (and 2^32 - 1); `zero_block` is one block of the unit's
// size, filled with zeros here, that each command sends.
template <class Run>
bool write_same_zeroes(Run&& run, ByteView zero_block, uint64_t max_blocks, uint64_t lba, uint64_t blocks) {
  if (max_blocks == 0 || zero_block.empty()) return false;
  if (max_blocks > 0xFFFFFFFFull) max_blocks = 0xFFFFFFFFull;
  zero_block.fill(0);
  while (blocks > 0) {
    const uint32_t n = static_cast<uint32_t>(blocks < max_blocks ? blocks : max_blocks);
    uint8_t cdb[16];
    build_write_same16(ByteView(cdb), lba, n);
    if (!run(ConstByteView(cdb), zero_block, false)) return false;
    lba += n;
    blocks -= n;
  }
  return true;
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
