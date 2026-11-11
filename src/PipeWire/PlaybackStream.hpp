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

// What a program of any kind plays into the graph. A PlaybackStream takes PCM in the program's own format - any of PipeWire's common sample formats,
// 1 to 32 channels, 4 to 192 kHz - and turns it into 16-bit mono or stereo at the program's rate for an audio::AudioStream of the mixer, which resamples
// to the device and mixes it with everyone else's (the job PipeWire's audioconvert does). More than two channels are folded down to front left/right
// (centre and LFE into both, the rest to their side).
#pragma once
#include <stdint.h>
#include "Audio/AudioMixer.hpp"
#include "Audio/AudioStream.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"

namespace pipewire {

enum class SampleFormat : uint8_t { U8, S16Le, S16Be, S24Le, S24Be, S24_32Le, S32Le, S32Be, F32Le, F32Be, F64Le, Alaw, Ulaw };

struct PcmFormat {
  static constexpr uint32_t kMaxChannels = 32;
  SampleFormat format = SampleFormat::S16Le;
  uint32_t rate = 48000;
  uint32_t channels = 2;

  uint32_t sample_size() const;
  uint32_t frame_size() const { return sample_size() * channels; }
  bool valid() const { return rate >= audio::StreamFormat::kMinRate && rate <= audio::StreamFormat::kMaxRate && channels >= 1 && channels <= kMaxChannels; }
  // PipeWire's spa_audio_format number and name ("S16LE") of the format.
  uint32_t spa_format() const;
  const char* name() const;
};

// Converts whole frames of `in` into interleaved 16-bit little-endian samples, `out_channels` (1 or 2) per frame. Returns the bytes written; *consumed is
// the input bytes used (both whole frames). `positions` are the input's channels as SPA channel numbers (null: the usual order for that count).
uint32_t convert_to_s16(const PcmFormat& in, const uint32_t* positions, ConstByteView source, ByteView out, uint32_t out_channels, uint32_t* consumed);

class PlaybackStream {
 public:
  static constexpr uint32_t kChunkFrames = 1024;

  // `mixer` (may be null) is the one `stream` plays on: it tells when the device has really played what the stream gave it.
  PlaybackStream(allocators::IAllocator& allocator, const PcmFormat& format, SharedPointer<audio::AudioStream> stream, audio::AudioMixer* mixer = nullptr);
  PlaybackStream(const PlaybackStream&) = delete;
  PlaybackStream& operator=(const PlaybackStream&) = delete;

  const PcmFormat& format() const { return format_; }
  audio::AudioStream& stream() { return *stream_; }
  const SharedPointer<audio::AudioStream>& shared_stream() const { return stream_; }

  // Takes whole frames of `pcm` (the program's format) and returns the bytes taken; 0 when the stream is full or closed. Never blocks.
  uint32_t write(ConstByteView pcm);
  // Program bytes written but not yet played (approximate across rates).
  uint64_t buffered_bytes();
  // Program bytes played so far: taken by the mixer, less what is still in the device's queue (exact once drained()).
  uint64_t played_bytes();
  uint64_t written_bytes();
  // Program bytes written that the mixer has not taken yet (still in this stream; approximate across rates). What WASAPI calls the padding, and what a
  // program's own mixer keeps topped up; unlike buffered_bytes() it leaves out the device's queue.
  uint64_t waiting_bytes();
  // Everything written has been played: taken by the mixer, and the device has played it out too.
  bool drained();
  // Drops everything not yet played.
  void flush();
  void set_paused(bool paused) { stream_->set_paused(paused); }
  bool paused() const { return stream_->paused(); }
  // No more writes; the mixer plays out what is queued and lets the stream go.
  void close() { stream_->close(); }

 private:
  // Offers the converted bytes left from last time. True when none are left.
  bool flush_carry_locked();

  PcmFormat format_;
  uint32_t out_channels_;
  SharedPointer<audio::AudioStream> stream_;
  kernel::Mutex mutex_;
  ByteBuffer carry_;          // converted, not yet taken by the stream
  uint64_t carry_offset_ = 0;
  ByteBuffer scratch_;
  uint64_t written_ = 0;      // program bytes taken
  audio::AudioMixer* mixer_;
  uint64_t drain_mark_ = 0;   // the mixer's mixed_frames() when the stream was first seen empty (0: it is not)
};

}  // namespace pipewire
