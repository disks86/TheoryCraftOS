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
#include "Audio/Pan.hpp"
#include "Audio/Resampler.hpp"
#include "Audio/StreamFormat.hpp"
#include "Hardware/AudioDevice/AudioTypes.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"

namespace audio {

// One program's sound as it goes into the mixer: a small ring that the program
// writes to and the mixer drains in real time. The ring holds the device's format
// (48 kHz, 16-bit, stereo - see AudioTypes.hpp); the program writes in its own
// StreamFormat (any rate from 4 to 192 kHz, mono or stereo, 16 or 8 bits) and
// write() converts. Every stream has its own volume, mute and pan (a mono sound
// is placed between the speakers, a stereo one is balanced). Thread safe: the
// program and the mixer thread use it concurrently.
//
// Obtain one from AudioMixer::open_stream(). close() it when the sound is over;
// the mixer plays out what is still queued and then lets it go.
class AudioStream {
 public:
  static constexpr uint32_t kDefaultRingBytes = 32 * 1024;  // about 170 ms

  explicit AudioStream(allocators::IAllocator& allocator, const StreamFormat& format = StreamFormat())
      : allocator_(allocator), format_(format) {}
  ~AudioStream() { release_view(allocator_, ring_); }
  AudioStream(const AudioStream&) = delete;
  AudioStream& operator=(const AudioStream&) = delete;

  // False if the format is not valid or memory ran out. `ring_bytes` is the size of the ring in the device format.
  bool allocate(uint32_t ring_bytes = kDefaultRingBytes);
  const StreamFormat& format() const { return format_; }

  // Converts whole frames of `pcm` (in the stream's format) into the ring and returns the bytes
  // taken (0 if it is full or the stream is closed). Never blocks: offer the rest again later.
  uint32_t write(ConstByteView pcm);
  // Room and backlog, in bytes of the stream's format (converted from the ring, so approximate when the
  // rates differ).
  uint32_t free_bytes();
  // Bytes written but not yet taken by the mixer.
  uint32_t queued_bytes();
  // Throws away everything queued (for a seek or a stop): the mixer then has nothing from this stream
  // until the program writes again.
  void flush();
  // Nothing left in the ring (exact, unlike queued_bytes() when the rates differ).
  bool drained();

  // 0 .. kAudioMaxVolume, applied by the mixer.
  void set_volume(uint8_t volume) { __atomic_store_n(&volume_, volume < hardware::kAudioMaxVolume ? volume : hardware::kAudioMaxVolume, __ATOMIC_RELAXED); }
  uint8_t volume() const { return __atomic_load_n(&volume_, __ATOMIC_RELAXED); }
  // -100 (all left) .. 0 .. +100 (all right).
  void set_pan(int pan) { __atomic_store_n(&pan_, static_cast<int8_t>(pan < kPanLeft ? kPanLeft : pan > kPanRight ? kPanRight : pan), __ATOMIC_RELAXED); }
  int pan() const { return __atomic_load_n(&pan_, __ATOMIC_RELAXED); }
  void set_muted(bool muted) { __atomic_store_n(&muted_, muted, __ATOMIC_RELAXED); }
  bool muted() const { return __atomic_load_n(&muted_, __ATOMIC_RELAXED); }

  // No more writes. What is queued still plays.
  void close() { __atomic_store_n(&closed_, true, __ATOMIC_RELEASE); }
  bool closed() const { return __atomic_load_n(&closed_, __ATOMIC_ACQUIRE); }
  // Closed and fully taken by the mixer.
  bool finished() { return closed() && drained(); }

  // The loudest sample (0 .. 32767, after volume, pan and mute) the mixer has taken from this stream since
  // the last call; for level meters.
  uint32_t take_peak() { return __atomic_exchange_n(&peak_, 0u, __ATOMIC_RELAXED); }

  uint64_t frames_written() const { return __atomic_load_n(&frames_written_, __ATOMIC_RELAXED); }
  uint64_t frames_mixed() const { return __atomic_load_n(&frames_mixed_, __ATOMIC_RELAXED); }

  // Mixer side: adds up to `max_frames` queued frames, scaled by the stream's
  // volume, into `accumulator` (interleaved left/right, 32-bit so that several
  // streams cannot overflow before the final clamp). Returns the frames taken.
  uint32_t mix_into(int32_t* accumulator, uint32_t max_frames);

 private:
  void append(ConstByteView converted);

  allocators::IAllocator& allocator_;
  StreamFormat format_;
  FormatConverter converter_;  // from format_ to the device format; unused when they are the same
  ByteView ring_;
  kernel::Mutex mutex_;
  uint32_t read_ = 0;
  uint32_t queued_ = 0;
  uint8_t volume_ = hardware::kAudioMaxVolume;
  int8_t pan_ = 0;
  bool muted_ = false;
  bool closed_ = false;
  uint32_t peak_ = 0;
  uint64_t frames_written_ = 0;
  uint64_t frames_mixed_ = 0;
};

}  // namespace audio
