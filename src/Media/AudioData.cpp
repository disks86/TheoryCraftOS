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

#include "Media/AudioData.hpp"
#include "Media/AudioCodecs.hpp"


namespace media {

const char* audio_format_name(AudioFileFormat format) {
  switch (format) {
    case AudioFileFormat::Wav: return "WAV";
    case AudioFileFormat::Aiff: return "AIFF";
    case AudioFileFormat::Au: return "AU";
    case AudioFileFormat::Flac: return "FLAC";
    case AudioFileFormat::Vorbis: return "Ogg Vorbis";
    case AudioFileFormat::Mp3: return "MP3";
    case AudioFileFormat::Aac: return "AAC";
    case AudioFileFormat::Mp4: return "MP4";
    case AudioFileFormat::Unknown: break;
  }
  return "unknown";
}

AudioFileFormat detect_audio_format(ConstByteView d) {
  auto starts = [&](const char* tag, uint32_t at) {
    if (d.size() < at + 4) return false;
    for (uint32_t i = 0; i < 4; ++i)
      if (d[at + i] != static_cast<uint8_t>(tag[i])) return false;
    return true;
  };
  if (starts("RIFF", 0) && starts("WAVE", 8)) return AudioFileFormat::Wav;
  if (starts("FORM", 0) && (starts("AIFF", 8) || starts("AIFC", 8))) return AudioFileFormat::Aiff;
  if (starts(".snd", 0)) return AudioFileFormat::Au;
  if (starts("fLaC", 0)) return AudioFileFormat::Flac;
  // MP4/M4A: an 'ftyp' box right at the start (whether it has audio is for the decoder to say).
  if (starts("ftyp", 4)) return AudioFileFormat::Mp4;
  // AAC in ADTS framing: sync, layer 0, and a second header where the first frame ends.
  if (looks_like_adts(d)) return AudioFileFormat::Aac;
  // MP3 has no signature: an ID3v2 tag, or an MPEG audio frame header (sync, Layer III, a real bitrate and sample rate) right at the start.
  if (d.size() >= 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3' && d[3] < 0xFF) return AudioFileFormat::Mp3;
  if (d.size() >= 4 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0 && ((d[1] >> 1) & 3) != 0 && ((d[1] >> 3) & 3) != 1 && (d[2] >> 4) != 15 && (d[2] >> 4) != 0 && ((d[2] >> 2) & 3) != 3)
    return AudioFileFormat::Mp3;
  // Ogg holds many codecs: only a first packet that is a Vorbis identification header counts (the first page's lacing table is
  // 27 bytes in, then the packet).
  if (starts("OggS", 0) && d.size() > 27 + 7) {
    const uint32_t body = 27 + d[26];
    if (d.size() >= body + 7 && d[body] == 1 && d[body + 1] == 'v' && d[body + 2] == 'o' && d[body + 3] == 'r' && d[body + 4] == 'b' && d[body + 5] == 'i' && d[body + 6] == 's')
      return AudioFileFormat::Vorbis;
  }
  return AudioFileFormat::Unknown;
}

bool AudioData::create(const audio::StreamFormat& format, uint64_t frames) {
  clear();
  if (!format.valid() || format.type != audio::SampleType::S16 || frames == 0 || frames > (uint64_t{1} << 31)) return false;
  samples_ = allocate_view<int16_t>(allocator_, frames * format.channels, 16);
  if (samples_.empty()) return false;
  format_ = format;
  frames_ = frames;
  source_ = audio::MemoryPcmSource(bytes(), format_);
  return true;
}

void AudioData::clear() {
  release_view(allocator_, samples_);
  frames_ = 0;
  source_ = audio::MemoryPcmSource(ConstByteView(), audio::StreamFormat());
}

}  // namespace media
