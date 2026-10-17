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

#include "Audio/CaptureStream.hpp"

namespace audio {

using namespace hardware;

bool CaptureStream::allocate(uint32_t ring_bytes) {
  StreamFormat device;  // the defaults are the device's format
  if (!converter_.configure(device, format_)) return false;
  const uint32_t frame = format_.frame_size();
  return ring_.allocate(allocator_, ring_bytes - ring_bytes % frame, frame);
}

uint32_t CaptureStream::read(ByteView out) {
  const bool silent = muted();
  const int32_t level = gain();
  kernel::ScopedMutex guard(mutex_);
  const uint32_t bytes = ring_.read(out);
  if (silent || level < kAudioMaxVolume) {
    if (format_.type == SampleType::S16) {
      for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        const int32_t sample = silent ? 0 : static_cast<int16_t>(out[i] | (out[i + 1] << 8)) * level / kAudioMaxVolume;
        out[i] = static_cast<uint8_t>(sample);
        out[i + 1] = static_cast<uint8_t>(sample >> 8);
      }
    } else {
      for (uint32_t i = 0; i < bytes; ++i) out[i] = silent ? 128 : static_cast<uint8_t>((int32_t{out[i]} - 128) * level / kAudioMaxVolume + 128);
    }
  }
  return bytes;
}

uint32_t CaptureStream::available_bytes() {
  kernel::ScopedMutex guard(mutex_);
  return ring_.available();
}

void CaptureStream::flush() {
  kernel::ScopedMutex guard(mutex_);
  ring_.flush();
}

AudioCaptureStatistics CaptureStream::statistics() {
  kernel::ScopedMutex guard(mutex_);
  AudioCaptureStatistics stats;
  stats.frames_captured = ring_.frames_captured();
  stats.frames_read = ring_.frames_read();
  stats.frames_lost = ring_.frames_lost();
  stats.overruns = ring_.overruns();
  return stats;
}

void CaptureStream::deliver(ConstByteView pcm) {
  kernel::ScopedMutex guard(mutex_);
  if (format_.is_native()) {
    ring_.push(pcm);
    return;
  }
  uint8_t converted[2048];
  uint32_t taken = 0;
  while (taken < pcm.size()) {
    uint32_t used = 0;
    const uint32_t made = converter_.convert(pcm.dropped(taken), ByteView(converted), &used);
    if (made == 0 && used == 0) break;
    ring_.push(ConstByteView(converted).first(made));
    taken += used;
  }
}

}  // namespace audio
