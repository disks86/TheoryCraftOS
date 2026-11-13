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

// Encoded video turned into pictures, for a watcher that takes raw video (a program's raw video input, the desktop's Video Viewer) of a producer that
// sends compressed frames. Motion JPEG is decoded here (hardware::JpegDecoder: webcams leave out its Huffman tables); H.264, H.265, MPEG-2, VC-1, VP8
// and VP9 by a decoder session of the machine's video codecs (IVideoDecoders: the kernel's SystemVideoCodecs, a test's SoftwareVideoCodec), so the
// graph does not carry the codecs itself. Every buffer is one compressed picture (an access unit; H.264 and H.265 in Annex B byte-stream form, as
// PipeWire producers send them). A codec that reorders gives its pictures out later than their buffers: each call hands out the newest picture
// ready.
#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/VideoChannel.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

// SPA_MEDIA_SUBTYPE_* of the encoded video formats
constexpr uint32_t kSubtypeH264 = 0x20001, kSubtypeMjpg = 0x20002, kSubtypeMpeg2 = 0x20007, kSubtypeVc1 = 0x2000a, kSubtypeVp8 = 0x2000b,
                   kSubtypeVp9 = 0x2000c, kSubtypeH265 = 0x2000e;

// Where decoder sessions come from.
class IVideoDecoders {
 public:
  // Whether create_decoder() can serve `codec` at all.
  virtual bool can_decode(hardware::VideoCodec codec) const = 0;
  // A new session; empty when the codec cannot be decoded or memory ran out.
  virtual UniquePointer<hardware::IVideoDecoder> create_decoder(const hardware::DecoderConfig& config) = 0;

 protected:
  ~IVideoDecoders() = default;
};

// The codec of an encoded subtype (false: not one decoded through IVideoDecoders; Motion JPEG is not).
bool codec_of_subtype(uint32_t subtype, hardware::VideoCodec* out);
// Whether frames of `subtype` can be decoded: Motion JPEG always, the others when `decoders` serves their codec.
bool can_decode_subtype(uint32_t subtype, const IVideoDecoders* decoders);

// One stream's decoding: its decoder (made at the first frame, made again when the subtype changes) and the newest picture.
class VideoDecodeSession {
 public:
  explicit VideoDecodeSession(allocators::IAllocator& allocator) : allocator_(allocator), words_(allocator), picture_(allocator) {}
  VideoDecodeSession(const VideoDecodeSession&) = delete;
  VideoDecodeSession& operator=(const VideoDecodeSession&) = delete;

  // Gives the decoder the frame `data` of `subtype`. True when a picture is ready: *format becomes BGRA (SPA format 12) at the picture's size and
  // *pixels its bytes (width * 4 a row, no padding), valid until the next call. False when the frame cannot be decoded or no picture is ready yet
  // (*failed says which).
  bool decode(uint32_t subtype, ConstByteView data, IVideoDecoders* decoders, VideoFormat* format, ConstByteView* pixels, bool* failed);
  // Forgets the decoder (another stream follows).
  void reset() { subtype_ = 0; jpeg_ = UniquePointer<hardware::JpegDecoder>(); decoder_ = UniquePointer<hardware::IVideoDecoder>(); width_ = height_ = 0; }
  // Frames given that could not be decoded.
  uint64_t errors() const { return errors_; }

 private:
  bool take_picture(const hardware::VideoFrame& frame);

  allocators::IAllocator& allocator_;
  uint32_t subtype_ = 0;
  UniquePointer<hardware::JpegDecoder> jpeg_;
  List<uint32_t> words_;      // (Motion JPEG: the picture as 0xAARRGGBB)
  UniquePointer<hardware::IVideoDecoder> decoder_;
  ByteBuffer picture_;        // (the others: the newest picture, rows packed)
  uint32_t width_ = 0, height_ = 0;
  int64_t next_pts_us_ = 0;
  uint64_t errors_ = 0;
};

}  // namespace pipewire
