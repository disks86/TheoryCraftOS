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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"
#include "Hardware/VideoCodec/IVideoEncoder.hpp"
#include "Hardware/VideoCodec/Intel/IntelHevcEncodeCommands.hpp"
#include "Hardware/VideoCodec/Intel/IntelHevcHeaders.hpp"
#include "Hardware/VideoCodec/Intel/IntelEngineBuffers.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An H.265 encoding session on the video engine of an Xe-LP (Gen12: Tiger Lake and later) Intel GPU in VDEnc mode. Each picture is converted to limited-range NV12 and tiled into
// the engine's source surface; one batch (IntelHevcEncodeCommands) describes the picture, the headers to insert and the slice, and the VDEnc engine searches and decides while
// the HCP engine codes the CABAC slice data and reconstructs. The access unit is read back from the bitstream buffer, its size from the register the batch stored.
//
// Main-profile I and P pictures (the engine codes a P picture as a low-delay B one), 64x64 coding tree blocks, one slice, one reference picture, the quantiser fixed from the quality
// (CQP): quality 1 .. 100 maps to QP 51 .. 15 (75 is QP 24). A keyframe is an IDR picture preceded by the parameter sets; `keyframe_interval` pictures make a group (0 or 1: every
// picture is a keyframe). Nothing is held back. Width and height must be even.
//
// Takes the engine's lease; a job that does not finish wedges the engine for good and every later picture is Invalid.
class IntelHevcVdencEncoder final : public IVideoEncoder {
 public:
  IntelHevcVdencEncoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const EncoderConfig& config);
  ~IntelHevcVdencEncoder() = default;
  IntelHevcVdencEncoder(const IntelHevcVdencEncoder&) = delete;
  IntelHevcVdencEncoder& operator=(const IntelHevcVdencEncoder&) = delete;

  // Takes the lease and allocates the surfaces and scratch buffers. False when the configuration is not one the engine takes (H.265, even sizes of 16 .. 4096 samples, a supported
  // input format), the engine is not an Xe-LP one, or it is busy or out of memory.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Hevc; }
  SubmitResult submit(const VideoFrame& frame) override;
  bool receive(VideoPacket* packet) override;
  void flush() override {}
  VideoCodecStatistics statistics() const override { return stats_; }

  uint64_t pictures_encoded() const { return encoded_; }
  uint32_t quantiser() const { return qp_; }

 private:
  bool build_batch(bool idr, uint32_t poc, uint32_t* words);

  allocators::IAllocator& allocator_;
  EncoderConfig config_;
  IntelEngineBuffers buffers_;
  intel_enc::HevcEncodeLayout layout_;
  intel_enc::HevcScratchPlan plan_;
  intel_enc::HevcEncodeBuffers addresses_;
  intel_enc::HevcStreamHeaders stream_;
  uint32_t qp_ = 24, bitstream_bytes_ = 0;
  VideoFrameBuffer yuv_;
  ByteBuffer headers_, out_;
  bool prepared_ = false, pending_ = false, key_ = false;
  uint32_t gop_position_ = 0, poc_ = 0, recon_slot_ = 0;
  uint64_t pictures_ = 0, encoded_ = 0;
  int64_t pts_ = 0;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
