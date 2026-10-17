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

#include "Hardware/AudioDevice/PlaybackRing.hpp"

namespace hardware {

bool PlaybackRing::allocate(allocators::IAllocator& allocator, uint32_t bytes) {
  release(allocator);
  if (bytes == 0 || bytes % kAudioFrameSize != 0) return false;
  memory_ = allocate_dma_view<uint8_t>(allocator, bytes, 128);
  if (memory_.empty()) return false;
  reset();
  return true;
}

void PlaybackRing::release(allocators::IAllocator& allocator) {
  release_view(allocator, memory_);
  read_ = queued_ = 0;
}

void PlaybackRing::zero(uint32_t from, uint32_t bytes) {
  while (bytes > 0) {
    const uint32_t chunk = bytes < size() - from ? bytes : size() - from;
    memory_.subview(from, chunk).fill(0);
    from = (from + chunk) % size();
    bytes -= chunk;
  }
}

void PlaybackRing::reset(uint32_t position) {
  if (memory_.empty()) return;
  memory_.fill(0);
  read_ = position % size() / kAudioFrameSize * kAudioFrameSize;
  queued_ = 0;
}

void PlaybackRing::advance(uint32_t position) {
  if (memory_.empty()) return;
  position = position % size() / kAudioFrameSize * kAudioFrameSize;
  advance_by(position >= read_ ? position - read_ : size() - read_ + position);
}

void PlaybackRing::advance_by(uint64_t bytes) {
  if (memory_.empty()) return;
  bytes -= bytes % kAudioFrameSize;
  if (bytes == 0) return;
  const uint32_t from_queue = bytes < queued_ ? static_cast<uint32_t>(bytes) : queued_;
  if (bytes > queued_ && queued_ > 0) ++underruns_;  // the device went past the last queued sample
  frames_played_ += from_queue / kAudioFrameSize;
  zero(read_, bytes < size() ? static_cast<uint32_t>(bytes) : size());  // what has been played must not play again
  read_ = static_cast<uint32_t>((read_ + bytes % size()) % size());
  queued_ -= from_queue;
}

uint32_t PlaybackRing::write(ConstByteView pcm) {
  if (memory_.empty()) return 0;
  uint32_t bytes = pcm.size() < free() ? static_cast<uint32_t>(pcm.size()) : free();
  bytes -= bytes % kAudioFrameSize;
  uint32_t at = (read_ + queued_) % size();
  uint32_t done = 0;
  while (done < bytes) {
    const uint32_t chunk = bytes - done < size() - at ? bytes - done : size() - at;
    memory_.subview(at, chunk).copy_from(pcm.subview(done, chunk));
    at = (at + chunk) % size();
    done += chunk;
  }
  queued_ += bytes;
  frames_queued_ += bytes / kAudioFrameSize;
  return bytes;
}

}  // namespace hardware
