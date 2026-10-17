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
#include "Utility/ArrayView.hpp"

namespace hardware {

// The recording side of an audio device (a microphone or line input), in the same
// fixed format as playback: 48 kHz, 16-bit signed, stereo (a mono source is
// delivered on both channels). Polled: the device fills a ring from the moment it
// is up, read() collects from it, and when the program does not keep up the
// oldest audio is overwritten (an overrun). Call read(), available_bytes() or
// statistics() at least every ~300 ms or the device cannot tell how much it has
// recorded. It is part of an IAudioDevice, which starts and stops it
// (IAudioDevice::capture()); it has no lifetime of its own.
class IAudioCapture {
 public:
  virtual const char* name() const = 0;

  // Copies recorded audio into `out`, whole frames only, oldest first, and
  // returns the bytes copied (0 if nothing is waiting). Never blocks.
  virtual uint32_t read(ByteView out) = 0;
  // Bytes read() would return right now.
  virtual uint32_t available_bytes() = 0;
  // Throws away what has been recorded so far, so the next read() starts with
  // sound from now.
  virtual void flush() = 0;

  // Input level, 0 .. kAudioMaxVolume. Mute keeps the setting. Where the
  // hardware has no input amplifier the samples are scaled in software.
  virtual void set_gain(uint8_t gain) = 0;
  virtual uint8_t gain() const = 0;
  virtual void set_muted(bool muted) = 0;
  virtual bool muted() const = 0;

  virtual AudioCaptureStatistics statistics() = 0;

 protected:
  constexpr IAudioCapture() = default;
  ~IAudioCapture() = default;
};

}  // namespace hardware
