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
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/Intel/IntelEngineBuffers.hpp"
#include "Hardware/VideoCodec/Intel/IntelMfxCommands.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An MPEG-2 decoding session on the video engine of a Gen9.5 Intel GPU. The CPU decoder (Mpeg2Decoder) reads the sequence and picture headers, orders the pictures and
// keeps the output queue; what it hands to this class, through its offload interface, is each picture's headers and slices. For every picture a command batch is built
// the way the reference driver (gen8_mfd.c) builds it - pipe mode, surface, buffer addresses, picture state, both quantiser matrices, then a bitstream object for each
// slice - and run on the engine; the engine decodes into a tiled NV12 surface of its own (one per picture of the CPU decoder's pool, which is also what it predicts
// from), which is read back into the picture of the CPU decoder (so it comes out, cropped and converted, like any other). For a field picture only its field's lines.
//
// A sequence the engine cannot take (a size beyond the limits, or an engine that is not ready) is decoded on the CPU, whole sequences at a time. One session owns the
// engine at a time. A job that does not finish wedges the engine: every later picture is damaged (grey); nothing falls back to the CPU in the middle of a stream.
class IntelMpeg2Decoder final : public IVideoDecoder, private mpeg2::IOffload {
 public:
  IntelMpeg2Decoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config);
  ~IntelMpeg2Decoder();
  IntelMpeg2Decoder(const IntelMpeg2Decoder&) = delete;
  IntelMpeg2Decoder& operator=(const IntelMpeg2Decoder&) = delete;

  // Takes the engine's lease. False when the engine is not usable or busy with another session, or a codec configuration was given (MPEG-2 has none).
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Mpeg2; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override { cpu_.reset(); }
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;
  uint64_t pictures_decoded() const { return decoded_; }

 private:
  // mpeg2::IOffload
  bool accepts(const mpeg2::SequenceHeader& seq) override;
  bool decode_picture(const mpeg2::OffloadPicture& picture) override;

  bool build_slices(const mpeg2::OffloadPicture& picture, ArrayView<intel_mfx::Mpeg2SliceJob> jobs, uint64_t* count);

  allocators::IAllocator& allocator_;
  DecoderConfig config_;
  IntelEngineBuffers buffers_;
  Mpeg2Decoder cpu_;
  ByteBuffer host_bits_;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
