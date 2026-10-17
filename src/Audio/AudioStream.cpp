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

#include "Audio/AudioStream.hpp"

namespace audio {

using hardware::kAudioFrameSize;

bool AudioStream::allocate(uint32_t ring_bytes) {
  if (ring_bytes == 0 || ring_bytes % kAudioFrameSize != 0) return false;
  release_view(allocator_, ring_);
  ring_ = allocate_view<uint8_t>(allocator_, ring_bytes, 16);
  read_ = queued_ = 0;
  return !ring_.empty();
}

uint32_t AudioStream::write(ConstByteView pcm) {
  if (closed()) return 0;
  kernel::ScopedMutex guard(mutex_);
  if (ring_.empty()) return 0;
  const uint32_t size = static_cast<uint32_t>(ring_.size());
  uint32_t bytes = pcm.size() < size - queued_ ? static_cast<uint32_t>(pcm.size()) : size - queued_;
  bytes -= bytes % kAudioFrameSize;
  uint32_t at = (read_ + queued_) % size;
  for (uint32_t done = 0; done < bytes;) {
    const uint32_t chunk = bytes - done < size - at ? bytes - done : size - at;
    ring_.subview(at, chunk).copy_from(pcm.subview(done, chunk));
    at = (at + chunk) % size;
    done += chunk;
  }
  queued_ += bytes;
  __atomic_fetch_add(&frames_written_, bytes / kAudioFrameSize, __ATOMIC_RELAXED);
  return bytes;
}

uint32_t AudioStream::free_bytes() {
  if (closed()) return 0;
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(ring_.size()) - queued_;
}

uint32_t AudioStream::queued_bytes() {
  kernel::ScopedMutex guard(mutex_);
  return queued_;
}

uint32_t AudioStream::mix_into(int32_t* accumulator, uint32_t max_frames) {
  const bool silent = muted() || volume() == 0;
  const int32_t gain = volume();
  kernel::ScopedMutex guard(mutex_);
  if (ring_.empty()) return 0;
  const uint32_t size = static_cast<uint32_t>(ring_.size());
  const uint32_t frames = queued_ / kAudioFrameSize < max_frames ? queued_ / kAudioFrameSize : max_frames;
  // A muted stream still advances, so that it stays in step with the others.
  for (uint32_t i = 0; i < frames * hardware::kAudioChannels; ++i) {
    const uint32_t at = (read_ + i * hardware::kAudioBytesPerSample) % size;
    const int32_t sample = static_cast<int16_t>(ring_[at] | (ring_[(at + 1) % size] << 8));
    if (!silent) accumulator[i] += sample * gain / hardware::kAudioMaxVolume;
  }
  read_ = (read_ + frames * kAudioFrameSize) % size;
  queued_ -= frames * kAudioFrameSize;
  __atomic_fetch_add(&frames_mixed_, frames, __ATOMIC_RELAXED);
  return frames;
}

}  // namespace audio
