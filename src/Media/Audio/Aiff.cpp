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
uint32_t be16(ConstByteView v, uint64_t at) { return (uint32_t{v[at]} << 8) | v[at + 1]; }
uint32_t be32(ConstByteView v, uint64_t at) { return (be16(v, at) << 16) | be16(v, at + 2); }
bool tag_is(ConstByteView v, uint64_t at, const char* tag) {
  for (int i = 0; i < 4; ++i)
    if (v[at + i] != static_cast<uint8_t>(tag[i])) return false;
  return true;
}

// The 80-bit IEEE extended number AIFF uses for the sample rate, as an integer (0 if not sensible).
uint32_t extended_to_rate(ConstByteView v, uint64_t at) {
  const int32_t exponent = static_cast<int32_t>(be16(v, at) & 0x7FFF) - 16383;
  uint64_t mantissa = (uint64_t{be32(v, at + 2)} << 32) | be32(v, at + 6);
  if (v[at] & 0x80 || exponent < 0 || exponent > 40) return 0;
  const int32_t shift = 63 - exponent;
  const uint64_t value = shift >= 0 ? mantissa >> shift : mantissa << -shift;
  return value > 0xFFFFFFFFull ? 0 : static_cast<uint32_t>(value);
}

// An integer rate as an 80-bit extended number.
void rate_to_extended(uint32_t rate, uint8_t* out) {
  for (int i = 0; i < 10; ++i) out[i] = 0;
  if (rate == 0) return;
  int exponent = 31;
  while (!(rate & (1u << exponent))) --exponent;
  const uint64_t mantissa = uint64_t{rate} << (63 - exponent);
  const uint32_t biased = static_cast<uint32_t>(exponent + 16383);
  out[0] = static_cast<uint8_t>(biased >> 8);
  out[1] = static_cast<uint8_t>(biased);
  for (int i = 0; i < 8; ++i) out[2 + i] = static_cast<uint8_t>(mantissa >> (56 - 8 * i));
}
}  // namespace

bool decode_aiff(ConstByteView data, AudioData* out) {
  out->clear();
  if (data.size() < 12 || detect_audio_format(data) != AudioFileFormat::Aiff) return false;
  const bool aifc = tag_is(data, 8, "AIFC");
  uint32_t channels = 0, bits = 0, rate = 0;
  uint64_t frames = 0;
  bool have_comm = false, little = false;
  pcm::Encoding encoding = pcm::Encoding::S16;
  uint64_t at = 12;
  while (at + 8 <= data.size()) {
    const uint64_t size = be32(data, at + 4), body = at + 8;
    const uint64_t available = data.size() - body;
    const uint64_t length = size < available ? size : available;
    if (tag_is(data, at, "COMM")) {
      if (length < 18) return false;
      channels = be16(data, body);
      frames = be32(data, body + 2);
      bits = be16(data, body + 6);
      rate = extended_to_rate(data, body + 8);
      pcm::Encoding e = bits <= 8 ? pcm::Encoding::S8 : bits <= 16 ? pcm::Encoding::S16 : bits <= 24 ? pcm::Encoding::S24 : pcm::Encoding::S32;
      if (aifc) {
        if (length < 22) return false;
        if (tag_is(data, body + 18, "NONE") || tag_is(data, body + 18, "twos")) {
        } else if (tag_is(data, body + 18, "sowt")) {
          little = true;
        } else if (tag_is(data, body + 18, "raw ") && bits == 8) {
          e = pcm::Encoding::U8;
        } else if (tag_is(data, body + 18, "fl32") || tag_is(data, body + 18, "FL32")) {
          e = pcm::Encoding::F32;
        } else if (tag_is(data, body + 18, "fl64") || tag_is(data, body + 18, "FL64")) {
          e = pcm::Encoding::F64;
        } else if (tag_is(data, body + 18, "ulaw") || tag_is(data, body + 18, "ULAW")) {
          e = pcm::Encoding::ULaw;
        } else if (tag_is(data, body + 18, "alaw") || tag_is(data, body + 18, "ALAW")) {
          e = pcm::Encoding::ALaw;
        } else {
          return false;
        }
      }
      if (bits == 0 || bits > 32) return false;
      encoding = e;
      have_comm = true;
    } else if (tag_is(data, at, "SSND")) {
      if (!have_comm || length < 8 || channels == 0 || rate == 0) return false;
      const uint64_t offset = be32(data, body);
      if (8 + offset > length) return false;
      ConstByteView samples = data.subview(body + 8 + offset, length - 8 - offset);
      const uint64_t wanted = frames * channels * pcm::bytes_per_sample(encoding);
      if (wanted < samples.size()) samples = samples.first(wanted);
      return pcm::convert(samples, encoding, !little, channels, rate, out);
    }
    at = body + size + (size & 1);
  }
  return false;
}

bool encode_aiff(const AudioData& audio, ByteBuffer* out) {
  if (audio.empty()) return false;
  const uint32_t channels = audio.format().channels;
  const uint64_t bytes = audio.frame_count() * channels * 2;
  if (bytes > 0xFFFFFF00ull) return false;
  auto put_be16 = [&](uint32_t v) { return out->put8(static_cast<uint8_t>(v >> 8)) && out->put8(static_cast<uint8_t>(v)); };
  auto put_be32 = [&](uint32_t v) { return put_be16(v >> 16) && put_be16(v & 0xFFFF); };
  uint8_t extended[10];
  rate_to_extended(audio.format().sample_rate, extended);
  bool ok = out->put_tag("FORM") && put_be32(static_cast<uint32_t>(4 + 8 + 18 + 8 + 8 + bytes)) && out->put_tag("AIFF") && out->put_tag("COMM") && put_be32(18) &&
            put_be16(channels) && put_be32(static_cast<uint32_t>(audio.frame_count())) && put_be16(16) && out->append(ConstByteView(extended, 10)) &&
            out->put_tag("SSND") && put_be32(static_cast<uint32_t>(8 + bytes)) && put_be32(0) && put_be32(0);
  for (int16_t s : audio.samples()) ok = ok && put_be16(static_cast<uint16_t>(s));
  return ok;
}

}  // namespace media
