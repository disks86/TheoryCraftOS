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

#include "Media/ImageCodecs.hpp"

namespace media {

bool decode_image(ConstByteView data, Image* out, ImageFormat* detected) {
  const ImageFormat format = detect_image_format(data);
  if (detected) *detected = format;
  switch (format) {
    case ImageFormat::Png: return decode_png(data, out);
    case ImageFormat::Jpeg: return decode_jpeg(data, out);
    case ImageFormat::Bmp: return decode_bmp(data, out);
    case ImageFormat::Gif: return decode_gif(data, out);
    case ImageFormat::Pnm: return decode_pnm(data, out);
    case ImageFormat::Qoi: return decode_qoi(data, out);
    case ImageFormat::Tga: return decode_tga(data, out);
    case ImageFormat::Tiff: return decode_tiff(data, out);
    case ImageFormat::Unknown: break;
  }
  out->clear();
  return false;
}

bool encode_image(const Image& image, ImageFormat format, uint32_t quality, ByteBuffer* out) {
  if (image.empty()) return false;
  if (quality < 1) quality = 1;
  if (quality > 100) quality = 100;
  switch (format) {
    case ImageFormat::Png: return encode_png(image, 1 + quality * 8 / 100, out);
    case ImageFormat::Jpeg: return encode_jpeg(image, quality, out);
    case ImageFormat::Bmp: return encode_bmp(image, out);
    case ImageFormat::Gif: return encode_gif(image, out);
    case ImageFormat::Pnm: return encode_pnm(image, out);
    case ImageFormat::Qoi: return encode_qoi(image, out);
    case ImageFormat::Tga: return encode_tga(image, out);
    case ImageFormat::Tiff: return encode_tiff(image, out);
    case ImageFormat::Unknown: break;
  }
  return false;
}

}  // namespace media
