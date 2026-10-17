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

namespace {
uint32_t be32(ConstByteView v, uint64_t at) { return (uint32_t{v[at]} << 24) | (uint32_t{v[at + 1]} << 16) | (uint32_t{v[at + 2]} << 8) | v[at + 3]; }
}

bool decode_au(ConstByteView data, AudioData* out) {
  out->clear();
  if (data.size() < 24 || detect_audio_format(data) != AudioFileFormat::Au) return false;
  const uint32_t offset = be32(data, 4), size = be32(data, 8), encoding = be32(data, 12), rate = be32(data, 16), channels = be32(data, 20);
  if (offset < 24 || offset > data.size() || rate == 0 || channels == 0) return false;
  uint64_t length = data.size() - offset;
  if (size != 0xFFFFFFFFu && size < length) length = size;  // 0xFFFFFFFF means "until the end"
  const ConstByteView samples = data.subview(offset, length);
  using pcm::Encoding;
  switch (encoding) {
    case 1: return pcm::convert(samples, Encoding::ULaw, true, channels, rate, out);
    case 2: return pcm::convert(samples, Encoding::S8, true, channels, rate, out);
    case 3: return pcm::convert(samples, Encoding::S16, true, channels, rate, out);
    case 4: return pcm::convert(samples, Encoding::S24, true, channels, rate, out);
    case 5: return pcm::convert(samples, Encoding::S32, true, channels, rate, out);
    case 6: return pcm::convert(samples, Encoding::F32, true, channels, rate, out);
    case 7: return pcm::convert(samples, Encoding::F64, true, channels, rate, out);
    case 27: return pcm::convert(samples, Encoding::ALaw, true, channels, rate, out);
    default: return false;
  }
}

bool encode_au(const AudioData& audio, ByteBuffer* out) {
  if (audio.empty()) return false;
  auto put_be32 = [&](uint32_t v) { return out->put8(static_cast<uint8_t>(v >> 24)) && out->put8(static_cast<uint8_t>(v >> 16)) && out->put8(static_cast<uint8_t>(v >> 8)) && out->put8(static_cast<uint8_t>(v)); };
  const uint64_t bytes = audio.frame_count() * audio.format().channels * 2;
  if (bytes > 0xFFFFFF00ull) return false;
  bool ok = out->put_tag(".snd") && put_be32(24) && put_be32(static_cast<uint32_t>(bytes)) && put_be32(3) && put_be32(audio.format().sample_rate) && put_be32(audio.format().channels);
  for (int16_t s : audio.samples()) ok = ok && out->put8(static_cast<uint8_t>(static_cast<uint16_t>(s) >> 8)) && out->put8(static_cast<uint8_t>(s));
  return ok;
}

}  // namespace media
