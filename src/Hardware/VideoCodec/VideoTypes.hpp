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
#include "Utility/ArrayView.hpp"

namespace hardware {

// What a video stream is compressed with. Raw is uncompressed frames in one of the PixelFormats.
enum class VideoCodec : uint8_t { Raw, Mjpeg, H264, Hevc, Vp9, Av1, Vp8, Mpeg2, Vc1, Mpeg4 };   // Mpeg4: MPEG-4 Part 2 (Simple and Advanced Simple Profile; DivX, Xvid), not AVC
constexpr uint32_t kVideoCodecCount = 10;
constexpr uint32_t codec_mask(VideoCodec codec) { return 1u << static_cast<uint8_t>(codec); }
const char* codec_name(VideoCodec codec);

// How the pixels of a decoded (or to be encoded) frame are laid out.
enum class PixelFormat : uint8_t {
  Bgra8888,  // one plane, 4 bytes per pixel: blue, green, red, alpha (0xAARRGGBB as a little-endian word; what graphics::Surface holds)
  I420,      // three planes: Y (full size), then U and V (half width and height, rounded up)
  Nv12,      // two planes: Y (full size), then U and V interleaved (half height, rounded up)
  P010,      // NV12 with 10-bit samples: every sample is a 16-bit little-endian word holding the value in its top ten bits (value << 6); strides count bytes
};

// Sizes and planes of a frame in a given format. Rows are tight: a plane's stride is its width in bytes.
struct VideoFormat {
  static constexpr uint32_t kMaxDimension = 16384;

  uint32_t width = 0, height = 0;
  PixelFormat pixel_format = PixelFormat::Bgra8888;

  bool valid() const { return width >= 1 && height >= 1 && width <= kMaxDimension && height <= kMaxDimension; }
  uint32_t plane_count() const {
    return pixel_format == PixelFormat::Bgra8888 ? 1 : pixel_format == PixelFormat::I420 ? 3 : 2;
  }
  uint32_t chroma_width() const { return (width + 1) / 2; }
  uint32_t chroma_height() const { return (height + 1) / 2; }
  uint32_t plane_width(uint32_t plane) const;   // in samples (pixels for Bgra8888)
  uint32_t plane_height(uint32_t plane) const;  // in rows
  uint32_t plane_stride(uint32_t plane) const;  // in bytes
  uint32_t plane_bytes(uint32_t plane) const { return plane_stride(plane) * plane_height(plane); }
  uint64_t frame_bytes() const;
  bool operator==(const VideoFormat& o) const { return width == o.width && height == o.height && pixel_format == o.pixel_format; }
  bool operator!=(const VideoFormat& o) const { return !(*this == o); }
};

// One decoded picture: a view onto planes owned by whoever produced it (a decoder keeps it valid
// until its next call; a VideoFrameBuffer until it changes).
struct VideoFrame {
  VideoFormat format;
  const uint8_t* plane[3] = {nullptr, nullptr, nullptr};
  uint32_t stride[3] = {0, 0, 0};
  int64_t pts_us = 0;      // presentation time in microseconds from the start of the stream
  bool keyframe = true;
  // YUV pictures: false = full range (Y, U, V all 0..255, as JPEG), true = limited range (Y 16..235, U and V
  // 16..240, as most video). The matrix is always BT.601. Ignored for Bgra8888.
  bool limited_range = false;
};

// One compressed picture (an access unit) as stored in a file or sent to a decoder, or produced by an encoder.
struct VideoPacket {
  ConstByteView data;
  int64_t pts_us = 0;
  bool keyframe = true;  // can be decoded without earlier packets
};

struct DecoderConfig {
  VideoCodec codec = VideoCodec::Mjpeg;
  uint32_t width = 0, height = 0;  // what the container says; 0 if unknown (the stream's own headers win)
  PixelFormat output = PixelFormat::Bgra8888;  // what receive() should deliver
  PixelFormat raw_format = PixelFormat::I420;  // for VideoCodec::Raw: how the packets are laid out (width and height are needed)
  // For VideoCodec::H264: the avcC record (ISO 14496-15) of an MP4 or AVI file, which also says the packets hold
  // length-prefixed NAL units. Empty for an Annex B stream (start codes, parameter sets in the stream).
  ConstByteView codec_config;
};

struct EncoderConfig {
  VideoCodec codec = VideoCodec::Mjpeg;
  uint32_t width = 0, height = 0;
  PixelFormat input = PixelFormat::Bgra8888;  // what submit() will be given
  uint32_t fps_numerator = 30, fps_denominator = 1;
  uint32_t quality = 75;           // 1 .. 100; higher is better and bigger
  uint32_t keyframe_interval = 1;  // a keyframe every this many frames (codecs that only have keyframes ignore it)
};

enum class SubmitResult : uint8_t {
  Accepted,
  Full,     // the codec still holds output: receive() it first
  Invalid,  // not a decodable packet, or a frame that does not match the configuration
};

struct VideoCodecStatistics {
  uint64_t packets = 0;  // compressed pictures taken in (decoder) or given out (encoder)
  uint64_t frames = 0;   // decoded pictures given out (decoder) or taken in (encoder)
  uint64_t bytes = 0;    // compressed bytes
  uint64_t errors = 0;   // packets or frames refused or undecodable
};

}  // namespace hardware
