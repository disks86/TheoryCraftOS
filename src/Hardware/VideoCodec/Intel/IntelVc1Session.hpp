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
#include "Hardware/VideoCodec/Software/Vc1.hpp"
#include "Hardware/VideoCodec/Software/Vc1Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// A VC-1 decoding session on the video engine of a Gen9.5 Intel GPU. The CPU decoder (Vc1Decoder) reads the sequence header and each picture's header (it needs them to
// order the pictures), and hands the picture to this class through its offload interface. The engine reads no picture header: the picture state of the batch, built the
// way the reference driver (gen8_mfd.c) builds it, says everything the header says (type, quantizer, motion vector mode, transform and table selections, bit plane use,
// loop filter and overlap), the bit planes go in a buffer of their own, and the frame and its slices go in as bitstream objects that start at their first macroblock. The
// engine decodes into a tiled NV12 surface of its own (one per picture of the CPU decoder's pool, which is also what it predicts from) and keeps each P picture's
// direct motion vectors in a buffer for the B pictures after it; the surface is read back into the picture of the CPU decoder.
//
// A skipped picture (it repeats the one before it) is a copy of that picture's surface; the engine is not sent anything for it. Simple, Main and Advanced profile
// progressive pictures; a sequence the engine cannot take is decoded on the CPU, whole sequences at a time. One session owns the engine at a time. A job that does not
// finish wedges the engine: every later picture is damaged (grey); nothing falls back to the CPU in the middle of a stream.
class IntelVc1Decoder final : public IVideoDecoder, private vc1::IOffload {
 public:
  IntelVc1Decoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config);
  ~IntelVc1Decoder();
  IntelVc1Decoder(const IntelVc1Decoder&) = delete;
  IntelVc1Decoder& operator=(const IntelVc1Decoder&) = delete;

  // Takes the engine's lease and reads the sequence header of a Simple or Main profile stream (the codec configuration). False when the engine is not usable or busy with
  // another session, or the header is not one the decoder takes.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Vc1; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;
  uint64_t pictures_decoded() const { return decoded_; }

 private:
  // vc1::IOffload
  bool accepts(const vc1::SequenceHeader& seq) override;
  bool decode_picture(const vc1::OffloadPicture& picture) override;

  bool build_bitplane(const vc1::OffloadPicture& picture, uint32_t* address);

  allocators::IAllocator& allocator_;
  DecoderConfig config_;
  IntelEngineBuffers buffers_;
  Vc1Decoder cpu_;
  ByteBuffer host_bits_, host_plane_;
  int picture_type_[vc1::kPoolSize];   // what the picture in each pool entry was (the vc1::PictureType, 4: skipped), for the direct motion vector buffers
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
