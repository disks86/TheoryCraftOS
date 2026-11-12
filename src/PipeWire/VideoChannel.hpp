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

// Video in the kernel's PipeWire (Graph.hpp): what one program's video output (a pw_stream of media class Video/Source or Stream/Output/Video: a camera
// program, a screen sharer, PipeWire's video-src) shows, for the programs that watch it (pw_stream video inputs linked to it) and for the kernel itself
// (the desktop, tests). The producer publishes whole frames; each publish replaces the latest frame, which watchers take a private copy of, so a slow
// watcher misses frames but never sees one half written. Thread safe.
#pragma once
#include <stdint.h>
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"

namespace pipewire {

// A raw video format as PipeWire says it (spa/param/video/raw.h): the SPA format id, the size and the frame rate.
struct VideoFormat {
  static constexpr uint32_t kMaxSide = 8192;
  uint32_t format = 0;          // SPA_VIDEO_FORMAT_* (RGBx 7 .. BGR 16: the packed RGB ones are carried)
  uint32_t width = 0, height = 0;
  uint32_t rate_num = 0, rate_den = 1;
  // Bytes a pixel takes (0: not a packed RGB format this server carries).
  uint32_t pixel_bytes() const;
  // The bytes of one row with no padding (0 when not valid).
  uint32_t row_bytes() const { return valid() ? width * pixel_bytes() : 0; }
  uint64_t frame_bytes() const { return uint64_t{row_bytes()} * height; }
  bool valid() const { return pixel_bytes() != 0 && width >= 1 && height >= 1 && width <= kMaxSide && height <= kMaxSide; }
  bool operator==(const VideoFormat& o) const {
    return format == o.format && width == o.width && height == o.height && rate_num == o.rate_num && rate_den == o.rate_den;
  }
};

struct VideoFrame {
  explicit VideoFrame(allocators::IAllocator& allocator) : pixels(allocator) {}
  VideoFormat format;
  ByteBuffer pixels;            // rows of format.row_bytes(), no padding
  uint64_t sequence = 0;        // 1 for the first frame published, then one more each frame
  int64_t pts = 0;              // the producer's time stamp (its SPA_META_Header pts), 0 if it said none
};

class VideoChannel {
 public:
  explicit VideoChannel(allocators::IAllocator& allocator) : allocator_(allocator) {}
  VideoChannel(const VideoChannel&) = delete;
  VideoChannel& operator=(const VideoChannel&) = delete;

  // The producer's format (what watchers are offered); a new format takes effect for the frames published after it.
  void set_format(const VideoFormat& format);
  VideoFormat format();
  // A frame: `stride` bytes from one row's start to the next's in `data` (at least the row's bytes). False when it does not fit the format, there is no
  // memory, or the channel is closed.
  bool publish(ConstByteView data, uint32_t stride, int64_t pts);
  // The latest frame (null before the first one).
  SharedPointer<VideoFrame> latest();
  uint64_t frames();
  // The producer is gone: no more frames; watchers see closed().
  void close();
  bool closed();

 private:
  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;
  VideoFormat format_;
  SharedPointer<VideoFrame> latest_;
  uint64_t frames_ = 0;
  bool closed_ = false;
};

}  // namespace pipewire
