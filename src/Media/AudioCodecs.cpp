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

#include "Media/AudioCodecs.hpp"

namespace media {

bool decode_audio(ConstByteView data, AudioData* out, AudioFileFormat* detected) {
  const AudioFileFormat format = detect_audio_format(data);
  if (detected) *detected = format;
  switch (format) {
    case AudioFileFormat::Wav: return decode_wav(data, out);
    case AudioFileFormat::Aiff: return decode_aiff(data, out);
    case AudioFileFormat::Au: return decode_au(data, out);
    case AudioFileFormat::Flac: return decode_flac(data, out);
    case AudioFileFormat::Vorbis: return decode_vorbis(data, out);
    case AudioFileFormat::Mp3: return decode_mp3(data, out);
    case AudioFileFormat::Unknown: break;
  }
  out->clear();
  return false;
}

bool encode_audio(const AudioData& audio, AudioFileFormat format, ByteBuffer* out) {
  switch (format) {
    case AudioFileFormat::Wav: return encode_wav(audio, out);
    case AudioFileFormat::Aiff: return encode_aiff(audio, out);
    case AudioFileFormat::Au: return encode_au(audio, out);
    case AudioFileFormat::Flac: return encode_flac(audio, out);
    case AudioFileFormat::Vorbis:
    case AudioFileFormat::Mp3: break;  // decode only
    case AudioFileFormat::Unknown: break;
  }
  return false;
}

}  // namespace media
