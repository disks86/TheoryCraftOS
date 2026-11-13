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

#include "PipeWire/VideoChannel.hpp"

namespace pipewire {

namespace {
// SPA_VIDEO_FORMAT_*
enum : uint32_t {
  kI420 = 2, kYV12 = 3, kYUY2 = 4, kUYVY = 5, kAYUV = 6, kRGBx = 7, kABGR = 14, kRGB = 15, kBGR = 16, kY41B = 17, kY42B = 18, kYVYU = 19,
  kY444 = 20, kNV12 = 23, kNV21 = 24, kGRAY8 = 25, kGRAY16_BE = 26, kGRAY16_LE = 27, kRGB16 = 29, kBGR15 = 32, kARGB64 = 39, kAYUV64 = 40,
  kI420_10LE = 43, kI422_10LE = 45, kY444_10LE = 47, kNV16 = 51, kNV24 = 52, kNV61 = 60, kP010_10LE = 62, kVYUY = 64, kI420_12LE = 73,
  kRGBA_F16 = 78, kRGBA_F32 = 79, kxRGB_210LE = 80, kBGRA_102LE = 87
};
uint32_t half(uint32_t v) { return (v + 1) / 2; }
}  // namespace

bool is_encoded_video_subtype(uint32_t subtype) {
  // SPA_MEDIA_SUBTYPE_h264 0x20001 .. h265 0x2000E, but not bayer (0x2000D: raw sensor data, not carried)
  return subtype >= 0x20001 && subtype <= 0x2000E && subtype != 0x2000D;
}

uint32_t VideoFormat::planes() const {
  if (encoded()) return 0;
  switch (format) {
    case kI420: case kYV12: case kY41B: case kY42B: case kY444: case kI420_10LE: case kI422_10LE: case kY444_10LE: case kI420_12LE: return 3;
    case kNV12: case kNV21: case kNV16: case kNV24: case kNV61: case kP010_10LE: return 2;
    case kYUY2: case kUYVY: case kYVYU: case kVYUY: case kAYUV: case kGRAY8: case kGRAY16_BE: case kGRAY16_LE: case kRGB: case kBGR: case kARGB64: case kAYUV64:
    case kRGBA_F16: case kRGBA_F32: return 1;
    default:
      return (format >= kRGBx && format <= kABGR) || (format >= kRGB16 && format <= kBGR15) || (format >= kxRGB_210LE && format <= kBGRA_102LE) ? 1 : 0;
  }
}

VideoPlane VideoFormat::plane(uint32_t index) const {
  const uint32_t n = planes();
  if (index >= n) return VideoPlane();
  if (index == 0) {
    switch (format) {
      case kYUY2: case kUYVY: case kYVYU: case kVYUY: return VideoPlane{half(width) * 4, height, 1, 1};      // (two pixels in four bytes)
      case kAYUV: return VideoPlane{width * 4, height, 1, 1};
      case kRGB: case kBGR: return VideoPlane{width * 3, height, 1, 1};
      case kGRAY16_BE: case kGRAY16_LE: return VideoPlane{width * 2, height, 1, 1};
      case kARGB64: case kAYUV64: case kRGBA_F16: return VideoPlane{width * 8, height, 1, 1};
      case kRGBA_F32: return VideoPlane{width * 16, height, 1, 1};
      case kI420_10LE: case kI422_10LE: case kY444_10LE: case kI420_12LE: case kP010_10LE: return VideoPlane{width * 2, height, 1, 1};      // (16-bit samples)
      case kI420: case kYV12: case kY41B: case kY42B: case kY444: case kNV12: case kNV21: case kNV16: case kNV24: case kNV61: case kGRAY8:
        return VideoPlane{width, height, 1, 1};
      default: return VideoPlane{(format >= kRGB16 && format <= kBGR15 ? 2u : 4u) * width, height, 1, 1};
    }
  }
  switch (format) {
    case kI420: case kYV12: return VideoPlane{half(width), half(height), 1, 2};
    case kI420_10LE: case kI420_12LE: return VideoPlane{half(width) * 2, half(height), 1, 2};
    case kY42B: return VideoPlane{half(width), height, 1, 2};
    case kI422_10LE: return VideoPlane{half(width) * 2, height, 1, 2};
    case kY41B: return VideoPlane{(width + 3) / 4, height, 1, 4};
    case kY444: return VideoPlane{width, height, 1, 1};
    case kY444_10LE: return VideoPlane{width * 2, height, 1, 1};
    case kNV16: case kNV61: return VideoPlane{half(width) * 2, height, 1, 1};      // (U and V side by side, every row)
    case kNV24: return VideoPlane{width * 2, height, 2, 1};
    case kP010_10LE: return VideoPlane{half(width) * 4, half(height), 1, 1};
    default: return VideoPlane{half(width) * 2, half(height), 1, 1};      // NV12, NV21: U and V side by side
  }
}

uint64_t VideoFormat::plane_offset(uint32_t index) const {
  uint64_t at = 0;
  for (uint32_t i = 0; i < index && i < planes(); ++i) at += uint64_t{plane(i).row_bytes} * plane(i).rows;
  return at;
}

uint64_t VideoFormat::frame_bytes() const {
  if (!valid() || encoded()) return 0;
  return plane_offset(planes());
}

bool VideoFormat::valid() const {
  if (width > kMaxSide || height > kMaxSide) return false;
  if (encoded()) return is_encoded_video_subtype(subtype);
  return (subtype == kSubtypeRaw || (subtype == kSubtypeDsp && format == kRGBA_F32)) && planes() != 0 && width >= 1 && height >= 1;
}

void VideoChannel::set_format(const VideoFormat& format) {
  kernel::ScopedMutex guard(mutex_);
  format_ = format;
}

VideoFormat VideoChannel::format() {
  kernel::ScopedMutex guard(mutex_);
  return format_;
}

bool VideoChannel::publish(ArrayView<const VideoPlaneData> planes, int64_t pts, const VideoFrame* shared) {
  VideoFormat format;
  uint64_t sequence = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    if (closed_ || !format_.valid()) return false;
    format = format_;
    sequence = frames_ + 1;
  }
  // the frame is made outside the lock (a copy of the rows), then put in place
  SharedPointer<VideoFrame> frame = SharedPointer<VideoFrame>::make(allocator_, allocator_);
  if (!frame) return false;
  if (format.encoded()) {
    if (planes.size() < 1 || planes[0].data.size() == 0 || planes[0].data.size() > VideoFormat::kMaxEncodedFrame) return false;
    if (!frame->pixels.append(planes[0].data)) return false;
  } else {
    const uint32_t n = format.planes();
    if (planes.size() < n || !frame->pixels.put_zeros(format.frame_bytes())) return false;
    ByteView out = frame->pixels.writable_view();
    for (uint32_t i = 0; i < n; ++i) {
      const VideoPlane pl = format.plane(i);
      const VideoPlaneData& in = planes[i];
      if (in.stride < pl.row_bytes) return false;
      const uint64_t needed = uint64_t{in.stride} * (pl.rows - 1) + pl.row_bytes;      // (the last row needs no padding after it)
      if (in.data.size() < needed) return false;
      const uint64_t base = format.plane_offset(i);
      for (uint32_t y = 0; y < pl.rows; ++y)
        out.subview(base + uint64_t{y} * pl.row_bytes, pl.row_bytes).copy_from(in.data.subview(uint64_t{y} * in.stride, pl.row_bytes));
    }
  }
  frame->format = format;
  frame->sequence = sequence;
  frame->pts = pts;
  if (shared) {
    frame->share = shared->share;
    frame->share_buffer = shared->share_buffer;
    for (uint32_t i = 0; i < VideoFormat::kMaxPlanes; ++i) frame->chunks[i] = shared->chunks[i];
  }
  kernel::ScopedMutex guard(mutex_);
  if (closed_ || !(format_ == format)) return false;      // (closed or changed meanwhile: this frame is of no use)
  frames_ = sequence;
  latest_ = util::move(frame);
  return true;
}

bool VideoChannel::publish_shared(SharedPointer<VideoFrame> frame) {
  if (!frame || !frame->share) return false;
  kernel::ScopedMutex guard(mutex_);
  if (closed_ || !format_.valid()) return false;
  frame->format = format_;
  frame->pixels.clear();
  frame->sequence = frames_ + 1;
  frames_ = frame->sequence;
  latest_ = util::move(frame);
  return true;
}

void VideoChannel::set_share(SharedPointer<VideoShare> share) {
  kernel::ScopedMutex guard(mutex_);
  share_ = util::move(share);
}

SharedPointer<VideoShare> VideoChannel::share() {
  kernel::ScopedMutex guard(mutex_);
  return share_;
}

SharedPointer<VideoFrame> VideoChannel::latest() {
  kernel::ScopedMutex guard(mutex_);
  return latest_;
}

uint64_t VideoChannel::frames() {
  kernel::ScopedMutex guard(mutex_);
  return frames_;
}

void VideoChannel::close() {
  kernel::ScopedMutex guard(mutex_);
  closed_ = true;
  share_ = SharedPointer<VideoShare>();
}

bool VideoChannel::closed() {
  kernel::ScopedMutex guard(mutex_);
  return closed_;
}

}  // namespace pipewire
