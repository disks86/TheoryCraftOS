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

// Encoded video turned into pictures (and raw video into encoded, VideoEncodeSession), for a watcher that takes raw video (a program's raw video input, the desktop's Video Viewer) of a producer that
// sends compressed frames. Motion JPEG is decoded here (hardware::JpegDecoder: webcams leave out its Huffman tables); H.264, H.265, MPEG-2, VC-1, VP8
// and VP9 by a decoder session of the machine's video codecs (IVideoCodecs: the kernel's SystemVideoCodecs, a test's SoftwareVideoCodec), so the
// graph does not carry the codecs itself. Every buffer is one compressed picture (an access unit; H.264 and H.265 in Annex B byte-stream form, as
// PipeWire producers send them). A codec that reorders gives its pictures out later than their buffers: each call hands out the newest picture
// ready.
#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/IVideoEncoder.hpp"
#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/VideoChannel.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

// SPA_MEDIA_SUBTYPE_* of the encoded video formats
constexpr uint32_t kSubtypeH264 = 0x20001, kSubtypeMjpg = 0x20002, kSubtypeH263 = 0x20005, kSubtypeMpeg2 = 0x20007, kSubtypeMpeg4 = 0x20008, kSubtypeXvid = 0x20009,
                   kSubtypeVc1 = 0x2000a, kSubtypeVp8 = 0x2000b, kSubtypeVp9 = 0x2000c, kSubtypeH265 = 0x2000e;

// Where decoder (and encoder) sessions come from: the machine's video codecs.
class IVideoCodecs {
 public:
  // Whether create_decoder() can serve `codec` at all.
  virtual bool can_decode(hardware::VideoCodec codec) const = 0;
  // A new session; empty when the codec cannot be decoded or memory ran out.
  virtual UniquePointer<hardware::IVideoDecoder> create_decoder(const hardware::DecoderConfig& config) = 0;
  // Encoder sessions, for a watcher that takes a raw producer's video compressed (none unless given).
  virtual bool can_encode(hardware::VideoCodec codec) const {
    (void)codec;
    return false;
  }
  virtual UniquePointer<hardware::IVideoEncoder> create_encoder(const hardware::EncoderConfig& config) {
    (void)config;
    return UniquePointer<hardware::IVideoEncoder>();
  }

 protected:
  ~IVideoCodecs() = default;
};

// The codec of an encoded subtype (false: not one decoded through IVideoCodecs; Motion JPEG is not).
bool codec_of_subtype(uint32_t subtype, hardware::VideoCodec* out);
// Whether frames of `subtype` can be decoded: Motion JPEG always, the others when `decoders` serves their codec.
bool can_decode_subtype(uint32_t subtype, const IVideoCodecs* decoders);
// Whether raw frames can be made `subtype` (Motion JPEG, H.264, ...): when `codecs` encodes its codec.
bool can_encode_subtype(uint32_t subtype, const IVideoCodecs* codecs);
// The size of the pictures in a compressed frame (a producer that did not say it): what a decoder finds in it. False when it cannot tell from this one.
bool picture_size(uint32_t subtype, ConstByteView frame, IVideoCodecs* codecs, allocators::IAllocator& allocator, uint32_t* width, uint32_t* height);

// One stream's decoding: its decoder (made at the first frame, made again when the subtype changes) and the newest picture.
class VideoDecodeSession {
 public:
  explicit VideoDecodeSession(allocators::IAllocator& allocator) : allocator_(allocator), words_(allocator), picture_(allocator) {}
  VideoDecodeSession(const VideoDecodeSession&) = delete;
  VideoDecodeSession& operator=(const VideoDecodeSession&) = delete;

  // Gives the decoder the frame `data` of `subtype`. True when a picture is ready: *format becomes BGRA (SPA format 12) at the picture's size and
  // *pixels its bytes (width * 4 a row, no padding), valid until the next call. False when the frame cannot be decoded or no picture is ready yet
  // (*failed says which).
  bool decode(uint32_t subtype, ConstByteView data, IVideoCodecs* decoders, VideoFormat* format, ConstByteView* pixels, bool* failed);
  // Forgets the decoder (another stream follows).
  void reset() { subtype_ = 0; jpeg_ = UniquePointer<hardware::JpegDecoder>(); decoder_ = UniquePointer<hardware::IVideoDecoder>(); width_ = height_ = 0; }
  // Frames given that could not be decoded.
  uint64_t errors() const { return errors_; }
  // The pictures' size as far as it is known (the last picture's, else what the decoder found in the stream's headers).
  bool size(uint32_t* width, uint32_t* height) const;

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

// One stream's encoding (a raw producer for a watcher that takes its video compressed): its encoder, made at the first frame for the watcher's codec
// and size, and the newest compressed frame.
class VideoEncodeSession {
 public:
  explicit VideoEncodeSession(allocators::IAllocator& allocator) : allocator_(allocator), bgra_(allocator), packet_(allocator) {}
  VideoEncodeSession(const VideoEncodeSession&) = delete;
  VideoEncodeSession& operator=(const VideoEncodeSession&) = delete;
  // The raw frame `frame` of `from` as one compressed frame of `to` (its subtype, size and rate): *out its bytes, valid until the next call. False when
  // it cannot be encoded or the encoder holds it back.
  bool encode(const VideoFormat& from, ConstByteView frame, const VideoFormat& to, IVideoCodecs* codecs, ConstByteView* out);
  uint64_t frames() const { return frames_; }

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<hardware::IVideoEncoder> encoder_;
  VideoFormat made_for_;
  ByteBuffer bgra_, packet_;
  uint64_t frames_ = 0;
};

}  // namespace pipewire
