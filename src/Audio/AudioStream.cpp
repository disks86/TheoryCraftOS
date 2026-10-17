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
  StreamFormat device;  // the defaults are the device's format
  if (!converter_.configure(format_, device)) return false;
  release_view(allocator_, ring_);
  ring_ = allocate_view<uint8_t>(allocator_, ring_bytes, 16);
  read_ = queued_ = 0;
  return !ring_.empty();
}

// Appends device-format bytes at the write position.
void AudioStream::append(ConstByteView converted) {
  const uint32_t size = static_cast<uint32_t>(ring_.size());
  uint32_t at = (read_ + queued_) % size;
  for (uint32_t done = 0; done < converted.size();) {
    const uint32_t chunk = converted.size() - done < size - at ? static_cast<uint32_t>(converted.size()) - done : size - at;
    ring_.subview(at, chunk).copy_from(converted.subview(done, chunk));
    at = (at + chunk) % size;
    done += chunk;
  }
  queued_ += static_cast<uint32_t>(converted.size());
}

uint32_t AudioStream::write(ConstByteView pcm) {
  if (closed()) return 0;
  kernel::ScopedMutex guard(mutex_);
  if (ring_.empty()) return 0;
  const uint32_t size = static_cast<uint32_t>(ring_.size());
  uint32_t taken = 0;
  if (format_.is_native()) {
    uint32_t bytes = pcm.size() < size - queued_ ? static_cast<uint32_t>(pcm.size()) : size - queued_;
    bytes -= bytes % kAudioFrameSize;
    append(pcm.first(bytes));
    taken = bytes;
  } else {
    uint8_t converted[2048];
    while (taken < pcm.size() && queued_ < size) {
      const uint32_t room = size - queued_ < sizeof converted ? size - queued_ : static_cast<uint32_t>(sizeof converted);
      uint32_t used = 0;
      const uint32_t made = converter_.convert(pcm.dropped(taken), ByteView(converted).first(room), &used);
      if (made == 0 && used == 0) break;
      append(ConstByteView(converted).first(made));
      taken += used;
    }
  }
  __atomic_fetch_add(&frames_written_, taken / format_.frame_size(), __ATOMIC_RELAXED);
  return taken;
}

uint32_t AudioStream::free_bytes() {
  if (closed()) return 0;
  kernel::ScopedMutex guard(mutex_);
  const uint32_t room = static_cast<uint32_t>(ring_.size()) - queued_;
  return format_.is_native() ? room : static_cast<uint32_t>(converter_.input_bytes_for(room));
}

void AudioStream::flush() {
  kernel::ScopedMutex guard(mutex_);
  read_ = 0;
  queued_ = 0;
}

uint32_t AudioStream::queued_bytes() {
  kernel::ScopedMutex guard(mutex_);
  return format_.is_native() ? queued_ : static_cast<uint32_t>(converter_.input_bytes_for(queued_));
}

bool AudioStream::drained() {
  kernel::ScopedMutex guard(mutex_);
  return queued_ == 0;
}

uint32_t AudioStream::mix_into(int32_t* accumulator, uint32_t max_frames) {
  const bool silent = muted() || volume() == 0;
  int32_t left_gain, right_gain;  // Q15: the volume and the pan together
  pan_gains(pan(), format_.channels == 1, &left_gain, &right_gain);
  left_gain = left_gain * volume() / hardware::kAudioMaxVolume;
  right_gain = right_gain * volume() / hardware::kAudioMaxVolume;
  kernel::ScopedMutex guard(mutex_);
  if (ring_.empty()) return 0;
  const uint32_t size = static_cast<uint32_t>(ring_.size());
  const uint32_t frames = queued_ / kAudioFrameSize < max_frames ? queued_ / kAudioFrameSize : max_frames;
  uint32_t peak = 0;
  // A muted stream still advances, so that it stays in step with the others.
  for (uint32_t i = 0; i < frames * hardware::kAudioChannels; ++i) {
    const uint32_t at = (read_ + i * hardware::kAudioBytesPerSample) % size;
    const int32_t sample = static_cast<int16_t>(ring_[at] | (ring_[(at + 1) % size] << 8));
    if (silent) continue;
    const int32_t scaled = (sample * ((i & 1) ? right_gain : left_gain)) >> 15;
    accumulator[i] += scaled;
    const uint32_t magnitude = static_cast<uint32_t>(scaled < 0 ? -scaled : scaled);
    if (magnitude > peak) peak = magnitude;
  }
  read_ = (read_ + frames * kAudioFrameSize) % size;
  queued_ -= frames * kAudioFrameSize;
  if (peak > 32767) peak = 32767;
  for (uint32_t seen = __atomic_load_n(&peak_, __ATOMIC_RELAXED);
       peak > seen && !__atomic_compare_exchange_n(&peak_, &seen, peak, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);) {
  }
  __atomic_fetch_add(&frames_mixed_, frames, __ATOMIC_RELAXED);
  return frames;
}

}  // namespace audio
