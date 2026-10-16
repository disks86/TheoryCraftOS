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
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

// A window onto part of another storage device: block 0 of this device is block
// `first_block` of the parent, and it ends after `block_count` blocks. That lets
// a filesystem driver treat a partition exactly like a whole disk. The parent
// must outlive it; requests are passed straight through (the parent does its own
// locking), after being range checked against the window.
class PartitionStorageDevice final : public IStorageDevice {
 public:
  PartitionStorageDevice(IStorageDevice& parent, uint64_t first_block, uint64_t block_count, uint32_t index)
      : parent_(parent), first_(first_block), count_(block_count), block_size_(parent.info().block_size) {
    name_.assign(parent.name());
    name_.push_back('p');
    name_.append_uint(index);
  }

  const char* name() const override { return name_.c_str(); }
  bool initialize() override { return true; }
  void shutdown() override {}

  StorageInfo info() const override {
    StorageInfo result = parent_.info();
    result.block_count = count_;
    return result;
  }
  StorageStatistics statistics() const override { return stats_; }

  bool read(uint64_t lba, ByteView buffer) override {
    if (!in_range(lba, buffer.size()) || !parent_.read(first_ + lba, buffer)) return fail();
    ++stats_.reads;
    stats_.blocks_read += buffer.size() / block_size_;
    return true;
  }
  bool write(uint64_t lba, ConstByteView buffer) override {
    if (!in_range(lba, buffer.size()) || !parent_.write(first_ + lba, buffer)) return fail();
    ++stats_.writes;
    stats_.blocks_written += buffer.size() / block_size_;
    return true;
  }
  bool flush() override { return parent_.flush(); }

 private:
  bool in_range(uint64_t lba, uint64_t bytes) const {
    if (bytes % block_size_) return false;
    const uint64_t blocks = bytes / block_size_;
    return lba <= count_ && blocks <= count_ - lba;
  }
  bool fail() {
    ++stats_.errors;
    return false;
  }

  IStorageDevice& parent_;
  const uint64_t first_;
  const uint64_t count_;
  const uint32_t block_size_;
  FixedString<48> name_;
  StorageStatistics stats_;
};

}  // namespace hardware
