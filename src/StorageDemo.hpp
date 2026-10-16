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

// Exercises every storage device: reports what it is and how big, reads its first
// and last blocks, checks that one large read equals the same range read in
// pieces, and - only on a device whose first block begins with the marker
// "TCOS-SCRATCH" - writes, flushes and reads back a test pattern on its last
// block (restoring it afterwards). Other devices are never written to.
// Returns false if any check failed.
bool run_storage_demo(hardware::SystemStorageDevices& storage, allocators::IAllocator& allocator);
