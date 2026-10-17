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

#pragma once
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/ImageCodecs.hpp"
#include "Media/VideoFiles.hpp"

namespace media {

// Where the work of a call ran.
enum class CodecPath : uint8_t {
  Cpu,       // the software codecs in Media/
  Hardware,  // a codec device that really accelerates the format (a GPU's JPEG/video engine)
};

// One front door for decoding and encoding pictures and sounds. Each call goes to the best way the machine has: the formats
// a video codec device accelerates (JPEG, as Motion JPEG pictures) are given to that device first, and when there is
// none, or the device refuses the data, the software codecs do the job - so a call that works on the CPU still works
// when an engine is flaky or absent. Everything else (PNG, GIF, BMP, QOI, PNM, TGA, WAV, AIFF, AU, FLAC) is software; none
// of those has a hardware engine on current machines.
//
// `video` may be null (no codec devices: everything runs on the CPU). Not thread safe by itself, but it keeps no state
// apart from the pointer: use one per thread, or serialise calls, when sharing the codec devices.
class MediaCodecs {
 public:
  explicit MediaCodecs(hardware::SystemVideoCodecs* video = nullptr) : video_(video) {}

  bool decode_image(ConstByteView data, Image* out, ImageFormat* detected = nullptr, CodecPath* path = nullptr) const;
  bool encode_image(const Image& image, ImageFormat format, uint32_t quality, ByteBuffer* out, CodecPath* path = nullptr) const;
  bool decode_audio(ConstByteView data, AudioData* out, AudioFileFormat* detected = nullptr, CodecPath* path = nullptr) const;
  bool encode_audio(const AudioData& audio, AudioFileFormat format, ByteBuffer* out, CodecPath* path = nullptr) const;
  // Any video file the player reads by decoding it into frames: GIF, APNG, animated WebP or Y4M (AVI is read by the player itself).
  bool decode_video(ConstByteView data, Image* out, VideoFileFormat* detected = nullptr) const;

  // True if JPEG pictures would go to an accelerating device right now.
  bool jpeg_accelerated() const { return accelerated_device(/*encode=*/false) != nullptr; }
  bool jpeg_encode_accelerated() const { return accelerated_device(/*encode=*/true) != nullptr; }

 private:
  hardware::IVideoCodecDevice* accelerated_device(bool encode) const;
  bool decode_jpeg_hardware(ConstByteView data, Image* out) const;
  bool encode_jpeg_hardware(const Image& image, uint32_t quality, ByteBuffer* out) const;

  hardware::SystemVideoCodecs* video_;
};

}  // namespace media
