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

uint32_t VideoFormat::pixel_bytes() const {
  switch (format) {
    case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: return 4;      // RGBx BGRx xRGB xBGR RGBA BGRA ARGB ABGR
    case 15: case 16: return 3;                                                           // RGB BGR
    default: return 0;
  }
}

void VideoChannel::set_format(const VideoFormat& format) {
  kernel::ScopedMutex guard(mutex_);
  format_ = format;
}

VideoFormat VideoChannel::format() {
  kernel::ScopedMutex guard(mutex_);
  return format_;
}

bool VideoChannel::publish(ConstByteView data, uint32_t stride, int64_t pts) {
  VideoFormat format;
  uint64_t sequence = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    if (closed_ || !format_.valid()) return false;
    format = format_;
    sequence = frames_ + 1;
  }
  // the frame is made outside the lock (a copy of the rows), then put in place
  const uint32_t row = format.row_bytes();
  if (stride < row) return false;
  const uint64_t needed = uint64_t{stride} * (format.height - 1) + row;      // (the last row needs no padding after it)
  if (data.size() < needed) return false;
  SharedPointer<VideoFrame> frame = SharedPointer<VideoFrame>::make(allocator_, allocator_);
  if (!frame || !frame->pixels.put_zeros(format.frame_bytes())) return false;
  ByteView out = frame->pixels.writable_view();
  for (uint32_t y = 0; y < format.height; ++y) out.subview(uint64_t{y} * row, row).copy_from(data.subview(uint64_t{y} * stride, row));
  frame->format = format;
  frame->sequence = sequence;
  frame->pts = pts;
  kernel::ScopedMutex guard(mutex_);
  if (closed_ || !(format_ == format)) return false;      // (closed or changed meanwhile: this frame is of no use)
  frames_ = sequence;
  latest_ = util::move(frame);
  return true;
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
}

bool VideoChannel::closed() {
  kernel::ScopedMutex guard(mutex_);
  return closed_;
}

}  // namespace pipewire
