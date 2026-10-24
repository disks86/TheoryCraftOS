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
#include "Hardware/VideoCodec/AMD/AmdVcn1Ring.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnAvcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnDecodeMessages.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnWorkMemory.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/Software/Av1.hpp"
#include "Hardware/VideoCodec/Software/Av1Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An AV1 decoding session on a VCN decode engine. The CPU decoder (Av1Decoder) parses every OBU, checks the headers, keeps the reference slots and the output; what
// it hands to this class, through its offload interface, is the frame: the headers and the tile data. For every frame the engine is sent what Mesa sends it - a message
// with the frame header's fields and which engine picture buffer each reference slot holds, the tiles back to back, the buffers it works in (the context buffer
// starts with four initial entropy contexts) - and writes the decoded picture, as linear NV12, into a target surface the CPU reads back into the CPU decoder's picture
// (so the picture comes out, converted, like any other, and a later show_existing_frame needs nothing from the engine).
//
// What the engine is given: 8-bit 4:2:0 streams with no film grain, no super-resolution and no spatial layers, up to 4096 x 2304. Anything else is decoded by the
// CPU, the whole stream. One session owns the engine's work memory at a time. A job that does not finish wedges the ring for good: every later frame is damaged
// (grey); nothing falls back to the CPU in the middle of a stream, since the CPU decoder would not have the reference pictures.
//
// This class drives the decode ring of AmdVcn1Ring (the VCN 1.0 register protocol); the engines that decode AV1 are newer (VCN 3.1.2 and 4) and need their own ring and
// start sequence, which this OS does not have yet.
class AmdVcnAv1Decoder final : public IVideoDecoder, private av1::IOffload {
 public:
  using Engine = AmdVcnAvcDecoder::Engine;

  AmdVcnAv1Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config);
  ~AmdVcnAv1Decoder();
  AmdVcnAv1Decoder(const AmdVcnAv1Decoder&) = delete;
  AmdVcnAv1Decoder& operator=(const AmdVcnAv1Decoder&) = delete;

  // Takes the engine's lease and reads the av1C record, if the config has one. False when the engine is busy or the record is not usable. The session with the engine
  // opens at the first frame, once the stream's size is known.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Av1; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;

  const amd_vcn::Feedback& last_feedback() const { return feedback_; }
  bool session_open() const { return open_; }
  uint64_t frames_decoded() const { return decoded_; }

 private:
  // av1::IOffload
  bool accepts(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr) override;
  bool decode_frame(const av1::OffloadFrame& frame) override;
  void show_existing(unsigned idx, bool refresh_all) override;
  void new_sequence() override;

  bool open_session(const av1::SequenceHeader& seq);
  bool allocate_buffers();
  void close_session();
  bool send_session_message(bool create);
  bool gather_bitstream(ArrayView<const av1::TileData> tiles);
  void read_back(const av1::OffloadFrame& frame, const amd_vcn::TargetLayout& layout);
  uint8_t pick_slot() const;

  allocators::IAllocator& allocator_;
  Engine engine_;
  DecoderConfig config_;
  Av1Decoder cpu_;
  bool leased_ = false;
  bool open_ = false;
  uint32_t max_width_ = 0, max_height_ = 0;

  amd_vcn::Av1BufferSizes sizes_;
  amd_vcn::TargetLayout layout_;   // for the biggest picture of the stream: the target surface is made for it
  AmdVcnWorkMemory::Piece message_, session_context_, context_, dpb_buffer_, bitstream_, target_;
  ByteBuffer host_bits_, host_message_, row_;
  uint8_t ref_slot_[8];            // the engine picture buffer each of the stream's eight reference slots holds
  amd_vcn::Feedback feedback_;
  uint32_t picture_number_ = 0;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
