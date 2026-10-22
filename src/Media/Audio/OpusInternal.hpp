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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

namespace opus {
class Decoder;
}

// An Opus decoder (RFC 6716): SILK, CELT and hybrid frames, mono and stereo, 2.5 to 60 ms frames and the code 0-3 packet layouts,
// always at 48 kHz. See Opus.cpp for what it covers. Ported from the reference decoder (libopus, BSD-3-Clause).
class OpusDecoder {
 public:
  explicit OpusDecoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~OpusDecoder();
  OpusDecoder(const OpusDecoder&) = delete;
  OpusDecoder& operator=(const OpusDecoder&) = delete;

  static constexpr uint32_t kSampleRate = 48000;
  static constexpr uint32_t kMaxFrames = 5760;  // 120 ms: the most one packet can hold

  // `channels` is 1 or 2: how many the output has (a stream coded in the other layout is converted).
  bool init(uint32_t channels);
  // Forgets the signal history (after a seek).
  void reset();
  // Decodes one packet into `pcm` (interleaved 16-bit samples, room for kMaxFrames * channels). Returns the number of frames per
  // channel, or -1 if the packet is not valid. An empty packet or a one-byte DTX packet produces silence of the previous length.
  int decode(ConstByteView packet, ArrayView<int16_t> pcm);
  uint32_t channels() const;

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<opus::Decoder> impl_;
};

// The packet structure (RFC 6716 section 3): how many frames a packet holds and of what size. False if it is malformed.
struct OpusPacketInfo {
  uint8_t toc = 0;
  uint32_t frame_count = 0;
  uint32_t frame_size[48] = {};    // bytes of each frame
  uint32_t frame_offset[48] = {};  // where each frame starts in the packet
  uint32_t samples_per_frame = 0;  // at 48 kHz
  uint32_t channels = 1;           // as coded
  uint32_t bandwidth = 0;          // 0 narrow .. 4 full (OPUS_BANDWIDTH_NARROWBAND = 1101 is not used here)
  uint32_t mode = 0;               // 0 SILK, 1 hybrid, 2 CELT
};
bool parse_opus_packet(ConstByteView packet, OpusPacketInfo* info);

}  // namespace media
