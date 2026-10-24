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
#include "Hardware/VideoCodec/Software/Vp9.hpp"
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// A VP9 decoding session on the VCN 1.0 decode engine. The CPU decoder (Vp9Decoder) reads every uncompressed header (frame sizes, loop filter, quantizer and
// segmentation parameters, reference slots), keeps the reference slots and the output; what it hands to this class, through its offload interface, is the frame: those
// parameters and the frame's bytes. For every frame the engine is sent what Mesa sends it - a message with the frame header's fields and which engine picture buffer each
// reference slot holds, the frame as it is (uncompressed header, compressed header and tiles), the buffers it works in (the context buffer and the table next to the
// message start with the default probabilities) - and writes the decoded picture, as linear NV12, into a target surface the CPU reads back into the CPU decoder's picture
// (so the picture comes out, converted, like any other, and a later show_existing_frame needs nothing from the engine).
//
// What the engine is given: 8-bit 4:2:0 streams (profile 0) up to 4096 x 2304. Anything else is decoded by the CPU, the whole stream. The session is made for the size
// of the key frame that starts the stream; a later frame that is bigger (reference scaling up) cannot be decoded and is shown grey. One session owns the engine's work
// memory at a time. A job that does not finish wedges the ring for good: every later frame is damaged (grey); nothing falls back to the CPU in the middle of a stream, since
// the CPU decoder would not have the reference pictures.
class AmdVcnVp9Decoder final : public IVideoDecoder, private vp9::IOffload {
 public:
  using Engine = AmdVcnAvcDecoder::Engine;

  AmdVcnVp9Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config);
  ~AmdVcnVp9Decoder();
  AmdVcnVp9Decoder(const AmdVcnVp9Decoder&) = delete;
  AmdVcnVp9Decoder& operator=(const AmdVcnVp9Decoder&) = delete;

  // Takes the engine's lease. False when the engine is busy. The session with the engine opens at the first key frame, once its size is known.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Vp9; }
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
  // vp9::IOffload
  bool accepts(const vp9::FrameHeaderInfo& info, int ss_x, int ss_y) override;
  bool decode_frame(const vp9::OffloadFrame& frame) override;

  bool open_session(uint32_t width, uint32_t height);
  bool allocate_buffers();
  void close_session();
  bool send_session_message(bool create);
  void read_back(const vp9::OffloadFrame& frame, const amd_vcn::TargetLayout& layout);
  uint8_t pick_slot() const;

  allocators::IAllocator& allocator_;
  Engine engine_;
  DecoderConfig config_;
  Vp9Decoder cpu_;
  bool leased_ = false;
  bool open_ = false;
  uint32_t width_ = 0, height_ = 0;   // the size the session was made for

  amd_vcn::Vp9BufferSizes sizes_;
  amd_vcn::TargetLayout layout_;
  AmdVcnWorkMemory::Piece message_, session_context_, context_, dpb_buffer_, bitstream_, target_;
  ByteBuffer host_bits_, host_message_, row_;
  uint8_t ref_slot_[8];               // the engine picture buffer each of the stream's eight reference slots holds
  amd_vcn::Feedback feedback_;
  uint32_t picture_number_ = 0;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
