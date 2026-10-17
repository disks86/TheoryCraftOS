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
#include "Hardware/IHardware.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

enum class StorageKind : uint8_t { Disk, Optical };

struct StorageInfo {
  StorageKind kind = StorageKind::Disk;
  uint64_t block_count = 0;
  uint32_t block_size = 512;  // bytes per logical block
  bool read_only = false;
  bool removable = false;
  char model[41] = {};   // NUL-terminated, trailing blanks trimmed
  char serial[21] = {};  // empty when the device reports none
};

struct StorageStatistics {
  uint64_t reads = 0, writes = 0;
  uint64_t blocks_read = 0, blocks_written = 0;
  uint64_t errors = 0;
};

// A block storage device: a hard disk, SSD or optical drive that reads and
// writes whole logical blocks addressed by number. Calls complete before they
// return (the driver polls the device); `buffer` needs no particular alignment
// and any size is fine - the driver splits big requests itself. Implementations
// live in subfolders by interface (Virtio/, NVMe/, Ahci/, Ide/) plus a UEFI
// fallback for controllers without a native driver.
class IStorageDevice : public IHardware {
 public:
  // False once the device is gone (a USB stick that was pulled out): every request then fails at once. Devices that cannot be removed
  // are always present.
  virtual bool present() const { return true; }

  virtual StorageInfo info() const = 0;
  virtual StorageStatistics statistics() const = 0;

  // Reads buffer.size() / block_size blocks starting at `lba` into `buffer`. False
  // if the size is not a whole number of blocks, the range is outside the device,
  // or the device reports an error.
  virtual bool read(uint64_t lba, ByteView buffer) = 0;
  // Writes the blocks in `buffer`; false for read-only media and on any device error.
  virtual bool write(uint64_t lba, ConstByteView buffer) = 0;
  // Forces the device's write cache to the medium.
  virtual bool flush() = 0;

 protected:
  constexpr IStorageDevice() = default;
  ~IStorageDevice() = default;
};

}  // namespace hardware
