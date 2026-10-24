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

namespace av1 {
class Core;
class IOffload;
class IFrameObserver;
}

// An AV1 decoder on the CPU: the Main, High and Professional profiles (8-, 10- and 12-bit; 4:0:0, 4:2:0, 4:2:2 and 4:4:4), every coding tool
// of the specification (all block sizes and partitions, intra and inter prediction with compound, OBMC, warped and global motion, palette,
// intra block copy, CfL, filter-intra, switchable transforms and quantiser matrices, segmentation, delta q/lf, tiles, super-resolution,
// loop filter, CDEF, loop restoration, film grain, scalable streams at operating point 0). A packet is a temporal unit as the low-overhead
// bitstream format stores it (what MP4, WebM and IVF carry). Pictures come out as 8-bit I420 (deeper samples are rounded down, 4:4:4 and
// 4:2:2 chroma is averaged down, monochrome pictures get neutral chroma), at most one per packet (the last shown frame of the unit).
class Av1Decoder {
 public:
  explicit Av1Decoder(allocators::IAllocator& allocator) : allocator_(allocator), out_(allocator) {}
  ~Av1Decoder();
  Av1Decoder(const Av1Decoder&) = delete;
  Av1Decoder& operator=(const Av1Decoder&) = delete;

  // Feeds the configuration OBUs of an MP4 'av1C' or Matroska CodecPrivate box (its four byte header is skipped); they carry the sequence
  // header for streams that do not repeat it. False if the box is damaged.
  bool configure(ConstByteView av1c);
  // Decodes one temporal unit. False if it holds data that cannot be decoded (the decoder then waits for the next key frame).
  bool decode(ConstByteView packet, int64_t pts_us);
  // The picture of the last packet, once. The planes stay valid until the next call on the decoder.
  bool receive(VideoFrame* frame);
  void flush() {}
  // Forget everything and wait for a key frame (after a seek).
  void reset();
  // The size of the pictures; zero until the first frame was decoded.
  VideoFormat output_format() const { return format_; }
  bool ready() const { return pending_; }
  // Hands the pixel work to something else (a hardware engine, see Av1Core.hpp): this decoder still parses every OBU and keeps the reference slots and the output, but
  // the frames `offload` accepts are decoded by it. Set before decoding.
  void set_offload(av1::IOffload* offload);
  // For diagnostics and tests: the observer is told of every frame this decoder decodes itself (see Av1Core.hpp).
  void set_frame_observer(av1::IFrameObserver* observer);

 private:
  bool convert();

  allocators::IAllocator& allocator_;
  UniquePointer<av1::Core> core_;
  VideoFrameBuffer out_;
  VideoFormat format_;
  int64_t pts_ = 0;
  bool pending_ = false, keyframe_ = true, limited_range_ = true;
};

}  // namespace hardware
