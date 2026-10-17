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
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace h264 {
class Core;
}

// An H.264 / AVC decoder on the CPU: Baseline, Main and High profile video (8-bit 4:2:0, progressive frames, CAVLC or
// CABAC, I/P/B slices, multiple reference frames, weighted prediction, 8x8 transform and scaling matrices, multiple
// slices, deblocking). Not covered: interlaced coding (field pictures and MBAFF), flexible macroblock ordering,
// data partitioning, SP/SI slices, high bit depths and 4:2:2 / 4:4:4. Pictures come out in display order.
//
// Input is either Annex B (start codes, with SPS and PPS in the stream) or - after configure() with an avcC record,
// as stored in MP4 and AVI files - length-prefixed NAL units.
class H264Decoder {
 public:
  explicit H264Decoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~H264Decoder();
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  // Takes the parameter sets out of an avcC record (ISO 14496-15) and switches the input to length-prefixed NAL units.
  bool configure(ConstByteView avcc);
  // Decodes one access unit (all NAL units of a picture). False if the data holds nothing decodable.
  bool decode(ConstByteView access_unit, int64_t pts_us);
  // The next picture in display order, as I420 (cropped). The planes stay valid until the next call on the decoder.
  bool receive(VideoFrame* frame);
  // The stream ends: pictures held for reordering become available.
  void flush();
  // Drops everything and waits for an IDR picture (after a seek).
  void reset();
  // The size of the pictures; zero until a sequence parameter set was seen.
  VideoFormat output_format() const;
  bool ready() const;  // a picture can be received

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<h264::Core> core_;
};

}  // namespace hardware
