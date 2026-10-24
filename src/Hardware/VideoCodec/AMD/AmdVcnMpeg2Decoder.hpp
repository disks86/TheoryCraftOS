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
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An MPEG-2 decoding session on the VCN 1.0 decode engine. The CPU decoder (Mpeg2Decoder) reads the sequence and picture headers, orders the pictures and keeps the
// output queue; what it hands to this class, through its offload interface, is each picture's headers and slices. For every picture the engine is sent what Mesa
// sends it: a message with the picture coding parameters, both quantiser matrices and the picture buffer slots of the picture and of the pictures it predicts from, the
// slices behind it with their start codes, and the buffers it works in. The engine keeps the reference pictures in the picture buffer, in the slots this class names,
// and writes the decoded picture, as linear NV12, into a target surface the CPU reads back into the picture of the CPU decoder (so it comes out, cropped and
// converted, like any other).
//
// The CPU decoder's pictures live in a pool of ten; the engine's buffer holds six. This class gives the pictures that are still needed (the two anchors, the picture
// being decoded) a slot each, and takes the slots back when the CPU decoder has dropped a picture. For a field picture only the lines of its field are read back.
//
// A sequence the engine cannot take (a size beyond the limits) is decoded on the CPU, whole sequences at a time. One session owns the engine's work memory at a time.
// A job that does not finish wedges the ring for good: every later picture is damaged (grey); nothing falls back to the CPU in the middle of a stream.
class AmdVcnMpeg2Decoder final : public IVideoDecoder, private mpeg2::IOffload {
 public:
  using Engine = AmdVcnAvcDecoder::Engine;

  AmdVcnMpeg2Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config);
  ~AmdVcnMpeg2Decoder();
  AmdVcnMpeg2Decoder(const AmdVcnMpeg2Decoder&) = delete;
  AmdVcnMpeg2Decoder& operator=(const AmdVcnMpeg2Decoder&) = delete;

  // Takes the engine's lease. False when the engine is busy. The session with the engine opens at the first picture, once its size is known.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Mpeg2; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;

  // What the engine reported about the last picture (the feedback buffer), for diagnostics.
  const amd_vcn::Feedback& last_feedback() const { return feedback_; }
  bool session_open() const { return open_; }
  uint64_t pictures_decoded() const { return decoded_; }

 private:
  // mpeg2::IOffload
  bool accepts(const mpeg2::SequenceHeader& seq) override;
  bool decode_picture(const mpeg2::OffloadPicture& picture) override;

  bool open_session(const mpeg2::SequenceHeader& seq);
  bool allocate_buffers();
  void close_session();
  bool send_session_message(bool create);
  bool gather_bitstream(ArrayView<const ConstByteView> slices);
  bool assign_slots(const mpeg2::OffloadPicture& picture, amd_vcn::Mpeg2Slots* slots);
  void read_back(const mpeg2::OffloadPicture& picture);

  static constexpr int kNoSlot = -1;

  allocators::IAllocator& allocator_;
  Engine engine_;
  DecoderConfig config_;
  Mpeg2Decoder cpu_;
  bool leased_ = false;    // this session holds the work memory's lease
  bool open_ = false;      // the engine has a session
  uint32_t width_ = 0, height_ = 0;

  amd_vcn::Mpeg2BufferSizes sizes_;
  amd_vcn::TargetLayout layout_;
  AmdVcnWorkMemory::Piece message_, session_context_, dpb_buffer_, bitstream_, target_;
  ByteBuffer host_bits_, host_message_, row_;
  amd_vcn::Feedback feedback_;
  int engine_slot_of_[mpeg2::kPoolSize];                    // the engine slot of a CPU decoder picture (kNoSlot: none)
  int owner_[amd_vcn::kMpeg2References];                    // the CPU decoder picture in an engine slot (kNoSlot: free)
  uint32_t picture_number_ = 0;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
