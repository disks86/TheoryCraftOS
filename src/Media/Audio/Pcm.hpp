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
#include "Media/AudioCodecs.hpp"

namespace media {
namespace pcm {

enum class Encoding : uint8_t { U8, S8, S16, S24, S32, F32, F64, ALaw, ULaw };

constexpr uint32_t bytes_per_sample(Encoding e) {
  switch (e) {
    case Encoding::U8: case Encoding::S8: case Encoding::ALaw: case Encoding::ULaw: return 1;
    case Encoding::S16: return 2;
    case Encoding::S24: return 3;
    case Encoding::S32: case Encoding::F32: return 4;
    case Encoding::F64: return 8;
  }
  return 1;
}

// The 16-bit audio for `data` (whole frames of `channels` samples in `encoding`; multi-byte samples big- or little-endian),
// into `out`. Channels beyond the first two are dropped. False if the layout is not valid or memory ran out.
bool convert(ConstByteView data, Encoding encoding, bool big_endian, uint32_t channels, uint32_t sample_rate, AudioData* out);

// How many frames `bytes` of 16-bit audio hold in the given layout.
inline uint64_t frames_in(uint64_t bytes, Encoding e, uint32_t channels) { return channels ? bytes / (uint64_t{channels} * bytes_per_sample(e)) : 0; }

}  // namespace pcm
}  // namespace media
