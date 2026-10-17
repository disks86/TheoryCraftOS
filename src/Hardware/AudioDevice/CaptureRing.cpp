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

#include "Hardware/AudioDevice/CaptureRing.hpp"

namespace hardware {

bool CaptureRing::allocate(allocators::IAllocator& allocator, uint32_t bytes, uint32_t frame_size) {
  release(allocator);
  if (frame_size == 0 || bytes == 0 || bytes % frame_size != 0) return false;
  frame_size_ = frame_size;
  memory_ = allocate_view<uint8_t>(allocator, bytes, 128);
  if (memory_.empty()) return false;
  reset();
  return true;
}

void CaptureRing::release(allocators::IAllocator& allocator) {
  release_view(allocator, memory_);
  head_ = queued_ = 0;
}

void CaptureRing::reset(uint32_t position) {
  if (memory_.empty()) return;
  memory_.fill(0);
  head_ = position % size() / frame_size_ * frame_size_;
  queued_ = 0;
}

// `bytes` more were recorded: they are queued, and anything beyond the buffer's size pushes the oldest out.
void CaptureRing::note_arrival(uint64_t bytes) {
  bytes -= bytes % frame_size_;
  if (bytes == 0) return;
  frames_captured_ += bytes / frame_size_;
  const uint64_t total = uint64_t{queued_} + bytes;
  if (total > size()) {
    const uint64_t lost = total - size();
    frames_lost_ += lost / frame_size_;
    ++overruns_;
    queued_ = size();
  } else {
    queued_ = static_cast<uint32_t>(total);
  }
}

void CaptureRing::advance(uint32_t position) {
  if (memory_.empty()) return;
  position = position % size() / frame_size_ * frame_size_;
  advance_by(position >= head_ ? position - head_ : size() - head_ + position);
}

void CaptureRing::advance_by(uint64_t bytes) {
  if (memory_.empty()) return;
  bytes -= bytes % frame_size_;
  if (bytes == 0) return;
  note_arrival(bytes);
  head_ = static_cast<uint32_t>((head_ + bytes % size()) % size());
}

void CaptureRing::push(ConstByteView pcm) {
  if (memory_.empty()) return;
  uint64_t bytes = pcm.size() - pcm.size() % frame_size_;
  // Only the last buffer's worth of a huge block can survive.
  uint64_t skip = bytes > size() ? bytes - size() : 0;
  skip -= skip % frame_size_;
  uint32_t at = static_cast<uint32_t>((head_ + skip) % size());
  for (uint64_t done = skip; done < bytes;) {
    const uint32_t chunk = static_cast<uint32_t>(bytes - done < size() - at ? bytes - done : size() - at);
    memory_.subview(at, chunk).copy_from(pcm.subview(done, chunk));
    at = (at + chunk) % size();
    done += chunk;
  }
  note_arrival(bytes);
  head_ = at;
}

uint32_t CaptureRing::read(ByteView out) {
  if (memory_.empty()) return 0;
  uint32_t bytes = out.size() < queued_ ? static_cast<uint32_t>(out.size()) : queued_;
  bytes -= bytes % frame_size_;
  uint32_t at = (head_ + size() - queued_) % size();
  for (uint32_t done = 0; done < bytes;) {
    const uint32_t chunk = bytes - done < size() - at ? bytes - done : size() - at;
    out.subview(done, chunk).copy_from(ConstByteView(memory_).subview(at, chunk));
    at = (at + chunk) % size();
    done += chunk;
  }
  queued_ -= bytes;
  frames_read_ += bytes / frame_size_;
  return bytes;
}

void CaptureRing::flush() { queued_ = 0; }

}  // namespace hardware
