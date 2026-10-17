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

#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"

namespace hardware {

bool VideoFrameBuffer::allocate(const VideoFormat& format) {
  if (!format.valid()) return false;
  if (!memory_.empty() && format == format_) return true;
  release_view(allocator_, memory_);
  memory_ = allocate_view<uint8_t>(allocator_, format.frame_bytes(), 64);
  if (memory_.empty()) return false;
  format_ = format;
  return true;
}

ByteView VideoFrameBuffer::plane(uint32_t index) const {
  if (memory_.empty() || index >= format_.plane_count()) return ByteView();
  uint64_t offset = 0;
  for (uint32_t p = 0; p < index; ++p) offset += format_.plane_bytes(p);
  return memory_.subview(offset, format_.plane_bytes(index));
}

VideoFrame VideoFrameBuffer::frame(int64_t pts_us, bool keyframe) const {
  VideoFrame f;
  f.format = format_;
  f.pts_us = pts_us;
  f.keyframe = keyframe;
  for (uint32_t p = 0; p < format_.plane_count() && !memory_.empty(); ++p) {
    f.plane[p] = plane(p).data();
    f.stride[p] = format_.plane_stride(p);
  }
  return f;
}

}  // namespace hardware
