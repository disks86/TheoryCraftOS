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

#include "Hardware/VideoCodec/AMD/AmdIpDiscovery.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
// The container, as the amdgpu driver (MIT) documents it. All fields are little-endian.
//   binary_header: u32 signature; u16 version_major, version_minor, checksum, size; then a list of tables {u16 offset, ...}. Table 0 is the IP list.
//   ip_discovery_header: u32 signature "IPDS"; u16 version, size; u32 id; u16 num_dies; 16 x {u16 die_id, u16 die_offset}; flags.
//   die_header: u16 die_id, num_ips; then the IPs: u16 hw_id; u8 instance, num_base_address, major, minor, revision, flags; base_address[].
constexpr uint32_t kBinarySignature = 0x28211407;
constexpr uint32_t kIpSignature = 0x53445049;   // "IPDS"
constexpr uint32_t kBinaryHeaderFixed = 12;     // signature, versions, checksum, size
constexpr uint32_t kChecksumCoversFrom = 10;    // the checksum field is the last of the first ten bytes and not part of the sum
constexpr uint32_t kMaxDies = 16;
constexpr uint32_t kIpFlagsOffset = 4 + 2 + 2 + 4 + 2 + kMaxDies * 4;  // flags byte of the IP list header
constexpr uint16_t kWideBaseAddressVersion = 4;                         // from this version the base addresses may be 64 bit

bool checksum_matches(ConstByteView table, uint32_t size, uint16_t expected) {
  if (size < kChecksumCoversFrom || size > table.size()) return false;
  uint16_t sum = 0;
  for (uint32_t i = kChecksumCoversFrom; i < size; ++i) sum = static_cast<uint16_t>(sum + table[i]);
  return sum == expected;
}

void count(AmdIpDiscoveryInfo& info, uint16_t hw_id, const AmdIpVersion& version) {
  switch (hw_id) {
    case kAmdHwIdGc: if (info.gc_instances++ == 0) info.gc = version; break;
    case kAmdHwIdVcn: if (info.vcn_instances++ == 0) info.vcn = version; break;
    case kAmdHwIdSdma0: case kAmdHwIdSdma1: case kAmdHwIdSdma2: case kAmdHwIdSdma3: ++info.sdma_instances; break;
    default: break;
  }
}
}  // namespace

uint32_t amd_ip_discovery_declared_size(ConstByteView header) {
  ByteReader in(header);
  if (in.le32() != kBinarySignature) return 0;
  in.skip(4);   // versions
  in.skip(2);   // checksum
  const uint16_t size = in.le16();
  return in.ok() ? size : 0;
}

AmdIpDiscoveryInfo amd_parse_ip_discovery(ConstByteView table) {
  AmdIpDiscoveryInfo info;
  ByteReader head(table);
  if (head.le32() != kBinarySignature) return info;
  const uint16_t version_major = head.le16(), version_minor = head.le16(), checksum = head.le16(), size = head.le16();
  const uint16_t ip_offset = head.le16();   // table 0: the IP list
  if (!head.ok() || size < kBinaryHeaderFixed || size > table.size() || ip_offset < kBinaryHeaderFixed || ip_offset >= size) return info;
  const ConstByteView binary = table.first(size);

  // The IP list header, and the die table in it.
  ByteReader list(binary.subview(ip_offset));
  if (list.le32() != kIpSignature) return info;
  const uint16_t list_version = list.le16();
  list.skip(2 + 4);   // size, id
  const uint16_t num_dies = list.le16();
  if (!list.ok() || num_dies == 0 || num_dies > kMaxDies) return info;
  uint16_t die_offsets[kMaxDies] = {};
  for (uint32_t i = 0; i < kMaxDies; ++i) {
    list.skip(2);   // die id
    die_offsets[i] = list.le16();
  }
  ConstByteView flags_view = binary.subview(ip_offset);
  if (flags_view.size() <= kIpFlagsOffset) return info;
  const bool wide_base = list_version >= kWideBaseAddressVersion && (flags_view[kIpFlagsOffset] & 1);
  const uint32_t base_bytes = wide_base ? 8 : 4;

  for (uint32_t die = 0; die < num_dies; ++die) {
    if (die_offsets[die] >= size) return info;
    ByteReader in(binary.subview(die_offsets[die]));
    in.skip(2);   // die id
    const uint16_t num_ips = in.le16();
    for (uint32_t i = 0; i < num_ips; ++i) {
      const uint16_t hw_id = in.le16();
      in.skip(1);   // instance number
      const uint8_t num_base_address = in.u8();
      AmdIpVersion version;
      version.major = in.u8();
      version.minor = in.u8();
      version.revision = in.u8();
      in.skip(1);   // flags (harvest or variant, depending on the list version)
      in.skip(uint64_t{num_base_address} * base_bytes);
      if (!in.ok()) return info;   // an entry that runs past the table: not a table we can trust
      count(info, hw_id, version);
      ++info.ip_count;
    }
  }

  info.valid = true;
  info.checksum_ok = checksum_matches(binary, size, checksum);
  info.binary_version = static_cast<uint16_t>((version_major << 8) | (version_minor & 0xFF));
  info.table_version = list_version;
  return info;
}

}  // namespace hardware
