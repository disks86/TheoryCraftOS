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

// The software side of a cyclic capture buffer, the mirror image of PlaybackRing:
// the hardware writes the buffer round and round and this tracks what is new.
// The owner reports the device's write position (a byte offset) through
// advance(); a program takes the audio out with read(). When the program falls a
// whole buffer behind, the oldest audio is lost and counted. A device that is not
// DMA based (virtio-sound, the software source) hands over finished blocks with
// push() instead.
class CaptureRing {
 public:
  // A buffer of `bytes` (a multiple of the frame size), aligned to 128 bytes. False when out of memory.
  bool allocate(allocators::IAllocator& allocator, uint32_t bytes);
  void release(allocators::IAllocator& allocator);

  uint8_t* data() const { return memory_.data(); }
  uint64_t address() const { return reinterpret_cast<uint64_t>(memory_.data()); }
  uint32_t size() const { return static_cast<uint32_t>(memory_.size()); }

  // Nothing recorded; the device is assumed to be writing at `position`.
  void reset(uint32_t position = 0);
  // The device is now writing at `position` (taken modulo the buffer size).
  void advance(uint32_t position);
  // Same, for a device that reports the amount instead (any amount, even several buffers' worth, of which
  // only the last buffer's worth can still be there).
  void advance_by(uint64_t bytes);
  // Appends whole frames at the write position (overwriting the oldest if full).
  void push(ConstByteView pcm);

  // Copies out the oldest whole frames; returns the bytes copied.
  uint32_t read(ByteView out);
  void flush();
  uint32_t available() const { return queued_; }

  uint64_t frames_captured() const { return frames_captured_; }
  uint64_t frames_read() const { return frames_read_; }
  uint64_t frames_lost() const { return frames_lost_; }
  uint64_t overruns() const { return overruns_; }

 private:
  void note_arrival(uint64_t bytes);

  ByteView memory_;
  uint32_t head_ = 0;    // where the next recorded byte goes
  uint32_t queued_ = 0;  // bytes recorded and not read; they end at head_
  uint64_t frames_captured_ = 0;
  uint64_t frames_read_ = 0;
  uint64_t frames_lost_ = 0;
  uint64_t overruns_ = 0;
};

}  // namespace hardware
