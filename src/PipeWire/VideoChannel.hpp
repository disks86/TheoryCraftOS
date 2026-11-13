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

// One plane of a raw frame: its rows of row_bytes; a plane after the first in one block has the first's stride times stride_num / stride_den
// (rounded up).
struct VideoPlane {
  uint32_t row_bytes = 0, rows = 0, stride_num = 1, stride_den = 1;
};

// A video format as PipeWire says it (spa/param/video/raw.h, spa/param/format.h): raw (the SPA format id, the size, the frame rate) or encoded (the
// media subtype: H.264, MJPEG, VP8, VP9, H.265, ...; its frames are whole compressed pictures of any length).
struct VideoFormat {
  static constexpr uint32_t kMaxSide = 8192;
  static constexpr uint32_t kMaxPlanes = 4;
  static constexpr uint32_t kSubtypeRaw = 1, kSubtypeDsp = 2;      // (dsp: PipeWire's float video, "32 bit float RGBA video", laid out as RGBA_F32)
  static constexpr uint64_t kMaxEncodedFrame = 16u << 20;
  uint32_t subtype = kSubtypeRaw;      // SPA_MEDIA_SUBTYPE_raw, or an encoded one (SPA_MEDIA_SUBTYPE_h264 ...)
  uint32_t format = 0;          // (raw) SPA_VIDEO_FORMAT_*: packed RGB (8, 10 and 16 bits, float), packed and planar YUV (8, 10, 12 bits), grey
  uint32_t width = 0, height = 0;      // (encoded: may be 0, unknown)
  uint32_t rate_num = 0, rate_den = 1;
  bool encoded() const { return subtype != kSubtypeRaw && subtype != kSubtypeDsp; }
  // (raw) How many planes and what each is (0 planes: not a format this server carries).
  uint32_t planes() const;
  VideoPlane plane(uint32_t index) const;
  // (raw) The bytes of the first plane's row with no padding (0 when not valid).
  uint32_t row_bytes() const { return valid() && !encoded() ? plane(0).row_bytes : 0; }
  // (raw) A whole frame with no padding, its planes one after another; 0 for encoded video.
  uint64_t frame_bytes() const;
  uint64_t plane_offset(uint32_t index) const;      // (raw) where a plane starts in such a frame
  bool valid() const;
  bool operator==(const VideoFormat& o) const {
    return subtype == o.subtype && format == o.format && width == o.width && height == o.height && rate_num == o.rate_num && rate_den == o.rate_den;
  }
};

// Whether a media subtype is an encoded video one this server carries.
bool is_encoded_video_subtype(uint32_t subtype);

struct VideoFrame {
  explicit VideoFrame(allocators::IAllocator& allocator) : pixels(allocator) {}
  VideoFormat format;
  ByteBuffer pixels;            // raw: the planes with no padding, one after another (format.plane_offset()); encoded: the frame's bytes
  uint64_t sequence = 0;        // 1 for the first frame published, then one more each frame
  int64_t pts = 0;              // the producer's time stamp (its SPA_META_Header pts), 0 if it said none
};

// Where a plane of a frame to publish is: its bytes, from its first row's start, and the bytes from one row's start to the next (raw only).
struct VideoPlaneData {
  ConstByteView data;
  uint32_t stride = 0;
};

class VideoChannel {
 public:
  explicit VideoChannel(allocators::IAllocator& allocator) : allocator_(allocator) {}
  VideoChannel(const VideoChannel&) = delete;
  VideoChannel& operator=(const VideoChannel&) = delete;

  // The producer's format (what watchers are offered); a new format takes effect for the frames published after it.
  void set_format(const VideoFormat& format);
  VideoFormat format();
  // A frame: raw, each plane of the format (strides at least the planes' rows); encoded, one with the frame's bytes (at most kMaxEncodedFrame). False
  // when it does not fit the format, there is no memory, or the channel is closed.
  bool publish(ArrayView<const VideoPlaneData> planes, int64_t pts);
  // A frame of one plane (packed formats; encoded video with stride 0).
  bool publish(ConstByteView data, uint32_t stride, int64_t pts) {
    const VideoPlaneData one{data, stride};
    return publish(ArrayView<const VideoPlaneData>(&one, 1), pts);
  }
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
