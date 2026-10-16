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
#include "Utility/FixedString.hpp"

namespace hardware {

enum class PartitionScheme : uint8_t { None, Mbr, Gpt };

// A GUID as stored on disk (the first three fields little-endian).
struct Guid {
  uint8_t bytes[16] = {};
  bool is_zero() const {
    for (uint8_t b : bytes)
      if (b) return false;
    return true;
  }
  bool operator==(const Guid& other) const { return __builtin_memcmp(bytes, other.bytes, 16) == 0; }
};

// One partition of a disk, as the partition table describes it.
struct PartitionInfo {
  PartitionScheme scheme = PartitionScheme::None;
  uint32_t index = 0;          // 1-based: the table slot (MBR logical partitions count on from 5)
  uint64_t first_block = 0;    // in the device's logical blocks
  uint64_t block_count = 0;
  bool bootable = false;       // MBR active flag, or GPT "legacy BIOS bootable"
  bool logical = false;        // inside an MBR extended partition
  uint8_t mbr_type = 0;        // MBR only
  Guid type_guid;              // GPT only
  Guid unique_guid;            // GPT only
  uint64_t attributes = 0;     // GPT only
  FixedString<112> name;       // GPT only, UTF-8
};

}  // namespace hardware
