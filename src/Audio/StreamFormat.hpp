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
#include "Hardware/AudioDevice/AudioTypes.hpp"

namespace audio {

enum class SampleType : uint8_t {
  S16,  // signed 16-bit little endian
  U8,   // unsigned 8-bit, silence at 128
};

// The format of the audio a program writes to a playback stream or reads from a
// capture stream. The audio device itself always runs at 48 kHz, 16-bit, stereo;
// the streams convert between this format and that one (sample rate, mono or
// stereo, 8 or 16 bits), so programs can use whatever suits their sound.
struct StreamFormat {
  static constexpr uint32_t kMinRate = 4000;
  static constexpr uint32_t kMaxRate = 192000;

  uint32_t sample_rate = hardware::kAudioSampleRate;
  uint8_t channels = 2;  // 1 (mono) or 2 (left, right interleaved)
  SampleType type = SampleType::S16;

  constexpr uint32_t bytes_per_sample() const { return type == SampleType::S16 ? 2 : 1; }
  constexpr uint32_t frame_size() const { return channels * bytes_per_sample(); }
  constexpr bool valid() const {
    return sample_rate >= kMinRate && sample_rate <= kMaxRate && (channels == 1 || channels == 2);
  }
  // The format the audio devices run in: needs no conversion.
  constexpr bool is_native() const {
    return sample_rate == hardware::kAudioSampleRate && channels == 2 && type == SampleType::S16;
  }
};

}  // namespace audio
