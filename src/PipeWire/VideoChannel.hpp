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
#include "PipeWire/ProtocolServer.hpp"
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
  // (raw) The DRM format modifier of DMA-BUF frames (SPA_FORMAT_VIDEO_modifier), when the format says one: 0 is linear, what /dev/dxg's exports are.
  // Not part of the comparison: frames of a format carry the same pixels whatever their memory.
  bool has_modifier = false;
  uint64_t modifier = 0;
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

// A producer's own buffers, which watchers that can take them as they are are given (zero-copy: the DMA-BUFs or memfds a program allocated, NativeServer):
// their descriptors, how each block is mapped, and for each buffer how many hold it. Only the native server's thread uses one; the channel and its
// frames only carry it to the watchers.
struct VideoShare {
  static constexpr uint32_t kMaxBuffers = 64;
  explicit VideoShare(allocators::IAllocator& a) : fds(a), mapoffsets(a), maxsizes(a), flags(a) {}
  uint32_t n_buffers = 0, blocks = 1;
  uint32_t data_type = 0;       // SPA_DATA_DmaBuf or SPA_DATA_MemFd (every block the same)
  List<Fd> fds;                 // buffer i's block k at i * blocks + k
  List<uint32_t> mapoffsets, maxsizes, flags;
  uint32_t users[kMaxBuffers] = {};      // watchers holding each buffer
  bool taken[kMaxBuffers] = {};          // taken from the producer and not handed back yet
  uint64_t taken_at[kMaxBuffers] = {};   // (when: the oldest goes back first)
  uint64_t clock = 0;
  uint32_t latest = 0xFFFFFFFFu;         // the buffer of the newest frame (held for the watchers that have not had it)
  uint32_t pending = 0xFFFFFFFFu;        // a frame taken that is not drawn yet (its acquire point not reached, the GPU still on it)
  uint64_t acquire_point[kMaxBuffers] = {}, release_point[kMaxBuffers] = {};      // (explicit sync) each buffer's points when it was taken
  bool promised[kMaxBuffers] = {};       // (explicit sync) its release point is to be signalled when it goes back
  uint32_t attached = 0;                 // watchers' ports given these buffers
  bool closed = false;                   // the producer let go of them (another format, other buffers, gone)
};
// Where a block of a shared frame's data is in its buffer (its chunk: offset, size, stride).
struct VideoShareChunk {
  uint32_t offset = 0, size = 0;
  int32_t stride = 0;
};

struct VideoFrame {
  explicit VideoFrame(allocators::IAllocator& allocator) : pixels(allocator) {}
  VideoFormat format;
  ByteBuffer pixels;            // raw: the planes with no padding, one after another (format.plane_offset()); encoded: the frame's bytes. Empty for a
                                // frame only shared (nobody needed a copy): `share` holds it
  uint64_t sequence = 0;        // 1 for the first frame published, then one more each frame
  int64_t pts = 0;              // the producer's time stamp (its SPA_META_Header pts), 0 if it said none
  // (a producer whose buffers are shared) the buffer this frame is in, and where each block's data is
  SharedPointer<VideoShare> share;
  uint32_t share_buffer = 0xFFFFFFFFu;
  VideoShareChunk chunks[VideoFormat::kMaxPlanes];
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
  // `shared`: (a producer whose buffers are shared) the frame's place in them, carried with the copy.
  bool publish(ArrayView<const VideoPlaneData> planes, int64_t pts, const VideoFrame* shared = nullptr);
  // A frame that is only in the producer's shared buffers (no copy: every watcher takes them as they are). `frame`'s share fields say where; its
  // format, sequence and pixels are set here.
  bool publish_shared(SharedPointer<VideoFrame> frame);
  // The producer's shared buffers (null: not shared), for watchers being set up.
  void set_share(SharedPointer<VideoShare> share);
  SharedPointer<VideoShare> share();
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
  SharedPointer<VideoShare> share_;
  uint64_t frames_ = 0;
  bool closed_ = false;
};

}  // namespace pipewire
