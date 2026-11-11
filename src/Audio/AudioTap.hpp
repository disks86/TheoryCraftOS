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

// Something that listens to audio as it goes by: the mixer hands every chunk it mixes to its taps (a monitor of the output, what PulseAudio calls the
// sink's monitor source and WASAPI loopback capture), the capture hub hands every chunk it records to its streams. The audio is in the devices' format,
// 48 kHz 16-bit stereo. deliver() is called with the giver's lock held, so it must be quick and must not call back into the giver.
#pragma once
#include "Utility/ArrayView.hpp"

namespace audio {

class IAudioTap {
 public:
  virtual void deliver(ConstByteView pcm) = 0;
  // A closed tap is let go at the next chunk.
  virtual bool closed() const = 0;

 protected:
  IAudioTap() = default;
  ~IAudioTap() = default;
};

}  // namespace audio
