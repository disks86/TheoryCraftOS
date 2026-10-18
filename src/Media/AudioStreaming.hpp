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

// Sound files played as a stream: the file is read through an IByteSource a little at a time and decoded as playback goes on, so
// neither the file nor the whole decoded sound is ever in memory - only a window of a second or so of decoded sound.
//
// open_audio_stream() looks at the file's contents and returns a PCM source (16-bit, mono or stereo) for it, or null if it is not a
// sound file this OS reads or cannot be decoded. The source gives the same sound as decode_audio() would, and can be read at any frame:
// frames after the last one read are decoded in order, anywhere else the decoder first finds its way back to that place.
//   WAV (PCM, float, A-law, mu-law, IMA and Microsoft ADPCM), AIFF / AIFF-C, AU   straight from the file, converted a window at a time
//   FLAC, MP3 (and MP2 / MP1), AAC (ADTS), Ogg Vorbis, M4A / MP4 / MOV (AAC-LC or MP3)   decoded one frame or packet at a time
// The source reads from `source` whenever it is read, so `source` (and what it reads) must outlive it.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Audio/PcmSource.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/ByteSource.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

UniquePointer<audio::IPcmSource> open_audio_stream(IByteSource& source, allocators::IAllocator& allocator, AudioFileFormat* detected = nullptr);

// The pieces open_audio_stream() is made of; each returns null if the file is not that kind of sound (or is damaged).
UniquePointer<audio::IPcmSource> open_wav_stream(IByteSource& source, allocators::IAllocator& allocator);
UniquePointer<audio::IPcmSource> open_aiff_stream(IByteSource& source, allocators::IAllocator& allocator);
UniquePointer<audio::IPcmSource> open_au_stream(IByteSource& source, allocators::IAllocator& allocator);
UniquePointer<audio::IPcmSource> open_flac_stream(IByteSource& source, allocators::IAllocator& allocator);
UniquePointer<audio::IPcmSource> open_mp3_stream(IByteSource& source, allocators::IAllocator& allocator);
UniquePointer<audio::IPcmSource> open_aac_stream(IByteSource& source, allocators::IAllocator& allocator);
UniquePointer<audio::IPcmSource> open_vorbis_stream(IByteSource& source, allocators::IAllocator& allocator);
UniquePointer<audio::IPcmSource> open_mp4_audio_stream(IByteSource& source, allocators::IAllocator& allocator);

// Sound that is stored as WAVE data - PCM, float, A-law, mu-law, IMA or Microsoft ADPCM - in `data` (a run of `data.size()` bytes in
// the source), described by a WAVEFORMATEX (`wave_format`, copied): the AVI player uses this for the sound track.
UniquePointer<audio::IPcmSource> open_wave_data_stream(IByteSource& data, ConstByteView wave_format, allocators::IAllocator& allocator);

// MPEG audio frames (nothing but frames, no tags), decoded one for one: a frame that cannot be decoded - for want of the earlier frames its
// data refers to - is silence rather than missing, so the sound is as long as the frames say. (Used for MP3 in an MP4 file.)
bool decode_mp3_frames(ConstByteView data, AudioData* out);

// The base of the streams: keeps a window of decoded sound and fills it from the decoder when a read goes outside it.
class StreamingPcmSource : public audio::IPcmSource {
 public:
  audio::StreamFormat format() const override { return format_; }
  uint64_t frame_count() const override { return frames_; }
  uint32_t read(uint64_t frame, ByteView out) override;

 protected:
  StreamingPcmSource() = default;
  ~StreamingPcmSource() = default;

  // Makes the window hold sound that includes `frame` (< frame_count()): decode it and call set_window(). False if that fails.
  virtual bool refill(uint64_t frame) = 0;
  // The window is `pcm` (borrowed until the next refill()), whole frames in the stream's format, the first one being frame `first`.
  void set_window(uint64_t first, ConstByteView pcm) {
    window_first_ = first;
    window_ = pcm;
  }
  uint64_t window_end_frame() const { return window_first_ + (format_.frame_size() ? window_.size() / format_.frame_size() : 0); }

  audio::StreamFormat format_;
  uint64_t frames_ = 0;

 private:
  uint64_t window_first_ = 0;
  ConstByteView window_;
};

}  // namespace media
