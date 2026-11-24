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

// What the HEVC and AV1 EncodeFrames share (VideoEncodeCodecs.hpp): 16-bit pictures to and from widened planes, the rate control words, the output metadata record.
#include "Kernel/Dxgkrnl/VideoEncodeCodecs.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk {

namespace {

uint32_t pixel_bytes(uint32_t bit_depth) { return bit_depth > 8 ? 8 : 4; }

bool image_fits(ConstByteView memory, uint64_t pitch, uint32_t image_width, uint32_t image_height, uint32_t width, uint32_t height, uint32_t bytes) {
  if (width == 0 || height == 0 || width > image_width || height > image_height || pitch < uint64_t{image_width} * bytes) return false;
  return memory.size() >= pitch * (height - 1) + uint64_t{width} * bytes;
}

}  // namespace

bool planes_fit(const VideoPictureImages& images, uint32_t width, uint32_t height, uint32_t bit_depth) {
  const uint32_t b = pixel_bytes(bit_depth);
  return image_fits(images.luma.memory, images.luma.pitch, images.luma.width, images.luma.height, width, height, b) &&
         image_fits(images.chroma.memory, images.chroma.pitch, images.chroma.width, images.chroma.height, (width + 1) / 2, (height + 1) / 2, b);
}
bool planes_fit(const VideoWritablePlaneImage& luma, const VideoWritablePlaneImage& chroma, uint32_t width, uint32_t height, uint32_t bit_depth) {
  const uint32_t b = pixel_bytes(bit_depth);
  return image_fits(ConstByteView(luma.memory), luma.pitch, luma.width, luma.height, width, height, b) &&
         image_fits(ConstByteView(chroma.memory), chroma.pitch, chroma.width, chroma.height, (width + 1) / 2, (height + 1) / 2, b);
}

Picture16::~Picture16() { release_view(allocator_, samples_); }

bool Picture16::make(uint32_t width, uint32_t height) {
  release_view(allocator_, samples_);
  width_ = width;
  height_ = height;
  const uint64_t count = uint64_t{width} * height + 2 * (uint64_t{(width + 1) / 2} * ((height + 1) / 2));
  samples_ = allocate_view<uint16_t>(allocator_, count);
  if (samples_.empty()) return false;
  samples_.fill(0);
  return true;
}

hardware::EncWritablePlanes16 Picture16::writable() const {
  const uint64_t luma = uint64_t{width_} * height_, chroma = uint64_t{(width_ + 1) / 2} * ((height_ + 1) / 2);
  return {samples_.subview(0, luma), samples_.subview(luma, chroma), samples_.subview(luma + chroma, chroma), width_, height_};
}
hardware::EncPlanes16 Picture16::planes() const { return writable().view(); }

void Picture16::read(const VideoPictureImages& in, uint32_t bit_depth) {
  const hardware::EncWritablePlanes16 p = writable();
  const uint32_t cw = p.chroma_width(), ch = p.chroma_height();
  if (bit_depth > 8) {
    // (P010: little-endian 16-bit words, the sample in the top ten bits)
    for (uint32_t y = 0; y < height_; ++y)
      for (uint32_t x = 0; x < width_; ++x)
        p.y[uint64_t{y} * width_ + x] = static_cast<uint16_t>(ByteReader(in.luma.memory.subview(in.luma.pitch * y + uint64_t{x} * 8, 2)).le16() >> 6);
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < cw; ++x) {
        ByteReader px(in.chroma.memory.subview(in.chroma.pitch * y + uint64_t{x} * 8, 4));
        p.u[uint64_t{y} * cw + x] = static_cast<uint16_t>(px.le16() >> 6);
        p.v[uint64_t{y} * cw + x] = static_cast<uint16_t>(px.le16() >> 6);
      }
    return;
  }
  for (uint32_t y = 0; y < height_; ++y)
    for (uint32_t x = 0; x < width_; ++x) p.y[uint64_t{y} * width_ + x] = in.luma.memory[in.luma.pitch * y + uint64_t{x} * 4];
  for (uint32_t y = 0; y < ch; ++y)
    for (uint32_t x = 0; x < cw; ++x) {
      const uint64_t at = in.chroma.pitch * y + uint64_t{x} * 4;
      p.u[uint64_t{y} * cw + x] = in.chroma.memory[at];
      p.v[uint64_t{y} * cw + x] = in.chroma.memory[at + 1];
    }
}

