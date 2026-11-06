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

#include "FileSystem/DiscardBatch.hpp"

namespace filesystem {

void DiscardBatch::attach(hardware::IStorageDevice* device, bool writable) {
  device_ = device && writable && device->info().discard ? device : nullptr;
  count_ = 0;
  block_size_ = device_ && device_->info().block_size ? device_->info().block_size : 512;
}

void DiscardBatch::remove_at(uint32_t index) {
  ranges_[index] = ranges_[count_ - 1];
  --count_;
}

// Merges with a range it touches in the same state (repeatedly: the merged one may now touch another), else takes a free slot, else
// replaces the smallest range if it is bigger than that.
void DiscardBatch::add(Range range) {
  if (range.count == 0) return;
  for (bool merged = true; merged;) {
    merged = false;
    for (uint32_t i = 0; i < count_; ++i) {
      Range& r = ranges_[i];
      if (r.committed != range.committed) continue;
      const uint64_t r_end = r.lba + r.count, end = range.lba + range.count;
      if (range.lba > r_end || r.lba > end) continue;   // apart (touching counts as together)
      const uint64_t lba = r.lba < range.lba ? r.lba : range.lba;
      range.count = (r_end > end ? r_end : end) - lba;
      range.lba = lba;
      remove_at(i);
      merged = true;
      break;
    }
  }
  if (count_ < kMaxRanges) {
    ranges_[count_++] = range;
    return;
  }
  uint32_t smallest = 0;
  for (uint32_t i = 1; i < count_; ++i)
    if (ranges_[i].count < ranges_[smallest].count) smallest = i;
  if (ranges_[smallest].count >= range.count) {
    stats_.dropped += range.count;
    return;
  }
  stats_.dropped += ranges_[smallest].count;
  ranges_[smallest] = range;
}

void DiscardBatch::freed(uint64_t lba, uint64_t count, bool committed) {
  if (!device_ || count == 0 || lba > UINT64_MAX - count) return;
  add(Range{lba, count, committed});
}

// Cuts [lba, lba + count) out of every range: a range around it splits in two (the part that does not fit when the batch is full is
// dropped).
void DiscardBatch::reused(uint64_t lba, uint64_t count) {
  if (!device_ || count == 0 || count_ == 0) return;
  const uint64_t end = lba > UINT64_MAX - count ? UINT64_MAX : lba + count;
  for (uint32_t i = 0; i < count_;) {
    Range& r = ranges_[i];
    const uint64_t r_end = r.lba + r.count;
    if (end <= r.lba || lba >= r_end) {
      ++i;
      continue;
    }
    const Range before{r.lba, lba > r.lba ? lba - r.lba : 0, r.committed};
    const Range after{end, r_end > end ? r_end - end : 0, r.committed};
    remove_at(i);   // (the range moved into slot i is looked at next)
    if (before.count) add(before);
    if (after.count) add(after);
    if (before.count || after.count) i = 0;   // (add() may have reordered: start over; every pass shrinks what overlaps)
  }
}

void DiscardBatch::freed_bytes(uint64_t offset, uint64_t bytes, uint32_t device_block_size, bool committed) {
  if (!device_ || device_block_size == 0 || bytes == 0 || offset > UINT64_MAX - bytes) return;
  const uint64_t first = (offset + device_block_size - 1) / device_block_size;   // (offset + bytes cannot wrap, so neither can this much)
  const uint64_t end = (offset + bytes) / device_block_size;
  if (end > first) freed(first, end - first, committed);
}

void DiscardBatch::reused_bytes(uint64_t offset, uint64_t bytes, uint32_t device_block_size) {
  if (!device_ || device_block_size == 0 || bytes == 0) return;
  const uint64_t last = offset > UINT64_MAX - bytes ? UINT64_MAX : offset + bytes - 1;
  const uint64_t first = offset / device_block_size;
  reused(first, last / device_block_size - first + 1);
}

void DiscardBatch::commit() {
  for (uint32_t i = 0; i < count_; ++i) ranges_[i].committed = true;
  // Committed neighbours of what was just committed merge now: take them out and put them back.
  Range all[kMaxRanges];
  const uint32_t n = count_;
  for (uint32_t i = 0; i < n; ++i) all[i] = ranges_[i];
  count_ = 0;
  for (uint32_t i = 0; i < n; ++i) add(all[i]);
}

void DiscardBatch::rollback() {
  for (uint32_t i = 0; i < count_;) {
    if (!ranges_[i].committed) remove_at(i);
    else ++i;
  }
}

bool DiscardBatch::has_committed() const {
  for (uint32_t i = 0; i < count_; ++i)
    if (ranges_[i].committed) return true;
  return false;
}

bool DiscardBatch::wants_issue() const {
  uint64_t blocks = 0;
  uint32_t committed = 0;
  for (uint32_t i = 0; i < count_; ++i) {
    if (!ranges_[i].committed) continue;
    blocks += ranges_[i].count;
    ++committed;
  }
  return committed == kMaxRanges || blocks >= kIssueBytes / block_size_;
}

bool DiscardBatch::discard_now(uint64_t lba, uint64_t count) {
  if (!device_ || count == 0) return false;
  ++stats_.requests;
  if (!device_->discard(lba, count)) {
    ++stats_.failed;
    return false;
  }
  stats_.blocks += count;
  return true;
}

bool DiscardBatch::discard_bytes_now(uint64_t offset, uint64_t bytes, uint32_t device_block_size, uint64_t* bytes_done) {
  if (!device_ || device_block_size == 0 || bytes == 0 || offset > UINT64_MAX - bytes) return true;
  const uint64_t first = (offset + device_block_size - 1) / device_block_size, end = (offset + bytes) / device_block_size;
  if (end <= first) return true;
  if (!discard_now(first, end - first)) return false;
  *bytes_done += (end - first) * device_block_size;
  return true;
}

uint32_t DiscardBatch::issue() {
  if (!device_) return 0;
  uint32_t sent = 0;
  for (uint32_t i = 0; i < count_;) {
    if (!ranges_[i].committed) {
      ++i;
      continue;
    }
    const Range r = ranges_[i];
    remove_at(i);
    ++stats_.requests;
    ++sent;
    if (device_->discard(r.lba, r.count)) stats_.blocks += r.count;
    else ++stats_.failed;
  }
  return sent;
}

}  // namespace filesystem
