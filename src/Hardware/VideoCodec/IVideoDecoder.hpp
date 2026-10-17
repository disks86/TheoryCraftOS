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
#include "Hardware/VideoCodec/VideoTypes.hpp"

namespace hardware {

// A video decoding session: compressed pictures go in with submit(), pictures come out of receive() in
// display order. Polled and non-blocking: a decoder may need several packets before the first frame
// appears (a codec with reordering) or hold output until it is read (submit() then says Full).
// Sessions come from an IVideoCodecDevice, which is where the work runs - on a GPU's decode engine, or on
// the CPU.
class IVideoDecoder {
 public:
  virtual VideoCodec codec() const = 0;
  virtual SubmitResult submit(const VideoPacket& packet) = 0;
  // The next picture, in the format the session was configured for. `*frame` points into memory the decoder
  // owns, valid until the next call on the decoder. False if nothing is ready.
  virtual bool receive(VideoFrame* frame) = 0;
  // The stream has ended: pictures still held back become available to receive().
  virtual void flush() = 0;
  // Forget everything in flight and wait for a keyframe (after a seek).
  virtual void reset() = 0;
  // The size of the decoded pictures; zero until known (after the first packet).
  virtual VideoFormat output_format() const = 0;
  virtual VideoCodecStatistics statistics() const = 0;

 protected:
  constexpr IVideoDecoder() = default;
  ~IVideoDecoder() = default;
};

}  // namespace hardware
