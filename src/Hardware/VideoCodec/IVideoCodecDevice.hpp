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
#include "Hardware/IHardware.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/IVideoEncoder.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct VideoCodecInfo {
  // A video engine of its own is on this machine (a GPU's decode/encode block, a media chip).
  bool hardware_engine = false;
  // The sessions run on that engine. False means they run on the CPU - either because there is no engine, or
  // because this OS has no driver for it yet.
  bool accelerated = false;
  // Codecs whose sessions run on the engine even though the device as a whole is not `accelerated` (a driver for part of the engine's work). codec_mask() bits.
  uint32_t accelerated_decode_codecs = 0;
  uint32_t accelerated_encode_codecs = 0;
  uint32_t decode_codecs = 0;  // codec_mask() bits create_decoder() can serve
  uint32_t encode_codecs = 0;  // and create_encoder()
  uint32_t max_width = 0, max_height = 0;
};

// Something that decodes and encodes video: a hardware engine, or the software fallback that is always
// there. A program asks the device for a session for the codec it needs. Several sessions may be open at
// once; they are independent.
class IVideoCodecDevice : public IHardware {
 public:
  virtual const VideoCodecInfo& info() const = 0;
  bool can_decode(VideoCodec codec) const { return info().decode_codecs & codec_mask(codec); }
  bool can_encode(VideoCodec codec) const { return info().encode_codecs & codec_mask(codec); }
  // A new session, or an empty pointer if the codec or size is not supported or memory ran out.
  virtual UniquePointer<IVideoDecoder> create_decoder(const DecoderConfig& config) = 0;
  virtual UniquePointer<IVideoEncoder> create_encoder(const EncoderConfig& config) = 0;
  // One line on what the device found out about its hardware (the GPU generation, the video engines the probe saw), for the boot log; empty if it has nothing to say.
  // Valid until the device is shut down.
  virtual const char* engine_summary() const { return ""; }

 protected:
  constexpr IVideoCodecDevice() = default;
  ~IVideoCodecDevice() = default;
};

}  // namespace hardware
