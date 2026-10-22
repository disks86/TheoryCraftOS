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
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace h264 {
class Encoder;
}

// An H.264 / AVC encoder on the CPU: Baseline profile (CAVLC, progressive, one slice per picture, one reference
// frame), I and P pictures. Intra macroblocks are Intra16x16 (DC, vertical, horizontal); inter macroblocks are
// 16x16 with whole-pixel motion found by a diamond search, or skipped. Quantiser is fixed per stream, taken from
// the quality (1 .. 100). The in-loop deblocking filter is switched off in the stream, so what the encoder
// reconstructs is exactly what any decoder shows.
//
// Pictures go in as full-range I420 (what convert_frame() produces); the stream says so in its VUI. Packets come
// out as Annex B access units: keyframes carry the SPS and PPS in front of the IDR slice.
class H264Encoder {
 public:
  explicit H264Encoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~H264Encoder();
  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;

  // Width and height must be even. quality 1 .. 100; keyframe_interval 0 or 1 means every picture is a keyframe.
  bool configure(uint32_t width, uint32_t height, uint32_t fps_numerator, uint32_t fps_denominator, uint32_t quality,
                 uint32_t keyframe_interval);
  // Encodes one picture and appends its access unit to `out`. `i420` must match the configured size.
  bool encode(const VideoFrame& i420, bool force_keyframe, ByteBuffer* out, bool* keyframe);
  // The encoder's own reconstruction of the picture it coded last (cropped view, valid until the next encode()).
  bool reconstruction(VideoFrame* frame) const;
  // The avcC record (ISO 14496-15) for the stream, for an MP4 or AVI muxer.
  bool avcc(ByteBuffer* out) const;

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<h264::Encoder> core_;
};

}  // namespace hardware
