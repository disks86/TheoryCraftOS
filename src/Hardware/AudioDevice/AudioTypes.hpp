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
#include <stdint.h>

namespace hardware {

// Every audio device in this OS plays one fixed format: 48 kHz, 16-bit signed
// little-endian samples, two channels interleaved (left, right). All the
// hardware supported here can do it natively, so nothing resamples.
constexpr uint32_t kAudioSampleRate = 48000;
constexpr uint32_t kAudioChannels = 2;
constexpr uint32_t kAudioBytesPerSample = 2;
constexpr uint32_t kAudioFrameSize = kAudioChannels * kAudioBytesPerSample;  // bytes per left+right pair
constexpr uint8_t kAudioMaxVolume = 100;

struct AudioStatistics {
  uint64_t frames_played = 0;    // frames the device has consumed (a count; silence after an underrun is not included)
  uint64_t frames_queued = 0;    // frames accepted by write()
  uint64_t underruns = 0;        // times the device ran dry while playing
  uint64_t write_failures = 0;   // write() calls refused (device not running, nothing fits)
};

}  // namespace hardware
