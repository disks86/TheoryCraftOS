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
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"

// Reads the partition table of every storage device, lists the partitions, and
// tries to mount each one (or the whole device when it has no table) as FAT. For
// each volume that mounts it prints the type and label and the first entries of
// the root directory, and for an EFI System Partition checks for
// /EFI/BOOT/BOOTX64.EFI and reads it. Read only: nothing is written. Partitions
// that are not FAT are reported and skipped. Returns false if a device or the
// memory failed (a damaged or foreign filesystem is not a failure).
bool run_filesystem_demo(hardware::SystemStorageDevices& storage, allocators::IAllocator& allocator);
