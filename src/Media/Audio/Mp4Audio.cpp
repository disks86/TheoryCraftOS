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

// Audio out of an MP4/M4A file: AAC-LC (decoded block by block), or MP3 (the samples are frames of one MPEG audio stream).
#include "Media/Audio/AacInternal.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/Mp4.hpp"

namespace media {

namespace {
// Copies the part of `whole` that the track's edit list plays (its start offset and length) into `out`.
bool trim_to_edit(const AudioData& whole, const Mp4Track& t, AudioData* out) {
  const uint32_t rate = whole.format().sample_rate, channels = whole.format().channels;
  uint64_t frames = whole.frame_count(), skip = 0;
  if (t.skip > 0) skip = static_cast<uint64_t>(t.skip) * rate / t.timescale;
  if (t.play_duration >= 0) {
    const uint64_t keep = static_cast<uint64_t>(t.play_duration) * rate / t.timescale;
    if (skip + keep < frames) frames = skip + keep;
  }
  if (skip >= frames) skip = 0;
  frames -= skip;
  if (frames == 0 || !out->create(whole.format(), frames)) return false;
  __builtin_memcpy(out->samples().data(), whole.samples().data() + skip * channels, frames * channels * 2);
  return true;
}
}  // namespace

bool decode_mp4_audio(ConstByteView data, AudioData* out) {
  out->clear();
  allocators::IAllocator& allocator = out->allocator();
  Mp4File file(allocator);
  if (!file.open(data)) return false;
  const Mp4Track* t = file.find(Mp4TrackKind::Audio);
  if (!t || t->sample_count == 0) return false;
  const uint32_t mp4a = 0x6d703461;
  if (t->codec == mp4a && (t->object_type == 0x6B || t->object_type == 0x69)) {  // MPEG-1/2 audio: Layer III frames in the samples
    ByteBuffer stream(allocator);
    for (uint32_t i = 0; i < t->sample_count; ++i)
      if (!stream.append(file.sample(*t, i))) return false;
    AudioData whole(allocator);
    if (!decode_mp3(stream.view(), &whole)) return false;
    return trim_to_edit(whole, *t, out);
  }
  if (t->codec != mp4a || (t->object_type != 0x40 && t->object_type != 0x66 && t->object_type != 0x67 && t->object_type != 0x68)) return false;
  uint32_t aot, index, channel_config;
  if (!parse_audio_specific_config(t->config, &aot, &index, &channel_config)) return false;  // AAC-LC only
  AacDecoder decoder(allocator);
  if (!decoder.init(index)) return false;
  ByteBuffer pcm(allocator);
  uint32_t channels = 0;
  int16_t block[2048];
  for (uint32_t i = 0; i < t->sample_count; ++i) {
    if (!decoder.decode_block(file.sample(*t, i), block)) {
      if (channels == 0) return false;
      for (int k = 0; k < 2048; ++k) block[k] = 0;  // a block that cannot be decoded is a gap
    }
    if (channels == 0) channels = decoder.channels();
    if (!pcm.append(ConstByteView(reinterpret_cast<const uint8_t*>(block), uint64_t{1024} * channels * 2))) return false;
  }
  // The edit list drops the encoder's priming samples and the padding at the end.
  const uint32_t rate = decoder.sample_rate();
  uint64_t frames = pcm.size() / (channels * 2), skip = 0;
  if (t->skip > 0) skip = static_cast<uint64_t>(t->skip) * rate / t->timescale;
  if (t->play_duration >= 0) {
    const uint64_t keep = static_cast<uint64_t>(t->play_duration) * rate / t->timescale;
    if (skip + keep < frames) frames = skip + keep;
  }
  if (skip >= frames) skip = 0;
  frames -= skip;
  audio::StreamFormat format;
  format.sample_rate = rate;
  format.channels = static_cast<uint8_t>(channels);
  if (frames == 0 || !out->create(format, frames)) return false;
  __builtin_memcpy(out->samples().data(), pcm.view().data() + skip * channels * 2, frames * channels * 2);
  return true;
}

}  // namespace media
