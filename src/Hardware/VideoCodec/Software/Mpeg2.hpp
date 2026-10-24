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


// An MPEG-2 video decoder on the CPU (ISO/IEC 13818-2): Main and High profile 4:2:0 video as it is stored in an elementary stream, a program stream or a transport
// stream - frame pictures and field pictures, intra, predicted and bidirectionally predicted pictures, frame and field macroblock prediction (16x16, 16x8, field and
// dual prime), frame and field DCT, both scans and both quantizer scale types, quantizer matrices, intra DC precision of 8 to 11 bits, slices and skipped macroblocks.
// Not covered: MPEG-1 streams (those without a sequence extension), the 4:2:2 and 4:4:4 profiles, scalability and data partitioning. Pictures come out in display order,
// cropped to the sequence's size, as I420.
//
// The inverse transform is the integer one of the reference decoders of the standard (Chen and Wang's), which meets the accuracy requirements of IEEE 1180: another
// decoder's pictures differ from this one's by a few levels in places, and the difference does not accumulate beyond a picture group.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace mpeg2 {
class Core;
class IOffload;
class IFrameObserver;
}

class Mpeg2Decoder {
 public:
  explicit Mpeg2Decoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Mpeg2Decoder();
  Mpeg2Decoder(const Mpeg2Decoder&) = delete;
  Mpeg2Decoder& operator=(const Mpeg2Decoder&) = delete;

  // Decodes the pictures in `data`: any number of whole start-code units (a picture, or the pieces of one in several calls: a picture is complete when the next one
  // starts or the stream ends). False if nothing in it could be decoded.
  bool decode(ConstByteView data, int64_t pts_us);
  // The next picture in display order, as I420 (cropped). The planes stay valid until the next call on the decoder.
  bool receive(VideoFrame* frame);
  // The stream ends: the pictures held for reordering become available.
  void flush();
  // Drops everything and waits for the next sequence header and intra picture (after a seek).
  void reset();
  // The size of the pictures; zero until a sequence header was seen.
  VideoFormat output_format() const;
  bool ready() const;  // a picture can be received

  // Hands the pixel work to something else (a hardware engine, see Mpeg2Core.hpp): this decoder still reads every header, keeps the reference pictures and the output
  // order, but the slices go to `offload`. Set before decoding.
  void set_offload(mpeg2::IOffload* offload);
  // For diagnostics and tests: told of every picture this decoder decodes itself (see Mpeg2Core.hpp).
  void set_frame_observer(mpeg2::IFrameObserver* observer);

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<mpeg2::Core> core_;
};

}  // namespace hardware
