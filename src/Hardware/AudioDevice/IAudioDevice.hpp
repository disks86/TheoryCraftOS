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
#include "Hardware/AudioDevice/AudioTypes.hpp"
#include "Hardware/IHardware.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// An audio output (speakers, headphones, a line out). It plays the fixed format
// in AudioTypes.hpp. Polled: nothing blocks. write() copies samples into a ring
// the device plays from; when the ring runs dry the device plays silence (an
// underrun) until more arrives. Because the position is read back by polling,
// call write(), queued_bytes() or free_bytes() at least every 300 ms or so while
// audio is playing, or the device cannot tell how much it has consumed.
// Implementations live in subfolders by vendor/transport (Intel/, Virtio/,
// Software/).
class IAudioDevice : public IHardware {
 public:
  // Copies as many whole frames of `pcm` as fit into the device's ring and
  // returns the number of bytes taken (0 if it is full or the device is stopped).
  // Samples left over are the caller's to offer again later.
  virtual uint32_t write(ConstByteView pcm) = 0;
  // Bytes of audio accepted but not yet played.
  virtual uint32_t queued_bytes() = 0;
  // Bytes write() would accept right now.
  virtual uint32_t free_bytes() = 0;

  // 0 (silent) .. kAudioMaxVolume. Mute keeps the volume setting.
  virtual void set_volume(uint8_t volume) = 0;
  virtual uint8_t volume() const = 0;
  virtual void set_muted(bool muted) = 0;
  virtual bool muted() const = 0;

  virtual AudioStatistics statistics() = 0;

 protected:
  constexpr IAudioDevice() = default;
  ~IAudioDevice() = default;
};

}  // namespace hardware
