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

#include "Media/Image.hpp"

namespace media {

const char* image_format_name(ImageFormat format) {
  switch (format) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::Bmp: return "BMP";
    case ImageFormat::Gif: return "GIF";
    case ImageFormat::Pnm: return "PNM";
    case ImageFormat::Qoi: return "QOI";
    case ImageFormat::Tga: return "TGA";
    case ImageFormat::Tiff: return "TIFF";
    case ImageFormat::Unknown: break;
  }
  return "unknown";
}

ImageFormat detect_image_format(ConstByteView d) {
  auto starts = [&](const char* tag, uint32_t n) {
    if (d.size() < n) return false;
    for (uint32_t i = 0; i < n; ++i)
      if (d[i] != static_cast<uint8_t>(tag[i])) return false;
    return true;
  };
  if (starts("\x89PNG\r\n\x1a\n", 8)) return ImageFormat::Png;
  if (d.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return ImageFormat::Jpeg;
  if (starts("BM", 2) && d.size() >= 26) return ImageFormat::Bmp;
  if (starts("GIF87a", 6) || starts("GIF89a", 6)) return ImageFormat::Gif;
  if (starts("qoif", 4)) return ImageFormat::Qoi;
  if (d.size() >= 8 && ((starts("II", 2) && d[2] == 42 && d[3] == 0) || (starts("MM", 2) && d[2] == 0 && d[3] == 42))) return ImageFormat::Tiff;
  if (d.size() >= 3 && d[0] == 'P' && d[1] >= '1' && d[1] <= '7' && (d[2] == ' ' || d[2] == '\n' || d[2] == '\r' || d[2] == '\t')) return ImageFormat::Pnm;
  // TGA has no signature. Accept a header that makes sense: a known image type, a colour map type 0 or 1, a sane depth.
  if (d.size() >= 18) {
    const uint8_t map_type = d[1], type = d[2], depth = d[16];
    const bool known_type = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
    const bool sane_depth = depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32;
    const uint32_t w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8);
    if (known_type && map_type <= 1 && sane_depth && w > 0 && h > 0 && (map_type == 1) == (type == 1 || type == 9)) return ImageFormat::Tga;
  }
  return ImageFormat::Unknown;
}

bool Image::create(uint32_t width, uint32_t height, uint32_t frames) {
  clear();
  if (width == 0 || height == 0 || frames == 0 || uint64_t{width} * height * frames > kMaxPixels) return false;
  pixels_ = allocate_view<uint32_t>(allocator_, uint64_t{width} * height * frames, 64);
  delays_ = allocate_view<uint32_t>(allocator_, frames);
  if (pixels_.empty() || delays_.empty()) {
    clear();
    return false;
  }
  pixels_.fill(0);
  delays_.fill(100);
  width_ = width;
  height_ = height;
  frames_ = frames;
  return true;
}

void Image::clear() {
  release_view(allocator_, pixels_);
  release_view(allocator_, delays_);
  width_ = height_ = frames_ = loops_ = 0;
  has_alpha_ = false;
}

graphics::Surface Image::surface(uint32_t index) const {
  graphics::Surface s;
  if (index >= frames_) return s;
  s.pixels = frame(index);
  s.width = width_;
  s.height = height_;
  s.pitch = width_;
  return s;
}

uint64_t Image::total_duration_ms() const {
  uint64_t sum = 0;
  for (uint32_t i = 0; i < frames_; ++i) sum += delays_[i];
  return sum;
}

}  // namespace media
