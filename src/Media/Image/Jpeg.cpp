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

#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "Media/ImageCodecs.hpp"

namespace media {

bool decode_jpeg(ConstByteView data, Image* out) {
  out->clear();
  hardware::JpegDecoder decoder(out->allocator());
  if (!decoder.decode(data) || !out->create(decoder.width(), decoder.height(), 1)) return false;
  if (!decoder.output_bgra(out->frame(0).data(), decoder.width())) {
    out->clear();
    return false;
  }
  return true;
}

bool encode_jpeg(const Image& image, uint32_t quality, ByteBuffer* out) {
  if (image.empty()) return false;
  hardware::VideoFrame frame;
  frame.format = {image.width(), image.height(), hardware::PixelFormat::Bgra8888};
  frame.plane[0] = reinterpret_cast<const uint8_t*>(image.frame(0).data());
  frame.stride[0] = image.width() * 4;
  hardware::JpegEncoder encoder(image.allocator());
  return encoder.encode(frame, quality) && out->append(encoder.output());
}

}  // namespace media
