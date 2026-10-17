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
uint32_t le16(ConstByteView v, uint64_t at) { return v[at] | (v[at + 1] << 8); }
uint32_t le32(ConstByteView v, uint64_t at) { return le16(v, at) | (le16(v, at + 2) << 16); }
bool tag_is(ConstByteView v, uint64_t at, const char* tag) {
  for (int i = 0; i < 4; ++i)
    if (v[at + i] != static_cast<uint8_t>(tag[i])) return false;
  return true;
}

constexpr int16_t kImaStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,    34,    37,
                                  41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,
                                  230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,
                                  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
                                  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int8_t kImaIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct ImaState {
  int32_t predictor = 0;
  int32_t index = 0;
  int16_t decode(uint32_t nibble) {
    const int32_t step = kImaStep[index];
    int32_t diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    predictor += (nibble & 8) ? -diff : diff;
    predictor = predictor > 32767 ? 32767 : predictor < -32768 ? -32768 : predictor;
    index += kImaIndex[nibble & 7];
    index = index < 0 ? 0 : index > 88 ? 88 : index;
    return static_cast<int16_t>(predictor);
  }
};

// IMA ADPCM in blocks of `block` bytes: per channel a 4-byte header (predictor, step index), then the nibbles, four bytes
// of one channel at a time.
bool decode_ima(ConstByteView data, uint32_t channels, uint32_t block, uint32_t rate, AudioData* out) {
  if (channels < 1 || channels > 2 || block < 4 * channels + 1 || block > 65536) return false;
  const uint32_t per_block = (block - 4 * channels) * 2 / channels + 1;
  const uint64_t blocks = data.size() / block;
  if (blocks == 0) return false;
  audio::StreamFormat format;
  format.sample_rate = rate;
  format.channels = static_cast<uint8_t>(channels);
  format.type = audio::SampleType::S16;
  if (!out->create(format, blocks * per_block)) return false;
  ArrayView<int16_t> samples = out->samples();
  for (uint64_t b = 0; b < blocks; ++b) {
    const uint8_t* in = data.data() + b * block;
    ImaState state[2];
    int16_t* dest = samples.data() + b * per_block * channels;
    for (uint32_t c = 0; c < channels; ++c) {
      state[c].predictor = static_cast<int16_t>(in[4 * c] | (in[4 * c + 1] << 8));
      state[c].index = in[4 * c + 2] > 88 ? 88 : in[4 * c + 2];
      dest[c] = static_cast<int16_t>(state[c].predictor);
    }
    uint32_t at = 4 * channels;
    for (uint32_t frame = 1; frame < per_block;) {
      for (uint32_t c = 0; c < channels; ++c)
        for (uint32_t k = 0; k < 4 && at < block; ++k, ++at) {  // (a block size that is not a multiple of 4 bytes per channel ends mid-group)
          if (frame + 2 * k < per_block) dest[(frame + 2 * k) * channels + c] = state[c].decode(in[at] & 15);
          if (frame + 2 * k + 1 < per_block) dest[(frame + 2 * k + 1) * channels + c] = state[c].decode(in[at] >> 4);
        }
      frame += 8;
    }
  }
  return true;
}

constexpr int32_t kMsAdaptation[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
constexpr int16_t kMsDefaultCoefficients[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};

bool decode_ms(ConstByteView data, uint32_t channels, uint32_t block, uint32_t rate, const int16_t (*coefficients)[2], uint32_t coefficient_count, AudioData* out) {
  if (channels < 1 || channels > 2 || block < 7 * channels + 1 || block > 65536) return false;
  const uint32_t per_block = 2 + (block - 7 * channels) * 2 / channels;
  const uint64_t blocks = data.size() / block;
  if (blocks == 0) return false;
  audio::StreamFormat format;
  format.sample_rate = rate;
  format.channels = static_cast<uint8_t>(channels);
  format.type = audio::SampleType::S16;
  if (!out->create(format, blocks * per_block)) return false;
  ArrayView<int16_t> samples = out->samples();
  for (uint64_t b = 0; b < blocks; ++b) {
    const uint8_t* in = data.data() + b * block;
    int16_t* dest = samples.data() + b * per_block * channels;
    uint32_t predictor[2];
    int32_t delta[2], s1[2], s2[2];
    uint32_t at = 0;
    for (uint32_t c = 0; c < channels; ++c) {
      predictor[c] = in[at++];
      if (predictor[c] >= coefficient_count) return false;
    }
    for (uint32_t c = 0; c < channels; ++c, at += 2) delta[c] = static_cast<int16_t>(in[at] | (in[at + 1] << 8));
    for (uint32_t c = 0; c < channels; ++c, at += 2) s1[c] = static_cast<int16_t>(in[at] | (in[at + 1] << 8));
    for (uint32_t c = 0; c < channels; ++c, at += 2) s2[c] = static_cast<int16_t>(in[at] | (in[at + 1] << 8));
    for (uint32_t c = 0; c < channels; ++c) {
      dest[c] = static_cast<int16_t>(s2[c]);
      dest[channels + c] = static_cast<int16_t>(s1[c]);
    }
    uint32_t frame = 2, channel = 0;
    for (; at < block && frame < per_block; ++at) {
      for (uint32_t half = 0; half < 2 && frame < per_block; ++half) {
        const uint32_t nibble = half == 0 ? in[at] >> 4 : in[at] & 15;
        const int32_t signed_nibble = nibble >= 8 ? static_cast<int32_t>(nibble) - 16 : static_cast<int32_t>(nibble);
        int32_t value = (s1[channel] * coefficients[predictor[channel]][0] + s2[channel] * coefficients[predictor[channel]][1]) / 256 + signed_nibble * delta[channel];
        value = value > 32767 ? 32767 : value < -32768 ? -32768 : value;
        dest[frame * channels + channel] = static_cast<int16_t>(value);
        s2[channel] = s1[channel];
        s1[channel] = value;
        delta[channel] = kMsAdaptation[nibble] * delta[channel] / 256;
        if (delta[channel] < 16) delta[channel] = 16;
        if (++channel == channels) {
          channel = 0;
          ++frame;
        }
      }
    }
  }
  return true;
}
}  // namespace

