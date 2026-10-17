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
#include "Allocators/IAllocator.hpp"
#include "Audio/StreamFormat.hpp"
#include "Audio/PcmSource.hpp"
#include "Utility/AllocatedView.hpp"

namespace media {

enum class AudioFileFormat : uint8_t { Unknown, Wav, Aiff, Au, Flac, Vorbis, Mp3 };
const char* audio_format_name(AudioFileFormat format);
constexpr uint32_t audio_format_bit(AudioFileFormat format) { return 1u << static_cast<uint8_t>(format); }
AudioFileFormat detect_audio_format(ConstByteView data);

// Decoded sound: 16-bit signed samples, mono or stereo, interleaved, at the file's sample rate (24- and 32-bit,
// floating point, A-law, mu-law and ADPCM sources are all brought to this; more than two channels keep the front pair).
// It plays through the audio system directly: source() is a PCM source over the samples.
class AudioData {
 public:
  explicit AudioData(allocators::IAllocator& allocator) : allocator_(allocator), source_(ConstByteView(), audio::StreamFormat()) {}
  ~AudioData() { clear(); }
  AudioData(const AudioData&) = delete;
  AudioData& operator=(const AudioData&) = delete;

  // Room for `frames` frames in `format` (S16, one or two channels). Contents unspecified. False if the format is not
  // valid or memory ran out.
  bool create(const audio::StreamFormat& format, uint64_t frames);
  void clear();

  bool empty() const { return samples_.empty(); }
  const audio::StreamFormat& format() const { return format_; }
  uint64_t frame_count() const { return frames_; }
  uint64_t duration_ms() const { return format_.sample_rate ? frames_ * 1000 / format_.sample_rate : 0; }
  ArrayView<int16_t> samples() const { return samples_; }
  ConstByteView bytes() const { return ConstByteView(reinterpret_cast<const uint8_t*>(samples_.data()), samples_.size() * 2); }
  // A PCM source over the samples, for AudioPlayer. Valid while this object is, and until create()/clear().
  audio::IPcmSource* source() { return &source_; }
  allocators::IAllocator& allocator() const { return allocator_; }

 private:
  allocators::IAllocator& allocator_;
  ArrayView<int16_t> samples_;
  audio::StreamFormat format_;
  uint64_t frames_ = 0;
  audio::MemoryPcmSource source_;
};

}  // namespace media
