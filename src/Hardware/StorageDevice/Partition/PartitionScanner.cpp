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

#include "Hardware/StorageDevice/Partition/PartitionScanner.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/Crc32.hpp"
#include "Utility/Utf8.hpp"

namespace hardware {

namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }
uint64_t le64(const uint8_t* p) { return uint64_t{le32(p)} | (uint64_t{le32(p + 4)} << 32); }

constexpr uint8_t kEfiSystemGuid[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x1E, 0x6B, 0x7B};
constexpr uint8_t kBasicDataGuid[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
constexpr uint8_t kLinuxGuid[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};

constexpr uint32_t kMaxGptEntries = 4096;
constexpr uint64_t kMaxGptArrayBytes = 1 << 20;
constexpr uint32_t kMaxLogicalPartitions = 128;

bool is_extended_type(uint8_t type) { return type == 0x05 || type == 0x0F || type == 0x85; }

enum class Found : uint8_t { Yes, No, Error };

// Holds the device and a block-sized scratch buffer for the scan.
class Scanner {
 public:
  Scanner(IStorageDevice& device, allocators::IAllocator& allocator, List<PartitionInfo>* out)
      : device_(device), allocator_(allocator), out_(out), info_(device.info()) {}
  ~Scanner() { release_view(allocator_, block_); }

  bool scan(PartitionScheme* scheme);

 private:
  Found scan_gpt();
  Found try_gpt_header(uint64_t lba);
  bool scan_logical(uint64_t extended_start, uint64_t extended_count, uint32_t* next_index);
  bool read_block(uint64_t lba) { return device_.read(lba, block_); }
  bool looks_like_boot_sector() const;
  bool add(const PartitionInfo& info) { return out_->push_back(info); }

  IStorageDevice& device_;
  allocators::IAllocator& allocator_;
  List<PartitionInfo>* out_;
  StorageInfo info_;
  ByteView block_;
};

// A filesystem's boot sector also ends in 55 AA; its BPB fields tell it apart from an MBR.
bool Scanner::looks_like_boot_sector() const {
  const uint8_t* b = block_.data();
  if (b[0] != 0xEB && b[0] != 0xE9) return false;
  const uint32_t bytes_per_sector = le16(b + 11);
  const uint8_t per_cluster = b[13];
  return bytes_per_sector >= 512 && bytes_per_sector <= 4096 && (bytes_per_sector & (bytes_per_sector - 1)) == 0 &&
         per_cluster != 0 && (per_cluster & (per_cluster - 1)) == 0 && b[16] >= 1 && b[16] <= 4;
}

bool Scanner::scan(PartitionScheme* scheme) {
  *scheme = PartitionScheme::None;
  if (info_.block_count == 0 || info_.block_size < 512) return true;
  block_ = allocate_view<uint8_t>(allocator_, info_.block_size);
  if (block_.empty()) return false;
  if (!read_block(0)) return false;
  const uint8_t* mbr = block_.data();
  if (mbr[510] != 0x55 || mbr[511] != 0xAA || looks_like_boot_sector()) return true;

  struct Slot {
    uint8_t status, type;
    uint64_t start, count;
  } slots[4];
  bool protective = false;
  for (int i = 0; i < 4; ++i) {
    const uint8_t* e = mbr + 446 + i * 16;
    slots[i] = {e[0], e[4], le32(e + 8), le32(e + 12)};
    if ((e[0] != 0x00 && e[0] != 0x80)) return true;  // not a partition table after all
    if (e[4] == 0xEE) protective = true;
  }

  if (protective) {
    const Found gpt = scan_gpt();
    if (gpt == Found::Error) return false;
    if (gpt == Found::Yes) {
      *scheme = PartitionScheme::Gpt;
      return true;
    }
    out_->clear();
  }

  uint64_t extended_start = 0, extended_count = 0;
  for (int i = 0; i < 4; ++i) {
    const Slot& s = slots[i];
    if (s.type == 0 || s.type == 0xEE || s.count == 0 || s.start == 0) continue;
    if (s.start >= info_.block_count || s.count > info_.block_count - s.start) continue;
    if (is_extended_type(s.type)) {
      if (!extended_count) extended_start = s.start, extended_count = s.count;
      continue;
    }
    PartitionInfo p;
    p.scheme = PartitionScheme::Mbr;
    p.index = static_cast<uint32_t>(i + 1);
    p.first_block = s.start;
    p.block_count = s.count;
    p.mbr_type = s.type;
    p.bootable = s.status == 0x80;
    if (!add(p)) return false;
  }
  uint32_t next_index = 5;
  if (extended_count && !scan_logical(extended_start, extended_count, &next_index)) return false;
  if (out_->count()) *scheme = PartitionScheme::Mbr;
  return true;
}

// Walks the chain of extended boot records. Each describes one logical partition
// (relative to itself) and links to the next EBR (relative to the extended partition).
bool Scanner::scan_logical(uint64_t extended_start, uint64_t extended_count, uint32_t* next_index) {
  const uint64_t extended_end = extended_start + extended_count;
  uint64_t ebr = extended_start;
  for (uint32_t hops = 0; hops < kMaxLogicalPartitions; ++hops) {
    if (!read_block(ebr)) return false;
    const uint8_t* b = block_.data();
    if (b[510] != 0x55 || b[511] != 0xAA) return true;
    const uint8_t* first = b + 446;
    const uint8_t* link = b + 446 + 16;
    const uint8_t type = first[4];
    const uint64_t start = ebr + le32(first + 8), count = le32(first + 12);
    const uint8_t link_type = link[4];
    const uint64_t link_at = extended_start + le32(link + 8);
    if (type != 0 && !is_extended_type(type) && count != 0 && start >= ebr && start < extended_end && count <= extended_end - start) {
      PartitionInfo p;
      p.scheme = PartitionScheme::Mbr;
      p.index = (*next_index)++;
      p.first_block = start;
      p.block_count = count;
      p.mbr_type = type;
      p.bootable = first[0] == 0x80;
      p.logical = true;
      if (!add(p)) return false;
    }
    if (!is_extended_type(link_type) || link_at <= ebr || link_at >= extended_end) return true;  // the chain only moves forward
    ebr = link_at;
  }
  return true;
}

Found Scanner::scan_gpt() {
  Found primary = try_gpt_header(1);
  if (primary != Found::No) return primary;
  out_->clear();
  if (info_.block_count < 3) return Found::No;
  return try_gpt_header(info_.block_count - 1);  // the backup header sits in the last block
}

Found Scanner::try_gpt_header(uint64_t lba) {
  if (lba >= info_.block_count) return Found::No;
  if (!read_block(lba)) return Found::Error;
  const uint8_t* h = block_.data();
  if (__builtin_memcmp(h, "EFI PART", 8) != 0) return Found::No;
  const uint32_t header_size = le32(h + 12);
  if (header_size < 92 || header_size > info_.block_size) return Found::No;
  uint8_t copy[512];
  if (header_size > sizeof copy) return Found::No;
  __builtin_memcpy(copy, h, header_size);
  __builtin_memset(copy + 16, 0, 4);
  if (crc32(ConstByteView(copy, header_size)) != le32(h + 16)) return Found::No;
  if (le64(h + 24) != lba) return Found::No;
  const uint64_t first_usable = le64(h + 40), last_usable = le64(h + 48), entries_lba = le64(h + 72);
  const uint32_t entry_count = le32(h + 80), entry_size = le32(h + 84), entries_crc = le32(h + 88);
  if (first_usable > last_usable || last_usable >= info_.block_count) return Found::No;
  if (entry_count == 0 || entry_count > kMaxGptEntries || entry_size < 128 || entry_size > 4096 || (entry_size & 7)) return Found::No;
  const uint64_t array_bytes = uint64_t{entry_count} * entry_size;
  if (array_bytes > kMaxGptArrayBytes) return Found::No;
  const uint64_t array_blocks = (array_bytes + info_.block_size - 1) / info_.block_size;
  if (entries_lba >= info_.block_count || array_blocks > info_.block_count - entries_lba) return Found::No;

  ArrayView<uint8_t> array = allocate_view<uint8_t>(allocator_, array_blocks * info_.block_size);
  if (array.empty()) return Found::Error;
  Found result = Found::No;
  if (!device_.read(entries_lba, array)) {
    result = Found::Error;
  } else if (crc32(ConstByteView(array).first(array_bytes)) == entries_crc) {
    result = Found::Yes;
    uint32_t slot_number = 0;
    for (uint32_t i = 0; i < entry_count && result == Found::Yes; ++i) {
      const uint8_t* e = array.data() + uint64_t{i} * entry_size;
      ++slot_number;
      PartitionInfo p;
      __builtin_memcpy(p.type_guid.bytes, e, 16);
      if (p.type_guid.is_zero()) continue;
      const uint64_t first = le64(e + 32), last = le64(e + 40);
      if (first < first_usable || last > last_usable || first > last) continue;
      __builtin_memcpy(p.unique_guid.bytes, e + 16, 16);
      p.scheme = PartitionScheme::Gpt;
      p.index = slot_number;
      p.first_block = first;
      p.block_count = last - first + 1;
      p.attributes = le64(e + 48);
      p.bootable = (p.attributes & 4) != 0;
      for (uint32_t c = 0; c < 36; ++c) {
        uint32_t unit = le16(e + 56 + c * 2);
        if (unit == 0) break;
        if (unit >= 0xD800 && unit < 0xDC00 && c + 1 < 36) {
          const uint32_t low = le16(e + 56 + (c + 1) * 2);
          if (low >= 0xDC00 && low < 0xE000) {
            unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            ++c;
          }
        }
        if (!append_utf8(&p.name, unit)) break;
      }
      if (!add(p)) result = Found::Error;
    }
  }
  release_view(allocator_, array);
  return result;
}

}  // namespace

