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

#include "Hardware/VideoCodec/VideoTypes.hpp"

namespace hardware {

const char* codec_name(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::Raw: return "raw";
    case VideoCodec::Mjpeg: return "MJPEG";
    case VideoCodec::H264: return "H.264";
    case VideoCodec::Hevc: return "H.265";
    case VideoCodec::Vp9: return "VP9";
    case VideoCodec::Av1: return "AV1";
    case VideoCodec::Vp8: return "VP8";
  }
  return "?";
}

uint32_t VideoFormat::plane_width(uint32_t plane) const {
  if (pixel_format == PixelFormat::Bgra8888 || plane == 0) return width;
  return chroma_width();
}

uint32_t VideoFormat::plane_height(uint32_t plane) const {
  if (pixel_format == PixelFormat::Bgra8888 || plane == 0) return height;
  return chroma_height();
}

uint32_t VideoFormat::plane_stride(uint32_t plane) const {
  switch (pixel_format) {
    case PixelFormat::Bgra8888: return width * 4;
    case PixelFormat::I420: return plane == 0 ? width : chroma_width();
    case PixelFormat::Nv12: return plane == 0 ? width : chroma_width() * 2;
  }
  return 0;
}

uint64_t VideoFormat::frame_bytes() const {
  uint64_t total = 0;
  for (uint32_t p = 0; p < plane_count(); ++p) total += uint64_t{plane_stride(p)} * plane_height(p);
  return total;
}

}  // namespace hardware
