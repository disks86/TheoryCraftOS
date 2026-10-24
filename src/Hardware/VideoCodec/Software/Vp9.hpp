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

namespace vp9 {
class Core;
class IOffload;
class IFrameObserver;
}

// A VP9 decoder on the CPU: profiles 0 to 3 (8-, 10- and 12-bit; 4:2:0, 4:2:2, 4:4:0 and 4:4:4), key, intra-only and inter frames with
// compound prediction, scaled references, segmentation, tiles, lossless mode, hidden frames, show-existing-frame and superframes. A packet
// is what a WebM or IVF file stores: one frame or a superframe. Pictures come out as 8-bit I420 (deeper samples are rounded down, chroma
// that is not 4:2:0 is averaged down), at most one per packet (the last shown frame of a superframe).
class Vp9Decoder {
 public:
  explicit Vp9Decoder(allocators::IAllocator& allocator) : allocator_(allocator), out_(allocator) {}
  ~Vp9Decoder();
  Vp9Decoder(const Vp9Decoder&) = delete;
  Vp9Decoder& operator=(const Vp9Decoder&) = delete;

  // Decodes one packet. False if it holds a frame that cannot be decoded (the decoder then waits for the next key frame).
  bool decode(ConstByteView packet, int64_t pts_us);
  // The picture of the last packet, once. The planes stay valid until the next call on the decoder.
  bool receive(VideoFrame* frame);
  void flush() {}
  // Forget everything and wait for a key frame (after a seek).
  void reset();
  // The size of the pictures; zero until the first frame was decoded.
  VideoFormat output_format() const { return format_; }
  bool ready() const { return pending_; }
  // Hands the pixel work to something else (a hardware engine, see Vp9Core.hpp): this decoder still reads every uncompressed header, keeps the reference slots and the
  // output, but the frames of the streams `offload` accepts are decoded by it. Set before decoding.
  void set_offload(vp9::IOffload* offload);
  // For diagnostics and tests: told of every frame this decoder decodes itself (see Vp9Core.hpp).
  void set_frame_observer(vp9::IFrameObserver* observer);

 private:
  bool convert(int index);

  allocators::IAllocator& allocator_;
  UniquePointer<vp9::Core> core_;
  VideoFrameBuffer out_;
  VideoFormat format_;
  int64_t pts_ = 0;
  bool pending_ = false, keyframe_ = true, limited_range_ = true;
};

}  // namespace hardware
