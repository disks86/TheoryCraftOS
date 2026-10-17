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
#include "Media/AudioData.hpp"
#include "Utility/ByteBuffer.hpp"

namespace media {

// Audio decoders and encoders in software. Decoders replace the contents of the AudioData and return false (leaving it
// empty) for anything they cannot decode; they never read outside `data`.
//
//   WAV   decode: PCM 8/16/24/32-bit, IEEE float 32/64, A-law, mu-law, IMA ADPCM, Microsoft ADPCM (also in WAVE_FORMAT_EXTENSIBLE).
//         encode: 16-bit PCM.
//   AIFF  decode: AIFF and AIFF-C with PCM 8/16/24/32-bit (big or little endian), float, A-law, mu-law. encode: 16-bit PCM.
//   AU    decode: PCM 8/16/24/32-bit, float 32/64, A-law, mu-law. encode: 16-bit PCM.
//   FLAC  decode: all of the format (1-8 channels, 4-32 bits, fixed and LPC predictors, all stereo modes, CRC-checked). encode:
//         fixed predictors with Rice coding, adaptive stereo, 16-bit.
//   Ogg Vorbis  decode only: floor 1 streams (all current encoders), mono and stereo (more channels keep the front pair).
// Not provided: MP3, AAC, Opus - lossy codecs whose code tables are not here.
bool decode_wav(ConstByteView data, AudioData* out);
bool encode_wav(const AudioData& audio, ByteBuffer* out);
bool decode_aiff(ConstByteView data, AudioData* out);
bool encode_aiff(const AudioData& audio, ByteBuffer* out);
bool decode_au(ConstByteView data, AudioData* out);
bool encode_au(const AudioData& audio, ByteBuffer* out);
bool decode_flac(ConstByteView data, AudioData* out);
bool encode_flac(const AudioData& audio, ByteBuffer* out);
bool decode_vorbis(ConstByteView data, AudioData* out);

constexpr uint32_t kEncodableAudioFormats = 0x1E;

bool decode_audio(ConstByteView data, AudioData* out, AudioFileFormat* detected = nullptr);
bool encode_audio(const AudioData& audio, AudioFileFormat format, ByteBuffer* out);

// Sample conversions the decoders share.
int16_t decode_mulaw(uint8_t v);
int16_t decode_alaw(uint8_t v);

}  // namespace media
