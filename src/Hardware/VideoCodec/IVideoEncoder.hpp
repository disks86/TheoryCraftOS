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

// A video encoding session: pictures go in with submit(), compressed packets come out of receive().
// Polled and non-blocking, like IVideoDecoder; a codec with reordering or rate control may hold pictures
// back until flush().
class IVideoEncoder {
 public:
  virtual VideoCodec codec() const = 0;
  // Takes one picture, which must be in the session's input format and size. The encoder copies what it
  // needs before returning.
  virtual SubmitResult submit(const VideoFrame& frame) = 0;
  // The next compressed packet. `*packet` points into memory the encoder owns, valid until the next call
  // on the encoder. False if nothing is ready.
  virtual bool receive(VideoPacket* packet) = 0;
  // No more pictures: whatever is held back is encoded and becomes available.
  virtual void flush() = 0;
  virtual VideoCodecStatistics statistics() const = 0;

 protected:
  constexpr IVideoEncoder() = default;
  ~IVideoEncoder() = default;
};

}  // namespace hardware
