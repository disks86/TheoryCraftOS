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
#include "Allocators/IAllocator.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "Hardware/StorageDevice/Partition/PartitionTypes.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

// Reads a disk's partition table: GPT (header and entry-array CRCs checked, the
// backup copy used if the primary is damaged, the protective MBR recognised),
// MBR (four primaries plus the chain of extended/logical partitions), or neither.
// A device with no table, or whose first block is a filesystem's boot sector
// (a "superfloppy"), reports PartitionScheme::None and an empty list: use the
// whole device. Entries that are malformed or reach outside the device are left
// out. Returns false only if the device failed or memory ran out.
bool scan_partitions(IStorageDevice& device, allocators::IAllocator& allocator, List<PartitionInfo>* partitions,
                     PartitionScheme* scheme);

// A short description: "EFI System", "FAT32", "Linux", "Microsoft basic data", ... or "unknown".
const char* partition_type_name(const PartitionInfo& partition);
bool is_efi_system_partition(const PartitionInfo& partition);

}  // namespace hardware
