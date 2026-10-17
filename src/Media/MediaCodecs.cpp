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

#include "Media/MediaCodecs.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"

namespace media {

namespace {
// The picture size from a JPEG's start-of-frame marker; false if there is none before the scan.
bool jpeg_dimensions(ConstByteView d, uint32_t* width, uint32_t* height) {
  uint64_t at = 2;
  while (at + 4 <= d.size()) {
    if (d[at] != 0xFF) return false;
    const uint8_t marker = d[at + 1];
    if (marker == 0xFF) {
      ++at;
      continue;
    }
    if (marker == 0xDA || marker == 0xD9) return false;
    const uint64_t length = (uint64_t{d[at + 2]} << 8) | d[at + 3];
    const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (sof) {
      if (at + 9 > d.size()) return false;
      *height = (uint32_t{d[at + 5]} << 8) | d[at + 6];
      *width = (uint32_t{d[at + 7]} << 8) | d[at + 8];
      return *width > 0 && *height > 0;
    }
    at += 2 + length;
  }
  return false;
}
}  // namespace

hardware::IVideoCodecDevice* MediaCodecs::accelerated_device(bool encode) const {
  if (!video_) return nullptr;
  for (uint32_t i = 0; i < video_->count(); ++i) {
    hardware::IVideoCodecDevice* d = video_->at(i);
    if (d && d->info().accelerated && (encode ? d->can_encode(hardware::VideoCodec::Mjpeg) : d->can_decode(hardware::VideoCodec::Mjpeg))) return d;
  }
  return nullptr;
}

bool MediaCodecs::decode_jpeg_hardware(ConstByteView data, Image* out) const {
  hardware::IVideoCodecDevice* device = accelerated_device(false);
  uint32_t width = 0, height = 0;
  if (!device || !jpeg_dimensions(data, &width, &height) || uint64_t{width} * height > Image::kMaxPixels) return false;
  hardware::DecoderConfig config;
  config.codec = hardware::VideoCodec::Mjpeg;
  config.width = width;
  config.height = height;
  config.output = hardware::PixelFormat::Bgra8888;
  UniquePointer<hardware::IVideoDecoder> decoder = device->create_decoder(config);
  if (!decoder) return false;
  hardware::VideoPacket packet;
  packet.data = data;
  hardware::VideoFrame frame;
  if (decoder->submit(packet) != hardware::SubmitResult::Accepted) return false;
  if (!decoder->receive(&frame) && (decoder->flush(), !decoder->receive(&frame))) return false;
  if (frame.format.width != width || frame.format.height != height || !out->create(width, height, 1)) return false;
  if (!hardware::convert_to_bgra(frame, out->frame(0).data(), width)) {
    out->clear();
    return false;
  }
  out->set_has_alpha(false);
  return true;
}

bool MediaCodecs::encode_jpeg_hardware(const Image& image, uint32_t quality, ByteBuffer* out) const {
  hardware::IVideoCodecDevice* device = accelerated_device(true);
  if (!device || image.empty() || image.width() > hardware::VideoFormat::kMaxDimension || image.height() > hardware::VideoFormat::kMaxDimension) return false;
  hardware::EncoderConfig config;
  config.codec = hardware::VideoCodec::Mjpeg;
  config.width = image.width();
  config.height = image.height();
  config.input = hardware::PixelFormat::Bgra8888;
  config.quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
  UniquePointer<hardware::IVideoEncoder> encoder = device->create_encoder(config);
  if (!encoder) return false;
  hardware::VideoFrame frame;
  frame.format = {image.width(), image.height(), hardware::PixelFormat::Bgra8888};
  frame.plane[0] = reinterpret_cast<const uint8_t*>(image.frame(0).data());
  frame.stride[0] = image.width() * 4;
  if (encoder->submit(frame) != hardware::SubmitResult::Accepted) return false;
  hardware::VideoPacket packet;
  if (!encoder->receive(&packet) && (encoder->flush(), !encoder->receive(&packet))) return false;
  return out->append(packet.data);
}

bool MediaCodecs::decode_image(ConstByteView data, Image* out, ImageFormat* detected, CodecPath* path) const {
  const ImageFormat format = detect_image_format(data);
  if (detected) *detected = format;
  if (format == ImageFormat::Jpeg && decode_jpeg_hardware(data, out)) {
    if (path) *path = CodecPath::Hardware;
    return true;
  }
  out->clear();
  if (path) *path = CodecPath::Cpu;
  return media::decode_image(data, out, nullptr);
}

bool MediaCodecs::encode_image(const Image& image, ImageFormat format, uint32_t quality, ByteBuffer* out, CodecPath* path) const {
  if (format == ImageFormat::Jpeg) {
    const uint64_t mark = out->size();
    if (encode_jpeg_hardware(image, quality, out)) {
      if (path) *path = CodecPath::Hardware;
      return true;
    }
    out->truncate(mark);
  }
  if (path) *path = CodecPath::Cpu;
  return media::encode_image(image, format, quality, out);
}

bool MediaCodecs::decode_audio(ConstByteView data, AudioData* out, AudioFileFormat* detected, CodecPath* path) const {
  if (path) *path = CodecPath::Cpu;
  return media::decode_audio(data, out, detected);
}

bool MediaCodecs::encode_audio(const AudioData& audio, AudioFileFormat format, ByteBuffer* out, CodecPath* path) const {
  if (path) *path = CodecPath::Cpu;
  return media::encode_audio(audio, format, out);
}

bool MediaCodecs::decode_video(ConstByteView data, Image* out, VideoFileFormat* detected) const {
  const VideoFileFormat format = detect_video_file(data);
  if (detected) *detected = format;
  switch (format) {
    case VideoFileFormat::Y4m: return decode_y4m(data, out);
    case VideoFileFormat::Gif:
    case VideoFileFormat::Apng: return media::decode_image(data, out, nullptr);
    default: out->clear(); return false;
  }
}

}  // namespace media
