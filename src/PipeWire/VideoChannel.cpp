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

// How a raw format's planes are laid out: for each plane, its subsampling (a sample every sx columns and sy rows), how many samples make a unit and how
// many bytes a unit takes (packed formats: 2 pixels in 4 bytes, 6 in 16, ...), what its rows and their count are rounded up to (a tiled format's
// tiles), and its stride against the first plane's when the planes are one after another in a block. A palette plane is `fixed` bytes, one row.
struct PlaneSpec {
  uint8_t sx = 1, sy = 1, unit_px = 1, unit_bytes = 1;
  uint8_t stride_num = 1, stride_den = 1;
  uint16_t row_align = 1, rows_align = 1;
  uint16_t fixed = 0;
};
struct Layout {
  uint8_t planes = 0;
  PlaneSpec p[VideoFormat::kMaxPlanes];
};
constexpr PlaneSpec bytes_px(uint8_t bytes) { return PlaneSpec{1, 1, 1, bytes, 1, 1, 1, 1, 0}; }
constexpr PlaneSpec packed(uint8_t px, uint8_t bytes) { return PlaneSpec{1, 1, px, bytes, 1, 1, 1, 1, 0}; }
constexpr PlaneSpec chroma(uint8_t sx, uint8_t sy, uint8_t bytes, uint8_t num, uint8_t den) { return PlaneSpec{sx, sy, 1, bytes, num, den, 1, 1, 0}; }
// planar YUV of `bytes` a sample, chroma every sx x sy, with an alpha plane or not
Layout planar_yuv(uint8_t sx, uint8_t sy, uint8_t bytes, bool alpha) {
  Layout l;
  l.planes = alpha ? 4 : 3;
  l.p[0] = bytes_px(bytes);
  l.p[1] = l.p[2] = chroma(sx, sy, bytes, 1, sx);
  if (alpha) l.p[3] = bytes_px(bytes);
  return l;
}
Layout planar_rgb(uint8_t bytes, bool alpha) {
  Layout l;
  l.planes = alpha ? 4 : 3;
  for (uint32_t i = 0; i < l.planes; ++i) l.p[i] = bytes_px(bytes);
  return l;
}
Layout semi(uint8_t sx, uint8_t sy, uint8_t bytes, uint8_t num, uint8_t den) {
  Layout l;
  l.planes = 2;
  l.p[0] = bytes_px(bytes);
  l.p[1] = chroma(sx, sy, uint8_t(2 * bytes), num, den);
  return l;
}
Layout one(PlaneSpec spec) {
  Layout l;
  l.planes = 1;
  l.p[0] = spec;
  return l;
}

