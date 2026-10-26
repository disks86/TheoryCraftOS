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
#include "Hardware/VideoCodec/AMD/AmdVce3Messages.hpp"
#include "Hardware/VideoCodec/AMD/AmdVce3Ring.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnWorkMemory.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/IVideoEncoder.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An H.264 encoding session on the VCE 3.4 encode engine (Radeon RX Vega M). Pictures go in as the session's input format, are converted to I420 and packed into a limited-range
// NV12 surface in the engine's work memory, and the engine is sent one task per picture (AmdVce3Messages: session, encode, feedback) through its ring (AmdVce3Ring); the
// compressed access unit is read back from the bitstream buffer. The first picture opens the engine's session (create, then the configuration). The engine inserts the SPS and PPS
// in front of every IDR picture.
//
// Baseline-profile I and P pictures with one reference picture, the quantiser fixed from the quality (CQP): quality 1 .. 100 maps to QP 51 .. 15 (quality 75 is QP 24). A keyframe is an IDR
// picture and starts a group of pictures: `keyframe_interval` pictures (0 or 1: every picture is a keyframe). Nothing is held back (flush() does nothing).
//
// One session owns the engine's work memory at a time. A job that does not finish wedges the ring for good: every later picture is Invalid.
class AmdVce3Encoder final : public IVideoEncoder {
 public:
  struct Engine {
    AmdVce3Ring* ring = nullptr;
    AmdVcnWorkMemory* memory = nullptr;
    uint32_t stream_handle = 1;        // names the session to the firmware
    uint32_t job_timeout_ms = 2000;    // a picture takes milliseconds; two seconds is a dead engine
  };

  AmdVce3Encoder(allocators::IAllocator& allocator, const Engine& engine, const EncoderConfig& config);
  ~AmdVce3Encoder();
  AmdVce3Encoder(const AmdVce3Encoder&) = delete;
  AmdVce3Encoder& operator=(const AmdVce3Encoder&) = delete;

  // Takes the engine's lease and allocates the work buffers for the configured size. False when the configuration is not one the engine takes (H.264, within the limits, a
  // supported input format), the engine is busy, or the work memory is too small.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::H264; }
  SubmitResult submit(const VideoFrame& frame) override;
  bool receive(VideoPacket* packet) override;
  void flush() override {}
  VideoCodecStatistics statistics() const override { return stats_; }

  bool session_open() const { return open_; }
  uint64_t pictures_encoded() const { return encoded_; }

 private:
  bool open_session();
  void close_session();
  bool run(amd_vce3::TaskWriter& writer, ArrayView<uint32_t> storage);
  void pack_input(const VideoFrameBuffer& i420);

  allocators::IAllocator& allocator_;
  Engine engine_;
  EncoderConfig config_;
  amd_vce3::EncoderSetup setup_;
  amd_vce3::PictureLayout layout_;
  uint32_t slots_ = 0;
  bool leased_ = false, open_ = false;
  AmdVcnWorkMemory::Piece input_, cpb_, bitstream_, feedback_;
  uint32_t bitstream_bytes_ = 0;
  VideoFrameBuffer yuv_;
  ByteBuffer packed_, out_, scratch_;
  // The group of pictures: where in it, the last picture's state (the next P picture's reference), and which buffer slot it is in.
  uint32_t gop_position_ = 0, frame_num_ = 0, pictures_ = 0;
  uint32_t last_slot_ = 0;
  amd_vce3::ReferencePicture last_;
  bool pending_ = false, key_ = false;
  int64_t pts_ = 0;
  uint64_t encoded_ = 0;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
