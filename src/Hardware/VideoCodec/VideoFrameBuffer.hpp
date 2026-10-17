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

namespace hardware {

// Memory for one frame of a given format, owned. Encoders and decoders keep one of these for their output;
// programs use one to hold a frame they want to encode. The memory is not cleared.
class VideoFrameBuffer {
 public:
  explicit VideoFrameBuffer(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~VideoFrameBuffer() { release_view(allocator_, memory_); }
  VideoFrameBuffer(const VideoFrameBuffer&) = delete;
  VideoFrameBuffer& operator=(const VideoFrameBuffer&) = delete;

  // Makes room for `format` (keeping the memory if it already fits). False if the format is invalid or memory ran out.
  bool allocate(const VideoFormat& format);
  const VideoFormat& format() const { return format_; }
  bool valid() const { return !memory_.empty(); }
  // The bytes of plane `index`.
  ByteView plane(uint32_t index) const;
  // A view of the whole frame, with the timestamp given.
  VideoFrame frame(int64_t pts_us = 0, bool keyframe = true) const;

 private:
  allocators::IAllocator& allocator_;
  ByteView memory_;
  VideoFormat format_;
};

}  // namespace hardware