bool scan_partitions(IStorageDevice& device, allocators::IAllocator& allocator, List<PartitionInfo>* partitions,
                     PartitionScheme* scheme) {
  partitions->clear();
  Scanner scanner(device, allocator, partitions);
  if (!scanner.scan(scheme)) {
    partitions->clear();
    *scheme = PartitionScheme::None;
    return false;
  }
  return true;
}

bool is_efi_system_partition(const PartitionInfo& p) {
  if (p.scheme == PartitionScheme::Gpt) return __builtin_memcmp(p.type_guid.bytes, kEfiSystemGuid, 16) == 0;
  return p.mbr_type == 0xEF;
}

const char* partition_type_name(const PartitionInfo& p) {
  if (p.scheme == PartitionScheme::Gpt) {
    if (__builtin_memcmp(p.type_guid.bytes, kEfiSystemGuid, 16) == 0) return "EFI System";
    if (__builtin_memcmp(p.type_guid.bytes, kBasicDataGuid, 16) == 0) return "Microsoft basic data";
    if (__builtin_memcmp(p.type_guid.bytes, kLinuxGuid, 16) == 0) return "Linux filesystem";
    return "unknown";
  }
  switch (p.mbr_type) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEF: return "EFI System";
    default: return "unknown";
  }
}

}  // namespace hardware
