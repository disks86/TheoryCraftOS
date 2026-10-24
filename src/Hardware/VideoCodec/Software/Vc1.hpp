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


// A VC-1 video decoder on the CPU (SMPTE 421M): Simple and Main profile (WMV9) and Advanced profile bitstreams of progressive 4:2:0 frames - I, P, B and BI pictures,
// 1MV and 4MV prediction, quarter and half sample motion, intensity compensation, all four transform sizes, bit planes, quantizer variations, overlap smoothing and the
// in-loop filter. Not covered: interlaced content (field and frame coded pictures), range reduction and multiple resolution coding, sprites, pan-scan windows and
// the Complex profile; streams that use one of them are refused. The pictures are bit exact with the reference decoders.
//
// Input: for Simple and Main profile configure() is given the 4 byte sequence header (STRUCT_C of a Windows Media video track) and the picture size, and every decode()
// is one frame as it is stored in the container. Advanced profile is the elementary stream with start codes (sequence header 0F, entry point 0E, frame 0D, slice 0B);
// decode() takes one frame with the headers in front of it, or a frame without any start code.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace vc1 {
class Core;
class IOffload;
class IFrameObserver;
}

class Vc1Decoder {
 public:
  explicit Vc1Decoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Vc1Decoder();
  Vc1Decoder(const Vc1Decoder&) = delete;
  Vc1Decoder& operator=(const Vc1Decoder&) = delete;

  // Simple and Main profile: the sequence header bytes and the coded size. An Advanced profile stream needs no configuration (and `extradata` may then hold the
  // sequence header and entry point units, with start codes). False if the header is not one this decoder takes.
  bool configure(ConstByteView extradata, uint32_t width, uint32_t height);
  // Decodes one frame (see above). False if nothing in it could be decoded.
  bool decode(ConstByteView data, int64_t pts_us);
  // The next picture in display order, as I420 (cropped). The planes stay valid until the next call on the decoder.
  bool receive(VideoFrame* frame);
  // The stream ends: the picture held for reordering becomes available.
  void flush();
  // Drops everything and waits for the next intra picture (after a seek).
  void reset();
  VideoFormat output_format() const;
  bool ready() const;  // a picture can be received

  // Hands the pixel work to something else (a hardware engine, see Vc1Core.hpp): this decoder still reads every header, keeps the reference pictures and the output
  // order, but the picture data goes to `offload`. Set before decoding.
  void set_offload(vc1::IOffload* offload);
  // For diagnostics and tests: told of every picture this decoder decodes itself (see Vc1Core.hpp).
  void set_frame_observer(vc1::IFrameObserver* observer);

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<vc1::Core> core_;
};

}  // namespace hardware
