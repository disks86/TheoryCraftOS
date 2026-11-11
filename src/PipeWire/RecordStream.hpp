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

// What a program of any kind records from the graph: the other half of PlaybackStream. A RecordStream reads an audio::CaptureStream of the capture hub
// (16-bit mono or stereo at the program's rate: the hub resamples from the device) and turns it into the program's own format - any of PipeWire's common
// sample formats, 1 to 32 channels - as it is read. With more than two channels each one gets its side of the stereo input (centre, mono and the
// channels without a side get both sides' average, LFE silence).
//
// A paused stream (PulseAudio's cork, waveInStop, IAudioClient::Stop) hears nothing: what the microphone records meanwhile is dropped, so it starts
// again with sound from the moment it is resumed. Thread safe.
#pragma once
#include <stdint.h>
#include "Audio/CaptureStream.hpp"
#include "Kernel/Sync.hpp"
#include "PipeWire/PlaybackStream.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"

namespace pipewire {

// Converts whole frames of interleaved 16-bit little-endian samples, `in_channels` (1 or 2) per frame, into `out`'s format. Returns the bytes written;
// *consumed is the input bytes used (both whole frames). `positions` are the output's channels as SPA channel numbers (null: the usual order).
uint32_t convert_from_s16(ConstByteView source, uint32_t in_channels, const PcmFormat& out, const uint32_t* positions, ByteView target, uint32_t* consumed);

class RecordStream {
 public:
  static constexpr uint32_t kChunkFrames = 1024;

  RecordStream(allocators::IAllocator& allocator, const PcmFormat& format, SharedPointer<audio::CaptureStream> capture);
  RecordStream(const RecordStream&) = delete;
  RecordStream& operator=(const RecordStream&) = delete;

  const PcmFormat& format() const { return format_; }
  audio::CaptureStream& capture() { return *capture_; }

  // Copies out the oldest whole frames recorded, in the program's format; returns the bytes copied (0 when nothing is waiting or it is paused).
  // Never blocks.
  uint32_t read(ByteView out);
  // Program bytes read() would return now.
  uint64_t available_bytes();
  // Program bytes read so far.
  uint64_t read_bytes();
  // Program bytes the microphone recorded that the stream lost because it was not read in time (not counting the time it was paused).
  uint64_t lost_bytes();
  // Throws away what has been recorded so far.
  void flush();
  void set_paused(bool paused);
  bool paused() const { return __atomic_load_n(&paused_, __ATOMIC_ACQUIRE); }
  // No more audio; the hub lets the capture stream go.
  void close() { capture_->close(); }
  bool closed() const { return capture_->closed(); }

 private:
  PcmFormat format_;
  uint32_t in_channels_;
  SharedPointer<audio::CaptureStream> capture_;
  kernel::Mutex mutex_;
  ByteBuffer scratch_;        // 16-bit frames read from the capture stream, before conversion
  uint64_t read_ = 0;         // program bytes handed out
  uint64_t lost_base_ = 0;    // the capture stream's lost frames that were the pause's doing
  bool paused_ = false;
};

}  // namespace pipewire
