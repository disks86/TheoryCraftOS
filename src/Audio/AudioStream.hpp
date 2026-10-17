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
#include "Hardware/AudioDevice/AudioTypes.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"

namespace audio {

// One program's sound as it goes into the mixer: a small ring of audio in the
// fixed format (48 kHz, 16-bit, stereo - see AudioTypes.hpp) that the program
// writes to and the mixer drains in real time. Every stream has its own volume
// and mute. Thread safe: the program and the mixer thread use it concurrently.
//
// Obtain one from AudioMixer::open_stream(). close() it when the sound is over;
// the mixer plays out what is still queued and then lets it go.
class AudioStream {
 public:
  static constexpr uint32_t kDefaultRingBytes = 32 * 1024;  // about 170 ms

  explicit AudioStream(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~AudioStream() { release_view(allocator_, ring_); }
  AudioStream(const AudioStream&) = delete;
  AudioStream& operator=(const AudioStream&) = delete;

  bool allocate(uint32_t ring_bytes = kDefaultRingBytes);

  // Copies whole frames of `pcm` into the ring and returns the bytes taken (0 if
  // it is full or the stream is closed). Never blocks: offer the rest again later.
  uint32_t write(ConstByteView pcm);
  uint32_t free_bytes();
  // Bytes written but not yet taken by the mixer.
  uint32_t queued_bytes();

  // 0 .. kAudioMaxVolume, applied by the mixer.
  void set_volume(uint8_t volume) { __atomic_store_n(&volume_, volume < hardware::kAudioMaxVolume ? volume : hardware::kAudioMaxVolume, __ATOMIC_RELAXED); }
  uint8_t volume() const { return __atomic_load_n(&volume_, __ATOMIC_RELAXED); }
  void set_muted(bool muted) { __atomic_store_n(&muted_, muted, __ATOMIC_RELAXED); }
  bool muted() const { return __atomic_load_n(&muted_, __ATOMIC_RELAXED); }

  // No more writes. What is queued still plays.
  void close() { __atomic_store_n(&closed_, true, __ATOMIC_RELEASE); }
  bool closed() const { return __atomic_load_n(&closed_, __ATOMIC_ACQUIRE); }
  // Closed and fully taken by the mixer.
  bool finished() { return closed() && queued_bytes() == 0; }

  uint64_t frames_written() const { return __atomic_load_n(&frames_written_, __ATOMIC_RELAXED); }
  uint64_t frames_mixed() const { return __atomic_load_n(&frames_mixed_, __ATOMIC_RELAXED); }

  // Mixer side: adds up to `max_frames` queued frames, scaled by the stream's
  // volume, into `accumulator` (interleaved left/right, 32-bit so that several
  // streams cannot overflow before the final clamp). Returns the frames taken.
  uint32_t mix_into(int32_t* accumulator, uint32_t max_frames);

 private:
  allocators::IAllocator& allocator_;
  ByteView ring_;
  kernel::Mutex mutex_;
  uint32_t read_ = 0;
  uint32_t queued_ = 0;
  uint8_t volume_ = hardware::kAudioMaxVolume;
  bool muted_ = false;
  bool closed_ = false;
  uint64_t frames_written_ = 0;
  uint64_t frames_mixed_ = 0;
};

}  // namespace audio
