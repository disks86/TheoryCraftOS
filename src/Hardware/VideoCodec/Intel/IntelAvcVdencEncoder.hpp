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
#include "Hardware/VideoCodec/Intel/IntelAvcHeaders.hpp"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommands.hpp"
#include "Hardware/VideoCodec/Intel/IntelEngineBuffers.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An H.264 encoding session on the video engine of a Gen9.5 (Kaby Lake, Coffee Lake, Comet Lake), Gen11 (Ice Lake) or Gen12 (Xe-LP) Intel GPU in VDEnc mode. Each picture is converted to limited-range NV12
// and tiled into the engine's source surface; one batch (IntelEncodeCommands) describes the picture, the headers to insert and the slice, and the engine searches, decides,
// codes and reconstructs on its own. The access unit is read back from the bitstream buffer, its size from the register the batch stored.
//
// Baseline-profile I and P pictures, one slice, one reference picture, the quantiser fixed from the quality (CQP): quality 1 .. 100 maps to QP 51 .. 15 (75 is QP 24). A keyframe is
// an IDR picture preceded by the parameter sets; `keyframe_interval` pictures make a group (0 or 1: every picture is a keyframe). Nothing is held back.
//
// Takes the engine's lease; a job that does not finish wedges the engine for good and every later picture is Invalid.
class IntelAvcVdencEncoder final : public IVideoEncoder {
 public:
  IntelAvcVdencEncoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const EncoderConfig& config);
  ~IntelAvcVdencEncoder() = default;
  IntelAvcVdencEncoder(const IntelAvcVdencEncoder&) = delete;
  IntelAvcVdencEncoder& operator=(const IntelAvcVdencEncoder&) = delete;

  // Takes the lease and allocates the surfaces and scratch buffers. False when the configuration is not one the engine takes (H.264, 16 .. 4096 samples a side, a supported
  // input format) or the engine is busy or out of memory.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::H264; }
  SubmitResult submit(const VideoFrame& frame) override;
  bool receive(VideoPacket* packet) override;
  void flush() override {}
  VideoCodecStatistics statistics() const override { return stats_; }

  uint64_t pictures_encoded() const { return encoded_; }
  uint32_t quantiser() const { return qp_; }

 private:
  intel_enc::EncodeGeneration generation() const {
    return buffers_.gen12() ? intel_enc::EncodeGeneration::Gen12 : buffers_.gen11() ? intel_enc::EncodeGeneration::Gen11 : intel_enc::EncodeGeneration::Gen9;
  }
  bool build_batch(bool idr, uint32_t frame_num, uint32_t* words);

  allocators::IAllocator& allocator_;
  EncoderConfig config_;
  IntelEngineBuffers buffers_;
  intel_enc::AvcEncodeLayout layout_;
  intel_enc::AvcEncodeBuffers addresses_;
  intel_enc::AvcStreamHeaders stream_;
  uint32_t width_mbs_ = 0, height_mbs_ = 0, qp_ = 24, bitstream_bytes_ = 0;
  VideoFrameBuffer yuv_;
  ByteBuffer headers_, out_;
  bool prepared_ = false, pending_ = false, key_ = false;
  uint32_t gop_position_ = 0, frame_num_ = 0, idr_id_ = 0, recon_slot_ = 0;
  uint64_t pictures_ = 0, encoded_ = 0;
  int64_t pts_ = 0;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
