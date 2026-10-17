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
#include "Allocators/IAllocator.hpp"
#include "Hardware/AudioDevice/CaptureRing.hpp"
#include "Kernel/Sync.hpp"

namespace audio {

// One program's view of the microphone: a ring the capture hub fills with every
// frame recorded from the moment the stream was opened, and the program reads at
// its own pace (fixed format: 48 kHz, 16-bit, stereo). If the program falls a
// whole ring behind, the oldest audio is overwritten and counted as lost. Each
// stream has its own gain and mute. Thread safe.
//
// Obtain one from AudioCaptureHub::open_stream(); close() it when done.
class CaptureStream {
 public:
  static constexpr uint32_t kDefaultRingBytes = 64 * 1024;  // about 340 ms

  explicit CaptureStream(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~CaptureStream() { ring_.release(allocator_); }
  CaptureStream(const CaptureStream&) = delete;
  CaptureStream& operator=(const CaptureStream&) = delete;

  bool allocate(uint32_t ring_bytes = kDefaultRingBytes);

  // Copies out the oldest whole frames, scaled by the stream's gain; returns the bytes copied.
  uint32_t read(ByteView out);
  uint32_t available_bytes();
  // Throws away what has been recorded so far.
  void flush();

  void set_gain(uint8_t gain) { __atomic_store_n(&gain_, gain < hardware::kAudioMaxVolume ? gain : hardware::kAudioMaxVolume, __ATOMIC_RELAXED); }
  uint8_t gain() const { return __atomic_load_n(&gain_, __ATOMIC_RELAXED); }
  void set_muted(bool muted) { __atomic_store_n(&muted_, muted, __ATOMIC_RELAXED); }
  bool muted() const { return __atomic_load_n(&muted_, __ATOMIC_RELAXED); }

  // No more audio is delivered; the hub lets the stream go.
  void close() { __atomic_store_n(&closed_, true, __ATOMIC_RELEASE); }
  bool closed() const { return __atomic_load_n(&closed_, __ATOMIC_ACQUIRE); }

  hardware::AudioCaptureStatistics statistics();

  // Hub side: appends recorded frames.
  void deliver(ConstByteView pcm);

 private:
  allocators::IAllocator& allocator_;
  hardware::CaptureRing ring_;
  kernel::Mutex mutex_;
  uint8_t gain_ = hardware::kAudioMaxVolume;
  bool muted_ = false;
  bool closed_ = false;
};

}  // namespace audio