void Picture16::write(const VideoWritablePlaneImage& luma, const VideoWritablePlaneImage& chroma, uint32_t bit_depth) const {
  const hardware::EncPlanes16 p = planes();
  const uint32_t cw = p.chroma_width(), ch = p.chroma_height();
  if (bit_depth > 8) {
    for (uint32_t y = 0; y < height_; ++y)
      for (uint32_t x = 0; x < width_; ++x) {
        ByteWriter px(luma.memory.subview(luma.pitch * y + uint64_t{x} * 8, 8));
        px.le16(static_cast<uint16_t>(p.y[uint64_t{y} * width_ + x] << 6));
        px.le16(0);
        px.le16(0);
        px.le16(0xFFFF);
      }
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < cw; ++x) {
        ByteWriter px(chroma.memory.subview(chroma.pitch * y + uint64_t{x} * 8, 8));
        px.le16(static_cast<uint16_t>(p.u[uint64_t{y} * cw + x] << 6));
        px.le16(static_cast<uint16_t>(p.v[uint64_t{y} * cw + x] << 6));
        px.le16(0);
        px.le16(0xFFFF);
      }
    return;
  }
  for (uint32_t y = 0; y < height_; ++y)
    for (uint32_t x = 0; x < width_; ++x) {
      ByteView px = luma.memory.subview(luma.pitch * y + uint64_t{x} * 4, 4);
      px[0] = static_cast<uint8_t>(p.y[uint64_t{y} * width_ + x]);
      px[1] = px[2] = 0;
      px[3] = 0xFF;
    }
  for (uint32_t y = 0; y < ch; ++y)
    for (uint32_t x = 0; x < cw; ++x) {
      ByteView px = chroma.memory.subview(chroma.pitch * y + uint64_t{x} * 4, 4);
      px[0] = static_cast<uint8_t>(p.u[uint64_t{y} * cw + x]);
      px[1] = static_cast<uint8_t>(p.v[uint64_t{y} * cw + x]);
      px[2] = 0;
      px[3] = 0xFF;
    }
}

bool rate_control_from_words(ArrayView<const uint32_t> w, hardware::RateControlConfig* rc) {
  if (w.size() < 31 || w[10] > 4) return false;
  rc->mode = static_cast<hardware::RateControlMode>(w[10]);
  rc->flags = w[11];
  rc->fps_numerator = w[12];
  rc->fps_denominator = w[13];
  rc->qp_intra = w[14];
  rc->qp_p = w[15];
  rc->qp_b = w[16];
  rc->initial_qp = w[17];
  rc->min_qp = w[18];
  rc->max_qp = w[19];
  rc->target_bitrate = w[20] | uint64_t{w[21]} << 32;
  rc->peak_bitrate = w[22] | uint64_t{w[23]} << 32;
  rc->vbv_capacity = w[24] | uint64_t{w[25]} << 32;
  rc->vbv_initial_fullness = w[26] | uint64_t{w[27]} << 32;
  rc->max_frame_bits = w[28] | uint64_t{w[29]} << 32;
  rc->quality_target = w[30];
  return true;
}

void write_output_metadata(ByteWriter& w, uint64_t errors, uint64_t written, const hardware::EncStatistics& s, uint64_t subregions) {
  w.le64(errors);
  w.le64(errors || s.units == 0 ? 0 : s.qp_sum / s.units);   // AverageQP
  w.le64(s.intra);
  w.le64(s.inter);
  w.le64(s.skipped);
  w.le64(s.inter ? static_cast<uint64_t>(s.vector_x_sum / static_cast<int64_t>(s.inter)) : 0);
  w.le64(s.inter ? static_cast<uint64_t>(s.vector_y_sum / static_cast<int64_t>(s.inter)) : 0);
  w.le64(errors ? 0 : written);
  w.le64(errors ? 0 : subregions);
}

}  // namespace dxgk
