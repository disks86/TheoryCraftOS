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
#include "Hardware/VideoCodec/AMD/AmdUvd6AvcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Messages.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Ring.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnWorkMemory.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/Software/Vc1.hpp"
#include "Hardware/VideoCodec/Software/Vc1Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// A VC-1 decoding session on the UVD 6 decode engine (Radeon RX Vega M). The CPU decoder (Vc1Decoder) reads the sequence header and each picture's header, orders the pictures and
// keeps the output queue; what it hands to this class, through its offload interface, is each picture's data: the frame (Advanced profile: the frame and slice units,
// each behind its start code; Simple and Main profile: the frame as the container stores it). For every picture the engine is sent what Mesa sends it: a message with
// the profile and the sequence and entry point flags (radeon_uvd.c's get_vc1_msg: struct ruvd_vc1 has no picture indexes), the frame data behind it (the firmware parses the
// picture header, the bit planes and the macroblocks itself), and the buffers it works in. The engine keeps the decoded pictures and finds the anchors by decoding order,
// and writes the decoded picture, as linear NV12, into a target surface the CPU reads back into the picture of the CPU decoder (so it comes out, cropped and converted,
// like any other). A skipped P picture (Advanced profile: it repeats the one before it) is not sent to the engine; the CPU decoder copies it. Unlike VCN's, the
// firmware's feedback area is not read back.
//
// A sequence the engine cannot take (a size beyond the limits) is decoded on the CPU, whole sequences at a time. One session owns the engine's work memory at a time.
// A job that does not finish wedges the ring for good: every later picture is damaged (grey); nothing falls back to the CPU in the middle of a stream.
class AmdUvd6Vc1Decoder final : public IVideoDecoder, private vc1::IOffload {
 public:
  using Engine = AmdUvd6AvcDecoder::Engine;

  AmdUvd6Vc1Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config);
  ~AmdUvd6Vc1Decoder();
  AmdUvd6Vc1Decoder(const AmdUvd6Vc1Decoder&) = delete;
  AmdUvd6Vc1Decoder& operator=(const AmdUvd6Vc1Decoder&) = delete;

  // Takes the engine's lease and gives the CPU decoder the sequence header of a Simple or Main profile stream (the codec configuration). False when the engine is busy or
  // the header is not one the decoder takes. The session with the engine opens at the first picture, once its size is known.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Vc1; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;

  bool session_open() const { return open_; }
  uint64_t pictures_decoded() const { return decoded_; }

 private:
  // vc1::IOffload
  bool accepts(const vc1::SequenceHeader& seq) override;
  bool decode_picture(const vc1::OffloadPicture& picture) override;

  bool open_session(const vc1::SequenceHeader& seq);
  bool allocate_buffers();
  void close_session();
  bool send_session_message(bool create);
  bool gather_bitstream(const vc1::OffloadPicture& picture);
  void read_back(const vc1::OffloadPicture& picture);

  allocators::IAllocator& allocator_;
  Engine engine_;
  DecoderConfig config_;
  Vc1Decoder cpu_;
  bool leased_ = false;    // this session holds the work memory's lease
  bool open_ = false;      // the engine has a session
  uint32_t width_ = 0, height_ = 0;

  amd_uvd6::Vc1BufferSizes sizes_;
  amd_vcn::TargetLayout layout_;
  AmdVcnWorkMemory::Piece message_, session_context_, dpb_buffer_, bitstream_, target_;
  ByteBuffer host_bits_, host_message_, row_;
  uint32_t picture_number_ = 0;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
