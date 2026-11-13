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


// An MPEG-4 Part 2 video decoder on the CPU (ISO/IEC 14496-2), which also decodes H.263 baseline (ITU-T H.263, the MPEG-4 short video header): the Simple and
// Advanced Simple profiles as DivX, Xvid and FFmpeg write them - I, P and B planes, the H.263 and the MPEG quantiser (with matrices), AC/DC prediction, four
// motion vectors per macroblock, quarter-sample motion, unrestricted motion vectors, video packets (resync markers), packed bitstreams (several planes in one
// packet), and interlaced video (field DCT, field motion vectors, field direct mode). Not covered: data partitioning and reversible VLCs, global motion compensation
// (S planes), shapes, scalability, studio profiles, and the optional modes of H.263 (Annexes D to T). Pictures come out in display order, cropped, as I420.
//
// The decoder follows FFmpeg's (which the encoders in use were tuned against) where the standard leaves room: the inverse transform is the integer "simple" IDCT
// that FFmpeg uses (another decoder's IDCT - Xvid's, say - differs by a level here and there, which does not accumulate beyond a picture group), the edges of a
// reference picture are its whole macroblocks, and the motion vector prediction, direct mode and quarter-sample filter round as FFmpeg does.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace mpeg4 {
class Core;
}

class Mpeg4Decoder {
 public:
  explicit Mpeg4Decoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Mpeg4Decoder();
  Mpeg4Decoder(const Mpeg4Decoder&) = delete;
  Mpeg4Decoder& operator=(const Mpeg4Decoder&) = delete;

  // Decodes what `data` holds: start-code units of MPEG-4 (a layer header, one or more planes) or one H.263 picture (starting with its picture start code). The
  // layer header may also come alone (an MP4 file's decoder configuration). False if nothing in it could be used.
  [[nodiscard]] bool decode(ConstByteView data, int64_t pts_us);
  // The next picture in display order, as I420 (cropped). The planes stay valid until the next call on the decoder.
  [[nodiscard]] bool receive(VideoFrame* frame);
  // The stream ends: the picture held for reordering becomes available.
  void flush();
  // Drops the pictures and waits for the next intra picture (after a seek). The layer header is kept.
  void reset();
  // The size of the pictures; zero until a layer or picture header was seen.
  VideoFormat output_format() const;
  bool ready() const;  // a picture can be received

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<mpeg4::Core> core_;
};

}  // namespace hardware
