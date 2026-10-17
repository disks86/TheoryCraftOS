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

#include "Media/Audio/Pcm.hpp"

namespace media {

int16_t decode_mulaw(uint8_t v) {
  v = static_cast<uint8_t>(~v);
  int32_t t = ((v & 0x0F) << 3) + 0x84;
  t <<= (v & 0x70) >> 4;
  return static_cast<int16_t>((v & 0x80) ? 0x84 - t : t - 0x84);
}

int16_t decode_alaw(uint8_t v) {
  v ^= 0x55;
  int32_t t = (v & 0x0F) << 4;
  const int32_t segment = (v & 0x70) >> 4;
  switch (segment) {
    case 0: t += 8; break;
    case 1: t += 0x108; break;
    default: t += 0x108; t <<= segment - 1; break;
  }
  return static_cast<int16_t>((v & 0x80) ? t : -t);
}

namespace pcm {

namespace {
int16_t clamp16(double v) { return static_cast<int16_t>(v >= 32767.0 ? 32767 : v <= -32768.0 ? -32768 : static_cast<int32_t>(v)); }
}

bool convert(ConstByteView data, Encoding encoding, bool big_endian, uint32_t channels, uint32_t sample_rate, AudioData* out) {
  out->clear();
  if (channels < 1 || channels > 64) return false;
  const uint32_t width = bytes_per_sample(encoding);
  const uint64_t frames = data.size() / (uint64_t{channels} * width);
  const uint32_t out_channels = channels > 1 ? 2 : 1;
  audio::StreamFormat format;
  format.sample_rate = sample_rate;
  format.channels = static_cast<uint8_t>(out_channels);
  format.type = audio::SampleType::S16;
  if (frames == 0 || !out->create(format, frames)) return false;
  ArrayView<int16_t> samples = out->samples();
  auto sample_at = [&](uint64_t frame, uint32_t channel) -> int16_t {
    const uint8_t* p = data.data() + (frame * channels + channel) * width;
    switch (encoding) {
      case Encoding::U8: return static_cast<int16_t>((int32_t{p[0]} - 128) * 256);
      case Encoding::S8: return static_cast<int16_t>(static_cast<int8_t>(p[0]) * 256);
      case Encoding::ALaw: return decode_alaw(p[0]);
      case Encoding::ULaw: return decode_mulaw(p[0]);
      case Encoding::S16: return static_cast<int16_t>(big_endian ? (p[0] << 8) | p[1] : p[0] | (p[1] << 8));
      case Encoding::S24: return static_cast<int16_t>(big_endian ? (p[0] << 8) | p[1] : (p[2] << 8) | p[1]);  // the top 16 bits
      case Encoding::S32: return static_cast<int16_t>(big_endian ? (p[0] << 8) | p[1] : (p[3] << 8) | p[2]);
      case Encoding::F32: {
        uint32_t bits = big_endian ? (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]
                                   : (uint32_t{p[3]} << 24) | (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0];
        float f;
        __builtin_memcpy(&f, &bits, 4);
        return clamp16(static_cast<double>(f) * 32768.0);
      }
      case Encoding::F64: {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits |= uint64_t{p[big_endian ? 7 - i : i]} << (8 * i);
        double d;
        __builtin_memcpy(&d, &bits, 8);
        return clamp16(d * 32768.0);
      }
    }
    return 0;
  };
  for (uint64_t f = 0; f < frames; ++f)
    for (uint32_t c = 0; c < out_channels; ++c) samples[f * out_channels + c] = sample_at(f, c);
  return true;
}

}  // namespace pcm
}  // namespace media