bool decode_wav(ConstByteView data, AudioData* out) {
  out->clear();
  if (data.size() < 12 || detect_audio_format(data) != AudioFileFormat::Wav) return false;
  uint32_t tag = 0, channels = 0, rate = 0, block_align = 0, bits = 0;
  uint64_t fmt_extra = 0, fmt_extra_size = 0;
  bool have_format = false;
  uint64_t at = 12;
  while (at + 8 <= data.size()) {
    const uint64_t size = le32(data, at + 4), body = at + 8;
    const uint64_t available = data.size() - body;
    const uint64_t length = size < available ? size : available;
    if (tag_is(data, at, "fmt ")) {
      if (length < 16) return false;
      tag = le16(data, body);
      channels = le16(data, body + 2);
      rate = le32(data, body + 4);
      block_align = le16(data, body + 12);
      bits = le16(data, body + 14);
      if (length >= 18) {
        fmt_extra_size = le16(data, body + 16);
        fmt_extra = body + 18;
        if (fmt_extra + fmt_extra_size > body + length) fmt_extra_size = body + length - fmt_extra;
      }
      if (tag == 0xFFFE) {  // extensible: the real format is the first two bytes of the sub-format GUID
        if (fmt_extra_size < 22) return false;
        tag = le16(data, fmt_extra + 6);
      }
      have_format = true;
    } else if (tag_is(data, at, "data")) {
      if (!have_format || channels == 0 || rate == 0) return false;
      const ConstByteView pcm_data = data.subview(body, length);
      using pcm::Encoding;
      switch (tag) {
        case 1:  // PCM
          if (bits == 8) return pcm::convert(pcm_data, Encoding::U8, false, channels, rate, out);
          if (bits == 16) return pcm::convert(pcm_data, Encoding::S16, false, channels, rate, out);
          if (bits == 24) return pcm::convert(pcm_data, Encoding::S24, false, channels, rate, out);
          if (bits == 32) return pcm::convert(pcm_data, Encoding::S32, false, channels, rate, out);
          return false;
        case 3:  // IEEE float
          if (bits == 32) return pcm::convert(pcm_data, Encoding::F32, false, channels, rate, out);
          if (bits == 64) return pcm::convert(pcm_data, Encoding::F64, false, channels, rate, out);
          return false;
        case 6: return pcm::convert(pcm_data, Encoding::ALaw, false, channels, rate, out);
        case 7: return pcm::convert(pcm_data, Encoding::ULaw, false, channels, rate, out);
        case 0x11: return decode_ima(pcm_data, channels, block_align, rate, out);
        case 2: {  // Microsoft ADPCM: the coefficient pairs are in the format's extra data
          int16_t coefficients[16][2];
          uint32_t count = 7;
          for (uint32_t i = 0; i < 7; ++i) {
            coefficients[i][0] = kMsDefaultCoefficients[i][0];
            coefficients[i][1] = kMsDefaultCoefficients[i][1];
          }
          if (fmt_extra_size >= 4) {
            const uint32_t n = le16(data, fmt_extra + 2);
            if (n >= 1 && n <= 16 && 4 + uint64_t{n} * 4 <= fmt_extra_size) {
              count = n;
              for (uint32_t i = 0; i < n; ++i) {
                coefficients[i][0] = static_cast<int16_t>(le16(data, fmt_extra + 4 + 4 * i));
                coefficients[i][1] = static_cast<int16_t>(le16(data, fmt_extra + 6 + 4 * i));
              }
            }
          }
          return decode_ms(pcm_data, channels, block_align, rate, coefficients, count, out);
        }
        default: return false;
      }
    }
    at = body + size + (size & 1);
  }
  return false;
}

bool encode_wav(const AudioData& audio, ByteBuffer* out) {
  if (audio.empty()) return false;
  const uint32_t channels = audio.format().channels, rate = audio.format().sample_rate;
  const uint64_t bytes = audio.frame_count() * channels * 2;
  if (bytes > 0xFFFFFF00ull) return false;
  bool ok = out->put_tag("RIFF") && out->put32(static_cast<uint32_t>(36 + bytes)) && out->put_tag("WAVE") && out->put_tag("fmt ") && out->put32(16) &&
            out->put16(1) && out->put16(channels) && out->put32(rate) && out->put32(rate * channels * 2) && out->put16(channels * 2) && out->put16(16) &&
            out->put_tag("data") && out->put32(static_cast<uint32_t>(bytes));
  for (int16_t s : audio.samples()) ok = ok && out->put16(static_cast<uint16_t>(s));
  return ok;
}

}  // namespace media
