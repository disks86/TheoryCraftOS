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
#include "Utility/AllocatedView.hpp"

namespace hardware {

// The software side of a cyclic DMA playback buffer: hardware reads the buffer
// round and round, this tracks where it has got to and where the next samples go.
// The buffer holds silence except for audio that is queued, so an underrun just
// plays the silence that follows the last queued sample.
//
// The owner reports the device's read position (a byte offset into the buffer)
// through advance(). Only the distance since the previous report counts, so a
// report is needed at least once per trip round the buffer. An underrun is counted
// when the device passes the last queued sample, including at the end of a stream.
class PlaybackRing {
 public:
  // A buffer of `bytes` (a multiple of the frame size), aligned to 128 bytes as
  // the DMA engines like. False when out of memory.
  bool allocate(allocators::IAllocator& allocator, uint32_t bytes);
  void release(allocators::IAllocator& allocator);

  uint8_t* data() const { return memory_.data(); }
  uint64_t address() const { return reinterpret_cast<uint64_t>(memory_.data()); }
  uint32_t size() const { return static_cast<uint32_t>(memory_.size()); }

  // Everything silent, nothing queued, the device assumed to be at `position`.
  void reset(uint32_t position = 0);
  // The device is now reading at `position` (taken modulo the buffer size).
  void advance(uint32_t position);
  // Same, for a device that reports how much it has played instead: `bytes` more
  // than at the previous report (any amount, even several trips round the buffer).
  void advance_by(uint64_t bytes);
  // Copies in whole frames; returns the bytes taken.
  uint32_t write(ConstByteView pcm);

  // Copies up to out.size() bytes (whole frames) of the audio that is queued, oldest first, without consuming it; returns the bytes copied.
  uint32_t peek(ByteView out) const;

  uint32_t queued() const { return queued_; }
  uint32_t free() const { return size() - queued_; }
  uint64_t frames_played() const { return frames_played_; }
  uint64_t frames_queued() const { return frames_queued_; }
  uint64_t underruns() const { return underruns_; }

 private:
  void zero(uint32_t from, uint32_t bytes);

  ByteView memory_;
  uint32_t read_ = 0;    // where the device was at the last report
  uint32_t queued_ = 0;  // bytes of audio from read_ onwards
  uint64_t frames_played_ = 0;
  uint64_t frames_queued_ = 0;
  uint64_t underruns_ = 0;
};

}  // namespace hardware
