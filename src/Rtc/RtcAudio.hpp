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

// The sound a peer connection works with: where the microphone's samples come from and where received sound goes. Everything is 8 kHz, mono, 16 bit (what G.711 carries);
// the machine's side (src/Desktop) converts. Samples are read and written as they come: the peer connection is polled often enough to keep a 20 ms packet's worth ready.
#pragma once
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace rtc {

class IRtcAudioSource {
 public:
  // Takes up to out.size() samples that were recorded since the last read; returns how many.
  virtual uint32_t read(ArrayView<int16_t> out) = 0;

 protected:
  IRtcAudioSource() = default;
  ~IRtcAudioSource() = default;
};

class IRtcAudioSink {
 public:
  virtual void write(ConstByteView pcm16le) = 0;   // little-endian 16-bit samples
  // Sets how loud it plays, 0 to 100.
  virtual void set_volume(uint32_t percent) { (void)percent; }

 protected:
  IRtcAudioSink() = default;
  ~IRtcAudioSink() = default;
};

class IRtcAudioDevices {
 public:
  virtual bool microphone_present() = 0;
  // May the origin use the microphone? 1 yes, 0 no, -1 not decided.
  virtual int microphone_permission(StringView origin) = 0;
  // Asks the user (if it has not been decided): the same answers; -1 means the question is open and the answer comes later (ask microphone_permission() again).
  virtual int microphone_request(StringView origin) = 0;
  virtual UniquePointer<IRtcAudioSource> open_microphone() = 0;
  virtual UniquePointer<IRtcAudioSink> open_speaker() = 0;

 protected:
  IRtcAudioDevices() = default;
  ~IRtcAudioDevices() = default;
};

}  // namespace rtc
