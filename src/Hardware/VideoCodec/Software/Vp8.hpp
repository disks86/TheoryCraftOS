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
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace vp8 {
class Core;
}

// A VP8 decoder on the CPU (RFC 6386): all four versions (the 6-tap and bilinear predictors, full-sample chroma vectors, the normal and simple
// loop filters), key and inter frames with golden and alternate references and their sign biases, split motion vectors, segmentation, up to
// eight token partitions, entropy contexts that are kept or dropped after a frame, hidden frames, and pictures of any size (the scaling bits
// of key frames are only a display hint and are ignored). A packet is what a WebM, IVF or AVI file stores: one frame. A packet with no bytes
// repeats the last picture. Pictures come out as 8-bit limited-range I420, at most one per packet (none for a hidden frame).
class Vp8Decoder {
 public:
  explicit Vp8Decoder(allocators::IAllocator& allocator) : allocator_(allocator), out_(allocator) {}
  ~Vp8Decoder();
  Vp8Decoder(const Vp8Decoder&) = delete;
  Vp8Decoder& operator=(const Vp8Decoder&) = delete;

  // Decodes one packet. False if it cannot be decoded (the decoder then waits for the next key frame).
  bool decode(ConstByteView packet, int64_t pts_us);
  // The picture of the last packet, once. The planes stay valid until the next call on the decoder.
  bool receive(VideoFrame* frame);
  void flush() {}
  // Forget everything and wait for a key frame (after a seek).
  void reset();
  // The size of the pictures; zero until the first frame was decoded.
  VideoFormat output_format() const { return format_; }
  bool ready() const { return pending_; }

 private:
  bool convert();

  allocators::IAllocator& allocator_;
  UniquePointer<vp8::Core> core_;
  VideoFrameBuffer out_;
  VideoFormat format_;
  int64_t pts_ = 0;
  bool pending_ = false, keyframe_ = true, has_picture_ = false;
};

}  // namespace hardware
