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
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/IVideoCodecDevice.hpp"

namespace hardware {

// The video codec that always exists: sessions that run on the CPU. It decodes and encodes
//   - Motion JPEG (every picture a baseline JPEG; see Jpeg.hpp), the format of many cameras and simple capture files, and
//   - raw pictures (I420, NV12 or Bgra8888 frames one after the other), and decodes
//   - H.264 / AVC (progressive Baseline, Main and High profile; see H264.hpp),
// at any size up to VideoFormat::kMaxDimension. Codecs that need a real decode engine (H.265, VP9, AV1) are not
// offered: the engines in GPUs and media chips are not driven by this OS yet, and they are the only way those
// are served (and H.264 encoding is theirs too). The vendor classes (NvidiaVideoEngine, AmdVcnVideoEngine, IntelMediaEngine) are this device plus the
// knowledge that such an engine is on the machine.
class SoftwareVideoCodec : public IVideoCodecDevice {
 public:
  explicit SoftwareVideoCodec(allocators::IAllocator& allocator, bool hardware_engine = false) : allocator_(allocator) {
    info_.hardware_engine = hardware_engine;
    info_.accelerated = false;
    info_.encode_codecs = codec_mask(VideoCodec::Raw) | codec_mask(VideoCodec::Mjpeg);
    info_.decode_codecs = info_.encode_codecs | codec_mask(VideoCodec::H264);
    info_.max_width = info_.max_height = VideoFormat::kMaxDimension;
  }

  const char* name() const override { return "Software video codec"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  const VideoCodecInfo& info() const override { return info_; }
  UniquePointer<IVideoDecoder> create_decoder(const DecoderConfig& config) override;
  UniquePointer<IVideoEncoder> create_encoder(const EncoderConfig& config) override;

 private:
  allocators::IAllocator& allocator_;
  VideoCodecInfo info_;
};

}  // namespace hardware
