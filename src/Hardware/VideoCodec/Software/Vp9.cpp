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


#include "Hardware/VideoCodec/Software/Vp9.hpp"
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"

namespace hardware {

Vp9Decoder::~Vp9Decoder() {}

void Vp9Decoder::reset() {
  if (core_) core_->reset();
  pending_ = false;
}

// Rounds the picture at `index` down to 8-bit I420.
bool Vp9Decoder::convert(int index) {
  const vp9::Frame& f = core_->frame(index);
  const VideoFormat format{static_cast<uint32_t>(f.width), static_cast<uint32_t>(f.height), PixelFormat::I420};
  if (!format.valid() || !out_.allocate(format)) return false;
  const int bd = f.bit_depth;
  auto to8 = [bd](int v) {
    if (bd == 8) return v;
    const int r = (v + (1 << (bd - 9))) >> (bd - 8);
    return r > 255 ? 255 : r;
  };
  for (uint32_t plane = 0; plane < 3; ++plane) {
    ByteView dst = out_.plane(plane);
    const uint32_t dst_stride = format.plane_stride(plane);
    const uint32_t w = format.plane_width(plane), h = format.plane_height(plane);
    const uint32_t src_stride = f.stride[plane];
    const int src_w = f.crop_w[plane], src_h = f.crop_h[plane];
    // How many source samples make one output sample in each direction (chroma that is not 4:2:0 is averaged down).
    const int fx = plane ? (f.ss_x ? 1 : 2) : 1, fy = plane ? (f.ss_y ? 1 : 2) : 1;
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        int sum = 0;
        for (int j = 0; j < fy; ++j) {
          const int sy = static_cast<int>(y) * fy + j < src_h ? static_cast<int>(y) * fy + j : src_h - 1;
          for (int i = 0; i < fx; ++i) {
            const int sx = static_cast<int>(x) * fx + i < src_w ? static_cast<int>(x) * fx + i : src_w - 1;
            sum += f.plane[plane][static_cast<uint64_t>(sy) * src_stride + static_cast<uint64_t>(sx)];
          }
        }
        const int n = fx * fy;
        dst[static_cast<uint64_t>(y) * dst_stride + x] = static_cast<uint8_t>(to8((sum + n / 2) / n));
      }
    }
  }
  format_ = format;
  limited_range_ = !f.full_range;
  return true;
}

bool Vp9Decoder::decode(ConstByteView packet, int64_t pts_us) {
  if (!core_) core_ = UniquePointer<vp9::Core>::make(allocator_, allocator_);
  if (!core_ || packet.empty()) return false;
  // A superframe ends with an index of the frame sizes.
  uint64_t sizes[8];
  int count = 1;
  sizes[0] = packet.size();
  const uint8_t marker = packet[packet.size() - 1];
  if ((marker & 0xe0) == 0xc0) {
    const int frames = (marker & 7) + 1, mag = ((marker >> 3) & 3) + 1;
    const uint64_t index_size = 2 + static_cast<uint64_t>(mag) * frames;
    if (packet.size() >= index_size && packet[packet.size() - index_size] == marker) {
      uint64_t pos = packet.size() - index_size + 1;
      uint64_t total = 0;
      count = frames;
      for (int i = 0; i < frames; ++i) {
        uint64_t sz = 0;
        for (int b = 0; b < mag; ++b) sz |= uint64_t{packet[pos++]} << (8 * b);
        sizes[i] = sz;
        if (sz > packet.size() - index_size - total) return false;
        total += sz;
      }
    }
  }
  uint64_t offset = 0;
  bool ok = true;
  for (int i = 0; i < count; ++i) {
    const uint64_t size = sizes[i];
    if (size == 0) continue;
    int shown = -1;
    vp9::FrameHeaderInfo info;
    if (!core_->decode_frame(packet.subview(offset, size), &shown, &info)) {
      ok = false;
      break;
    }
    offset += size;
    if (shown >= 0) {
      if (!convert(shown)) {
        ok = false;
        break;
      }
      pending_ = true;
      pts_ = pts_us;
      keyframe_ = info.key_frame;
    }
  }
  return ok;
}

bool Vp9Decoder::receive(VideoFrame* frame) {
  if (!pending_) return false;
  pending_ = false;
  *frame = out_.frame(pts_, keyframe_);
  frame->limited_range = limited_range_;
  return true;
}

}  // namespace hardware
