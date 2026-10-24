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

#include "Hardware/VideoCodec/Software/Av1.hpp"
#include "Hardware/VideoCodec/Software/Av1Core.hpp"

namespace hardware {

namespace {

// Runs the OBUs of `data` through the parser and the decoding core, the loop of dav1d_parse_obus. False on the first corrupt OBU or
// undecodable frame: the rest of the data is dropped, as dav1d does.
bool run_obus(av1::Core& core, ConstByteView data) {
  av1::ObuParser& parser = core.parser();
  ConstByteView rest = data;
  while (!rest.empty()) {
    uint64_t used = 0;
    av1::ObuEvent event;
    if (!parser.parse_obu(rest, used, event)) return false;
    if (used == 0 || used > rest.size()) return false;
    rest = rest.subview(used);
    switch (event.kind) {
      case av1::ObuEventKind::SequenceHeader:
        if (event.new_sequence) core.new_sequence();
        break;
      case av1::ObuEventKind::ShowExistingFrame:
        if (!core.show_existing_frame(event.existing_frame_idx, event.refresh_all_refs)) return false;
        break;
      case av1::ObuEventKind::FrameReady:
        if (!core.decode_frame()) return false;
        break;
      default:
        break;
    }
  }
  return true;
}

}  // namespace

Av1Decoder::~Av1Decoder() {}

void Av1Decoder::reset() {
  if (core_) core_->reset();
  pending_ = false;
  format_ = VideoFormat();
  keyframe_ = true;
}

bool Av1Decoder::configure(ConstByteView av1c) {
  // av1C: marker and version, profile and level, flags, delay; then the configuration OBUs.
  if (av1c.size() < 4 || (av1c[0] & 0x80) == 0 || (av1c[0] & 0x7f) != 1) return false;
  if (!core_) core_ = UniquePointer<av1::Core>::make(allocator_, allocator_);
  if (!core_) return false;
  return run_obus(*core_, av1c.subview(4));
}

// Rounds the picture down to 8-bit I420.
bool Av1Decoder::convert() {
  const av1::Picture* pic = core_->output();
  if (!pic) return false;
  const VideoFormat format{static_cast<uint32_t>(pic->width()), static_cast<uint32_t>(pic->height()), PixelFormat::I420};
  if (!format.valid() || !out_.allocate(format)) return false;
  const int bd = pic->bpc();
  auto to8 = [bd](int v) {
    if (bd == 8) return v;
    const int r = (v + (1 << (bd - 9))) >> (bd - 8);
    return r > 255 ? 255 : r;
  };
  const bool mono = pic->layout() == av1::PIXEL_LAYOUT_I400;
  for (uint32_t plane = 0; plane < 3; ++plane) {
    ByteView dst = out_.plane(plane);
    const uint32_t dst_stride = format.plane_stride(plane);
    const uint32_t w = format.plane_width(plane), h = format.plane_height(plane);
    if (mono && plane) {
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) dst[static_cast<uint64_t>(y) * dst_stride + x] = 128;
      continue;
    }
    const av1::PixelView src = pic->plane(static_cast<int>(plane));
    const int src_w = pic->plane_width(static_cast<int>(plane)), src_h = pic->plane_height(static_cast<int>(plane));
    // How many source samples make one output sample in each direction (chroma that is not 4:2:0 is averaged down).
    const int fx = plane ? (pic->ss_hor() ? 1 : 2) : 1, fy = plane ? (pic->ss_ver() ? 1 : 2) : 1;
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        int sum = 0;
        for (int j = 0; j < fy; ++j) {
          const int sy = static_cast<int>(y) * fy + j < src_h ? static_cast<int>(y) * fy + j : src_h - 1;
          for (int i = 0; i < fx; ++i) {
            const int sx = static_cast<int>(x) * fx + i < src_w ? static_cast<int>(x) * fx + i : src_w - 1;
            sum += src.at(sx, sy);
          }
        }
        const int n = fx * fy;
        dst[static_cast<uint64_t>(y) * dst_stride + x] = static_cast<uint8_t>(to8((sum + n / 2) / n));
      }
    }
  }
  format_ = format;
  keyframe_ = pic->hdr.frame_type == av1::FRAME_TYPE_KEY;
  return true;
}

bool Av1Decoder::decode(ConstByteView packet, int64_t pts_us) {
  if (!core_) core_ = UniquePointer<av1::Core>::make(allocator_, allocator_);
  if (!core_ || packet.empty()) return false;
  core_->clear_output();
  const bool ok = run_obus(*core_, packet);
  // The last picture shown by the unit comes out even if a later OBU of the unit was corrupt.
  if (core_->output()) {
    const av1::SequenceHeader* seq = core_->parser().sequence_header();
    if (seq) limited_range_ = seq->color_range == 0;
    if (convert()) {
      pending_ = true;
      pts_ = pts_us;
    }
  }
  return ok;
}

bool Av1Decoder::receive(VideoFrame* frame) {
  if (!pending_) return false;
  pending_ = false;
  *frame = out_.frame(pts_, keyframe_);
  frame->limited_range = limited_range_;
  return true;
}

}  // namespace hardware
