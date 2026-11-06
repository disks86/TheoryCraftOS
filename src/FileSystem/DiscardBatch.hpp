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

// The blocks a file system freed, gathered so that the device can be told they are unused (IStorageDevice::discard: TRIM, deallocate)
// once that is safe. Discarding too early loses data: after a crash the volume may still say the blocks belong to the deleted file, and
// a block freed and then allocated again must keep what was written into it since. So (as Linux's ext4 does with its discard option):
//
// - a file system reports every range it frees (freed) and every range it allocates (reused), in device blocks; a reused block leaves the
//   batch, wherever it falls in a range;
// - a file system with transactions (BeFS) reports frees as not yet committed, and commits or rolls them back with the transaction;
// - its flush() calls issue() after the device has flushed - the frees are on the medium then - and the committed ranges are discarded,
//   neighbours merged into one request.
//
// The batch holds kMaxRanges ranges. Discard is a hint: when it is full the smallest range is dropped, so the memory is bounded and
// fragmented frees of a huge delete cost nothing. Nothing happens on a device without discard or a volume mounted read only. Not
// thread-safe: the file system's own lock covers it.
#pragma once
#include <stdint.h>
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "ObjectContainer/Array.hpp"

namespace filesystem {

struct DiscardStatistics {
  uint64_t requests = 0;   // discard() calls made
  uint64_t blocks = 0;     // device blocks they covered
  uint64_t failed = 0;     // calls the device refused
  uint64_t dropped = 0;    // device blocks left out because the batch was full
};

class DiscardBatch {
 public:
  static constexpr uint32_t kMaxRanges = 64;

  // Turns the batch on for `device` if it can discard and the volume is writable; off otherwise (every call is then a no-op).
  void attach(hardware::IStorageDevice* device, bool writable);
  bool enabled() const { return device_ != nullptr; }

  void freed(uint64_t lba, uint64_t count, bool committed = true);
  void reused(uint64_t lba, uint64_t count);
  // The same by bytes from the start of the device, for file systems whose blocks are not device blocks: a free covers only the device
  // blocks wholly inside it (a device block also holding something in use is never discarded), a reuse every device block it touches.
  void freed_bytes(uint64_t offset, uint64_t bytes, uint32_t device_block_size, bool committed = true);
  void reused_bytes(uint64_t offset, uint64_t bytes, uint32_t device_block_size);
  void commit();     // the uncommitted frees are part of a transaction that is now durable
  void rollback();   // ... that was dropped: those blocks are still in use
  // Discards the committed ranges (call after the device flush that made them durable). Returns how many requests went out.
  uint32_t issue();

  uint32_t pending() const { return count_; }
  const DiscardStatistics& statistics() const { return stats_; }

 private:
  struct Range {
    uint64_t lba = 0, count = 0;
    bool committed = false;
  };
  void add(Range range);
  void remove_at(uint32_t index);

  hardware::IStorageDevice* device_ = nullptr;   // non-owning; null when off
  Array<Range, kMaxRanges> ranges_;
  uint32_t count_ = 0;
  DiscardStatistics stats_;
};

}  // namespace filesystem
