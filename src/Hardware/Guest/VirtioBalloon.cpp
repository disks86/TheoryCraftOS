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

#include "Hardware/Guest/VirtioBalloon.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint32_t kConfigNumPages = 0, kConfigActual = 4;
constexpr uint64_t kPage = 4096;
}  // namespace

uint32_t VirtioBalloon::read_config32(uint32_t offset) const {
  uint32_t v = 0;
  for (uint32_t i = 0; i < 4; ++i) v |= uint32_t{transport_.read_config8(offset + i)} << (8 * i);
  return v;
}

void VirtioBalloon::write_actual() const {
  const uint32_t v = actual_pages();
  for (uint32_t i = 0; i < 4; ++i) transport_.write_config8(kConfigActual + i, static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t VirtioBalloon::target_pages() const { return read_config32(kConfigNumPages); }

bool VirtioBalloon::initialize() {
  if (!transport_.initialize() || !transport_.setup_queue(0, &inflate_) || !transport_.setup_queue(1, &deflate_)) {
    shutdown();
    return false;
  }
  pfns_ = allocate_dma_view<uint32_t>(allocator_, kChunkPages, 64);
  chunk_base_ = allocate_view<uint8_t*>(allocator_, kMaxChunks);
  if (pfns_.empty() || chunk_base_.empty() || !transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;
  write_actual();
  return true;
}

// Hands the pages of `chunk` (filled into pfns_) to the device on `queue` and waits until it is done with them.
bool VirtioBalloon::exchange(uint16_t queue, uint32_t chunk) {
  uint8_t* interior = chunk_base_[chunk] + kPage;
  for (uint32_t i = 0; i < kChunkPages; ++i) pfns_[i] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(interior) + i * kPage) / kPage);
  Virtqueue& q = queue == 0 ? inflate_ : deflate_;
  q.post(0, pfns_.data(), kChunkPages * sizeof(uint32_t), false);
  transport_.notify(queue);
  uint16_t id;
  uint32_t length;
  const uint64_t start = uptime_us();
  while (!q.take_used(&id, &length)) {
    if (uptime_us() - start > kTimeoutUs) return false;
    delay_us(20);
  }
  return true;
}

bool VirtioBalloon::service() {
  if (!started_) return false;
  const uint32_t target = target_pages();
  const uint32_t want = target / kChunkPages < kMaxChunks ? target / kChunkPages : kMaxChunks;
  const uint32_t before = chunks_;
  while (chunks_ < want) {
    // Two extra pages keep the allocator's neighbouring bookkeeping out of the ballooned range.
    uint8_t* block = static_cast<uint8_t*>(allocator_.allocate_aligned((kChunkPages + 2) * kPage, kPage));
    if (!block) break;
    chunk_base_[chunks_] = block;
    if (!exchange(0, chunks_)) {
      // The host did not take them in time. It may still, later - leaking the chunk is safer than reusing it.
      break;
    }
    ++chunks_;
  }
  while (chunks_ > want) {
    if (!exchange(1, chunks_ - 1)) break;
    allocator_.deallocate(chunk_base_[--chunks_]);
  }
  if (chunks_ != before) write_actual();
  return chunks_ != before;
}

void VirtioBalloon::shutdown() {
  if (started_) {
    started_ = false;
    transport_.shutdown();  // the host keeps nothing of ours after the reset; the pages are ours again
    for (uint32_t i = 0; i < chunks_; ++i) allocator_.deallocate(chunk_base_[i]);
    chunks_ = 0;
  } else {
    transport_.shutdown();
  }
  inflate_.release(allocator_);
  deflate_.release(allocator_);
  release_view(allocator_, pfns_);
  release_view(allocator_, chunk_base_);
}

}  // namespace hardware