// SPA_VIDEO_FORMAT_* (every raw one spa/param/video/raw.h has, but ENCODED)
bool layout_of(uint32_t format, Layout* out) {
  switch (format) {
    case 2: case 3: *out = planar_yuv(2, 2, 1, false); return true;                  // I420, YV12
    case 4: case 5: case 19: case 64: *out = one(packed(2, 4)); return true;           // YUY2, UYVY, YVYU, VYUY
    case 6: *out = one(bytes_px(4)); return true;                                      // AYUV
    case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: *out = one(bytes_px(4)); return true;      // 32-bit RGB
    case 15: case 16: case 28: case 63: *out = one(bytes_px(3)); return true;          // RGB, BGR, v308, IYU2
    case 17: *out = planar_yuv(4, 1, 1, false); return true;                           // Y41B
    case 18: *out = planar_yuv(2, 1, 1, false); return true;                           // Y42B
    case 20: *out = planar_yuv(1, 1, 1, false); return true;                           // Y444
    case 21: *out = one(packed(6, 16)); return true;                                   // v210: six pixels in four 32-bit words
    case 22: *out = one(packed(2, 8)); return true;                                    // v216
    case 23: case 24: *out = semi(2, 2, 1, 1, 1); return true;                         // NV12, NV21
    case 25: *out = one(bytes_px(1)); return true;                                     // GRAY8
    case 26: case 27: *out = one(bytes_px(2)); return true;                            // GRAY16
    case 29: case 30: case 31: case 32: *out = one(bytes_px(2)); return true;          // RGB16, BGR16, RGB15, BGR15
    case 33: *out = one(packed(2, 5)); return true;                                    // UYVP: two pixels in 40 bits
    case 34: *out = planar_yuv(2, 2, 1, true); return true;                            // A420
    case 35: {                                                                         // RGB8P: indices, then the palette (256 0xAARRGGBB words)
      Layout l;
      l.planes = 2;
      l.p[0] = bytes_px(1);
      l.p[1] = PlaneSpec{1, 1, 1, 1, 1, 1, 1, 1, 1024};
      *out = l;
      return true;
    }
    case 36: case 37: *out = planar_yuv(4, 4, 1, false); return true;                  // YUV9, YVU9
    case 38: *out = one(packed(4, 6)); return true;                                    // IYU1: four pixels in six bytes
    case 39: case 40: *out = one(bytes_px(8)); return true;                            // ARGB64, AYUV64
    case 41: *out = one(bytes_px(4)); return true;                                     // r210
    case 42: case 43: case 72: case 73: *out = planar_yuv(2, 2, 2, false); return true;      // I420_10, I420_12
    case 44: case 45: case 74: case 75: *out = planar_yuv(2, 1, 2, false); return true;      // I422_10, I422_12
    case 46: case 47: case 76: case 77: *out = planar_yuv(1, 1, 2, false); return true;      // Y444_10, Y444_12
    case 48: *out = planar_rgb(1, false); return true;                                 // GBR
    case 49: case 50: case 68: case 69: *out = planar_rgb(2, false); return true;      // GBR_10, GBR_12
    case 65: *out = planar_rgb(1, true); return true;                                  // GBRA
    case 66: case 67: case 70: case 71: *out = planar_rgb(2, true); return true;       // GBRA_10, GBRA_12
    case 51: *out = semi(2, 1, 1, 1, 1); return true;                                  // NV16
    case 52: *out = semi(1, 1, 1, 2, 1); return true;                                  // NV24
    case 53: {                                                                         // NV12_64Z32: NV12 in tiles of 64 x 32 bytes
      Layout l = semi(2, 2, 1, 1, 1);
      l.p[0].row_align = 128;
      l.p[0].rows_align = 32;
      l.p[1].row_align = 128;
      l.p[1].rows_align = 32;
      *out = l;
      return true;
    }
    case 54: case 55: *out = planar_yuv(2, 2, 2, true); return true;                   // A420_10
    case 56: case 57: *out = planar_yuv(2, 1, 2, true); return true;                   // A422_10
    case 58: case 59: *out = planar_yuv(1, 1, 2, true); return true;                   // A444_10
    case 60: *out = semi(2, 1, 1, 1, 1); return true;                                  // NV61
    case 61: case 62: *out = semi(2, 2, 2, 1, 1); return true;                         // P010_10BE, P010_10LE
    case 78: *out = one(bytes_px(8)); return true;                                     // RGBA_F16
    case 79: *out = one(bytes_px(16)); return true;                                    // RGBA_F32 (dsp video too)
    case 80: case 81: case 82: case 83: case 84: case 85: case 86: case 87: *out = one(bytes_px(4)); return true;      // 10-bit RGB
    default: return false;
  }
}

uint32_t round_up(uint64_t v, uint32_t to) { return static_cast<uint32_t>((v + to - 1) / to * to); }

}  // namespace

bool is_encoded_video_subtype(uint32_t subtype) {
  // SPA_MEDIA_SUBTYPE_h264 0x20001 .. h265 0x2000E, but not bayer (0x2000D: raw sensor data, not carried)
  return subtype >= 0x20001 && subtype <= 0x2000E && subtype != 0x2000D;
}

uint32_t VideoFormat::planes() const {
  if (encoded()) return 0;
  Layout l;
  return layout_of(format, &l) ? l.planes : 0;
}

VideoPlane VideoFormat::plane(uint32_t index) const {
  Layout l;
  if (encoded() || !layout_of(format, &l) || index >= l.planes) return VideoPlane();
  const PlaneSpec& s = l.p[index];
  if (s.fixed) return VideoPlane{s.fixed, 1, 0, 1};      // (a palette: its own size, whatever the stride)
  const uint64_t samples = (uint64_t{width} + s.sx - 1) / s.sx, units = (samples + s.unit_px - 1) / s.unit_px;
  const uint64_t rows = (uint64_t{height} + s.sy - 1) / s.sy;
  return VideoPlane{round_up(units * s.unit_bytes, s.row_align), round_up(rows, s.rows_align), s.stride_num, s.stride_den};
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
  return (subtype == kSubtypeRaw || (subtype == kSubtypeDsp && format == 79)) && planes() != 0 && width >= 1 && height >= 1;
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
